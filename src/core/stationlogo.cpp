#include "stationlogo.h"

#include <LittleFS.h>
#include <strings.h>

namespace {

constexpr char STATION_MAP_FILE[] = "/images/stations/map.csv";
constexpr size_t MAP_LINE_LEN = 512;

bool isRemoteUrl(const char* value) {
    return value &&
           (strncasecmp(value, "http://", 7) == 0 ||
            strncasecmp(value, "https://", 8) == 0);
}

void discardLineRemainder(File& file) {
    while (file.available()) {
        if (file.read() == '\n') break;
    }
}

}  // namespace

bool stationLogoLookup(const char* stationName, StationLogoInfo& info) {
    info = {};
    if (!stationName || !stationName[0]) return false;

    File map = LittleFS.open(STATION_MAP_FILE, "r");
    if (!map) return false;

    char line[MAP_LINE_LEN];
    bool found = false;
    while (map.available()) {
        size_t length = 0;
        bool complete = false;
        while (map.available() && length < sizeof(line) - 1) {
            const char character = static_cast<char>(map.read());
            if (character == '\n') {
                complete = true;
                break;
            }
            if (character != '\r') line[length++] = character;
        }
        if (!complete && map.available()) {
            discardLineRemainder(map);
            continue;
        }
        line[length] = '\0';
        if (!length || line[0] == '#') continue;

        char* row = line;
        if (length >= 3 && static_cast<uint8_t>(row[0]) == 0xEF &&
            static_cast<uint8_t>(row[1]) == 0xBB &&
            static_cast<uint8_t>(row[2]) == 0xBF) {
            row += 3;
        }

        char* firstTab = strchr(row, '\t');
        if (!firstTab) continue;
        *firstTab = '\0';
        if (strcasecmp(row, stationName) != 0) continue;

        char* second = firstTab + 1;
        char* secondTab = strchr(second, '\t');
        if (secondTab) *secondTab++ = '\0';

        if (secondTab) {
            // Three-column format. The URL in the second field belongs to the
            // PC-side map builder; only the LittleFS filename is used here.
            strlcpy(info.localFile, secondTab, sizeof(info.localFile));
        } else if (isRemoteUrl(second)) {
            // A remote-only row deliberately has no runtime fallback image.
        } else {
            // Backward compatibility with the former two-column local map.
            strlcpy(info.localFile, second, sizeof(info.localFile));
        }
        found = true;
        break;
    }
    map.close();
    return found;
}

bool stationLogoLocalPath(const StationLogoInfo& info, char* path, size_t pathSize) {
    if (!path || pathSize == 0) return false;
    path[0] = '\0';
    if (!info.localFile[0]) return false;

    if (info.localFile[0] == '/') {
        strlcpy(path, info.localFile, pathSize);
    } else {
        snprintf(path, pathSize, "/images/stations/%s", info.localFile);
    }
    if (LittleFS.exists(path)) return true;
    path[0] = '\0';
    return false;
}
