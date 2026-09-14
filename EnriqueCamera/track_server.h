#pragma once

#include <stddef.h>

void trackServerStart();
void trackServerStop();
bool trackTakePendingWifi(char *ssid, size_t ssidLen, char *pass, size_t passLen);

void trackLoadPrefs();
bool trackHasCallsign();
const char *trackGetCallsign();
void trackSetCallsign(const char *cs);
