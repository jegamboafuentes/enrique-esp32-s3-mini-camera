#pragma once

#include <stddef.h>

void startCameraServer();
void stopCameraServer();
int streamClientCount();
bool takePendingWifi(char *ssid, size_t ssidLen, char *pass, size_t passLen);
bool takeForgetWifi();
