#pragma once

#include <stdint.h>

void webcamEnter();
void webcamLoop();
void webcamLeave();

void recogEnter();
void recogLoop(bool tapped);
void recogLeave();

void helloEnter();
void helloLoop();
void helloLeave();

void settingsEnter();
void settingsLoop(bool tapped, uint16_t tx, uint16_t ty);
void settingsLeave();
void settingsLoad();
void settingsApplyBrightness();
int settingsWallpaper();
void settingsSetWallpaper(int id);

void shootEnter();
void shootLoop(bool tapped, uint16_t tx, uint16_t ty);
void shootLeave();

void galleryEnter();
void galleryLoop(bool tapped, uint16_t tx, uint16_t ty);
void galleryLeave();

void flightsEnter();
void flightsLoop(bool tapped, uint16_t tx, uint16_t ty);
void flightsLeave();

void trackEnter();
void trackLoop(bool tapped, uint16_t tx, uint16_t ty);
void trackLeave();
