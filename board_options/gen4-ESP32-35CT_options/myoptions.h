/* FusionEdge hardware configuration for the 4D Systems gen4-ESP32-35CT. */
#pragma once

#ifndef ARDUINO_ESP32S3_DEV
#define ARDUINO_ESP32S3_DEV
#endif

#define LANGUAGE HU
#define CLOCK_TTS_LANGUAGE "hu" //Default language TTS e.g. pl,en,de,ru,fr,hu
// #define IMPERIALUNIT // English weather: Fahrenheit, inHg and mph (requires LANGUAGE EN)

// Animated cassette replaces the clock during the non-blank "While playing" screensaver.
// #define CASSETTE_FRAME_MS 100UL // Reel animation interval; lower values increase display load.
// #define CASSETTE_PNG_PATH "/images/screensaver/retro_audio_cassette.png"      // Silver/green cassette.
// #define CASSETTE_PNG_PATH "/images/screensaver/retro_audio_cassette_blue.png" // Blue/pink cassette.
// #define CASSETTE_PNG_PATH "/images/screensaver/retro_audio_cassette_red.png"  // Red/cream cassette.

// --- DLNA ---
//#define USE_DLNA
//#define dlnaHost "192.168.1.100" // your DLNA server IP address
//#define dlnaIDX  21 //(Music folder index: Synology = 21, MiniDLNA = 1, etc.)

/* -- Névnapok megjelenítése -- Display name days --
Supported languages: HU, PL, NL, GR, DE (UA Local/namedays/namedays_UA.h is not filled in.) */
#define NAMEDAYS_FILE HU

#define USE_BUILTIN_LED false

/* Integrated 3.5-inch ILI9488 display. */
#define DSP_MODEL DSP_ILI9488
#define LGFX_LCD_SPI_HOST 1
#define LGFX_LCD_SPI_WRITE_FREQ 20000000
#define LGFX_LCD_SPI_READ_FREQ 16000000
#define LGFX_PANEL_INVERT false
#define LGFX_ROTATION 0 // Physical panel offset; landscape rotation is applied by the display driver.

#define TFT_MISO -1
#define TFT_MOSI 13
#define TFT_SCK 14
#define TFT_CS 3
#define TFT_DC 21
#define TFT_RST 7

#define BRIGHTNESS_PIN 4

/* Integrated FocalTech capacitive touch controller. */
#define TS_MODEL TS_MODEL_FT6X36
#define TS_SDA 10
#define TS_SCL 9
#define TS_RST 11
#define TS_INT 8
#define TS_I2C_PORT 0
#define TS_I2C_ADDR 0x38
#define LGFX_TOUCH_BUS_SHARED false
#define LGFX_TOUCH_I2C_FREQ 400000

/* Integrated microSD slot. */
#define SDC_CS 1
#define SD_SPI_HOST HSPI
#define SD_SPIPINS 42, 41, 2
#define SDSPISPEED 4000000

/* External I2S DAC example. Review these pins before connecting hardware. */
#define I2S_DOUT 38
#define I2S_BCLK 39
#define I2S_LRC 40

#define EXT_WEATHER true

/* Optional controls and features. Enable only after assigning free GPIO pins. */
// #define ENC_BTNR 15
// #define ENC_BTNL 16
// #define ENC_BTNB 17
// #define ENC_INTERNALPULLUP true
// #define IR_PIN 18
// #define USE_BLUETOOTH

/* Optional battery monitor on the factory divider: R26=33k (top),
 * R27=100k (bottom), connected to GPIO5. No PCB rework is assumed.
 * At 4.2V this presents about 3.16V to the ADC, near/above the S3's
 * documented high-attenuation effective range. Compare the displayed
 * voltage with a meter, especially near full charge, before trusting %.
 */
// #define BATTERY_ADC_PIN 5
// #define BATTERY_DIVIDER_TOP_OHM 33000UL
// #define BATTERY_DIVIDER_BOTTOM_OHM 100000UL
// #define BATTERY_ADC_ATTENUATION ADC_11db
// #define BATTERY_VOLTAGE_OFFSET_MV 0
// #define BRIGHTNESS_PWM_FREQUENCY 4500
// #define WIFI_TX_POWER WIFI_POWER_11dBm
