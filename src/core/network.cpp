// v0.9.720
#include "options.h"
#include <ESPmDNS.h>
#include "time.h"
#include "rtcsupport.h"
#include "network.h"
#include "display.h"
#include "config.h"
#include "netserver.h"
#include "player.h"
#include "mqtt.h"
#include "timekeeper.h"
#include "../pluginsManager/pluginsManager.h"

#ifndef WIFI_ATTEMPTS
    #define WIFI_ATTEMPTS 16
#endif

#ifndef WIFI_SCAN_TIMEOUT_MS
    #define WIFI_SCAN_TIMEOUT_MS 12000
#endif

#ifndef SEARCH_WIFI_CORE_ID
    #define SEARCH_WIFI_CORE_ID 0
#endif
MyNetwork network;
static portMUX_TYPE networkTimeMux = portMUX_INITIALIZER_UNLOCKED;

void network_get_timeinfo_snapshot(struct tm* out) {
    if (!out) return;
    portENTER_CRITICAL(&networkTimeMux);
    *out = network.timeinfo;
    portEXIT_CRITICAL(&networkTimeMux);
}

void network_set_timeinfo(const struct tm& in) {
    portENTER_CRITICAL(&networkTimeMux);
    network.timeinfo = in;
    portEXIT_CRITICAL(&networkTimeMux);
}

void MyNetwork::WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    network.beginReconnect = false;
    player.lockOutput = false;
    delay(100);
    display.putRequest(NEWMODE, PLAYER);
    if (config.getMode() == PM_SDCARD) {
        network.status = CONNECTED;
        display.putRequest(NEWIP, 0);
    } else {
        display.putRequest(NEWMODE, PLAYER);
        if (network.lostPlaying) player.sendCommand({PR_PLAY, config.lastStation()});
    }
#ifdef MQTT_ROOT_TOPIC
    connectToMqtt();
#endif
}

void MyNetwork::WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!network.beginReconnect) {
        Serial.printf("Lost connection, reconnecting to %s...\n", config.ssids[config.store.lastSSID - 1].ssid);
        if (config.getMode() == PM_SDCARD) {
            network.status = SDREADY;
            display.putRequest(NEWIP, 0);
        } else {
            // AudioI2S may drop isRunning() before the Wi-Fi event reaches us.
            // Preserve the user's play intent so reconnect can still resume.
            network.lostPlaying = player.isRunning() || player.wantsPlayback();
            if (network.lostPlaying) {
                player.lockOutput = true;
                player.sendCommand({PR_STOP, 0});
            }
            display.putRequest(NEWMODE, LOST);
        }
    }
    network.beginReconnect = true;
    WiFi.reconnect();
}

// The scan ranks visible saved SSIDs, including weak ones. A weak or hidden
// saved network must remain a fallback, otherwise a -83 dBm workplace AP can
// be rejected even though it is the only usable network in wifi.csv.
static uint8_t buildWifiCandidates(uint8_t* out, uint8_t capacity, bool silent) {
    const uint8_t savedCount = config.ssidsCount > 5 ? 5 : config.ssidsCount;
    if (!out || !capacity || !savedCount) { return 0; }

    bool visible[5] = {};
    bool added[5] = {};
    int32_t bestRssi[5] = {-128, -128, -128, -128, -128};
    int networks = WIFI_SCAN_FAILED;

    // One saved network needs no ranking. The silent SD background task also
    // keeps its old no-scan behavior until runtime recovery is handled in
    // stage two, so a Wi-Fi scan cannot disturb SD/BT audio on core 0.
    if (savedCount > 1 && !silent) {
        display.putRequest(WIFISCANNING);
        WiFi.scanDelete();
        networks = WiFi.scanNetworks(true, true);
        const uint32_t scanStarted = millis();
        while (networks == WIFI_SCAN_RUNNING && millis() - scanStarted < WIFI_SCAN_TIMEOUT_MS) {
            delay(50);
            networks = WiFi.scanComplete();
        }
        if (networks == WIFI_SCAN_RUNNING) {
            if (!silent) { Serial.println("##[WIFI]# scan timed out; trying saved networks"); }
            networks = WIFI_SCAN_FAILED;
        }
        if (networks > 0) {
            for (int networkIndex = 0; networkIndex < networks; ++networkIndex) {
                const String foundSsid = WiFi.SSID(networkIndex);
                const int32_t foundRssi = WiFi.RSSI(networkIndex);
                for (uint8_t savedIndex = 0; savedIndex < savedCount; ++savedIndex) {
                    if (foundSsid == config.ssids[savedIndex].ssid &&
                        (!visible[savedIndex] || foundRssi > bestRssi[savedIndex])) {
                        visible[savedIndex] = true;
                        bestRssi[savedIndex] = foundRssi;
                    }
                }
            }
        }
        WiFi.scanDelete();
    }

    uint8_t count = 0;
    for (;;) {
        int bestIndex = -1;
        for (uint8_t i = 0; i < savedCount; ++i) {
            if (visible[i] && !added[i] &&
                (bestIndex < 0 || bestRssi[i] > bestRssi[bestIndex])) {
                bestIndex = i;
            }
        }
        if (bestIndex < 0 || count >= capacity) { break; }
        out[count++] = static_cast<uint8_t>(bestIndex);
        added[bestIndex] = true;
    }

    const uint8_t fallbackStart =
        (config.store.lastSSID > 0 && config.store.lastSSID <= savedCount)
            ? config.store.lastSSID - 1 : 0;
    for (uint8_t i = 0; i < savedCount && count < capacity; ++i) {
        const uint8_t savedIndex = (fallbackStart + i) % savedCount;
        if (!added[savedIndex]) {
            out[count++] = savedIndex;
            added[savedIndex] = true;
        }
    }

    if (!silent) {
        if (savedCount == 1) {
            Serial.println("##[WIFI]# one saved network; scan skipped");
        } else {
            Serial.printf("##[WIFI]# scan=%d saved=%u candidates=%u; visible saved networks first by RSSI\n",
                          networks, static_cast<unsigned>(savedCount), static_cast<unsigned>(count));
        }
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t index = out[i];
            if (visible[index]) {
                Serial.printf("##[WIFI]# candidate %u: %s (%ld dBm)\n",
                              static_cast<unsigned>(i + 1), config.ssids[index].ssid,
                              static_cast<long>(bestRssi[index]));
            } else {
                Serial.printf("##[WIFI]# candidate %u: %s (scan fallback)\n",
                              static_cast<unsigned>(i + 1), config.ssids[index].ssid);
            }
        }
    }
    return count;
}

bool MyNetwork::wifiBegin(bool silent) {
    if (config.ssidsCount == 0) { return false; }

    // Keep the existing Wi-Fi reset and TX power limit for this first stage.
    if (WiFi.getMode() != WIFI_MODE_NULL) { WiFi.disconnect(true); }
    WiFi.mode(WIFI_OFF);
    delay(1000);
    WiFi.mode(WIFI_STA);
    delay(500);
    WiFi.setTxPower(WIFI_TX_POWER);

    uint8_t candidates[5] = {};
    const uint8_t candidateCount = buildWifiCandidates(candidates, 5, silent);
    for (uint8_t attempt = 0; attempt < candidateCount; ++attempt) {
        const uint8_t index = candidates[attempt];
        if (attempt > 0) {
            WiFi.disconnect(false);
            delay(100);
        }
        if (!silent) {
            Serial.printf("##[BOOT]#\tAttempt %u/%u to connect to %s\n",
                          static_cast<unsigned>(attempt + 1), static_cast<unsigned>(candidateCount),
                          config.ssids[index].ssid);
            display.putRequest(BOOTSTRING, index);
        }
        const char* password = config.ssids[index].password[0]
            ? config.ssids[index].password : nullptr;
        WiFi.begin(config.ssids[index].ssid, password);
        // Preserve the old ~8.5 s per-network timeout for a fair weak-signal test.
        for (uint16_t wait = 0; wait <= WIFI_ATTEMPTS && WiFi.status() != WL_CONNECTED; ++wait) {
            if (!silent) { Serial.print("."); }
            delay(500);
            if (REAL_LEDBUILTIN != 255 && !silent) {
                digitalWrite(REAL_LEDBUILTIN, !digitalRead(REAL_LEDBUILTIN));
            }
        }
        if (WiFi.status() == WL_CONNECTED) {
            if (!silent) { Serial.println(); }
            config.setLastSSID(index + 1);
            Serial.printf("##[WIFI]# connected to %s, RSSI=%ld dBm\n",
                          config.ssids[index].ssid, static_cast<long>(WiFi.RSSI()));
            return true;
        }
        if (!silent) {
            Serial.println();
            Serial.printf("##[WIFI]# connection failed: %s (status=%d)\n",
                          config.ssids[index].ssid, static_cast<int>(WiFi.status()));
        }
    }
    WiFi.disconnect(false);
    return false;
}

void searchWiFi(void* pvParameters) {
    if (!network.wifiBegin(true)) {
        delay(10000);
        xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 3, NULL, SEARCH_WIFI_CORE_ID); // "task_prioritas" 0 eredeti, új 3
    } else {
        network.status = CONNECTED;
        netserver.begin(true);
        network.setWifiParams();
        display.putRequest(NEWIP, 0);
        // FusionEdge: itt szándékosan NEM indítunk automatikus restartot rossz RSSI
        // esetén (mint MyNetwork::begin()-ben), mert ez a háttér-task SD-kártya
        // lejátszás közben fut, és egy restart félbeszakítaná azt. Csak naplózunk,
        // hogy a probléma diagnosztizálható maradjon.
        constexpr int32_t kBootRssiLogThreshold = -80; // csak diagnosztikai naplózáshoz
        int32_t bootRssi = WiFi.RSSI();
        if (bootRssi <= kBootRssiLogThreshold) {
            BOOTLOG("WiFi RSSI %d dBm <= %d dBm threshold after background connect (SD mode - no auto-restart)",
                    (int)bootRssi, (int)kBootRssiLogThreshold);
        }
#ifdef MQTT_ROOT_TOPIC
        mqttInit();
#endif
    }
    vTaskDelete(NULL);
}

#define DBGAP false

void MyNetwork::begin() {
    BOOTLOG("network.begin");

    config.initNetwork();
    if (config.ssidsCount == 0 || DBGAP) {
        raiseSoftAP();
        return;
    }
    if (config.getMode() != PM_SDCARD) {
        if (!wifiBegin()) {
            raiseSoftAP();
            Serial.println("##[BOOT]#\tdone");
            return;
        }
        Serial.println(".");
        status = CONNECTED;
        setWifiParams();

#ifdef MQTT_ROOT_TOPIC
        mqttInit();
#endif
    } else {
        status = SDREADY;
        xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 3, NULL, SEARCH_WIFI_CORE_ID); // "task_prioritas" 0 eredeti, 3 új
    }

    Serial.println("##[BOOT]#\tdone");
    if (REAL_LEDBUILTIN != 255) digitalWrite(REAL_LEDBUILTIN, LOW);

#if RTCSUPPORTED
    if (config.isRTCFound()) {
        tm currentTime{};
        rtc.getTime(&currentTime);
        mktime(&currentTime);
        network_set_timeinfo(currentTime);
        display.putRequest(CLOCK);
    }
#endif
    if (network_on_connect) network_on_connect();
    pm.on_connect();
}

void MyNetwork::setWifiParams() {
    WiFi.setSleep(false);
    WiFi.onEvent(WiFiReconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(WiFiLostConnection, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    // config.setTimeConf(); //??
    if (strlen(config.store.mdnsname) > 0) MDNS.begin(config.store.mdnsname);
    Serial.printf("##[BOOT]#\tWeb UI: http://%s/\n", WiFi.localIP().toString().c_str());
}

void MyNetwork::requestTimeSync() {
}

void rebootTime() {
    ESP.restart();
}

void MyNetwork::raiseSoftAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid, apPassword);
    Serial.println("##[BOOT]#");
    BOOTLOG("************************************************");
    BOOTLOG("Running in AP mode");
    BOOTLOG("Connect to AP %s with password %s", apSsid, apPassword);
    BOOTLOG("and go to http://192.168.4.1/ to configure");
    Serial.println("##[BOOT]#\tWeb UI (AP): http://192.168.4.1/");
    BOOTLOG("************************************************");
    status = SOFT_AP;
    if (config.store.softapdelay > 0) timekeeper.waitAndDo(config.store.softapdelay * 60, rebootTime);
}

void MyNetwork::requestWeatherSync() {
    display.putRequest(NEWWEATHER);
}
