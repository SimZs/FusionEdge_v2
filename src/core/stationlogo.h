#pragma once

#include <Arduino.h>

constexpr size_t STATION_LOGO_PATH_LEN = 128;

struct StationLogoInfo {
    char localFile[STATION_LOGO_PATH_LEN] = {0};
};

// map.csv formats:
//   station<TAB>PC-side source URL<TAB>optional local image
//   station<TAB>local image                         (legacy)
// The firmware intentionally ignores the remote URL and never downloads a
// station logo at runtime.
bool stationLogoLookup(const char* stationName, StationLogoInfo& info);

// Returns a LittleFS path only when the optional local override exists.
bool stationLogoLocalPath(const StationLogoInfo& info, char* path, size_t pathSize);
