#include "options.h"
#ifdef MQTT_ROOT_TOPIC

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "config.h"
#include "mqtt.h"
#include "WiFi.h"
#include "player.h"
#include "commandhandler.h"
#include "netserver.h"
#ifdef USE_BLUETOOTH
#    include "bluetooth.h"
#endif

AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer = nullptr;

namespace {
struct MqttCommand { char text[MQTT_BURL_SIZE]; };
QueueHandle_t mqttCommandQueue = nullptr;
bool mqttInitialized = false;
char lastExtendedState[512] = {};
char availabilityTopic[100] = {};
volatile bool forceExtendedState = true;
volatile bool statusDirty = true;
volatile bool volumeDirty = true;
volatile bool playlistDirty = true;
char lastStatus[BUFLEN * 3] = {};
char lastVolume[5] = {};
volatile bool integrationAdvertisementPending = true;

bool escapeJson(const char* input, char* output, size_t capacity);

bool publishIntegrationAdvertisement() {
  // This is separate from Home Assistant's native MQTT entity discovery:
  // the FusionEdge integration uses it to offer one device/config flow.
  const String mac = WiFi.macAddress();
  char deviceId[13] = {};
  size_t used = 0;
  for (size_t i = 0; i < mac.length() && used < sizeof(deviceId) - 1; ++i) {
    const unsigned char ch = static_cast<unsigned char>(mac[i]);
    if (std::isxdigit(ch)) deviceId[used++] = static_cast<char>(std::toupper(ch));
  }
  if (used != 12 || strcmp(deviceId, "000000000000") == 0) {
    log_w("##[MQTT]# cannot advertise without a valid Wi-Fi MAC");
    return false;
  }

  char topic[80];
  if (snprintf(topic, sizeof(topic), "fusionedge/discovery/%s/config", deviceId) >=
      static_cast<int>(sizeof(topic))) return false;
  char rootTopic[160];
  char version[32];
  if (!escapeJson(MQTT_ROOT_TOPIC, rootTopic, sizeof(rootTopic)) ||
      !escapeJson(FW_VERSION, version, sizeof(version))) {
    log_w("##[MQTT]# integration advertisement fields are too long");
    return false;
  }
  char payload[256];
  const int length = snprintf(payload, sizeof(payload),
                              "{\"id\":\"%s\",\"name\":\"FusionEdge-%s\","
                              "\"root_topic\":\"%s\",\"sw_version\":\"%s\"}",
                              deviceId, deviceId + 8, rootTopic, version);
  if (length <= 0 || length >= static_cast<int>(sizeof(payload))) {
    log_w("##[MQTT]# integration advertisement is too long");
    return false;
  }
  if (!mqttClient.publish(topic, 0, true, payload)) {
    log_w("##[MQTT]# integration advertisement could not be queued");
    return false;
  }
  return true;
}

bool parseNumber(const char* text, int minimum, int maximum) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  return end != text && *end == '\0' && value >= minimum && value <= maximum;
}

bool validTimerTime(const char* value) {
  if (!value || strlen(value) != 5 || value[2] != ':') return false;
  for (size_t i = 0; i < 5; ++i) {
    if (i != 2 && (value[i] < '0' || value[i] > '9')) return false;
  }
  const int hour = (value[0] - '0') * 10 + (value[1] - '0');
  const int minute = (value[3] - '0') * 10 + (value[4] - '0');
  return hour < 24 && minute < 60;
}

bool escapeJson(const char* input, char* output, size_t capacity) {
  size_t used = 0;
  if (!capacity) return false;
  for (; input && *input && used + 1 < capacity; ++input) {
    const unsigned char ch = static_cast<unsigned char>(*input);
    if (ch == '"' || ch == '\\') {
      if (used + 2 >= capacity) break;
      output[used++] = '\\';
      output[used++] = static_cast<char>(ch);
    } else if (ch < 0x20) {
      if (used + 6 >= capacity) break;
      snprintf(output + used, capacity - used, "\\u%04x", ch);
      used += 6;
    } else if (ch >= 0x80) {
      const size_t bytes = (ch & 0xE0) == 0xC0 ? 2 :
                           (ch & 0xF0) == 0xE0 ? 3 :
                           (ch & 0xF8) == 0xF0 ? 4 : 0;
      if (!bytes || used + bytes >= capacity) break;
      bool valid = true;
      for (size_t i = 1; i < bytes; ++i) {
        if (!input[i] || (static_cast<unsigned char>(input[i]) & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }
      if (!valid) break;
      memcpy(output + used, input, bytes);
      used += bytes;
      input += bytes - 1;
    } else {
      output[used++] = static_cast<char>(ch);
    }
  }
  output[used] = '\0';
  return !input || !*input;
}

void publishExtendedState() {
  if (!mqttClient.connected()) return;
  const char* mode = "web";
  if (config.getMode() == PM_SDCARD) mode = "sd";
  if (config.getMode() == PM_BLUETOOTH) mode = "bt_input";
#ifdef USE_DLNA
  if (config.getMode() == PM_WEB && config.store.playlistSource == PL_SRC_DLNA) mode = "dlna";
#endif
  const char* role = "disabled";
  int output = -1;
  int connected = 0;
  int ready = 0;
#ifdef USE_BLUETOOTH
  role = bluetooth.moduleRole() == BT_ROLE_SINK ? "sink" :
         bluetooth.moduleRole() == BT_ROLE_SOURCE ? "source" : "unknown";
  output = bluetooth.sourceOutputMode();
  connected = bluetooth.connected() ? 1 : 0;
  ready = bluetooth.sourceUiReady() ? 1 : 0;
#endif
  const int playing =
#ifdef USE_BLUETOOTH
      config.getMode() == PM_BLUETOOTH ? (bluetooth.playing() ? 1 : 0) :
#endif
      (player.status() == PLAYING ? 1 : 0);
  char current[sizeof(lastExtendedState)];
  const int written = snprintf(
      current, sizeof(current),
      "{\"mode\":\"%s\",\"playing\":%d,\"volume\":%u,\"bt_role\":\"%s\",\"bt_connected\":%d,\"bt_ready\":%d,\"bt_output\":%d,\"screensaver\":%u,\"clock_tts\":%d,\"clock_tts_no_stream\":%d,\"ledstrip_enabled\":%d,\"ledstrip_effect\":%u,\"ledstrip_screensaver\":%d,\"timer_weekday_start\":\"%.7s\",\"timer_weekday_stop\":\"%.7s\",\"timer_weekend_start\":\"%.7s\",\"timer_weekend_stop\":\"%.7s\"}",
      mode, playing, static_cast<unsigned>(config.store.volume), role,
      connected, ready, output, static_cast<unsigned>(config.store.screensaverMode),
      config.store.clockTtsEnabled ? 1 : 0,
      config.store.clockTtsOnlyWhenNoStream ? 1 : 0,
      config.store.lsEnabled ? 1 : 0,
      static_cast<unsigned>(config.store.lsModel),
      config.store.lsSsEnabled ? 1 : 0,
      config.store.autoStartTime, config.store.autoStopTime,
      config.store.autoWeekendStartTime, config.store.autoWeekendStopTime);
  if (written < 0 || written >= static_cast<int>(sizeof(current))) {
    __atomic_store_n(&forceExtendedState, true, __ATOMIC_RELEASE);
    log_w("##[MQTT]# extended state exceeds buffer");
    return;
  }
  const bool force = __atomic_exchange_n(&forceExtendedState, false, __ATOMIC_ACQ_REL);
  if (!force && strcmp(current, lastExtendedState) == 0) return;
  char stateTopic[100];
  if (snprintf(stateTopic, sizeof(stateTopic), "%sstate", MQTT_ROOT_TOPIC) >=
      static_cast<int>(sizeof(stateTopic))) return;
  if (mqttClient.publish(stateTopic, 0, true, current)) {
    strlcpy(lastExtendedState, current, sizeof(lastExtendedState));
  } else {
    __atomic_store_n(&forceExtendedState, true, __ATOMIC_RELEASE);
  }
}

bool executeMqttCommand(char* text) {
  if (!text) return false;
  while (std::isspace(static_cast<unsigned char>(*text))) ++text;
  size_t length = strlen(text);
  while (length && std::isspace(static_cast<unsigned char>(text[length - 1]))) {
    text[--length] = '\0';
  }
  if (!length) return false;
  if (strncmp(text, "http://", 7) == 0 || strncmp(text, "https://", 8) == 0) {
    if (strlen(text) >= sizeof(player.burl)) return false;
    strlcpy(player.burl, text, sizeof(player.burl));
    player.sendCommand({PR_BURL, 0});
    return true;
  }
  char* arg = strchr(text, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') ++arg;
  } else {
    arg = nullptr;
  }
  if (!arg || !*arg) {
    if (strcmp(text, "turnoff") == 0) {
      const uint8_t smartStart = config.store.smartstart;
      config.setDspOn(0);
      player.sendCommand({PR_STOP, 0});
      config.saveValue(&config.store.smartstart, smartStart);
      return true;
    }
    if (strcmp(text, "turnon") == 0) {
      config.setDspOn(1);
      if (config.store.smartstart == 1) cmd.exec("start", "");
      return true;
    }
    if (strcmp(text, "start") == 0 || strcmp(text, "stop") == 0 ||
        strcmp(text, "toggle") == 0 || strcmp(text, "prev") == 0 ||
        strcmp(text, "next") == 0 || strcmp(text, "volm") == 0 ||
        strcmp(text, "volp") == 0) return cmd.exec(text, "");
#ifdef USE_BLUETOOTH
    if (strcmp(text, "btdisconnect") == 0 && bluetooth.moduleRole() == BT_ROLE_SOURCE)
      return cmd.exec(text, "");
#endif
    return false;
  }
  if (strcmp(text, "vol") == 0 && parseNumber(arg, 0, 21)) return cmd.exec("vol", arg);
  if (strcmp(text, "play") == 0 && parseNumber(arg, 1, 65535)) return cmd.exec("playstation", arg);
#if defined(USE_SD) || defined(USE_BLUETOOTH)
  if (strcmp(text, "mode") == 0) {
    int target = -1;
    if (strcmp(arg, "web") == 0) target = PM_WEB;
#ifdef USE_SD
    else if (strcmp(arg, "sd") == 0) target = PM_SDCARD;
#endif
#ifdef USE_BLUETOOTH
    else if (strcmp(arg, "bt") == 0) target = PM_BLUETOOTH;
#endif
#ifdef USE_DLNA
    else if (strcmp(arg, "dlna") == 0 && config.hasUsableDlnaPlaylist()) target = PM_WEB;
#endif
    if (target < 0) return false;
    config.newConfigMode = target;
#ifdef USE_DLNA
    config.newConfigPlaylistSource = strcmp(arg, "dlna") == 0 ? PL_SRC_DLNA :
                                     target == PM_WEB ? PL_SRC_WEB : -1;
#endif
    netserver.requestOnChange(CHANGEMODE, 0);
    return true;
  }
#endif
#ifdef USE_BLUETOOTH
  if (strcmp(text, "btoutput") == 0 && parseNumber(arg, 0, 2)) return cmd.exec("btsourcelocal", arg);
  if (strcmp(text, "btpair") == 0 && parseNumber(arg, 0, 1)) return cmd.exec("btpair", arg);
  if (strcmp(text, "btscan") == 0 && parseNumber(arg, 3, 30) &&
      bluetooth.moduleRole() == BT_ROLE_SOURCE) return cmd.exec("btscan", arg);
  if (strcmp(text, "btconnect") == 0 && strlen(arg) == 12 &&
      bluetooth.moduleRole() == BT_ROLE_SOURCE) {
    for (const char* p = arg; *p; ++p) {
      if (!std::isxdigit(static_cast<unsigned char>(*p))) return false;
    }
    return cmd.exec("btconnect", arg);
  }
#endif
  if (strcmp(text, "screensavermode") == 0 &&
      parseNumber(arg, 0, SCREENSAVER_VUMETER)) return cmd.exec(text, arg);
  if (strcmp(text, "screensaverenabled") == 0 && parseNumber(arg, 0, 1)) return cmd.exec(text, arg);
  if (strcmp(text, "clocktts") == 0 && parseNumber(arg, 0, 1)) return cmd.exec(text, arg);
  if (strcmp(text, "clockttsnostream") == 0 && parseNumber(arg, 0, 1)) return cmd.exec(text, arg);
  if (strcmp(text, "lsenabled") == 0 && parseNumber(arg, 0, 1)) return cmd.exec(text, arg);
  if (strcmp(text, "lsssena") == 0 && parseNumber(arg, 0, 1)) return cmd.exec(text, arg);
  if (strcmp(text, "lsmodel") == 0 && parseNumber(arg, 0, 3)) return cmd.exec(text, arg);
  if (strcmp(text, "autotimer") == 0) {
    char* value = strchr(arg, ' ');
    if (!value) return false;
    *value++ = '\0';
    const char* time = strcmp(value, "off") == 0 ? "" : value;
    if (time[0] && !validTimerTime(time)) return false;
    bool saved = false;
    if (strcmp(arg, "weekday_start") == 0) {
      saved = config.setAutoTimers(time, config.store.autoStopTime,
                                   config.store.autoWeekendStartTime,
                                   config.store.autoWeekendStopTime);
    } else if (strcmp(arg, "weekday_stop") == 0) {
      saved = config.setAutoTimers(config.store.autoStartTime, time,
                                   config.store.autoWeekendStartTime,
                                   config.store.autoWeekendStopTime);
    } else if (strcmp(arg, "weekend_start") == 0) {
      saved = config.setAutoTimers(config.store.autoStartTime, config.store.autoStopTime,
                                   time, config.store.autoWeekendStopTime);
    } else if (strcmp(arg, "weekend_stop") == 0) {
      saved = config.setAutoTimers(config.store.autoStartTime, config.store.autoStopTime,
                                   config.store.autoWeekendStartTime, time);
    }
    if (saved) {
      __atomic_store_n(&forceExtendedState, true, __ATOMIC_RELEASE);
      netserver.requestOnChangeNoWait(GETCONTROLS, 0);
    }
    return saved;
  }
  return false;
}
} // namespace

void connectToMqtt() {
  if (WiFi.isConnected() && !mqttClient.connected()) mqttClient.connect();
}

void mqttInit() {
  if (mqttInitialized) {
    connectToMqtt();
    return;
  }
  mqttCommandQueue = xQueueCreate(4, sizeof(MqttCommand));
  if (!mqttCommandQueue) {
    log_e("##[MQTT]# command queue allocation failed");
    return;
  }
  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE,
                                   nullptr, [](TimerHandle_t) { connectToMqtt(); });
  mqttInitialized = true;
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  if(strlen(MQTT_USER)>0) mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  if (snprintf(availabilityTopic, sizeof(availabilityTopic), "%savailability", MQTT_ROOT_TOPIC) <
      static_cast<int>(sizeof(availabilityTopic))) {
    mqttClient.setWill(availabilityTopic, 0, true, "offline");
  } else {
    availabilityTopic[0] = '\0';
  }
  log_i("##[MQTT]# enabled; connecting to broker");
  connectToMqtt();
}

void onMqttConnect(bool sessionPresent) {
  (void)sessionPresent;
  char topic[100];
  if (snprintf(topic, sizeof(topic), "%scommand", MQTT_ROOT_TOPIC) >=
      static_cast<int>(sizeof(topic))) return;
  mqttClient.subscribe(topic, 1);
  if (availabilityTopic[0]) {
    mqttClient.publish(availabilityTopic, 0, true, "online");
  }
  __atomic_store_n(&integrationAdvertisementPending,
                   !publishIntegrationAdvertisement(), __ATOMIC_RELEASE);
  __atomic_store_n(&forceExtendedState, true, __ATOMIC_RELEASE);
  mqttPublishStatus();
  mqttPublishVolume();
  mqttPublishPlaylist();
  log_i("##[MQTT]# connected and subscribed");
}

void mqttPublishStatus() {
  __atomic_store_n(&statusDirty, true, __ATOMIC_RELEASE);
}

static void publishStatusNow() {
  if(mqttClient.connected()){
    char topic[100];
    char status[BUFLEN * 3];
    if (snprintf(topic, sizeof(topic), "%sstatus", MQTT_ROOT_TOPIC) >=
        static_cast<int>(sizeof(topic))) return;
    char name[BUFLEN];
    char title[BUFLEN];
    escapeJson(config.station.name, name, sizeof(name));
    escapeJson(config.station.title, title, sizeof(title));
    const int playing =
#ifdef USE_BLUETOOTH
        config.getMode() == PM_BLUETOOTH ? (bluetooth.playing() ? 1 : 0) :
#endif
        (player.status() == PLAYING ? 1 : 0);
    snprintf(status, sizeof(status),
             "{\"status\": %d, \"station\": %d, \"name\": \"%s\", \"title\": \"%s\", \"on\": %d}",
             playing, config.lastStation(), name, title, config.store.dspon);
    const bool forced = __atomic_exchange_n(&statusDirty, false, __ATOMIC_ACQ_REL);
    if ((forced || strcmp(status, lastStatus) != 0) &&
        mqttClient.publish(topic, 0, true, status)) {
      strlcpy(lastStatus, status, sizeof(lastStatus));
    }
  }
}

void mqttPublishPlaylist() {
  __atomic_store_n(&playlistDirty, true, __ATOMIC_RELEASE);
}

static void publishPlaylistNow() {
  if(mqttClient.connected()){
    if (!__atomic_exchange_n(&playlistDirty, false, __ATOMIC_ACQ_REL)) return;
    char topic[100];
    char status[BUFLEN * 2];
    if (snprintf(topic, sizeof(topic), "%splaylist", MQTT_ROOT_TOPIC) >=
        static_cast<int>(sizeof(topic))) return;
    snprintf(status, sizeof(status), "http://%s%s", config.ipToStr(WiFi.localIP()), PLAYLIST_PATH);
    if (!mqttClient.publish(topic, 0, true, status)) {
      __atomic_store_n(&playlistDirty, true, __ATOMIC_RELEASE);
    }
  }
}

void mqttPublishVolume(){
  __atomic_store_n(&volumeDirty, true, __ATOMIC_RELEASE);
}

static void publishVolumeNow(){
  if(mqttClient.connected()){
    char topic[100];
    char vol[5];
    memset(vol, 0, 5);
    if (snprintf(topic, sizeof(topic), "%svolume", MQTT_ROOT_TOPIC) >=
        static_cast<int>(sizeof(topic))) return;
    snprintf(vol, sizeof(vol), "%d", config.store.volume);
    const bool forced = __atomic_exchange_n(&volumeDirty, false, __ATOMIC_ACQ_REL);
    if ((forced || strcmp(vol, lastVolume) != 0) &&
        mqttClient.publish(topic, 0, true, vol)) {
      strlcpy(lastVolume, vol, sizeof(lastVolume));
    }
  }
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  log_w("##[MQTT]# disconnected: reason=%d", static_cast<int>(reason));
  if (WiFi.isConnected() && mqttReconnectTimer) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  static char assembled[MQTT_BURL_SIZE];
  static size_t expected = 0;
  static size_t received = 0;
  char commandTopic[100];
  if (!mqttCommandQueue || !topic || !payload || properties.retain ||
      snprintf(commandTopic, sizeof(commandTopic), "%scommand", MQTT_ROOT_TOPIC) >=
          static_cast<int>(sizeof(commandTopic)) ||
      strcmp(topic, commandTopic) != 0) return;
  if (total == 0 || total >= sizeof(assembled) || index > total || len > total - index) {
    expected = 0;
    received = 0;
    return;
  }
  if (index == 0) { expected = total; received = 0; }
  if (expected != total || index != received) return;
  memcpy(assembled + index, payload, len);
  received += len;
  if (index + len != total) return;
  if (memchr(assembled, '\0', total)) {
    expected = 0;
    received = 0;
    return;
  }
  assembled[total] = '\0';
  MqttCommand command{};
  memcpy(command.text, assembled, total + 1);
  if (xQueueSend(mqttCommandQueue, &command, 0) != pdTRUE) {
    log_w("##[MQTT]# command queue full; message dropped");
  }
  expected = 0;
  received = 0;
}

void mqttLoop() {
  if (!mqttInitialized) return;
  MqttCommand command{};
  for (int n = 0; n < 2 && xQueueReceive(mqttCommandQueue, &command, 0) == pdTRUE; ++n) {
    if (!executeMqttCommand(command.text)) {
      log_w("##[MQTT]# rejected unsupported or invalid command");
      char errorTopic[100];
      if (mqttClient.connected() &&
          snprintf(errorTopic, sizeof(errorTopic), "%serror", MQTT_ROOT_TOPIC) <
              static_cast<int>(sizeof(errorTopic))) {
        mqttClient.publish(errorTopic, 0, false, "unsupported or invalid command");
      }
    }
  }
  static uint32_t lastRefresh = 0;
  if (mqttClient.connected() && millis() - lastRefresh >= 1000) {
    lastRefresh = millis();
    static uint32_t lastAdvertisementAttempt = 0;
    if (__atomic_load_n(&integrationAdvertisementPending, __ATOMIC_ACQUIRE) &&
        millis() - lastAdvertisementAttempt >= 5000) {
      lastAdvertisementAttempt = millis();
      if (publishIntegrationAdvertisement()) {
        __atomic_store_n(&integrationAdvertisementPending, false, __ATOMIC_RELEASE);
      }
    }
    publishStatusNow();
    publishVolumeNow();
    publishPlaylistNow();
    publishExtendedState();
  }
}

#endif // #ifdef MQTT_ROOT_TOPIC
