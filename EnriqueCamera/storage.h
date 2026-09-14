#pragma once

#include <FS.h>

bool storageBegin();
bool storageReady();
const char *storageLabel();
fs::FS &storageFS();
void storageEnsureDir(const char *path);
uint32_t storageNextIndex();
