/*
 * 
 Geiger counter based on DIY Assembled Geiger Counters Kit: https://www.aliexpress.com/item/4000229395987.html
 Display based on TTGO ESP32 1.14 inch LCD display Module:  https://www.aliexpress.com/item/4000606780227.html
 
 The TTGO contains an ST7735 TFT LCD screen

 The Geiger counter module provides a TIC pulse to GPIO27 which triggers an interrupt to increase the internal counter.
 
 The Geiger counter comes with a 3x AA battery housing and runs fine on 4.5V. The ESP32 also runs fine on 4.5V. In order
 to monitor the battery condition, I added 2 resistors (20k between 4.5V and GPIO26 and 33k between GPIO26 and GND), so 
 we will can use ADC read on GPIO26 to measure the battery voltage.
 
 Calculation of radiation dose: 1 Sievert (Sv) = 100 Rem = 100,000 mR ==> 1 μSv = 0.1 mR
 Doses is indicated in mR/hour.
 Background radiation is 365 mR/year => 0.042 mR/hour
 Max doses radiation workers 5000 mR/year => 0.57 mR/hour

 The tube is more or less lineair. See https://www.sparkfun.com/datasheets/Components/General/LND-712-Geiger-Tube.pdf
 so we can use the number of tics/ 10 seconds to estimate the doses in mR/hour:
    tics_per_10sec * TICFACTOR

 Analog indicator will be logarithmic: 0.1 <- green -> 1 <- orange -> 10 <- red -> 100 (mR/hour)
 The analog indicator has a range 0..100, so to display mR/hour on this scale we have to calculate
 33.3333*(log10(mRhour)+1)

 The bitmap shown on the analog display is converted from radiation64.png to .c or .h file using
 http://www.rinkydinkelectronics.com/_t_doimageconverter565.php

 We are using the esp32 touch sensor routine on GPIO12 to determine if a user wants to toggle between the
 analog meter display mode and the bar graph with historic data over the last 7 minutes (also showing battery condition).

*/

#include <TFT_eSPI.h>       // Hardware-specific library for TFT display
#include <SPI.h>
#include "radiation64.h"    // icon file with yellow radiation symbol
#include "Pangodream_18650_CL.h"

// Define Hardware PIN numbers
#define PINTIC 27           // GPIO27 is tic from geiger counter
#define PINTOUCH 12         // GPIO12 is touch interface display mode switch
#define TICFACTOR 0.05      // factor between number of tics/second --> mR/hr

// Program paramters
#define M_SIZE 1            // Define size of analog meter size (1 = full screen, 0.667 fills 2/3 of screen)
#define TOUCHTHRESHOLD 60   // value from touchRead() to consider the button to be touched (needs to be lower)

#define MIN_USB_VOL 4.9
#define ADC_PIN 34
#define CONV_FACTOR 1.8
#define READS 20

// Bar graph page parameters
#define TFT_GREY 0x5AEB     // color of bar graph outline
#define TEXTBARY 120        // Y position of text on bar graph page
#define BARX 21             // X position of bar graph
#define BARY 8              // Y position of bar graph
#define BARDX 215           // Horizontal size of bar graph
#define BARDY 102           // Vertical size of bar graph
#define BARITEMX 5          // Horizontal size of individual bar (including spacing)
#define BARSPACE 1          // Horizontal spacing between bars
#define BARGREEN 33         // Y threshold value that marks green colored bar
#define BARORANGE 67        // Y threshold value that marks magenta colored bar (red above)

Pangodream_18650_CL BL(ADC_PIN, CONV_FACTOR, READS);

// variables shared between main code and interrupt code
hw_timer_t * timer = NULL;
volatile uint32_t updateTime = 0;       // time for next update
volatile uint16_t tic_cnt = 0;

// To calculate a running average over 10 sec, we keep tic counts in 250ms intervals and add all 40 tic_buf values
#define TICBUFSIZE 40                   // running average buffer size
volatile uint16_t tic_buf[TICBUFSIZE];  // buffer for 40 times 250ms tick values (to have a running average for 10 seconds)
volatile uint16_t tic_buf_cnt = 0;

// In order to display a history of the last 7 minutes, we keep the last 50 values of 10sec tics
#define SEC10BUFSIZE 50                 // history array for displaymode==true
volatile uint16_t sec10 = 0;            // every 10 seconds counter
volatile uint16_t sec10_buf[SEC10BUFSIZE];  // buffer to hold 10 sec history (40*10 = 400 seconds)
volatile bool sec10updated = false;     // set to true when sec10_buf is updated

// Other vars
TFT_eSPI tft = TFT_eSPI();              // TFT custom library
bool displaymode = false;               // false= normal mode (analog meter) or true= history bar graph (and batt value)

// #########################################################################
// Interrupt routine called on each click from the geiger tube
//
void IRAM_ATTR TicISR() {
  tic_cnt++;
}

// #########################################################################
// Interrupt timer routine called every 250 ms
//
void IRAM_ATTR onTimer() {
  tic_buf[tic_buf_cnt++] = tic_cnt;
  tic_cnt = 0;
  if (tic_buf_cnt>=TICBUFSIZE) {
    uint16_t tot = 0;
    for (int i=0; i<TICBUFSIZE; i++) tot += tic_buf[i];
    sec10_buf[sec10++] = tot;
    tic_buf_cnt = 0;    
    if (sec10>=SEC10BUFSIZE) sec10 = 0;
    sec10updated = true;
  }
}

// #########################################################################
// setup code
//
void setup(void) {
  Serial.begin(57600); // For debug
  Serial.println("Geiger counter startup");

  // init TFT screen
  tft.init();
  tft.setRotation(3); 
  tft.fillScreen(TFT_WHITE);
  tft.setSwapBytes(true);
  analogMeterLog(); // Draw analogue meter outline

  updateTime = millis(); // Next update time

  // attach interrupt routine to TIC interface from the geiger counter module
  pinMode(PINTIC, INPUT);
  attachInterrupt(PINTIC, TicISR, FALLING);

  // attach interrupt routine to internal timer, to fire every 250 ms
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 250000, true); // 250 ms
  timerAlarmEnable(timer);

  // sweep needle one time at startup (just because it looks nice)
  plotNeedle(100, 2);
  delay(500);
  plotNeedle(0, 20);
  Serial.println("Geiger counter ready");
}

// #########################################################################
// main loop
//
void loop() {
  static unsigned long flashtime = millis(); // to ensure thr touch pin can only be pressed once/second

  // see if the touch sensor is active
  if (getTouch(PINTOUCH) && flashtime+1000<millis()) {
    // we see touch -> switch display mode
    displaymode = !displaymode;
    tft.fillScreen(TFT_WHITE);
    tft.setSwapBytes(true);
    tft.setTextColor(TFT_BLACK);
    if (displaymode) {
      // now switch to history bar graph mode
      tft.drawString("Batt", 10, TEXTBARY, 2);   // batt indicator lower left corner
      tft.drawString("V", 90, TEXTBARY, 2);
      tft.drawString("mR/hr", 140, TEXTBARY, 2); // value indicator lower right corner
      Serial.println("Set history bar mode");
      barGraph();       // paint bar graph outline
      barUpdateAll();   // display values
    } else {
      // now switch to analog display mode
      analogMeterLog(); // Draw analogue meter outline
      Serial.println("Analog meter mode");
    }
    flashtime = millis(); // ensure we wait 1 sec before switching mode again
  } 

  // curent mR/hr value to display - add all tics from walking average 10 seconds
  uint16_t v=0;
  for (int i=0; i<TICBUFSIZE; i++) {
     v+=tic_buf[i];
  }
  
  // convert tics to mR/hr and put in display buffer for TFT
  float mrem = tics2mrem(v);
  char buf[12];
  dtostrf(mrem, 7, (mrem<1 ? 2: (mrem<10 ? 1 : 0)), buf);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  if (displaymode) {
    // show current value in lower right hand corner
    tft.drawRightString(buf, 230, TEXTBARY, 2);
    
    // show value for batt
    dtostrf(BL.getBatteryVolts(), 7, 2, buf);
    tft.drawRightString(buf, 85, 120, 2);

    // update bar graph (redraw ever 10 secs)
    if (sec10updated) {
      barUpdateAll();
      sec10updated = false;
    } 
    barUpdateLast(mrem);  // update current value all the way to the right
  } else {
    // show value lower left hand corner
    tft.drawRightString(buf, 33, M_SIZE*(119 - 20), 2);
    // update needle of analog display
    plotNeedle(mrem2perc(mrem,100), 10);
  }
  delay(200);  // wait a while
}

// #########################################################################
// Convert tics to mR/hr
//
float tics2mrem(uint16_t tics) {
  return float(tics) * TICFACTOR;
}

// #########################################################################
// convert millirem value to a log percentage on analog and bar graph
//
int mrem2perc(float mrem, int maxperc) {
  if (mrem<=0) {
    return 0;
  } else {
    int v = (float(maxperc)/3)*(log10(mrem)+1);
    if (v>maxperc) v=maxperc;
    if (v<0) v=0;
    return v;
  }
}

// #########################################################################
//  Draw the analogue meter outline on the screen with a logarithmic scale and 3 areas
//
void analogMeterLog() {

  tft.setTextColor(TFT_BLACK);  // Text colour

  // Draw ticks every 5 degrees from -45 to +45 degrees (90 deg. FSD swing)
  for (int i = -45; i <= 45; i += 1) {
    // Long scale tick length
    int tl = 15;

    // Coodinates of tick to draw
    float sx = cos((i - 90) * 0.0174532925);
    float sy = sin((i - 90) * 0.0174532925);
    uint16_t x0 = sx * (M_SIZE*100 + tl) + M_SIZE*120;
    uint16_t y0 = sy * (M_SIZE*100 + tl) + M_SIZE*150;
    uint16_t x1 = sx * M_SIZE*100 + M_SIZE*120;
    uint16_t y1 = sy * M_SIZE*100 + M_SIZE*150;

    if (i % 5 == 0) {
      // Coordinates of next tick for zone fill
      float sx2 = cos((i + 5 - 90) * 0.0174532925);
      float sy2 = sin((i + 5 - 90) * 0.0174532925);
      int x2 = sx2 * (M_SIZE*100 + tl) + M_SIZE*120;
      int y2 = sy2 * (M_SIZE*100 + tl) + M_SIZE*150;
      int x3 = sx2 * M_SIZE*100 + M_SIZE*120;
      int y3 = sy2 * M_SIZE*100 + M_SIZE*150;
  
      // Green zone limits
      if (i < -15) {
        tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_GREEN);
        tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_GREEN);
      }
  
      // Orange zone limits
      if (i >= -15 && i < 15) {
        tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_ORANGE);
        tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_ORANGE);
      }
  
      // Red zone limits
      if (i >= 15 && i < 45) {
        tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_RED);
        tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_RED);
      }
    }
    // Short scale tick length
    if ((i+45) % 30 != 0) tl = 7;

    // Recalculate coords incase tick lenght changed
    x0 = sx * (M_SIZE*100 + tl) + M_SIZE*120;
    y0 = sy * (M_SIZE*100 + tl) + M_SIZE*150;
    x1 = sx * M_SIZE*100 + M_SIZE*120;
    y1 = sy * M_SIZE*100 + M_SIZE*150;

    // Only draw ticks on log scale
    switch ((i+45) % 30) {
      case 0:  // 1
      case 10: // 2
      case 16: // 3
      case 20: // 4
      case 23: // 5
      case 25: // 6
      case 27: // 7
      case 28: // 8
      case 29: // 9
          tft.drawLine(x0, y0, x1, y1, TFT_BLACK);
    }

    // Check if labels should be drawn, with position tweaks
    if ((i+45) % 30 == 0) {
      // Calculate label positions
      x0 = sx * (M_SIZE*100 + tl + 10) + M_SIZE*120;
      y0 = sy * (M_SIZE*100 + tl + 10) + M_SIZE*150;
      switch (i) {
        case -45: tft.drawCentreString("0.1", x0+4, y0-4, 1); break;
        case -15: tft.drawCentreString("1", x0+2, y0, 1); break;
        case 15: tft.drawCentreString("10", x0, y0, 1); break;
        case 45: tft.drawCentreString("100", x0, y0, 1); break;
      }
    }

    if (i % 5 == 0) {
      // Now draw the arc of the scale
      sx = cos((i + 5 - 90) * 0.0174532925);
      sy = sin((i + 5 - 90) * 0.0174532925);
      x0 = sx * M_SIZE*100 + M_SIZE*120;
      y0 = sy * M_SIZE*100 + M_SIZE*150;
      // Draw scale arc, don't draw the last part
      if (i < 45) tft.drawLine(x0, y0, x1, y1, TFT_BLACK);
    }
  }

  tft.drawString("mR/hr", M_SIZE*(3 + 230 - 40), M_SIZE*(119 - 20), 2); // Units at bottom right
  plotNeedle(1, 0); // Put meter needle at 1
  plotNeedle(0, 0); // and then at 0 (to ensure it is painted)
}


// #########################################################################
// Update needle position - range 0..100
// This function is blocking while needle moves (step by step) to the target position
// This ensures we have smooth needle movement
// timing depends on ms_delay. 10ms minimises needle flicker if text is drawn within
// needle sweep area
//
void plotNeedle(int value, byte ms_delay) {
  static int old_analog =  -999; // Value last displayed
  static float ltx = 0;    // Saved x coord of bottom of needle
  static uint16_t osx = M_SIZE*120, osy = M_SIZE*120; // Saved x & y coords

  if (value < -10) value = -10; // Limit value to emulate needle end stops
  if (value > 110) value = 110;

  // Move the needle until new value reached
  while (!(value == old_analog)) {
    if (old_analog < value) old_analog++; else old_analog--;

    if (ms_delay == 0) old_analog = value; // Update immediately if delay is 0

    // float sdeg = map(old_analog, -10, 110, -150, -30); // Map value to angle -60 +60
    float sdeg = map(old_analog, -10, 110, -145, -35); // Map value to angle -60 +60
    // Calculate tip of needle coords
    float sx = cos(sdeg * 0.0174532925);
    float sy = sin(sdeg * 0.0174532925);

    // Calculate x delta of needle start (does not start at pivot point)
    float tx = tan((sdeg + 90) * 0.0174532925);

    // Erase old needle image
    tft.drawLine(M_SIZE*(120 + 24 * ltx) - 1, M_SIZE*(150 - 24), osx - 1, osy, TFT_WHITE);
    tft.drawLine(M_SIZE*(120 + 24 * ltx), M_SIZE*(150 - 24), osx, osy, TFT_WHITE);
    tft.drawLine(M_SIZE*(120 + 24 * ltx) + 1, M_SIZE*(150 - 24), osx + 1, osy, TFT_WHITE);

    // Re-plot text and icon image under needle
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.pushImage(88, 68, radioWidth, radioHeight, radiation64);

    // Store new needle end coords for next erase
    ltx = tx;
    osx = M_SIZE*(sx * 98 + 120);
    osy = M_SIZE*(sy * 98 + 150);

    // Draw the needle in the new postion, magenta makes needle a bit bolder
    // draws 3 lines to thicken needle
    tft.drawLine(M_SIZE*(120 + 24 * ltx) - 1, M_SIZE*(150 - 24), osx - 1, osy, TFT_RED);
    tft.drawLine(M_SIZE*(120 + 24 * ltx), M_SIZE*(150 - 24), osx, osy, TFT_MAGENTA);
    tft.drawLine(M_SIZE*(120 + 24 * ltx) + 1, M_SIZE*(150 - 24), osx + 1, osy, TFT_RED);

    // Slow needle down slightly as it approaches new postion
    if (abs(old_analog - value) < 10) ms_delay += ms_delay / 5;

    // Wait before next update
    delay(ms_delay);
  }
}

// #########################################################################
// draw outline of bar graph
//
void barGraph() {
  // Meter outline
  tft.fillRect(BARX-3, BARY-3, BARDX+5, BARDY+6, TFT_GREY);
  tft.fillRect(BARX-1, BARY-1, BARDX+1, BARDY+2, TFT_WHITE);
  tft.drawRightString("0.1", BARX-2, BARY+BARDY-4, 1);
  tft.drawRightString("1", BARX-2, BARY+BARDY-BARGREEN-5, 1);
  tft.drawRightString("10", BARX-3, BARY+BARDY-BARORANGE-5, 1);
  tft.drawRightString("100", BARX-3, BARY-3, 1);
}

// #########################################################################
// update all items of bar graph (typically called once every 10 secs when sec10bufp[] has changed)
//
void barUpdateAll() {
  tft.fillRect(BARX-1, BARY-1, BARDX+1, BARDY+2, TFT_WHITE);
  tft.drawLine(BARX-1, BARY+BARDY-BARGREEN-2, BARX+BARDX-1, BARY+BARDY-BARGREEN-2, TFT_LIGHTGREY);
  tft.drawLine(BARX-1, BARY+BARDY-BARORANGE-2, BARX+BARDX-1, BARY+BARDY-BARORANGE-2, TFT_LIGHTGREY);
  tft.drawCentreString("History (7 min)", BARX+BARDX/2, BARY+5, 1);
  int pos = sec10;
  for (int i=0; i<SEC10BUFSIZE; i++) {
    pos--;
    if (pos<0) pos=SEC10BUFSIZE-1;
    if (BARDX-((i+2)*BARITEMX)>=0) {
      int v = mrem2perc(tics2mrem(sec10_buf[pos]), BARDY-2);
      tft.fillRect(BARX+BARDX-((i+2)*BARITEMX), BARY+(BARDY-v), BARITEMX-BARSPACE, v, (v<=BARGREEN ? TFT_GREEN : (v<=BARORANGE ? TFT_ORANGE : TFT_RED)));
    }
  }
}

// #########################################################################
// update only last bar entry of bar graph (typically called every 200ms)
//
void barUpdateLast(float mrem) {
  int v=mrem2perc(mrem, BARDY-2);
  tft.fillRect(BARX+BARDX-BARITEMX, BARY, BARITEMX-BARSPACE, BARDY, TFT_WHITE); 
  tft.drawLine(BARX+BARDX-BARITEMX, BARY+BARDY-BARGREEN-2, BARX+BARDX-1, BARY+BARDY-BARGREEN-2, TFT_LIGHTGREY);
  tft.drawLine(BARX+BARDX-BARITEMX, BARY+BARDY-BARORANGE-2, BARX+BARDX-1, BARY+BARDY-BARORANGE-2, TFT_LIGHTGREY);
  tft.fillRect(BARX+BARDX-BARITEMX, BARY+(BARDY-v), BARITEMX-BARSPACE, v, (v<=BARGREEN ? TFT_GREEN : (v<=BARORANGE ? TFT_ORANGE : TFT_RED))); 
}

// #########################################################################
// capacitive touch routine. To filter out spike noises we require the touchRead() value to be 100ms at a value below
// the defined threshhold
//
bool getTouch(int port) {
  int t;
  for (int i=0; i<10; i++) {
    t=touchRead(port);
    if (t>TOUCHTHRESHOLD) return false;
    delay(10);
  }
  return true;
}
