#pragma once

//
// MAKERSUN.NET
// NUVOLA32 Configurations
// rev. 090126
//

// ======================= I2S AUDIO PCM5102A =======================
#define WS  4    // WS (LRCK)
#define BCK   5    // BCLK
#define DATA  7    // DIN (Data Out)
#define SAMPLE_RATE 44100 // Sample rate for I2S audio
#define I2S_PORT I2S_NUM_0

// ======================= PCF8574 I/O EXPANDER ======================
#define USE_I2C     // Enable to use PCF8574

#ifndef USE_I2C
  #define BUTTON1 1
  #define BUTTON2 40
#endif

#define HW_AUDIO
#ifdef USE_I2C
    #define PCF8574_ADDR 0x38
    #define PCF8574_SCL 18
    #define PCF8574_SDA 21
    #define PCF_A        0
    #define PCF_DOWN     1
    #define PCF_START    2
    #define PCF_B        3
    #define PCF_SELECT   4
    #define PCF_UP       5
    #define PCF_RIGHT    6
    #define PCF_LEFT     7
#endif

// ======================= SD CARD (FSPI) ============================
#define SD_CLK   14
#define SD_MISO  12
#define SD_MOSI  13
#define SD_CS    15
#define SD_FREQ 20000000
#define FSROOT "/sd"

// ======================= TFT DISPLAY (HSPI) =========================
#define TFT_CLK   16
#define TFT_MOSI  17
#define TFT_MISO  11
#define TFT_DC    38
#define TFT_CS    39
#define TFT_RST   48

// ======================= CHARLIEPLEXING LEDs ========================
#define LED1 9
#define LED2 10
#define LED3 3
#define NUM_LEDS 6
#define PWM_FREQ       5000   // Hz
#define PWM_RESOLUTION 8      // bit → duty 0..255
#define PWM_CHANNEL    0      // canale LEDC usato per il source

// ======================= BATTERY MONITOR ============================
#define BATTERY_PIN 6
#define VREF_MV 3300.0f
#define ADC_MAX     4095.0
#define ADC_REF     3.3
// rapporto partitore: 2.0 = 100k / 100k
#define BAT_DIVIDER 2.0
#define BATTERY_UPDATE_MS 60000

// LED PWM
#define LUM      20   // luminosità % (0=spento, 100=max)
#define LEDC_CH   0
#define LEDC_FREQ 5000
#define LEDC_RES  8   // risoluzione duty cycle: 0..255

//#define USEWIFI
