#pragma once

#include <Arduino_GFX_Library.h>

#define PWDN_GPIO_NUM 17
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 8
#define SIOD_GPIO_NUM 21
#define SIOC_GPIO_NUM 16
#define Y9_GPIO_NUM 2
#define Y8_GPIO_NUM 7
#define Y7_GPIO_NUM 10
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 11
#define Y4_GPIO_NUM 15
#define Y3_GPIO_NUM 13
#define Y2_GPIO_NUM 12
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 4
#define PCLK_GPIO_NUM 9

#define PIN_LCD_SCLK 39
#define PIN_LCD_MOSI 38
#define PIN_LCD_MISO 40
#define PIN_LCD_DC 42
#define PIN_LCD_RST -1
#define PIN_LCD_CS 45
#define PIN_LCD_BL 1
#define PIN_BOOT 0
#define PIN_TP_SDA 48
#define PIN_TP_SCL 47
#define PIN_LED1 18
#define PIN_LED2 18
#define PIN_SD_CS 41

#define LCD_W 240
#define LCD_H 320

extern Arduino_GFX *gfx;

void cameraStop();
bool cameraInitJpeg();
bool cameraInitRgb565();
