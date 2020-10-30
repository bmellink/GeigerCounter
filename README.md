# GeigerCounter
Arduino ESP32 based Geiger counter

![Geiger counter overview](img/Overview.jpeg?raw=true "Geiger counter overview")

## Operation
The Geiger counter module provides a pulse to GPIO27 which triggers an interrupt to increase the internal counter.
 
We are using the esp32 touch sensor routine on GPIO12 to determine if a user wants to toggle between the
analog meter display mode and the bar graph with historic data over the last 7 minutes (also showing battery condition).

![Analog Display](img/AnalogDisplay.jpeg?raw=true "Analog Display")
![Bar Display](img/BarDisplay.jpeg?raw=true "Bar Display")

## Components
Geiger counter based on DIY Assembled Geiger Counters Kit: https://www.aliexpress.com/item/4000229395987.html
![Geiger Module](img/GeigerModule.jpeg?raw=true "Geiger Module")

Display based on TTGO ESP32 1.14 inch LCD display Module:  https://www.aliexpress.com/item/4000606780227.html
The TTGO contains an ST7735 TFT LCD screen.

![Top View](img/TopView.jpeg?raw=true "Top View")

The Geiger counter comes with a 3x AA battery housing and runs fine on 4.5V. The ESP32 also runs fine on 4.5V. 
In order to monitor the battery condition, I added 2 resistors (20k between 4.5V and GPIO26 and 33k between GPIO26 and GND), so we will can use ADV read on GPIO26 to measure the battery voltage.

I liked using transparent housing and ordered a Hammond 1591DTCL enclosure.

## Radiation calculation
Calculation of radiation dose: 1 Sievert (Sv) = 100 Rem = 100,000 mR ==> 1 μSv = 0.1 mR
Doses is indicated in mR/hour.
Background radiation is 365 mR/year => 0.042 mR/hour
Max doses radiation workers 5000 mR/year => 0.57 mR/hour

The tube is more or less lineair. See https://www.sparkfun.com/datasheets/Components/General/LND-712-Geiger-Tube.pdf so we can use the number of tics/ 10 seconds to estimate the doses in mR/hour:

```tics_per_10sec * TICFACTOR```

Analog indicator will be logarithmic: 

```0.1 <- green -> 1 <- orange -> 10 <- red -> 100 (mR/hour)```

The analog indicator has a range 0..100, so to display mR/hr on this scale we have to calculate

```33.3333*(log10(mRhour)+1)```

![radiation64.png](img/radiation64.png?raw=true)

The bitmap shown on the analog display is converted from radiation64.png to .c or .h file using
http://www.rinkydinkelectronics.com/_t_doimageconverter565.php

## Arduino code
The TTGO ESP32 module requires four things to be setup in your Arduino environment:
1. Setup of the ESP32 hardware environment by adding this URL to your board manager: https://dl.espressif.com/dl/package_esp32_index.json
2. Download the TFT_eSPI library through the library manager or from github: https://github.com/Bodmer/TFT_eSPI
3. In your library folder for TFT-eSPI configure the right display driver for the TTGO module. This should be done in the ```User_Setup_select.h```. Just remove the comment // from the line:

```#include <User_Setups/Setup25_TTGO_T_Display.h>    // Setup file for ESP32 and TTGO T-Display ST7789V SPI bus TFT```

4. Then select in Arduino Tools->Board type "ESP32 Dev Module"  

The Arduino code can be found in the folder [geiger](geiger)
