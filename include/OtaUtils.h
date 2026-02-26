#pragma once

#include <Arduino.h>

bool backupFirmwareToFS(const char* path = "/firmware.bak",
                        const char* buildDate = nullptr);
bool revertFirmwareFromFS(const char* path = "/firmware.bak");
bool applyFirmwareFromFS(const char* path = "/firmware.new",
                         const char* buildDate = nullptr);
bool firmwareBackupExists(const char* path = "/firmware.bak");
size_t firmwareBackupSize(const char* path = "/firmware.bak");
String getBackupBuildDate(const char* path = "/firmware.bak");
