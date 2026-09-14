#pragma once

#include <stddef.h>
#include <stdint.h>

void flightServerStart();
void flightServerStop();
bool flightTakePendingWifi(char *ssid, size_t ssidLen, char *pass, size_t passLen);

void flightLoadPrefs();
bool flightHasHome();
void flightGetHome(float *lat, float *lon);
const char *flightHomeLabel();
void flightSetHome(float lat, float lon);
bool flightHasAirport();
void flightGetAirport(float *lat, float *lon, char *name, size_t nameLen);
void flightSetAirport(float lat, float lon, const char *name);
void flightClearAirport();
float flightPassRadiusKm();
void flightSetPassRadiusKm(float km);

enum { FLIGHT_FILTER_BOTH = 0, FLIGHT_FILTER_IN = 1, FLIGHT_FILTER_OUT = 2 };
uint8_t flightGetFilter();
void flightSetFilter(uint8_t mode);
