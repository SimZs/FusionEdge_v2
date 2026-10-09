#include "options.h"

#ifdef USE_BLUETOOTH

#include "bluetooth.h"
#include "../../myoptions.h"
#include "bluetooth_config.h"
#include "player.h"
#include "config.h"
#include "display.h"
#include "netserver.h"
#include "spectrumAnalyzer.h"
#include "../displays/tools/language.h"
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(BT_UART_TX) || !defined(BT_UART_RX) || !defined(BT_UART_BAUD) || \
    !defined(BT_I2S_BCK) || !defined(BT_I2S_LRCK) || \
    !defined(BT_I2S_RX_DATA) || !defined(BT_I2S_TX_DATA)
#    error "USE_BLUETOOTH requires BT UART plus separate I2S RX/TX data pin definitions in myoptions.h"
#endif

// Player : public Audio (see player.h) - the global `player` IS the Audio
// instance that owns the DAC TX channel, so we drive the bridge through it.

BluetoothPlayer bluetooth;

// Audio.cpp provides this as a weak post-DSP hook. Keep the callback tiny:
// it runs in the normal Audio task and must never wait for the BT path.
void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
    (void)continueI2S;
    if (outBuff && validSamples > 0) {
        bluetooth.pushSourcePcm(outBuff, static_cast<size_t>(validSamples));
        // Keep the normal DAC I2S write active because it paces the decoder.
        // In BT-only source monitoring, copy the real PCM first, then replace
        // only the local-DAC block with digital silence.
        if (bluetooth.silenceLocalDacForSource()) {
            memset(outBuff, 0,
                   static_cast<size_t>(validSamples) * 2U * sizeof(int32_t));
        }
    }
}

static_assert(BT_SOURCE_PCM_RING_FRAMES > BT_SOURCE_PCM_PREFILL_FRAMES,
              "BT source PCM ring must be larger than its prefill threshold");
static_assert((BT_SOURCE_PCM_RING_FRAMES & (BT_SOURCE_PCM_RING_FRAMES - 1)) == 0,
              "BT source PCM ring size must be a power of two");
static_assert(BT_SOURCE_PCM_BLOCK_FRAMES > 0 &&
              BT_SOURCE_PCM_BLOCK_FRAMES <= BT_SOURCE_PCM_PREFILL_FRAMES,
              "BT source PCM block size is invalid");
static_assert(BT_SOURCE_PCM_TARGET_FRAMES >= BT_SOURCE_PCM_PREFILL_FRAMES &&
              BT_SOURCE_PCM_TARGET_FRAMES < BT_SOURCE_PCM_RING_FRAMES,
              "BT source PCM elastic-buffer target is invalid");
static_assert(BT_I2S_RX_DATA != BT_I2S_TX_DATA,
              "FSC-BT1058 I2S_DOUT and I2S_DIN require separate ESP GPIOs");
static_assert(BT_I2S_BCK != BT_I2S_LRCK &&
              BT_I2S_BCK != BT_I2S_RX_DATA &&
              BT_I2S_BCK != BT_I2S_TX_DATA &&
              BT_I2S_LRCK != BT_I2S_RX_DATA &&
              BT_I2S_LRCK != BT_I2S_TX_DATA,
              "FSC-BT1058 I2S clocks and data signals must use distinct GPIOs");

namespace {
constexpr int32_t BT_SINK_GAIN_UNITY_Q15 = 32768;

int32_t sinkGainFromDbQ15(float db) {
    if (db > 0.0f) db = 0.0f;
    if (db < -76.0f) return 0;
    const float linear = powf(10.0f, db / 20.0f);
    int32_t gain = static_cast<int32_t>(lroundf(
        linear * static_cast<float>(BT_SINK_GAIN_UNITY_Q15)));
    if (gain < 0) return 0;
    if (gain > BT_SINK_GAIN_UNITY_Q15) return BT_SINK_GAIN_UNITY_Q15;
    return gain;
}

void calculateSinkDacGains(uint8_t volumeStep, int8_t storedBalance,
                           float curveDb, int32_t& leftQ15,
                           int32_t& rightQ15) {
    if (volumeStep == 0) {
        leftQ15 = 0;
        rightQ15 = 0;
        return;
    }

    // Match Player/Audio semantics exactly. Config stores the UI direction,
    // while Audio receives its sign-inverted value.
    int32_t dspBalance = -static_cast<int32_t>(storedBalance);
    if (dspBalance < -16) dspBalance = -16;
    if (dspBalance > 16) dspBalance = 16;
    float leftDb = curveDb;
    float rightDb = curveDb;
    if (dspBalance > 0) {
        leftDb -= static_cast<float>(dspBalance);
    } else if (dspBalance < 0) {
        rightDb += static_cast<float>(dspBalance);
    }
    leftQ15 = sinkGainFromDbQ15(leftDb);
    rightQ15 = sinkGainFromDbQ15(rightDb);
}

String stripTrailingYoutubeQualifier(const String& input) {
    String cleaned = input;
    cleaned.trim();
    String lower = cleaned;
    lower.toLowerCase();
    static const char* const suffixes[] = {
        " (official music video)", " (official video)",
        " (official audio)", " [official music video]",
        " [official video]", " [official audio]"
    };
    for (const char* suffix : suffixes) {
        if (!lower.endsWith(suffix)) continue;
        cleaned.remove(cleaned.length() - strlen(suffix));
        cleaned.trim();
        break;
    }
    return cleaned;
}

String normalizeChannelPrefixedTitle(const String& rawTitle,
                                     const String& channel) {
    if (rawTitle.length() == 0 || channel.length() == 0) return rawTitle;

    String lowerTitle = rawTitle;
    String lowerChannel = channel;
    lowerTitle.toLowerCase();
    lowerChannel.toLowerCase();
    static const char* const separators[] = {": ", " - "};
    for (const char* separator : separators) {
        String prefix = lowerChannel;
        prefix += separator;
        if (!lowerTitle.startsWith(prefix)) continue;
        String track = rawTitle.substring(prefix.length());
        track = stripTrailingYoutubeQualifier(track);
        if (track.length() == 0) return rawTitle;
        String normalized = channel;
        normalized += " - ";
        normalized += track;
        return normalized;
    }
    return rawTitle;
}
} // namespace

#ifndef BT_I2S_SAMPLE_RATE
#define BT_I2S_SAMPLE_RATE 44100
#endif

#ifndef BT_I2S_BITS
#define BT_I2S_BITS 16
#endif

#ifndef BT_I2S_RX_MASTER
#define BT_I2S_RX_MASTER true
#endif

#ifndef BT_I2S_USE_MCLK
#define BT_I2S_USE_MCLK true
#endif

#ifndef BT_I2S_FORMAT
#define BT_I2S_FORMAT 0
#endif

#ifndef BT_I2S_BCLK_INV
#define BT_I2S_BCLK_INV false
#endif

#ifndef BT_I2S_WS_INV
#define BT_I2S_WS_INV false
#endif

#ifndef BT_I2S_32_SHIFT
#define BT_I2S_32_SHIFT 16
#endif

#ifndef BT_SPECTRUM_GAIN
#define BT_SPECTRUM_GAIN 1
#endif

#if BT_I2S_BITS != 16 && BT_I2S_BITS != 24 && BT_I2S_BITS != 32
#error "BT_I2S_BITS must be 16, 24 or 32"
#endif

#if BT_I2S_BITS == 24
#define BT_I2S_DATA_WIDTH I2S_DATA_BIT_WIDTH_24BIT
#elif BT_I2S_BITS == 32
#define BT_I2S_DATA_WIDTH I2S_DATA_BIT_WIDTH_32BIT
#else
#define BT_I2S_DATA_WIDTH I2S_DATA_BIT_WIDTH_16BIT
#endif

#if BT_I2S_FORMAT < 0 || BT_I2S_FORMAT > 2
#error "BT_I2S_FORMAT must be 0=Philips, 1=MSB, or 2=PCM"
#endif

static int32_t scaleSpectrumSample(int32_t sample) {
    int64_t scaled = (int64_t)sample * BT_SPECTRUM_GAIN;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < INT32_MIN) return INT32_MIN;
    return (int32_t)scaled;
}

static uint32_t parseUnsignedValue(const char* line) {
    const char* p = strchr(line, '=');
    return p ? (uint32_t)strtoul(p + 1, nullptr, 10) : 0;
}

static const char* moduleRoleName(btModuleRole_e role) {
    switch (role) {
        case BT_ROLE_SINK: return "sink/input";
        case BT_ROLE_SOURCE: return "source/output";
        default: return "unknown";
    }
}

// =====================================================================
//  UART AT driver
// =====================================================================

void BluetoothPlayer::begin() {
    _uart.setRxBufferSize(8192);
    _uart.begin(BT_UART_BAUD, SERIAL_8N1, BT_UART_RX, BT_UART_TX);
    _lineLen = 0;
    _lineOverflow = false;
    _consoleLen = 0;
    _consoleBuf[0] = '\0';
    _consoleCommandPending = false;
    _consoleCommandDeadlineMs = 0;
    _startupProbeStep = 0;
    _nextStartupProbeMs = millis() + 1500U;
    _startupProbeComplete = false;
    __atomic_store_n(&_moduleRole, BT_ROLE_UNKNOWN, __ATOMIC_RELEASE);
    _moduleI2sCfg = -1;
    _moduleAuxCfg = -1;
    _moduleProfile = -1;
    _moduleAvrcpCfg = -1;
    _moduleBluetoothEnabled = -1;
    _moduleName[0] = '\0';
    _nameBeforeRoleChange[0] = '\0';
    _aliasSyncState = ALIAS_SYNC_IDLE;
    _aliasSyncNextActionMs = 0;
    _aliasSyncDeadlineMs = 0;
    _aliasSyncAttempts = 0;
    _inputUiCommand = INPUT_UI_COMMAND_NONE;
    _inputUiClientId = 0;
    _inputUiCommandDeadlineMs = 0;
    _inputRefreshPending = true;
    _requestedDiscoverable = false;
    _moduleDiscoverable = -1;
    memset(_inputPairedDevices, 0, sizeof(_inputPairedDevices));
    _inputPairedDeviceCount = 0;
    memset(_inputPairedRefreshDevices, 0,
           sizeof(_inputPairedRefreshDevices));
    _inputPairedRefreshDeviceCount = 0;
    _inputPairedRefreshSawEnd = false;
    _inputRssiProbeIndex = 0;
    _inputRssiProbeMatched = false;
    _inputRssiProbeAddress[0] = '\0';
    _remoteRssi = -127;
    _remoteRssiAddress[0] = '\0';
    _inputForgetAddress[0] = '\0';
    _inputConnectedAddress[0] = '\0';
    _inputConnectedName[0] = '\0';
    _inputCodec[0] = '\0';
    _inputCodecSampleRate = 0;
    if (!_sinkControlQueue) {
        _sinkControlQueue = xQueueCreate(
            BT_SINK_CONTROL_QUEUE_LENGTH, sizeof(btSinkControl_e));
    } else {
        xQueueReset(_sinkControlQueue);
    }
    _sinkControlInFlight = BT_SINK_CONTROL_NONE;
    _sinkControlDeadlineMs = 0;
    __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_sinkVolumeStep,
                     config.store.volume <= 21U ? config.store.volume : 21U,
                     __ATOMIC_RELEASE);
    if (!_sinkControlQueue) {
        log_e("##[BT-CTRL]# failed to allocate sink control queue");
    }
    _previousSinkTitle = "";
    _pendingSinkTitle = "";
    _pendingSinkArtist = "";
    _pendingSinkAlbum = "";
    _sinkPlaybackSeen = false;
    _sinkMetaFreshAccepted = false;
    _sinkStaleMetaLogged = false;
    _lastA2dpStatus = 0;
    _lastAvrcpStatus = 0;
    _lastHfpStatus = 0;
    __atomic_store_n(&_remoteBatteryLevel, static_cast<int8_t>(-1),
                     __ATOMIC_RELEASE);
    _remoteBatteryAddress[0] = '\0';
    _sourceAudioPathReady = false;
    _sourceAudioEnablePending = false;
    _sourceAudioEnableDeadlineMs = 0;
    _sourceAudioEnableAttempts = 0;
    _nextSourceAudioEnableMs = 0;
    _lastAtTxMs = 0;
    _nextSourceRadioAutostartMs = millis() + 1000U;
    _sourceRadioManualStop = false;
    __atomic_store_n(&_requestedRole, BT_ROLE_UNKNOWN, __ATOMIC_RELEASE);
    _roleTransitionState = BT_ROLE_TRANSITION_IDLE;
    _roleTransitionNextActionMs = 0;
    _roleTransitionDeadlineMs = 0;
    _roleDisconnectDeadlineMs = 0;
    _roleChangeAttempts = 0;
    _roleConfigAttempts = 0;
    _roleConfigChanged = false;
    _sourceRadioEnabled = false;
    const uint8_t savedOutputMode =
        config.store.btSourceLocalDac <= BT_SOURCE_OUTPUT_LOCAL_ONLY
            ? config.store.btSourceLocalDac
            : static_cast<uint8_t>(BT_SOURCE_OUTPUT_LOCAL_AND_BLUETOOTH);
    __atomic_store_n(&_sourceOutputMode, savedOutputMode, __ATOMIC_RELEASE);
    _sourceLocalDacEnabled =
        savedOutputMode != BT_SOURCE_OUTPUT_BLUETOOTH_ONLY;
    _sourceRadioStopRequest = false;
    memset(_sourceScanDevices, 0, sizeof(_sourceScanDevices));
    _sourceScanDeviceCount = 0;
    _sourceScanActive = false;
    _sourceScanDeadlineMs = 0;
    _sourceScanRequestedSeconds = BT_SOURCE_SCAN_SECONDS;
    _sourceUiCommand = SOURCE_UI_COMMAND_NONE;
    _sourceUiCommandClientId = 0;
    _sourceUiCommandDeadlineMs = 0;
    _sourceSwitchState = SOURCE_SWITCH_IDLE;
    _sourceSwitchClientId = 0;
    _sourceSwitchDeadlineMs = 0;
    _sourceSwitchNextStatusQueryMs = 0;
    _sourceSwitchTargetAddress[0] = '\0';
    _sourceSwitchPreviousAddress[0] = '\0';
    _sourceSwitchPreviousName[0] = '\0';
    _sourceSwitchAddressReported = false;
    _sourceConnectPending = false;
    _sourceManualConnectPending = false;
    _sourceConnectDeadlineMs = 0;
    _sourceConnectedAddress[0] = '\0';
    _sourceConnectedName[0] = '\0';
    _moduleBluetoothEnabled = -1;
    _sourceRouteApplyPending = true;
    _sourceRouteVerifyPending = false;
    _sourceBluetoothTarget = true;
    _sourceRouteAttempts = 0;
    _sourceRouteClientId = 0;
    _sourceRouteNextActionMs = 0;
    _sourceReconnectAfterEnable = false;
    _sourceReconnectNextActionMs = 0;
    _sourceUiPlayingKnown = false;
    _sourceUiPlaying = false;
    __atomic_store_n(&_sourcePcmWriteFrame, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmReadFrame, 0U, __ATOMIC_RELAXED);
    _bridgeSampleRate = BT_I2S_SAMPLE_RATE;
    _sourceCodecSampleRate = 0;
    _streamBits = BT_I2S_BITS;
    delay(200);

    // The lab module is configured persistently outside this firmware.
    // Read-only startup checks are issued later by serviceStartupProbe(), one
    // at a time, because the BT1058 may still be booting at this point.

    // I2S RX csatorna létrehozása (idle, engedélyezés nélkül — az csak
    // startBridge()-ben történik). Az I2S újrakonfigurálását a keskeny
    // display DMA guard védi; ez nem nyit SPI tranzakciót és nem lockolja
    // tartósan a widgeteket.
    if (initI2SRx()) {
        log_i("##[BT]# UART2 initialized, I2S RX bridge ready");
    } else {
        log_e("##[BT]# I2S RX init failed - audio bridge unavailable (UART/meta still works)");
    }
#if CORE_DEBUG_LEVEL >= 3
    printLabHelp();
#endif
}

void BluetoothPlayer::end() {
    stopSourceRadio();
    stopSourceTone();
    stopBridge();
    deinitI2SRx();
    _uart.end();
    if (_sinkControlQueue) {
        vQueueDelete(_sinkControlQueue);
        _sinkControlQueue = nullptr;
    }
    _sinkControlInFlight = BT_SINK_CONTROL_NONE;
    _sinkControlDeadlineMs = 0;
    __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
    _state = BT_STATE_UNKNOWN;
}

void BluetoothPlayer::sendAT(const char* cmd) {
    _uart.print(cmd);
    _uart.print("\r\n");
    _lastAtTxMs = millis();
    log_d("##[BT]# TX: %s", cmd);
}

void BluetoothPlayer::handleLabConsole() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());

        if (c == '\r' || c == '\n') {
            if (_consoleLen == 0) continue;

            _consoleBuf[_consoleLen] = '\0';
            Serial.println();
            const bool startsWithAt = _consoleLen >= 2 &&
                (_consoleBuf[0] == 'A' || _consoleBuf[0] == 'a') &&
                (_consoleBuf[1] == 'T' || _consoleBuf[1] == 't');
            String localCommand(_consoleBuf);
            localCommand.trim();
            localCommand.toLowerCase();
            if (localCommand == "!role sink" || localCommand == "!role input") {
                if (requestRole(BT_ROLE_SINK)) {
                    Serial.println("##[BT-LAB]# BT input transition requested");
                } else {
                    Serial.println("##[BT-LAB]# role request refused; use !role status");
                }
            } else if (localCommand == "!role source" || localCommand == "!role output") {
                if (requestRole(BT_ROLE_SOURCE)) {
                    Serial.println("##[BT-LAB]# BT output transition requested");
                } else {
                    Serial.println("##[BT-LAB]# role request refused; use !role status");
                }
            } else if (localCommand == "!role" || localCommand == "!role status") {
                Serial.printf(
                    "##[BT-LAB]# role=%s requested=%s transition=%s MODCFG=%d AUXCFG=%d I2SCFG=%d PROFILE=%ld AVRCPCFG=%d mode=%u A2DP=%u AVRCP=%u bridge=%u radio=%u\r\n",
                    moduleRoleName(_moduleRole), moduleRoleName(_requestedRole),
                    roleTransitionStateName(),
                    _moduleRole == BT_ROLE_SOURCE ? 1 :
                    _moduleRole == BT_ROLE_SINK ? 0 : -1,
                    (int)_moduleAuxCfg, (int)_moduleI2sCfg, (long)_moduleProfile,
                    (int)_moduleAvrcpCfg,
                    (unsigned)config.getMode(), (unsigned)_lastA2dpStatus,
                    (unsigned)_lastAvrcpStatus, (unsigned)bridgeRunning(),
                    (unsigned)sourceRadioRunning());
            } else if (localCommand == "!tone on") {
                _sourceRadioManualStop = true;
                if (startSourceTone()) {
                    Serial.println("##[BT-LAB]# 1 kHz I2S source tone ON");
                } else {
                    Serial.println("##[BT-LAB]# tone start refused; see log above");
                }
            } else if (localCommand == "!tone off") {
                stopSourceTone();
                Serial.println("##[BT-LAB]# I2S source tone OFF");
            } else if (localCommand == "!tone" || localCommand == "!tone status") {
                Serial.printf("##[BT-LAB]# tone=%s role=%s AUXCFG=%d I2SCFG=%d\r\n",
                              sourceToneRunning() ? "ON" : "OFF",
                              _moduleRole == BT_ROLE_SOURCE ? "source" :
                              _moduleRole == BT_ROLE_SINK ? "sink" : "unknown",
                              (int)_moduleAuxCfg, (int)_moduleI2sCfg);
            } else if (localCommand == "!radio on") {
                _sourceRadioManualStop = false;
                if (startSourceRadio()) {
                    Serial.println("##[BT-LAB]# player PCM -> Bluetooth radio relay ON");
                } else {
                    Serial.println("##[BT-LAB]# radio relay start refused; see log above");
                }
            } else if (localCommand == "!radio off") {
                _sourceRadioManualStop = true;
                stopSourceRadio();
                Serial.println("##[BT-LAB]# player PCM -> Bluetooth radio relay OFF");
            } else if (localCommand == "!radio" || localCommand == "!radio status") {
                Serial.printf(
                    "##[BT-LAB]# radio=%s audio_path=%s/%u player=%s role=%s AUXCFG=%d I2SCFG=%d A2DP=%u buffer=%u/%u in=%u out=%u drop=%u rebuffer=%u underrun=%u txerr=%u hwm=%u peak=%u/%u nonzero=%u\r\n",
                    sourceRadioRunning() ? "ON" : "OFF",
                    _sourceAudioPathReady ? "ready" : "waiting",
                    (unsigned)_sourceAudioEnableAttempts,
                    player.isRunning() ? "playing" : "stopped",
                    _moduleRole == BT_ROLE_SOURCE ? "source" :
                    _moduleRole == BT_ROLE_SINK ? "sink" : "unknown",
                    (int)_moduleAuxCfg, (int)_moduleI2sCfg,
                    (unsigned)_lastA2dpStatus,
                    (unsigned)sourcePcmBufferedFrames(),
                    (unsigned)BT_SOURCE_PCM_RING_FRAMES,
                    (unsigned)__atomic_load_n(&_sourcePcmFramesIn, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmFramesOut, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmDroppedFrames, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmRebufferEvents, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmUnderrunBlocks, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmWriteErrors, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmHighWaterFrames, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmPeakLeft, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmPeakRight, __ATOMIC_RELAXED),
                    (unsigned)__atomic_load_n(&_sourcePcmNonZeroSamples, __ATOMIC_RELAXED));
            } else if (localCommand == "!local on") {
                config.saveValue(&config.store.btSourceLocalDac,
                                 static_cast<uint8_t>(BT_SOURCE_OUTPUT_LOCAL_AND_BLUETOOTH));
                setSourceOutputMode(BT_SOURCE_OUTPUT_LOCAL_AND_BLUETOOTH);
                Serial.println("##[BT-LAB]# local DAC monitor ON and saved (BT source mode plays locally too)");
            } else if (localCommand == "!local off") {
                config.saveValue(&config.store.btSourceLocalDac,
                                 static_cast<uint8_t>(BT_SOURCE_OUTPUT_BLUETOOTH_ONLY));
                setSourceOutputMode(BT_SOURCE_OUTPUT_BLUETOOTH_ONLY);
                Serial.println("##[BT-LAB]# local DAC monitor OFF and saved (BT source PCM remains active)");
            } else if (localCommand == "!local only") {
                config.saveValue(&config.store.btSourceLocalDac,
                                 static_cast<uint8_t>(BT_SOURCE_OUTPUT_LOCAL_ONLY));
                setSourceOutputMode(BT_SOURCE_OUTPUT_LOCAL_ONLY);
                Serial.println("##[BT-LAB]# local-only output saved (module Bluetooth radio will be disabled in source mode)");
            } else if (localCommand == "!local" || localCommand == "!local status") {
                Serial.printf("##[BT-LAB]# output mode=%u local DAC=%s effective_mute=%s module BT=%d GPIO2=unchanged\r\n",
                              (unsigned)sourceOutputMode(),
                              sourceLocalDacEnabled() ? "ON" : "OFF",
                              silenceLocalDacForSource() ? "YES" : "NO",
                              (int)_moduleBluetoothEnabled);
            } else if (localCommand == "!help") {
                printLabHelp();
            } else if (startsWithAt) {
                if (roleTransitionBusy() || _sourceAudioEnablePending ||
                    aliasSyncBusy() || inputUiBusy() || sinkControlBusy() || _sourceScanActive ||
                    _sourceUiCommand != SOURCE_UI_COMMAND_NONE) {
                    Serial.println("##[BT-LAB]# raw AT command refused while Bluetooth control is busy");
                    _consoleLen = 0;
                    _consoleBuf[0] = '\0';
                    continue;
                }
                if (strchr(_consoleBuf, '=')) {
                    Serial.println("##[BT-LAB]# parameterized command; may change runtime or stored state");
                }
                _consoleCommandPending = true;
                _consoleCommandDeadlineMs = millis() + 3000U;
                sendAT(_consoleBuf);
            } else {
                Serial.println("##[BT-LAB]# ignored; command must start with AT");
            }
            _consoleLen = 0;
            _consoleBuf[0] = '\0';
            continue;
        }

        if (c == '\b' || c == 0x7F) {
            if (_consoleLen > 0) {
                --_consoleLen;
                _consoleBuf[_consoleLen] = '\0';
            }
            continue;
        }

        if (c >= 0x20 && c <= 0x7E && _consoleLen < sizeof(_consoleBuf) - 1) {
            _consoleBuf[_consoleLen++] = c;
        }
    }
}

void BluetoothPlayer::printLabHelp() const {
    Serial.println("##[BT-LAB]# console: !role input/output/status | AT+... | !tone on/off/status | !radio on/off/status | !local on/off/only/status | !help");
    Serial.printf("##[BT-LAB]# I2S data: module DOUT -> GPIO%u, GPIO%u -> module DIN\r\n",
                  (unsigned)BT_I2S_RX_DATA, (unsigned)BT_I2S_TX_DATA);
}

bool BluetoothPlayer::roleTransitionBusy() const {
    return _roleTransitionState >= BT_ROLE_TRANSITION_ASSESS &&
           _roleTransitionState <= BT_ROLE_TRANSITION_REBOOT_CONFIG;
}

const char* BluetoothPlayer::roleTransitionStateName() const {
    switch (_roleTransitionState) {
        case BT_ROLE_TRANSITION_IDLE: return "idle";
        case BT_ROLE_TRANSITION_ASSESS: return "assess";
        case BT_ROLE_TRANSITION_WAIT_DISCONNECT: return "wait-disconnect";
        case BT_ROLE_TRANSITION_WAIT_REBOOT: return "wait-reboot/probe";
        case BT_ROLE_TRANSITION_APPLY_AUX: return "apply-aux";
        case BT_ROLE_TRANSITION_APPLY_I2S: return "apply-i2s";
        case BT_ROLE_TRANSITION_REBOOT_CONFIG: return "reboot-config";
        case BT_ROLE_TRANSITION_READY: return "ready";
        case BT_ROLE_TRANSITION_FAILED: return "failed";
        default: return "invalid";
    }
}

void BluetoothPlayer::invalidateStartupProbe(uint32_t delayMs) {
    _startupProbeStep = 0;
    _nextStartupProbeMs = millis() + delayMs;
    _startupProbeComplete = false;
    __atomic_store_n(&_moduleRole, BT_ROLE_UNKNOWN, __ATOMIC_RELEASE);
    _moduleI2sCfg = -1;
    _moduleAuxCfg = -1;
    _moduleProfile = -1;
    _moduleAvrcpCfg = -1;
    _lastA2dpStatus = 0;
    _lastAvrcpStatus = 0;
    _lastHfpStatus = 0;
    __atomic_store_n(&_remoteBatteryLevel, static_cast<int8_t>(-1),
                     __ATOMIC_RELEASE);
    _remoteBatteryAddress[0] = '\0';
    _sourceCodecSampleRate = 0;
    _sourceAudioPathReady = false;
    _sourceAudioEnablePending = false;
    _sourceAudioEnableDeadlineMs = 0;
    _sourceAudioEnableAttempts = 0;
    _nextSourceAudioEnableMs = 0;
    _aliasSyncState = ALIAS_SYNC_IDLE;
    _aliasSyncNextActionMs = 0;
    _aliasSyncDeadlineMs = 0;
    _aliasSyncAttempts = 0;
    _inputUiCommand = INPUT_UI_COMMAND_NONE;
    _inputUiCommandDeadlineMs = 0;
    _inputPairedRefreshDeviceCount = 0;
    _inputPairedRefreshSawEnd = false;
    _inputRssiProbeIndex = 0;
    _inputRssiProbeMatched = false;
    _inputRssiProbeAddress[0] = '\0';
    _remoteRssi = -127;
    _remoteRssiAddress[0] = '\0';
    _inputForgetAddress[0] = '\0';
    _inputRefreshPending = true;
    _moduleDiscoverable = -1;
    _inputConnectedAddress[0] = '\0';
    _inputConnectedName[0] = '\0';
    _inputCodec[0] = '\0';
    _inputCodecSampleRate = 0;
    if (_sinkControlQueue) xQueueReset(_sinkControlQueue);
    _sinkControlInFlight = BT_SINK_CONTROL_NONE;
    _sinkControlDeadlineMs = 0;
    __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
    _sourceRouteApplyPending = true;
    _sourceRouteVerifyPending = false;
    _sourceRouteAttempts = 0;
    _sourceRouteNextActionMs = 0;
    _sourceReconnectAfterEnable = false;
    _sourceReconnectNextActionMs = 0;
}

bool BluetoothPlayer::roleConfigurationMatches(btModuleRole_e role) const {
    if (role == BT_ROLE_SOURCE) {
        return _moduleRole == BT_ROLE_SOURCE && _moduleAuxCfg == 3 &&
               _moduleI2sCfg == BT1058_EXPECTED_SOURCE_I2SCFG;
    }
    if (role == BT_ROLE_SINK) {
        // AVRCPCFG is not advertised by every firmware build. If the query is
        // supported, require its ID3-report bit as part of a usable sink setup;
        // otherwise keep metadata best-effort instead of blocking audio.
        const bool metadataReady = _moduleAvrcpCfg < 0 ||
                                   (_moduleAvrcpCfg & 1) != 0;
        return _moduleRole == BT_ROLE_SINK &&
               _moduleI2sCfg == BT1058_EXPECTED_I2SCFG && metadataReady;
    }
    return false;
}

bool BluetoothPlayer::requestRole(btModuleRole_e role,
                                  int8_t targetPlaybackMode) {
    if ((role != BT_ROLE_SINK && role != BT_ROLE_SOURCE) ||
        roleTransitionBusy() || aliasSyncBusy() || inputUiBusy() ||
        _sourceUiCommand == SOURCE_UI_COMMAND_SET_BT_ENABLE ||
        _sourceUiCommand == SOURCE_UI_COMMAND_VERIFY_BT_ENABLE) {
        return false;
    }

    // Source -> sink must begin with a real player stop. Merely stopping the
    // BT PCM relay here would make the output silent while the display and
    // player still reported playback, and only the completed module reboot
    // would enqueue PR_STOP later. Keep the current relay alive until the
    // player task has consumed this command; serviceRoleTransition() performs
    // the I2S/module hand-over afterwards.
    if (role == BT_ROLE_SINK &&
        (player.isRunning() || player.status() == PLAYING ||
         player.wantsPlayback())) {
        if (!player.sendCommandNoWait({PR_STOP, 0})) {
            log_e("##[BT-ROLE]# sink transition refused: player stop queue is full");
            return false;
        }
        log_i("##[BT-ROLE]# player stop queued before sink transition");
    }

    if (_sourceScanActive) {
        sendAT("AT+SCAN=0");
        finishSourceScan("cancelled", "Scan cancelled by Bluetooth role change");
    }
    _sourceUiCommand = SOURCE_UI_COMMAND_NONE;
    _sourceConnectPending = false;
    _sourceManualConnectPending = false;
    _sourceSwitchState = SOURCE_SWITCH_IDLE;
    _sourceSwitchDeadlineMs = 0;
    _sourceScanRequestedSeconds = BT_SOURCE_SCAN_SECONDS;
    if (_sinkControlQueue) xQueueReset(_sinkControlQueue);
    __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);

    __atomic_store_n(&_requestedRole, role, __ATOMIC_RELEASE);
    _roleTransitionState = BT_ROLE_TRANSITION_ASSESS;
    _roleTransitionNextActionMs = millis();
    _roleTransitionDeadlineMs = millis() + BT_ROLE_SWITCH_TIMEOUT_MS;
    _roleDisconnectDeadlineMs = 0;
    _roleChangeAttempts = 0;
    _roleConfigAttempts = 0;
    _roleConfigChanged = false;
    strlcpy(_nameBeforeRoleChange, _moduleName,
            sizeof(_nameBeforeRoleChange));
    _sourceAudioEnablePending = false;
    _sourceAudioEnableDeadlineMs = 0;

    // Prevent source relay auto-start while a role hand-over is pending.
    // For a sink request, the active I2S owner is deliberately stopped later,
    // after PR_STOP has completed. Source requests can release it immediately.
    _sourceRadioManualStop = true;

    // BT input is an Edge playback mode; BT output is an output route for the
    // regular WEB/SD/DLNA player. Let changeMode() see the still-active bridge
    // so it preserves the previous playing state. The role state is already
    // busy here, therefore this nested call performs only the playback-mode
    // change and cannot recursively request another module role transition.
    if (role == BT_ROLE_SOURCE) {
        stopSourceRadio();
        stopSourceTone();
    }
    if (role == BT_ROLE_SOURCE && config.getMode() == PM_BLUETOOTH) {
        const int8_t destination =
            targetPlaybackMode == PM_WEB || targetPlaybackMode == PM_SDCARD
                ? targetPlaybackMode
                : static_cast<int8_t>(PM_WEB);
        config.changeMode(destination);
    } else if (role == BT_ROLE_SOURCE) {
        stopBridge();
    }

    // Treat every new sink connection as a fresh metadata generation. This is
    // important when the phone resumes the same track that was cached before
    // the temporary source-mode excursion.
    if (role == BT_ROLE_SINK) {
        beginSinkMetaGeneration(true);
    }
    log_i("##[BT-ROLE]# requested %s", moduleRoleName(role));
    return true;
}

void BluetoothPlayer::completeRoleTransition() {
    const btModuleRole_e completedRole = _requestedRole;
    _roleTransitionState = BT_ROLE_TRANSITION_READY;
    _roleDisconnectDeadlineMs = 0;

    if (completedRole == BT_ROLE_SINK) {
        _sourceRadioManualStop = true;
        _inputRefreshPending = true;
        if (_moduleProfile >= 0 &&
            (static_cast<uint32_t>(_moduleProfile) & (1UL << 7)) == 0) {
            log_w("##[BT-ROLE]# sink is ready, but AVRCP Controller is disabled in PROFILE=%ld",
                  (long)_moduleProfile);
        }
        if (_moduleAvrcpCfg >= 0 && (_moduleAvrcpCfg & 1) == 0) {
            log_w("##[BT-ROLE]# sink is ready, but AVRCP metadata reporting is disabled: AVRCPCFG=%d",
                  (int)_moduleAvrcpCfg);
        }
        if (config.getMode() != PM_BLUETOOTH) {
            config.changeMode(PM_BLUETOOTH);
        } else if (!bridgeRunningOrPending()) {
            startBridge();
        }
        // MODCFG reboot/probing can legitimately report A2DP standby while
        // the transition is in progress. Replace the temporary startup text
        // with a stable ready state only after the sink is fully verified.
        if (_title.length() == 0 && _artist.length() == 0 &&
            config.getMode() == PM_BLUETOOTH) {
            if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5) {
                config.setTitle(playing()
                                    ? "Playing"
                                    : "Bluetooth connected");
            } else {
                config.setTitle("Waiting for Bluetooth device...");
            }
        }
    } else {
        _sourceRadioManualStop = false;
        _sourceUiInitializationPending = true;
        _inputRefreshPending = true;
        if (config.getMode() == PM_BLUETOOTH) config.changeMode(PM_WEB);
        _nextSourceRadioAutostartMs = millis();
    }

    log_i("##[BT-ROLE]# transition complete: %s AUXCFG=%d I2SCFG=%d PROFILE=%ld NAME=%s",
          moduleRoleName(completedRole), (int)_moduleAuxCfg,
          (int)_moduleI2sCfg, (long)_moduleProfile,
          _moduleName[0] ? _moduleName : "unknown");
    _nameBeforeRoleChange[0] = '\0';
    _sourceRouteApplyPending = true;
    _sourceRouteAttempts = 0;
    _sourceRouteNextActionMs = millis();
    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
}

void BluetoothPlayer::failRoleTransition(const char* reason) {
    _roleTransitionState = BT_ROLE_TRANSITION_FAILED;
    _roleDisconnectDeadlineMs = 0;
    _sourceRadioManualStop = true;
    log_e("##[BT-ROLE]# transition to %s failed: %s",
          moduleRoleName(_requestedRole), reason ? reason : "unknown error");
    if (_requestedRole == BT_ROLE_SINK &&
        config.getMode() == PM_BLUETOOTH) {
        config.setTitle("Bluetooth input failed");
    }
    netserver.btSourceScanStateToWs(0, "error", _sourceScanDeviceCount,
                                    reason ? reason : "Bluetooth role switch failed");
}

void BluetoothPlayer::serviceRoleTransition() {
    if (!roleTransitionBusy()) return;
    // A transport command already on the UART owns the next terminal reply.
    // Let it complete (or time out) before sending role-management commands.
    if (_sinkControlInFlight != BT_SINK_CONTROL_NONE) return;

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _roleTransitionDeadlineMs) >= 0) {
        failRoleTransition("overall timeout");
        return;
    }
    if (static_cast<int32_t>(now - _roleTransitionNextActionMs) < 0) return;

    switch (_roleTransitionState) {
        case BT_ROLE_TRANSITION_ASSESS: {
            if (_requestedRole == BT_ROLE_SINK) {
                // Do not touch the source I2S path or module role until the
                // player has really stopped and its command queue is settled.
                if (player.isRunning() || player.status() == PLAYING ||
                    player.wantsPlayback() || !player.readyForWebStation()) {
                    return;
                }
                // The previous decoder has now really stopped (including SD
                // resume-position bookkeeping). Announce the new playback
                // mode before the slower module reboot/probe sequence, so the
                // display does not misleadingly remain on WEB/SD/DLNA.
                if (config.getMode() != PM_BLUETOOTH) {
                    config.changeMode(PM_BLUETOOTH);
                    if (config.getMode() != PM_BLUETOOTH) {
                        failRoleTransition("Bluetooth player screen activation failed");
                        return;
                    }
                }
                stopSourceRadio();
                stopSourceTone();
                stopBridge();
            }
            if (!_startupProbeComplete) return;

            // The tested BT1058 firmware can lose its persistent pairing
            // database when MODCFG changes while sink profiles are still
            // connected.  Disconnect every profile first and wait for the
            // unsolicited states to confirm a clean shutdown.  DSCA only
            // releases live links; unlike PLIST it cannot delete link keys.
            if (_requestedRole == BT_ROLE_SOURCE &&
                _moduleRole == BT_ROLE_SINK &&
                (_lastA2dpStatus > 1 || _lastAvrcpStatus > 1 ||
                 _lastHfpStatus > 1)) {
                log_i("##[BT-ROLE]# disconnecting active sink profiles before MODCFG: A2DP=%u AVRCP=%u HFP=%u",
                      (unsigned)_lastA2dpStatus,
                      (unsigned)_lastAvrcpStatus,
                      (unsigned)_lastHfpStatus);
                sendAT("AT+DSCA");
                _roleDisconnectDeadlineMs =
                    now + BT_ROLE_DISCONNECT_TIMEOUT_MS;
                _roleTransitionState = BT_ROLE_TRANSITION_WAIT_DISCONNECT;
                _roleTransitionNextActionMs =
                    now + BT_ROLE_DISCONNECT_SETTLE_MS;
                return;
            }

            if (_moduleRole != _requestedRole) {
                if (_roleChangeAttempts >= BT_ROLE_SWITCH_MAX_ATTEMPTS) {
                    failRoleTransition("MODCFG verification failed");
                    return;
                }
                ++_roleChangeAttempts;
                char command[20];
                snprintf(command, sizeof(command), "AT+MODCFG=%u",
                         _requestedRole == BT_ROLE_SOURCE ? 1U : 0U);
                log_i("##[BT-ROLE]# changing MODCFG (attempt %u/%u)",
                      (unsigned)_roleChangeAttempts,
                      (unsigned)BT_ROLE_SWITCH_MAX_ATTEMPTS);
                sendAT(command); // MODCFG reboots the module automatically.
                invalidateStartupProbe(BT_ROLE_SWITCH_PROBE_DELAY_MS);
                _roleTransitionState = BT_ROLE_TRANSITION_WAIT_REBOOT;
                _roleTransitionNextActionMs = now + BT_ROLE_SWITCH_PROBE_DELAY_MS;
                return;
            }

            if (roleConfigurationMatches(_requestedRole)) {
                const bool aliasNeedsRestore = config.store.btAlias[0] &&
                                               _moduleName[0] &&
                                               strcmp(config.store.btAlias,
                                                      _moduleName) != 0;
                if (aliasNeedsRestore &&
                    _aliasSyncState != ALIAS_SYNC_FAILED) {
                    // MODCFG resets NAME on the tested firmware. Keep the
                    // transition pending until the separate alias transaction
                    // has completed and verified its read-back.
                    return;
                }
                if (aliasNeedsRestore) {
                    log_w("##[BT-ROLE]# completing role switch with unsynchronized alias");
                }
                completeRoleTransition();
                return;
            }
            if (_roleConfigAttempts >= BT_ROLE_SWITCH_MAX_ATTEMPTS) {
                failRoleTransition("persistent audio configuration verification failed");
                return;
            }
            ++_roleConfigAttempts;
            _roleConfigChanged = false;
            _roleTransitionState = BT_ROLE_TRANSITION_APPLY_AUX;
            _roleTransitionNextActionMs = now;
            return;
        }

        case BT_ROLE_TRANSITION_WAIT_DISCONNECT: {
            const bool profilesDisconnected =
                _lastA2dpStatus <= 1 && _lastAvrcpStatus <= 1 &&
                _lastHfpStatus <= 1;
            if (profilesDisconnected) {
                log_i("##[BT-ROLE]# sink profiles disconnected; MODCFG may proceed safely");
                _roleDisconnectDeadlineMs = 0;
                _roleTransitionState = BT_ROLE_TRANSITION_ASSESS;
                _roleTransitionNextActionMs =
                    now + BT_ROLE_DISCONNECT_SETTLE_MS;
                return;
            }
            if (static_cast<int32_t>(now - _roleDisconnectDeadlineMs) >= 0) {
                failRoleTransition(
                    "active Bluetooth profiles did not disconnect; refusing unsafe MODCFG");
            }
            return;
        }

        case BT_ROLE_TRANSITION_WAIT_REBOOT:
            if (_startupProbeComplete) {
                _roleTransitionState = BT_ROLE_TRANSITION_ASSESS;
                _roleTransitionNextActionMs = now;
            }
            return;

        case BT_ROLE_TRANSITION_APPLY_AUX:
            if (_requestedRole == BT_ROLE_SOURCE && _moduleAuxCfg != 3) {
                log_i("##[BT-ROLE]# setting source audio input: AUXCFG=3 (I2S)");
                sendAT("AT+AUXCFG=3");
                _roleConfigChanged = true;
            }
            if (_requestedRole == BT_ROLE_SINK && _moduleAvrcpCfg >= 0 &&
                (_moduleAvrcpCfg & 1) == 0) {
                // Preserve progress/volume-sync bits and only enable the
                // documented automatic Title/Artist/Album report bit.
                char command[24];
                const uint16_t value = static_cast<uint16_t>(_moduleAvrcpCfg) | 1U;
                snprintf(command, sizeof(command), "AT+AVRCPCFG=%u",
                         (unsigned)value);
                log_i("##[BT-ROLE]# enabling sink metadata reports: AVRCPCFG=%u",
                      (unsigned)value);
                sendAT(command);
                _roleConfigChanged = true;
            }
            _roleTransitionState = BT_ROLE_TRANSITION_APPLY_I2S;
            _roleTransitionNextActionMs = now + BT_ROLE_SWITCH_COMMAND_SETTLE_MS;
            return;

        case BT_ROLE_TRANSITION_APPLY_I2S: {
            const int16_t expected = _requestedRole == BT_ROLE_SOURCE
                ? BT1058_EXPECTED_SOURCE_I2SCFG : BT1058_EXPECTED_I2SCFG;
            if (_moduleI2sCfg != expected) {
                char command[20];
                snprintf(command, sizeof(command), "AT+I2SCFG=%d", (int)expected);
                log_i("##[BT-ROLE]# setting %s I2S configuration: I2SCFG=%d",
                      moduleRoleName(_requestedRole), (int)expected);
                sendAT(command);
                _roleConfigChanged = true;
            }
            if (_roleConfigChanged) {
                _roleTransitionState = BT_ROLE_TRANSITION_REBOOT_CONFIG;
                _roleTransitionNextActionMs = now + BT_ROLE_SWITCH_COMMAND_SETTLE_MS;
            } else {
                _roleTransitionState = BT_ROLE_TRANSITION_ASSESS;
                _roleTransitionNextActionMs = now;
            }
            return;
        }

        case BT_ROLE_TRANSITION_REBOOT_CONFIG:
            log_i("##[BT-ROLE]# rebooting module to apply persistent audio configuration");
            sendAT("AT+REBOOT");
            invalidateStartupProbe(BT_ROLE_SWITCH_PROBE_DELAY_MS);
            _roleTransitionState = BT_ROLE_TRANSITION_WAIT_REBOOT;
            _roleTransitionNextActionMs = now + BT_ROLE_SWITCH_PROBE_DELAY_MS;
            return;

        default:
            return;
    }
}

void BluetoothPlayer::serviceStartupProbe() {
    static const char* const queries[] = {
        "AT",
        "AT+VER",
        "AT+MODCFG",
        "AT+NAME",
        "AT+BTEN",
        "AT+I2SCFG",
        "AT+AUXCFG",
        "AT+PROFILE",
        "AT+AVRCPCFG",
        "AT+STAT",
    };

    if (_startupProbeComplete || _consoleCommandPending) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _nextStartupProbeMs) < 0) return;

    if (_startupProbeStep >= sizeof(queries) / sizeof(queries[0])) {
        if (_moduleRole != BT_ROLE_UNKNOWN &&
            _moduleI2sCfg >= 0 && _moduleAuxCfg >= 0) {
            _startupProbeComplete = true;
            log_i("##[BT]# staged startup probe complete: role=%s AUXCFG=%d I2SCFG=%d",
                  _moduleRole == BT_ROLE_SOURCE ? "source" : "sink",
                  (int)_moduleAuxCfg, (int)_moduleI2sCfg);
        } else {
            _startupProbeStep = 0;
            _nextStartupProbeMs = now + 1500U;
            log_w("##[BT]# module startup state incomplete; scheduling read-only retry");
        }
        return;
    }

    sendAT(queries[_startupProbeStep++]);
    _nextStartupProbeMs = now + 350U;
}

void BluetoothPlayer::serviceSourceRadioStats() {
#if CORE_DEBUG_LEVEL >= 3
    if (!sourceRadioRunning()) return;
    const uint32_t now = millis();
    if (now - _sourceRadioLastStatsMs < 5000U) return;
    _sourceRadioLastStatsMs = now;

    const uint32_t blocks =
        __atomic_exchange_n(&_sourceRadioBlocks, 0U, __ATOMIC_RELAXED);
    TaskHandle_t task = _sourceRadioTaskHandle;
    const UBaseType_t stackHwm = task ? uxTaskGetStackHighWaterMark(task) : 0;
    log_i("##[BT-TX]# radio alive: blocks=%u buffered=%u/%u primed=%u ever=%u in=%u out=%u drop=%u rebuffer=%u underrun=%u txerr=%u hwm=%u elastic=%ldppm source=%uHz/%ukbps netbuf=%u peakL=%u peakR=%u nonzero=%u stack_hwm=%u",
          (unsigned)blocks,
          (unsigned)sourcePcmBufferedFrames(),
          (unsigned)BT_SOURCE_PCM_RING_FRAMES,
          (unsigned)__atomic_load_n(&_sourceRadioPrimed, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourceRadioEverPrimed, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmFramesIn, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmFramesOut, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmDroppedFrames, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmRebufferEvents, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmUnderrunBlocks, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmWriteErrors, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmHighWaterFrames, __ATOMIC_RELAXED),
          (long)__atomic_load_n(&_sourcePcmRateAdjustPpm, __ATOMIC_RELAXED),
          (unsigned)player.getSampleRate(),
          (unsigned)(player.getBitRate() / 1000U),
          (unsigned)player.inBufferFilled(),
          (unsigned)__atomic_load_n(&_sourcePcmPeakLeft, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmPeakRight, __ATOMIC_RELAXED),
          (unsigned)__atomic_load_n(&_sourcePcmNonZeroSamples, __ATOMIC_RELAXED),
          (unsigned)stackHwm);

    static uint32_t lastMemoryStatsMs = 0;
    if (lastMemoryStatsMs == 0 || now - lastMemoryStatsMs >= 30000U) {
        lastMemoryStatsMs = now;
        log_i("##[BT-TX]# memory: internal=%u largest=%u DMA=%u DMA_largest=%u PSRAM=%u largest=%u",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
#endif
}

void BluetoothPlayer::serviceSourceUiPlaybackState() {
    if (_moduleRole != BT_ROLE_SOURCE) {
        _sourceUiPlayingKnown = false;
        return;
    }

    const bool playing = player.isRunning();
    if (_sourceUiPlayingKnown && playing == _sourceUiPlaying) return;

    _sourceUiPlayingKnown = true;
    _sourceUiPlaying = playing;
    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
}

void BluetoothPlayer::serviceSinkStreamInfo() {
    if (_moduleRole != BT_ROLE_SINK ||
        (_lastA2dpStatus != 4 && _lastA2dpStatus != 5) ||
        _streamInfoRequested) {
        return;
    }
    if (!_startupProbeComplete || _consoleCommandPending || aliasSyncBusy() ||
        inputUiBusy() || sinkControlBusy() || roleTransitionBusy() ||
        _sourceAudioEnablePending || _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceScanActive || _inputRefreshPending) {
        return;
    }
    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - _lastAtTxMs) < BT_AT_COMMAND_GUARD_MS) {
        return;
    }

    // A2DPSTAT=4/5 is an authoritative active stream even when a minimal SBC
    // sender never emits PLAYSTAT. Querying here prevents SBC from remaining
    // "unknown" solely because AVRCP playback status was absent.
    _streamInfoRequested = true;
    sendAT("AT+A2DPDEC");
}

void BluetoothPlayer::serviceSourceRadioAutostart() {
#if BT_SOURCE_RADIO_AUTOSTART
    if (sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        if (sourceRadioRunning()) stopSourceRadio();
        return;
    }
    if (_sourceRadioManualStop || sourceRadioRunning() || sourceToneRunning() ||
        bridgeRunningOrPending() || !_startupProbeComplete || roleTransitionBusy()) {
        return;
    }
    if (_moduleRole != BT_ROLE_SOURCE || _moduleAuxCfg != 3 ||
        _moduleI2sCfg != 3 || config.getMode() == PM_BLUETOOTH ||
        !player.isRunning()) {
        return;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _nextSourceRadioAutostartMs) < 0) return;
    _nextSourceRadioAutostartMs =
        now + BT_SOURCE_RADIO_AUTOSTART_RETRY_MS;
    if (startSourceRadio()) {
        log_i("##[BT-TX]# radio relay restored automatically after boot/reconnect");
    } else {
        log_w("##[BT-TX]# automatic radio relay start failed; retry scheduled");
    }
#endif
}

void BluetoothPlayer::serviceSourceAudioEnable() {
    const uint32_t now = millis();
    if (_sourceAudioEnablePending) {
        if (static_cast<int32_t>(now - _sourceAudioEnableDeadlineMs) < 0) {
            return;
        }
        _sourceAudioEnablePending = false;
        _sourceAudioEnableDeadlineMs = 0;
        _nextSourceAudioEnableMs = now + BT_SOURCE_AUDIO_ENABLE_RETRY_MS;
        log_w("##[BT-TX]# A2DP source audio enable response timeout; retry scheduled");
    }

    if (!sourceRadioRunning() || _sourceAudioPathReady ||
        !_startupProbeComplete || _consoleCommandPending ||
        aliasSyncBusy() || inputUiBusy() || _sourceScanActive ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE ||
        roleTransitionBusy() || _moduleRole != BT_ROLE_SOURCE) {
        return;
    }

    // A2DP states 3..5 have all been observed on connected source endpoints.
    // Before that, A2DPAUDIO=1 is accepted too early or returns ERROR and its
    // effect is lost during the subsequent profile transition.
    if (_lastA2dpStatus < 3 || _lastA2dpStatus > 5) return;

    if (static_cast<int32_t>(now - _nextSourceAudioEnableMs) < 0) return;
    if (static_cast<uint32_t>(now - _lastAtTxMs) < BT_AT_COMMAND_GUARD_MS) {
        return;
    }
    _nextSourceAudioEnableMs = now + BT_SOURCE_AUDIO_ENABLE_RETRY_MS;
    ++_sourceAudioEnableAttempts;
    log_i("##[BT-TX]# arming A2DP source audio path (attempt %u, state=%u)",
          (unsigned)_sourceAudioEnableAttempts,
          (unsigned)_lastA2dpStatus);
    sendAT("AT+A2DPAUDIO=1");
    _sourceAudioEnablePending = true;
    _sourceAudioEnableDeadlineMs =
        now + BT_SOURCE_AUDIO_ENABLE_RESPONSE_TIMEOUT_MS;
}

void BluetoothPlayer::handleRemoteControlEvent(btRemoteCommand_e command,
                                                const char* wireEvent,
                                                const char* action) {
    log_i("##[BT-REMOTE]# speaker button: %s (%s), role=%s A2DP=%u AVRCP=%u",
          action, wireEvent, moduleRoleName(_moduleRole),
          (unsigned)_lastA2dpStatus, (unsigned)_lastAvrcpStatus);

    // CT events are meaningful for the Edge player only while the BT1058 is
    // the A2DP source / AVRCP target.  Never let a delayed UART event control
    // the radio after a direction change to Bluetooth input.
    if (_moduleRole != BT_ROLE_SOURCE || config.getMode() == PM_BLUETOOTH) {
        log_w("##[BT-REMOTE]# command ignored outside Bluetooth output mode: %s",
              action);
        return;
    }

    const bool playbackRequested =
        player.status() == PLAYING || player.wantsPlayback();
    bool queued = true;

    switch (command) {
        case BT_REMOTE_PLAY:
            if (!playbackRequested) {
                queued = player.sendCommandNoWait(
                    {PR_PLAY, (int)config.lastStation()});
            }
            break;

        case BT_REMOTE_PAUSE:
        case BT_REMOTE_STOP:
            if (playbackRequested) {
                queued = player.sendCommandNoWait({PR_STOP, 0});
            }
            break;

        case BT_REMOTE_NEXT:
            player.next();
            break;

        case BT_REMOTE_PREVIOUS:
            player.prev();
            break;

        case BT_REMOTE_VOLUME_UP:
            player.stepVol(true);
            break;

        case BT_REMOTE_VOLUME_DOWN:
            player.stepVol(false);
            break;
    }

    if (!queued) {
        log_w("##[BT-REMOTE]# player queue full; command dropped: %s", action);
        return;
    }
    log_i("##[BT-REMOTE]# dispatched to Edge player: %s%s",
          action,
          ((command == BT_REMOTE_PLAY && playbackRequested) ||
           ((command == BT_REMOTE_PAUSE || command == BT_REMOTE_STOP) &&
            !playbackRequested))
              ? " (already in requested state)" : "");
}

const char* BluetoothPlayer::sinkControlName(btSinkControl_e command) {
    switch (command) {
        case BT_SINK_CONTROL_PLAY: return "PLAY";
        case BT_SINK_CONTROL_PAUSE: return "PAUSE";
        case BT_SINK_CONTROL_TOGGLE: return "PLAY/PAUSE";
        case BT_SINK_CONTROL_NEXT: return "NEXT";
        case BT_SINK_CONTROL_PREVIOUS: return "PREVIOUS";
        case BT_SINK_CONTROL_SET_VOLUME: return "VOLUME";
        default: return "UNKNOWN";
    }
}

const char* BluetoothPlayer::sinkControlAtCommand(btSinkControl_e command) {
    switch (command) {
        case BT_SINK_CONTROL_PLAY: return "AT+PLAY";
        case BT_SINK_CONTROL_PAUSE: return "AT+PAUSE";
        case BT_SINK_CONTROL_TOGGLE: return "AT+PLAYPAUSE";
        case BT_SINK_CONTROL_NEXT: return "AT+FORWARD";
        case BT_SINK_CONTROL_PREVIOUS: return "AT+BACKWARD";
        default: return nullptr;
    }
}

bool BluetoothPlayer::sinkControlBusy() const {
    return _sinkControlInFlight != BT_SINK_CONTROL_NONE ||
           (_sinkControlQueue && uxQueueMessagesWaiting(_sinkControlQueue) > 0) ||
           __atomic_load_n(&_sinkVolumePending, __ATOMIC_ACQUIRE);
}

void BluetoothPlayer::queueSinkControl(btSinkControl_e command) {
    if (command == BT_SINK_CONTROL_NONE || !_sinkControlQueue) {
        log_e("##[BT-CTRL]# %s not queued: control queue unavailable",
              sinkControlName(command));
        return;
    }
    if (_moduleRole != BT_ROLE_SINK || config.getMode() != PM_BLUETOOTH ||
        !_startupProbeComplete) {
        log_w("##[BT-CTRL]# %s ignored outside ready Bluetooth input mode",
              sinkControlName(command));
        return;
    }
    if (_lastAvrcpStatus != 3 || _lastA2dpStatus < 3 || _lastA2dpStatus > 5) {
        log_w("##[BT-CTRL]# %s ignored: no connected AVRCP input device (A2DP=%u AVRCP=%u)",
              sinkControlName(command), (unsigned)_lastA2dpStatus,
              (unsigned)_lastAvrcpStatus);
        return;
    }
    if (xQueueSend(_sinkControlQueue, &command, 0) != pdTRUE) {
        log_w("##[BT-CTRL]# %s dropped: sink control queue is full",
              sinkControlName(command));
        return;
    }
    log_i("##[BT-CTRL]# queued: %s", sinkControlName(command));
}

void BluetoothPlayer::serviceSinkControl() {
    const uint32_t now = millis();
    if (_sinkControlInFlight != BT_SINK_CONTROL_NONE) {
        if (static_cast<int32_t>(now - _sinkControlDeadlineMs) < 0) return;
        log_w("##[BT-CTRL]# %s response timeout",
              sinkControlName(_sinkControlInFlight));
        _sinkControlInFlight = BT_SINK_CONTROL_NONE;
        _sinkControlDeadlineMs = 0;
    }
    const bool queuedTransport =
        _sinkControlQueue && uxQueueMessagesWaiting(_sinkControlQueue) > 0;
    const bool volumePending =
        __atomic_load_n(&_sinkVolumePending, __ATOMIC_ACQUIRE);
    if (!queuedTransport && !volumePending) {
        return;
    }

    if (_moduleRole != BT_ROLE_SINK || config.getMode() != PM_BLUETOOTH ||
        _lastAvrcpStatus != 3 || _lastA2dpStatus < 3 ||
        _lastA2dpStatus > 5) {
        xQueueReset(_sinkControlQueue);
        __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
        log_w("##[BT-CTRL]# queued controls discarded: Bluetooth input link is no longer active");
        return;
    }
    if (!_startupProbeComplete || _consoleCommandPending ||
        roleTransitionBusy() || aliasSyncBusy() || inputUiBusy() ||
        _sourceAudioEnablePending || _sourceScanActive ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        static_cast<uint32_t>(now - _lastAtTxMs) < BT_AT_COMMAND_GUARD_MS) {
        return;
    }

    btSinkControl_e command = BT_SINK_CONTROL_NONE;
    char volumeCommand[24] = {};
    const char* atCommand = nullptr;
    if (queuedTransport) {
        if (xQueueReceive(_sinkControlQueue, &command, 0) != pdTRUE) return;
        atCommand = sinkControlAtCommand(command);
    } else {
        command = BT_SINK_CONTROL_SET_VOLUME;
        const uint8_t step =
            __atomic_load_n(&_sinkVolumeStep, __ATOMIC_ACQUIRE);
        __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
        const uint8_t absoluteVolume = static_cast<uint8_t>(
            (static_cast<uint16_t>(step) * 127U + 10U) / 21U);
        snprintf(volumeCommand, sizeof(volumeCommand), "AT+ABSVOL=%u",
                 absoluteVolume);
        atCommand = volumeCommand;
    }
    if (!atCommand) return;

    _sinkControlInFlight = command;
    _sinkControlDeadlineMs = now + BT_SINK_CONTROL_RESPONSE_TIMEOUT_MS;
    sendAT(atCommand);
    log_i("##[BT-CTRL]# sent to phone/PC: %s (%s)",
          sinkControlName(command), atCommand);
}

void BluetoothPlayer::play() { queueSinkControl(BT_SINK_CONTROL_PLAY); }
void BluetoothPlayer::pause() { queueSinkControl(BT_SINK_CONTROL_PAUSE); }
void BluetoothPlayer::toggle() { queueSinkControl(BT_SINK_CONTROL_TOGGLE); }
void BluetoothPlayer::next() { queueSinkControl(BT_SINK_CONTROL_NEXT); }
void BluetoothPlayer::prev() { queueSinkControl(BT_SINK_CONTROL_PREVIOUS); }
void BluetoothPlayer::volUp() { sendAT("AT+ABSVOL=+"); }
void BluetoothPlayer::volDown() { sendAT("AT+ABSVOL=-"); }

void BluetoothPlayer::setVol(uint8_t step0to21) {
    if (step0to21 > 21) step0to21 = 21;
    // This is the Edge's local DAC volume, not the remote phone/PC absolute
    // volume. The bridge consumes the newest value without any UART traffic,
    // so encoder and WebUI changes also work on sources that ignore ABSVOL.
    __atomic_store_n(&_sinkVolumeStep, step0to21, __ATOMIC_RELEASE);
    __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
}

bool BluetoothPlayer::aliasSyncBusy() const {
    return _aliasSyncState == ALIAS_SYNC_WAIT_SET_RESPONSE ||
           _aliasSyncState == ALIAS_SYNC_WAIT_VERIFY_DELAY ||
           _aliasSyncState == ALIAS_SYNC_WAIT_VERIFY_RESPONSE;
}

void BluetoothPlayer::publishInputUiSnapshot(uint8_t clientId,
                                              const char* message) const {
    const char* desired = config.store.btAlias[0]
                              ? config.store.btAlias
                              : _moduleName;
    const bool synchronized = desired[0] && _moduleName[0] &&
                              strcmp(desired, _moduleName) == 0;
    const bool pending = aliasSyncBusy() || !_startupProbeComplete ||
                         (desired[0] && !synchronized);
    netserver.btInputStateToWs(
        clientId, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), desired, _moduleName,
        pending, synchronized, inputUiBusy() || _inputRefreshPending,
        _moduleDiscoverable, _inputConnectedAddress, _inputConnectedName,
        _inputCodec, _inputCodecSampleRate, message);
}

bool BluetoothPlayer::setInputAlias(const char* alias, uint8_t clientId) {
    String normalized(alias ? alias : "");
    normalized.trim();

    bool valid = normalized.length() > 0 &&
                 normalized.length() < BT_ALIAS_LENGTH;
    for (size_t i = 0; valid && i < normalized.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(normalized[i]);
        if (c < 0x20 || c == ',') valid = false;
    }
    if (!valid) {
        publishInputUiSnapshot(
            clientId,
            "Use 1-31 bytes; commas and control characters are not allowed.");
        return false;
    }

    config.saveValue(config.store.btAlias, normalized.c_str(),
                     sizeof(config.store.btAlias));
    if (!aliasSyncBusy()) {
        _aliasSyncState = ALIAS_SYNC_IDLE;
        _aliasSyncAttempts = 0;
        _aliasSyncDeadlineMs = 0;
        _aliasSyncNextActionMs = millis();
    }

    const bool alreadyActive = _moduleName[0] &&
                               strcmp(config.store.btAlias, _moduleName) == 0;
    log_i("##[BT-NAME]# WebUI alias saved: '%s'%s",
          config.store.btAlias, alreadyActive ? " (already active)" : "");
    publishInputUiSnapshot(clientId,
                           alreadyActive ? "Alias is already active."
                                         : "Alias saved; applying to the module...");
    return true;
}

void BluetoothPlayer::retryOrFailAliasSync(const char* reason) {
    _aliasSyncDeadlineMs = 0;
    if (_aliasSyncAttempts < BT_ALIAS_MAX_ATTEMPTS) {
        _aliasSyncState = ALIAS_SYNC_IDLE;
        _aliasSyncNextActionMs = millis() + BT_ALIAS_RETRY_DELAY_MS;
        log_w("##[BT-NAME]# alias synchronization retry scheduled: %s",
              reason ? reason : "verification failed");
        publishInputUiSnapshot(0, "Alias verification failed; retrying...");
        return;
    }

    _aliasSyncState = ALIAS_SYNC_FAILED;
    log_e("##[BT-NAME]# alias synchronization failed after %u attempts: %s",
          (unsigned)_aliasSyncAttempts,
          reason ? reason : "verification failed");
    publishInputUiSnapshot(0,
                           "Alias could not be applied; audio remains available.");
}

void BluetoothPlayer::serviceAliasSync() {
    const uint32_t now = millis();

    if ((_aliasSyncState == ALIAS_SYNC_WAIT_SET_RESPONSE ||
         _aliasSyncState == ALIAS_SYNC_WAIT_VERIFY_RESPONSE) &&
        static_cast<int32_t>(now - _aliasSyncDeadlineMs) >= 0) {
        retryOrFailAliasSync("command response timeout");
        return;
    }

    if (_aliasSyncState == ALIAS_SYNC_WAIT_VERIFY_DELAY) {
        if (static_cast<int32_t>(now - _aliasSyncNextActionMs) < 0) return;
        sendAT("AT+NAME");
        _aliasSyncState = ALIAS_SYNC_WAIT_VERIFY_RESPONSE;
        _aliasSyncDeadlineMs = now + BT_ALIAS_COMMAND_TIMEOUT_MS;
        return;
    }

    if (_aliasSyncState != ALIAS_SYNC_IDLE) return;
    if (!_startupProbeComplete || !config.store.btAlias[0] ||
        !_moduleName[0] ||
        strcmp(config.store.btAlias, _moduleName) == 0) {
        return;
    }
    const bool roleReadyForAlias =
        _roleTransitionState == BT_ROLE_TRANSITION_ASSESS &&
        roleConfigurationMatches(_requestedRole);
    if (_consoleCommandPending || _sourceAudioEnablePending || inputUiBusy() ||
        sinkControlBusy() ||
        _sourceScanActive || _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE ||
        (roleTransitionBusy() && !roleReadyForAlias)) {
        return;
    }
    if (static_cast<int32_t>(now - _aliasSyncNextActionMs) < 0 ||
        static_cast<uint32_t>(now - _lastAtTxMs) < BT_AT_COMMAND_GUARD_MS) {
        return;
    }

    char command[BT_ALIAS_LENGTH + 16];
    snprintf(command, sizeof(command), "AT+NAME=%s,0", config.store.btAlias);
    ++_aliasSyncAttempts;
    log_i("##[BT-NAME]# restoring alias '%s' (attempt %u/%u)",
          config.store.btAlias, (unsigned)_aliasSyncAttempts,
          (unsigned)BT_ALIAS_MAX_ATTEMPTS);
    sendAT(command);
    _aliasSyncState = ALIAS_SYNC_WAIT_SET_RESPONSE;
    _aliasSyncDeadlineMs = now + BT_ALIAS_COMMAND_TIMEOUT_MS;
    publishInputUiSnapshot(0, "Applying Bluetooth alias...");
}

bool BluetoothPlayer::inputUiBusy() const {
    return _inputUiCommand != INPUT_UI_COMMAND_NONE;
}

void BluetoothPlayer::publishInputPairedList(uint8_t clientId) const {
    netserver.btInputPairedStateToWs(clientId, "clear", 0, nullptr);
    for (uint8_t i = 0; i < _inputPairedDeviceCount; ++i) {
        const btPairedDevice_t& device = _inputPairedDevices[i];
        netserver.btInputPairedDeviceToWs(
            clientId, device.index, device.profile, device.address, device.name);
    }
    if (_remoteRssiAddress[0] && _remoteRssi < 0) {
        for (uint8_t i = 0; i < _inputPairedDeviceCount; ++i) {
            const btPairedDevice_t& device = _inputPairedDevices[i];
            if (strcasecmp(device.address, _remoteRssiAddress) == 0) {
                netserver.btSourceScanDeviceToWs(
                    clientId, device.address, device.name, _remoteRssi);
                break;
            }
        }
    }
    netserver.btInputPairedStateToWs(
        clientId, _inputPairedDeviceCount ? "done" : "idle",
        _inputPairedDeviceCount, nullptr);
}

void BluetoothPlayer::requestInputUiRefresh(uint8_t clientId) {
    _inputUiClientId = clientId;
    _inputRefreshPending = true;
    publishInputUiSnapshot(clientId);
    publishInputPairedList(clientId);
    if (_moduleRole == BT_ROLE_SOURCE) {
        // The paired list, active source connection and RSSI state are
        // separate WebSocket streams. Refresh all three together; the
        // asynchronous PLIST/RSSI pass below repairs a missed autoconnect
        // identity event and replaces any stale scan-cache value.
        publishSourceUiSnapshot(clientId);
    }
}

uint8_t BluetoothPlayer::displayPairedDeviceCount() const {
    return min(_inputPairedDeviceCount,
               static_cast<uint8_t>(BT_INPUT_PAIRED_MAX_DEVICES));
}

bool BluetoothPlayer::displayPairedDeviceAt(uint8_t index,
                                             btDisplayDevice_t& out) const {
    memset(&out, 0, sizeof(out));
    out.rssi = -127;
    const uint8_t count = displayPairedDeviceCount();
    if (index >= count) { return false; }

    const btPairedDevice_t& source = _inputPairedDevices[index];
    out.profile = source.profile;
    strlcpy(out.address, source.address, sizeof(out.address));
    strlcpy(out.name, source.name[0] ? source.name : "Unknown device",
            sizeof(out.name));

    const bool linkActive = _lastA2dpStatus >= 3 && _lastA2dpStatus <= 5;
    const char* connectedAddress = _moduleRole == BT_ROLE_SOURCE
                                       ? _sourceConnectedAddress
                                       : _moduleRole == BT_ROLE_SINK
                                             ? _inputConnectedAddress
                                             : nullptr;
    out.connected = linkActive && connectedAddress && connectedAddress[0] &&
                    strcasecmp(out.address, connectedAddress) == 0;
    if (_remoteRssiAddress[0] &&
        strcasecmp(out.address, _remoteRssiAddress) == 0) {
        out.rssi = _remoteRssi;
    }
    return true;
}

bool BluetoothPlayer::displayControlsBusy() const {
    return !_startupProbeComplete || roleTransitionBusy() || aliasSyncBusy() ||
           inputUiBusy() || sinkControlBusy() || _consoleCommandPending ||
           _sourceAudioEnablePending || _sourceScanActive ||
           _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
           _sourceSwitchState != SOURCE_SWITCH_IDLE ||
           sourceOutputRoutePending();
}

uint32_t BluetoothPlayer::displayFingerprint() const {
    uint32_t hash = 2166136261UL;
    auto mixByte = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 16777619UL;
    };
    auto mixText = [&mixByte](const char* value) {
        if (!value) { mixByte(0); return; }
        while (*value) { mixByte(static_cast<uint8_t>(*value++)); }
        mixByte(0);
    };

    mixByte(static_cast<uint8_t>(_moduleRole));
    mixByte(static_cast<uint8_t>(_lastA2dpStatus));
    mixByte(sourceOutputMode());
    mixByte(displayControlsBusy() ? 1U : 0U);
    mixByte(displayPairedDeviceCount());
    const uint16_t rssiBits = static_cast<uint16_t>(_remoteRssi);
    mixByte(static_cast<uint8_t>(rssiBits & 0xFFU));
    mixByte(static_cast<uint8_t>((rssiBits >> 8) & 0xFFU));
    mixText(_sourceConnectedAddress);
    mixText(_inputConnectedAddress);
    mixText(_remoteRssiAddress);
    for (uint8_t i = 0; i < displayPairedDeviceCount(); ++i) {
        const btPairedDevice_t& device = _inputPairedDevices[i];
        mixByte(static_cast<uint8_t>(device.profile & 0xFF));
        mixByte(static_cast<uint8_t>((device.profile >> 8) & 0xFF));
        mixText(device.address);
        mixText(device.name);
    }
    return hash;
}

void BluetoothPlayer::requestDisplayUiRefresh() {
    // A TFT only needs a current copy of the shared paired-device cache.  Do
    // not replace the WebUI client id while an input command is already in
    // flight; that command will update the same cache when it completes.
    if (inputUiBusy()) { return; }
    _inputUiClientId = 0;
    _inputRefreshPending = true;
}

bool BluetoothPlayer::setInputDiscoverable(bool enabled, uint8_t clientId) {
    if (_moduleRole != BT_ROLE_SINK || !_startupProbeComplete) {
        publishInputUiSnapshot(
            clientId,
            "Discoverability can be changed only in Bluetooth input mode.");
        return false;
    }
    if (inputUiBusy() || aliasSyncBusy() || sinkControlBusy() || roleTransitionBusy() ||
        _consoleCommandPending || _sourceAudioEnablePending ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE) {
        publishInputUiSnapshot(clientId,
                               "Bluetooth module is busy; try again shortly.");
        return false;
    }

    char command[16];
    snprintf(command, sizeof(command), "AT+PAIR=%u", enabled ? 1U : 0U);
    _requestedDiscoverable = enabled;
    _inputUiClientId = clientId;
    _inputUiCommand = INPUT_UI_SET_DISCOVERABLE;
    _inputUiCommandDeadlineMs = millis() + BT_INPUT_COMMAND_TIMEOUT_MS;
    sendAT(command);
    publishInputUiSnapshot(clientId,
                           enabled ? "Enabling discoverability..."
                                   : "Disabling discoverability...");
    return true;
}

bool BluetoothPlayer::forgetInputPairedDevice(const char* address,
                                               uint8_t clientId) {
    if (!validBluetoothAddress(address)) {
        netserver.btInputPairedStateToWs(clientId, "error",
                                         _inputPairedDeviceCount,
                                         "Invalid Bluetooth address.");
        return false;
    }
    if ((_moduleRole != BT_ROLE_SINK && _moduleRole != BT_ROLE_SOURCE) ||
        !_startupProbeComplete) {
        netserver.btInputPairedStateToWs(
            clientId, "error", _inputPairedDeviceCount,
            "Paired devices are not available until Bluetooth is ready.");
        return false;
    }
    if (inputUiBusy() || aliasSyncBusy() || sinkControlBusy() || roleTransitionBusy() ||
        _consoleCommandPending || _sourceAudioEnablePending ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE) {
        netserver.btInputPairedStateToWs(clientId, "error",
                                         _inputPairedDeviceCount,
                                         "Bluetooth module is busy.");
        return false;
    }

    const char* connectedAddress = _moduleRole == BT_ROLE_SOURCE
                                       ? _sourceConnectedAddress
                                       : _inputConnectedAddress;
    if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5 &&
        (!connectedAddress[0] ||
         strcasecmp(address, connectedAddress) == 0)) {
        netserver.btInputPairedStateToWs(
            clientId, "error", _inputPairedDeviceCount,
            connectedAddress[0]
                ? "Disconnect this Bluetooth device before forgetting it."
                : "Disconnect the active Bluetooth device before forgetting paired devices.");
        return false;
    }

    // A2DP=2 is an automatic or explicit connection attempt. The BT1058 can
    // reject PLIST deletion during this window with its own "wait for the
    // connecting device" error. Stop the attempt first, then chain the exact
    // requested deletion without changing the persistent AUTOCONN setting.
    if (_lastA2dpStatus == 2) {
        strlcpy(_inputForgetAddress, address,
                sizeof(_inputForgetAddress));
        _inputUiClientId = clientId;
        _inputUiCommand = INPUT_UI_CANCEL_RECONNECT_FOR_FORGET;
        _inputUiCommandDeadlineMs = millis() + BT_INPUT_COMMAND_TIMEOUT_MS;
        sendAT("AT+A2DPDISC");
        netserver.btInputPairedStateToWs(
            clientId, "forgetting", _inputPairedDeviceCount,
            "Stopping automatic reconnect before removing the device...");
        publishInputUiSnapshot(
            clientId,
            "Stopping automatic reconnect before removing the paired device...");
        return true;
    }

    beginForgetInputPairedDevice(address, clientId);
    return true;
}

void BluetoothPlayer::beginForgetInputPairedDevice(const char* address,
                                                    uint8_t clientId) {
    char command[32];
    snprintf(command, sizeof(command), "AT+PLIST=%s", address);
    strlcpy(_inputForgetAddress, address, sizeof(_inputForgetAddress));
    _inputUiClientId = clientId;
    _inputUiCommand = INPUT_UI_FORGET_DEVICE;
    _inputUiCommandDeadlineMs = millis() + BT_INPUT_COMMAND_TIMEOUT_MS;
    sendAT(command);
    netserver.btInputPairedStateToWs(clientId, "forgetting",
                                     _inputPairedDeviceCount,
                                     "Removing paired device...");
    publishInputUiSnapshot(clientId, "Removing paired device...");
}

void BluetoothPlayer::finishInputUiCommand(bool success,
                                            const char* message) {
    (void)success;
    const uint8_t clientId = _inputUiClientId;
    const bool sourceInitializationCompleted =
        _moduleRole == BT_ROLE_SOURCE &&
        _sourceUiInitializationPending;
    if (sourceInitializationCompleted) {
        _sourceUiInitializationPending = false;
    }
    _inputUiCommand = INPUT_UI_COMMAND_NONE;
    _inputUiCommandDeadlineMs = 0;
    _inputRssiProbeIndex = 0;
    _inputRssiProbeMatched = false;
    _inputRssiProbeAddress[0] = '\0';
    _inputForgetAddress[0] = '\0';
    _inputPairedRefreshDeviceCount = 0;
    _inputPairedRefreshSawEnd = false;
    publishInputUiSnapshot(clientId, message);
    if (sourceInitializationCompleted) {
        // This is the authoritative final edge of a source-role switch: the
        // route has already been verified and the initial paired-device read
        // has now either completed or failed safely with the old cache kept.
        publishSourceUiSnapshot(clientId);
    }
}

void BluetoothPlayer::serviceInputUiControl() {
    const uint32_t now = millis();
    if (inputUiBusy() &&
        static_cast<int32_t>(now - _inputUiCommandDeadlineMs) >= 0) {
        if (_inputUiCommand == INPUT_UI_REFRESH_RSSI) {
            log_w("##[BT]# RSSI query timed out for %s",
                  _inputRssiProbeAddress[0]
                      ? _inputRssiProbeAddress : "unknown device");
            if (beginNextInputRssiProbe()) return;
            _remoteRssi = -127;
            _remoteRssiAddress[0] = '\0';
            finishInputUiCommand(
                true,
                "Paired devices refreshed; live RSSI is unavailable.");
            if (_moduleRole == BT_ROLE_SOURCE) {
                publishSourceUiSnapshot(_inputUiClientId);
            }
        } else if (_inputUiCommand == INPUT_UI_REFRESH_LIST) {
            netserver.btInputPairedStateToWs(
                _inputUiClientId, "error", _inputPairedDeviceCount,
                "Paired-device refresh timed out.");
            finishInputUiCommand(false,
                                 "Paired-device refresh timed out.");
        } else {
            finishInputUiCommand(false, "Bluetooth input command timed out.");
        }
        return;
    }
    if (!_inputRefreshPending || inputUiBusy()) return;
    if ((_moduleRole != BT_ROLE_SINK && _moduleRole != BT_ROLE_SOURCE) ||
        !_startupProbeComplete || aliasSyncBusy() || sinkControlBusy() ||
        roleTransitionBusy() || _consoleCommandPending ||
        _sourceAudioEnablePending || _sourceScanActive ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE) {
        return;
    }
    if (static_cast<uint32_t>(now - _lastAtTxMs) < BT_AT_COMMAND_GUARD_MS) {
        return;
    }

    _inputRefreshPending = false;
    _inputRssiProbeIndex = 0;
    _inputRssiProbeMatched = false;
    _inputRssiProbeAddress[0] = '\0';
    const bool inputRole = _moduleRole == BT_ROLE_SINK;
    _inputUiCommand = inputRole ? INPUT_UI_REFRESH_PAIR
                                : INPUT_UI_REFRESH_LIST;
    _inputUiCommandDeadlineMs = now + BT_INPUT_COMMAND_TIMEOUT_MS;
    if (inputRole) {
        // DEVSTAT bit 1 is the documented BR/EDR discoverability state.
        // Discoverability belongs to input mode, so do not query or present it
        // as current while the module is acting as an A2DP source.
        sendAT("AT+DEVSTAT");
        publishInputUiSnapshot(_inputUiClientId,
                               "Refreshing Bluetooth input settings...");
    } else {
        // PLIST and per-address deletion are role-independent on the tested
        // BT1058 firmware. Keep the shared cache current in source mode;
        // discoverability remains a sink-only setting.
        memset(_inputPairedRefreshDevices, 0,
               sizeof(_inputPairedRefreshDevices));
        _inputPairedRefreshDeviceCount = 0;
        _inputPairedRefreshSawEnd = false;
        log_i("##[BT-PLIST]# refresh started: role=%s cached=%u",
              moduleRoleName(_moduleRole),
              (unsigned)_inputPairedDeviceCount);
        sendAT("AT+PLIST");
        netserver.btInputPairedStateToWs(
            _inputUiClientId, "refreshing", _inputPairedDeviceCount,
            "Refreshing the shared paired-device list...");
        publishInputUiSnapshot(_inputUiClientId,
                               "Refreshing paired devices...");
    }
}

void BluetoothPlayer::handlePairedListEntry(const char* value) {
    if (!value) return;
    const bool stagedRefresh =
        _inputUiCommand == INPUT_UI_REFRESH_LIST;
    if (!strcmp(value, "E")) {
        if (stagedRefresh) {
            _inputPairedRefreshSawEnd = true;
            log_i("##[BT-PLIST]# end marker received: staged=%u",
                  (unsigned)_inputPairedRefreshDeviceCount);
        }
        return;
    }

    const char* firstComma = strchr(value, ',');
    if (!firstComma) return;
    const char* secondField = firstComma + 1;
    const char* secondComma = strchr(secondField, ',');
    if (!secondComma) return;

    char first[8] = {};
    char second[16] = {};
    const size_t firstLength = min(static_cast<size_t>(firstComma - value),
                                   sizeof(first) - 1U);
    const size_t secondLength = min(
        static_cast<size_t>(secondComma - secondField), sizeof(second) - 1U);
    memcpy(first, value, firstLength);
    memcpy(second, secondField, secondLength);

    const uint8_t index = static_cast<uint8_t>(strtoul(first, nullptr, 10));
    uint16_t profile = 0;
    const char* address = nullptr;
    const char* name = nullptr;
    char addressBuffer[13] = {};

    if (validBluetoothAddress(second)) {
        strlcpy(addressBuffer, second, sizeof(addressBuffer));
        address = addressBuffer;
        name = secondComma + 1;
    } else {
        profile = static_cast<uint16_t>(strtoul(second, nullptr, 10));
        const char* thirdField = secondComma + 1;
        const char* thirdComma = strchr(thirdField, ',');
        const size_t addressLength = thirdComma
            ? static_cast<size_t>(thirdComma - thirdField)
            : strlen(thirdField);
        if (addressLength != 12) return;
        memcpy(addressBuffer, thirdField, 12);
        addressBuffer[12] = '\0';
        address = addressBuffer;
        name = thirdComma ? thirdComma + 1 : "";
    }
    if (!validBluetoothAddress(address)) return;

    btPairedDevice_t* devices = stagedRefresh
                                    ? _inputPairedRefreshDevices
                                    : _inputPairedDevices;
    uint8_t& deviceCount = stagedRefresh
                               ? _inputPairedRefreshDeviceCount
                               : _inputPairedDeviceCount;
    uint8_t target = deviceCount;
    for (uint8_t i = 0; i < deviceCount; ++i) {
        if (strcasecmp(devices[i].address, address) == 0) {
            target = i;
            break;
        }
    }
    if (target == deviceCount) {
        if (deviceCount >= BT_INPUT_PAIRED_MAX_DEVICES) return;
        ++deviceCount;
    }

    btPairedDevice_t& device = devices[target];
    device.index = index;
    device.profile = profile;
    strlcpy(device.address, address, sizeof(device.address));
    strlcpy(device.name, name && name[0] ? name : "Unknown device",
            sizeof(device.name));
    if (_inputConnectedAddress[0] &&
        strcasecmp(_inputConnectedAddress, device.address) == 0 &&
        (!_inputConnectedName[0] || !strcmp(_inputConnectedName, "Unknown device"))) {
        strlcpy(_inputConnectedName, device.name,
                sizeof(_inputConnectedName));
        publishInputUiSnapshot(0);
    }
    log_i("##[BT-PLIST]# %s entry: index=%u profile=%u address=%s name=%s",
          stagedRefresh ? "staged" : "live", (unsigned)device.index,
          (unsigned)device.profile, device.address, device.name);
    if (!stagedRefresh) {
        netserver.btInputPairedDeviceToWs(0, device.index, device.profile,
                                          device.address, device.name);
    }
}

bool BluetoothPlayer::beginNextInputRssiProbe() {
    if (_lastA2dpStatus < 3 || _lastA2dpStatus > 5 ||
        (_moduleRole != BT_ROLE_SINK && _moduleRole != BT_ROLE_SOURCE)) {
        return false;
    }

    const char* knownAddress = _moduleRole == BT_ROLE_SOURCE
                                   ? _sourceConnectedAddress
                                   : _inputConnectedAddress;
    while (_inputRssiProbeIndex < _inputPairedDeviceCount) {
        const btPairedDevice_t& device =
            _inputPairedDevices[_inputRssiProbeIndex++];
        const uint16_t expectedA2dpProfile =
            _moduleRole == BT_ROLE_SOURCE ? (1U << 6) : (1U << 5);
        if (device.profile != 0 &&
            (device.profile & expectedA2dpProfile) == 0) {
            continue;
        }
        if (knownAddress[0] &&
            strcasecmp(knownAddress, device.address) != 0) {
            continue;
        }

        char command[32];
        snprintf(command, sizeof(command), "AT+RSSI=%s", device.address);
        strlcpy(_inputRssiProbeAddress, device.address,
                sizeof(_inputRssiProbeAddress));
        _inputRssiProbeMatched = false;
        _inputUiCommand = INPUT_UI_REFRESH_RSSI;
        _inputUiCommandDeadlineMs = millis() + BT_INPUT_COMMAND_TIMEOUT_MS;
        sendAT(command);
        return true;
    }
    _inputRssiProbeAddress[0] = '\0';
    return false;
}

void BluetoothPlayer::handleRemoteRssi(const char* value) {
    if (!value) return;
    const char* comma = strchr(value, ',');
    if (!comma || static_cast<size_t>(comma - value) != 12U) return;

    char address[13] = {};
    memcpy(address, value, 12);
    if (!validBluetoothAddress(address)) return;

    const long parsedRssi = strtol(comma + 1, nullptr, 10);
    if (parsedRssi >= 0 || parsedRssi < -127) {
        // The BT1058 documents zero as an acquisition failure.
        return;
    }
    if (_inputUiCommand == INPUT_UI_REFRESH_RSSI &&
        strcasecmp(address, _inputRssiProbeAddress) != 0) {
        return;
    }

    const btPairedDevice_t* paired = nullptr;
    for (uint8_t i = 0; i < _inputPairedDeviceCount; ++i) {
        if (strcasecmp(_inputPairedDevices[i].address, address) == 0) {
            paired = &_inputPairedDevices[i];
            break;
        }
    }
    const char* name = paired ? paired->name : "Unknown device";
    _remoteRssi = static_cast<int16_t>(parsedRssi);
    strlcpy(_remoteRssiAddress, address, sizeof(_remoteRssiAddress));
    if (remoteBatteryLevel() >= 0 && !_remoteBatteryAddress[0]) {
        // During power-on autoconnect +HFPBATT may precede both HFPDEV and
        // A2DPDEV. The RSSI probe is now the authoritative identification of
        // the live BR/EDR peer, so attach that still-unassigned battery event
        // to the same device before publishing the WebUI state.
        strlcpy(_remoteBatteryAddress, address,
                sizeof(_remoteBatteryAddress));
        log_i("##[BT]# associated pending HFP battery with %s after RSSI identification",
              address);
    }

    if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5) {
        char connectedDevice[80];
        snprintf(connectedDevice, sizeof(connectedDevice), "%s,%s",
                 address, name);
        if (_moduleRole == BT_ROLE_SOURCE) {
            handleSourceConnectedDevice(connectedDevice, true);
        } else if (_moduleRole == BT_ROLE_SINK) {
            handleInputConnectedDevice(connectedDevice, true);
        }
    }

    netserver.btSourceScanDeviceToWs(0, address, name, _remoteRssi);
    if (_inputUiCommand == INPUT_UI_REFRESH_RSSI) {
        _inputRssiProbeMatched = true;
    }
    log_i("##[BT]# connected remote identified by RSSI: %s %d dBm name=%s",
          address, static_cast<int>(_remoteRssi), name);
}

bool BluetoothPlayer::validBluetoothAddress(const char* address) {
    if (!address || strlen(address) != 12) return false;
    for (size_t i = 0; i < 12; ++i) {
        if (!isxdigit(static_cast<unsigned char>(address[i]))) return false;
    }
    return true;
}

const btSourceScanDevice_t* BluetoothPlayer::findSourceScanDevice(
    const char* address) const {
    if (!address) return nullptr;
    for (uint8_t i = 0; i < _sourceScanDeviceCount; ++i) {
        if (strcasecmp(_sourceScanDevices[i].address, address) == 0) {
            return &_sourceScanDevices[i];
        }
    }
    return nullptr;
}

bool BluetoothPlayer::setSourceOutputMode(uint8_t mode, uint8_t clientId) {
    if (mode > BT_SOURCE_OUTPUT_LOCAL_ONLY) return false;

    const uint8_t previousMode = sourceOutputMode();
    __atomic_store_n(&_sourceOutputMode, mode, __ATOMIC_RELEASE);
    setSourceLocalDacEnabled(mode != BT_SOURCE_OUTPUT_BLUETOOTH_ONLY);
    const bool desiredBluetooth =
        _moduleRole != BT_ROLE_SOURCE ||
        mode != BT_SOURCE_OUTPUT_LOCAL_ONLY;
    _sourceRouteApplyPending = _moduleBluetoothEnabled < 0 ||
        ((_moduleBluetoothEnabled != 0) != desiredBluetooth);
    _sourceRouteClientId = clientId;
    _sourceRouteAttempts = 0;
    _sourceRouteNextActionMs = millis();
    if (_sourceUiCommand != SOURCE_UI_COMMAND_SET_BT_ENABLE &&
        _sourceUiCommand != SOURCE_UI_COMMAND_VERIFY_BT_ENABLE) {
        _sourceRouteVerifyPending = false;
    }
    if (mode != BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        _nextSourceRadioAutostartMs = millis();
        if (_moduleRole == BT_ROLE_SOURCE &&
            previousMode == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
            _sourceReconnectAfterEnable = true;
            _sourceReconnectNextActionMs = millis() + 500U;
        }
    } else {
        _sourceReconnectAfterEnable = false;
        _sourceReconnectNextActionMs = 0;
        _sourceSwitchState = SOURCE_SWITCH_IDLE;
        _sourceSwitchDeadlineMs = 0;
    }
    log_i("##[BT-TX]# source output route requested: %s",
          mode == BT_SOURCE_OUTPUT_BLUETOOTH_ONLY
              ? "Bluetooth only"
              : mode == BT_SOURCE_OUTPUT_LOCAL_ONLY
                    ? "local only"
                    : "local + Bluetooth");
    return true;
}

void BluetoothPlayer::publishSourceUiError(uint8_t clientId,
                                            const char* message) const {
    netserver.btSourceScanStateToWs(clientId, "error",
                                    _sourceScanDeviceCount, message);
    netserver.btSourceStateToWs(
        clientId, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
}

bool BluetoothPlayer::sourceUiReady() const {
    return _moduleRole == BT_ROLE_SOURCE && _startupProbeComplete &&
           !roleTransitionBusy() && !aliasSyncBusy() &&
           !sourceOutputRoutePending() &&
           _sourceSwitchState == SOURCE_SWITCH_IDLE &&
           !_sourceUiInitializationPending;
}

void BluetoothPlayer::publishSourceUiSnapshot(uint8_t clientId) const {
    netserver.btSourceScanStateToWs(clientId, "clear", 0, nullptr);
    bool remoteRssiPublished = false;
    for (uint8_t i = 0; i < _sourceScanDeviceCount; ++i) {
        const btSourceScanDevice_t& device = _sourceScanDevices[i];
        const bool isRemoteRssiDevice = _remoteRssiAddress[0] &&
            strcasecmp(device.address, _remoteRssiAddress) == 0;
        netserver.btSourceScanDeviceToWs(clientId, device.address,
                                         device.name,
                                         isRemoteRssiDevice
                                             ? _remoteRssi : device.rssi);
        if (isRemoteRssiDevice) {
            remoteRssiPublished = true;
        }
    }
    if (!remoteRssiPublished && _remoteRssiAddress[0] && _remoteRssi < 0) {
        const char* remoteName = "Unknown device";
        for (uint8_t i = 0; i < _inputPairedDeviceCount; ++i) {
            if (strcasecmp(_inputPairedDevices[i].address,
                           _remoteRssiAddress) == 0) {
                remoteName = _inputPairedDevices[i].name;
                break;
            }
        }
        netserver.btSourceScanDeviceToWs(
            clientId, _remoteRssiAddress, remoteName, _remoteRssi);
    }
    netserver.btSourceScanStateToWs(
        clientId,
        _sourceScanActive ? "scanning" :
        (_sourceScanDeviceCount > 0 ? "done" : "idle"),
        _sourceScanDeviceCount, nullptr);
    netserver.btSourceStateToWs(
        clientId, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
    log_i("##[BT-WEB]# source snapshot: A2DP=%u address=%s scan=%u RSSI=%d battery=%d/5 batteryDevice=%s",
          (unsigned)_lastA2dpStatus,
          _sourceConnectedAddress[0] ? _sourceConnectedAddress : "unknown",
          (unsigned)_sourceScanDeviceCount,
          _remoteRssiAddress[0] ? static_cast<int>(_remoteRssi) : 0,
          (int)remoteBatteryLevel(),
          _remoteBatteryAddress[0] ? _remoteBatteryAddress : "unknown");
}

bool BluetoothPlayer::beginSourceScan(uint8_t seconds, uint8_t clientId) {
    seconds = constrain(seconds, static_cast<uint8_t>(3),
                        static_cast<uint8_t>(30));
    memset(_sourceScanDevices, 0, sizeof(_sourceScanDevices));
    _sourceScanDeviceCount = 0;
    _sourceScanActive = true;
    _sourceScanDeadlineMs = millis() +
        static_cast<uint32_t>(seconds) * 1000U +
        BT_SOURCE_SCAN_TIMEOUT_MARGIN_MS;
    _sourceUiCommand = SOURCE_UI_COMMAND_SCAN;
    _sourceUiCommandClientId = clientId;
    _sourceUiCommandDeadlineMs = millis() + 3000U;

    netserver.btSourceScanStateToWs(0, "clear", 0, nullptr);
    netserver.btSourceScanStateToWs(
        0, "scanning", 0,
        "Searching for classic Bluetooth audio devices...");
    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), true,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);

    char command[24];
    snprintf(command, sizeof(command), "AT+SCAN=1,%u",
             static_cast<unsigned>(seconds));
    sendAT(command);
    log_i("##[BT-TX]# source device scan started for %u seconds",
          static_cast<unsigned>(seconds));
    return true;
}

bool BluetoothPlayer::startSourceScan(uint8_t seconds, uint8_t clientId) {
    if (_moduleRole != BT_ROLE_SOURCE || !_startupProbeComplete) {
        publishSourceUiError(clientId,
                             "Bluetooth output mode is not ready yet");
        return false;
    }
    if (sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        publishSourceUiError(clientId,
                             "Bluetooth is disabled in Local only mode");
        return false;
    }
    if (_sourceRouteApplyPending || _moduleBluetoothEnabled == 0) {
        publishSourceUiError(clientId,
                             "Bluetooth output is still being enabled");
        return false;
    }
    if (roleTransitionBusy() || _consoleCommandPending ||
        _sourceAudioEnablePending || aliasSyncBusy() || inputUiBusy() ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE) {
        publishSourceUiError(clientId,
                             "Bluetooth module is busy; try again shortly");
        return false;
    }
    if (_sourceScanActive) {
        publishSourceUiSnapshot(clientId);
        return true;
    }
    if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5) {
        publishSourceUiError(clientId,
                             "Disconnect the current speaker before scanning");
        return false;
    }

    seconds = constrain(seconds, static_cast<uint8_t>(3),
                        static_cast<uint8_t>(30));
    if (_lastA2dpStatus == 2 || _sourceConnectPending) {
        // A2DPSTAT=2 is only a connection attempt. It is commonly reported
        // during the module's power-on auto-reconnect window, even when the
        // user wants to discover a different or first speaker. Cancel that
        // attempt, then chain the requested manual scan from the terminal
        // response without changing AUTOCONN's persistent setting.
        _sourceScanRequestedSeconds = seconds;
        _sourceConnectPending = false;
        _sourceManualConnectPending = false;
        _sourceConnectDeadlineMs = 0;
        _sourceUiCommand = SOURCE_UI_COMMAND_CANCEL_RECONNECT_FOR_SCAN;
        _sourceUiCommandClientId = clientId;
        _sourceUiCommandDeadlineMs = millis() + 3000U;
        sendAT("AT+A2DPDISC");
        netserver.btSourceScanStateToWs(
            clientId, "preparing", _sourceScanDeviceCount,
            "Stopping automatic reconnect before scanning...");
        log_i("##[BT-TX]# scan requested during A2DP reconnect; cancelling reconnect first");
        return true;
    }

    return beginSourceScan(seconds, clientId);
}

bool BluetoothPlayer::stopSourceScan(uint8_t clientId) {
    if (_sourceSwitchState != SOURCE_SWITCH_IDLE) {
        publishSourceUiError(clientId,
                             "Speaker switch is in progress");
        return false;
    }
    if (!_sourceScanActive) {
        publishSourceUiSnapshot(clientId);
        return true;
    }
    if (_sourceUiCommand != SOURCE_UI_COMMAND_NONE &&
        _sourceUiCommand != SOURCE_UI_COMMAND_SCAN) {
        publishSourceUiError(clientId, "Bluetooth module is busy");
        return false;
    }

    _sourceUiCommand = SOURCE_UI_COMMAND_STOP_SCAN;
    _sourceUiCommandClientId = clientId;
    _sourceUiCommandDeadlineMs = millis() + 3000U;
    sendAT("AT+SCAN=0");
    netserver.btSourceScanStateToWs(clientId, "stopping",
                                    _sourceScanDeviceCount, nullptr);
    return true;
}

bool BluetoothPlayer::connectSourceDevice(const char* address,
                                           uint8_t clientId) {
    if (!validBluetoothAddress(address)) {
        publishSourceUiError(clientId, "Invalid Bluetooth address");
        return false;
    }
    if (sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        publishSourceUiError(clientId,
                             "Bluetooth is disabled in Local only mode");
        return false;
    }
    if (_sourceRouteApplyPending || _moduleBluetoothEnabled == 0) {
        publishSourceUiError(clientId,
                             "Bluetooth output is still being enabled");
        return false;
    }
    if (_moduleRole != BT_ROLE_SOURCE || !_startupProbeComplete) {
        publishSourceUiError(clientId,
                             "Bluetooth output mode is not ready yet");
        return false;
    }
    if (roleTransitionBusy() || _consoleCommandPending ||
        _sourceAudioEnablePending || aliasSyncBusy() || inputUiBusy() || _sourceScanActive ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE) {
        publishSourceUiError(clientId,
                             "Wait for scanning or role switching to finish");
        return false;
    }
    if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5 &&
        _sourceConnectedAddress[0] &&
        strcasecmp(_sourceConnectedAddress, address) == 0) {
        publishSourceUiSnapshot(clientId);
        return true;
    }
    if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5) {
        if (!_sourceConnectedAddress[0]) {
            publishSourceUiError(clientId,
                                 "Wait for the current speaker to be identified before switching");
            return false;
        }
        strlcpy(_sourceSwitchTargetAddress, address,
                sizeof(_sourceSwitchTargetAddress));
        strlcpy(_sourceSwitchPreviousAddress, _sourceConnectedAddress,
                sizeof(_sourceSwitchPreviousAddress));
        strlcpy(_sourceSwitchPreviousName, _sourceConnectedName,
                sizeof(_sourceSwitchPreviousName));
        _sourceSwitchClientId = clientId;
        _sourceSwitchState = SOURCE_SWITCH_DISCONNECTING;
        _sourceSwitchDeadlineMs = millis() + 7000U;
        _sourceUiCommand = SOURCE_UI_COMMAND_SWITCH_DISCONNECT;
        _sourceUiCommandClientId = clientId;
        _sourceUiCommandDeadlineMs = millis() + 3000U;
        sendAT("AT+A2DPDISC");
        netserver.btSourceScanStateToWs(
            clientId, "switching", _sourceScanDeviceCount,
            "Disconnecting the current speaker before switching...");
        netserver.btSourceStateToWs(
            0, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), false,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        log_i("##[BT-TX]# switching speaker %s -> %s: disconnect requested",
              _sourceSwitchPreviousAddress, _sourceSwitchTargetAddress);
        return true;
    }
    if (_lastA2dpStatus == 2 || _sourceConnectPending) {
        publishSourceUiError(clientId,
                             "Wait for the current connection attempt to finish");
        return false;
    }

    strlcpy(_sourceConnectedAddress, address,
            sizeof(_sourceConnectedAddress));
    const btSourceScanDevice_t* device = findSourceScanDevice(address);
    strlcpy(_sourceConnectedName, device ? device->name : "",
            sizeof(_sourceConnectedName));
    _sourceConnectPending = true;
    _sourceManualConnectPending = true;
    _sourceConnectDeadlineMs = millis() + BT_SOURCE_CONNECT_TIMEOUT_MS;
    _sourceUiCommand = SOURCE_UI_COMMAND_CONNECT;
    _sourceUiCommandClientId = clientId;
    _sourceUiCommandDeadlineMs = millis() + 3000U;

    char command[32];
    snprintf(command, sizeof(command), "AT+A2DPCONN=%s", address);
    sendAT(command);
    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), false, true,
        _sourceConnectedAddress, _sourceConnectedName);
    log_i("##[BT-TX]# source connect/pair requested: %s (%s)", address,
          _sourceConnectedName[0] ? _sourceConnectedName : "name unknown");
    return true;
}

bool BluetoothPlayer::disconnectSourceDevice(uint8_t clientId) {
    if (_moduleRole != BT_ROLE_SOURCE || !_startupProbeComplete) {
        publishSourceUiError(clientId,
                             "Bluetooth output mode is not ready yet");
        return false;
    }
    if (roleTransitionBusy() || _consoleCommandPending ||
        _sourceAudioEnablePending || aliasSyncBusy() || inputUiBusy() || _sourceScanActive ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        _sourceSwitchState != SOURCE_SWITCH_IDLE) {
        publishSourceUiError(clientId, "Bluetooth module is busy");
        return false;
    }
    if (_lastA2dpStatus < 2 || _lastA2dpStatus > 5) {
        publishSourceUiSnapshot(clientId);
        return true;
    }

    _sourceConnectPending = false;
    _sourceManualConnectPending = false;
    _sourceUiCommand = SOURCE_UI_COMMAND_DISCONNECT;
    _sourceUiCommandClientId = clientId;
    _sourceUiCommandDeadlineMs = millis() + 3000U;
    sendAT("AT+A2DPDISC");
    netserver.btSourceScanStateToWs(clientId, "disconnecting",
                                    _sourceScanDeviceCount, nullptr);
    log_i("##[BT-TX]# source disconnect requested");
    return true;
}

void BluetoothPlayer::finishSourceScan(const char* state,
                                       const char* message) {
    _sourceScanActive = false;
    _sourceScanDeadlineMs = 0;
    if (_sourceUiCommand == SOURCE_UI_COMMAND_SCAN ||
        _sourceUiCommand == SOURCE_UI_COMMAND_STOP_SCAN) {
        _sourceUiCommand = SOURCE_UI_COMMAND_NONE;
        _sourceUiCommandDeadlineMs = 0;
    }
    netserver.btSourceScanStateToWs(0, state ? state : "done",
                                    _sourceScanDeviceCount, message);
    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), false,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
}

void BluetoothPlayer::startSourceSwitchConnection(bool restorePrevious) {
    const char* address = restorePrevious ? _sourceSwitchPreviousAddress
                                          : _sourceSwitchTargetAddress;
    const uint32_t now = millis();
    _sourceSwitchState = restorePrevious ? SOURCE_SWITCH_CONNECTING_PREVIOUS
                                         : SOURCE_SWITCH_CONNECTING_TARGET;
    _sourceSwitchAddressReported = false;
    _sourceSwitchDeadlineMs = now + BT_SOURCE_CONNECT_TIMEOUT_MS;
    _sourceConnectPending = true;
    _sourceManualConnectPending = true;
    _sourceConnectDeadlineMs = _sourceSwitchDeadlineMs;
    strlcpy(_sourceConnectedAddress, address,
            sizeof(_sourceConnectedAddress));
    if (restorePrevious) {
        strlcpy(_sourceConnectedName, _sourceSwitchPreviousName,
                sizeof(_sourceConnectedName));
    } else {
        const btSourceScanDevice_t* device = findSourceScanDevice(address);
        strlcpy(_sourceConnectedName, device ? device->name : "",
                sizeof(_sourceConnectedName));
    }
    _sourceUiCommand = SOURCE_UI_COMMAND_CONNECT;
    _sourceUiCommandClientId = _sourceSwitchClientId;
    _sourceUiCommandDeadlineMs = now + 3000U;

    char command[32];
    snprintf(command, sizeof(command), "AT+A2DPCONN=%s", address);
    sendAT(command);
    netserver.btSourceScanStateToWs(
        _sourceSwitchClientId, restorePrevious ? "restoring" : "connecting",
        _sourceScanDeviceCount,
        restorePrevious ? "New speaker unavailable; reconnecting the previous speaker..."
                        : "Connecting to the selected speaker...");
    log_i("##[BT-TX]# speaker switch %s: %s",
          restorePrevious ? "rollback" : "target connect", address);
}

void BluetoothPlayer::queueSourceSwitchRollback(const char* reason) {
    if (_sourceSwitchState != SOURCE_SWITCH_CONNECTING_TARGET) return;
    _sourceSwitchState = SOURCE_SWITCH_WAIT_ROLLBACK;
    _sourceSwitchDeadlineMs = millis() + 7000U;
    _sourceSwitchNextStatusQueryMs = millis() + 500U;
    _sourceConnectPending = false;
    _sourceManualConnectPending = false;
    _sourceConnectDeadlineMs = 0;
    netserver.btSourceScanStateToWs(
        _sourceSwitchClientId, "restoring", _sourceScanDeviceCount,
        "New speaker did not connect; restoring the previous speaker...");
    log_w("##[BT-TX]# speaker switch failed (%s); restoring %s",
          reason ? reason : "unknown", _sourceSwitchPreviousAddress);
}

void BluetoothPlayer::completeSourceSwitchConnection(const char* address) {
    if (!_sourceSwitchAddressReported || _lastA2dpStatus < 3 ||
        _lastA2dpStatus > 5 || !address) return;

    const bool lateConnection =
        _sourceSwitchState == SOURCE_SWITCH_WAIT_ROLLBACK;
    const bool target = _sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET ||
        (lateConnection &&
         strcasecmp(address, _sourceSwitchTargetAddress) == 0);
    const bool previous =
        _sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS ||
        (lateConnection &&
         strcasecmp(address, _sourceSwitchPreviousAddress) == 0);
    if (!target && !previous && !lateConnection) return;
    const char* expected = target ? _sourceSwitchTargetAddress
                                  : _sourceSwitchPreviousAddress;
    const bool matched = strcasecmp(address, expected) == 0;
    _sourceSwitchState = SOURCE_SWITCH_IDLE;
    _sourceSwitchDeadlineMs = 0;
    _sourceConnectPending = false;
    _sourceManualConnectPending = false;
    _sourceConnectDeadlineMs = 0;
    if (target && matched) {
        netserver.btSourceScanStateToWs(
            _sourceSwitchClientId, "connected", _sourceScanDeviceCount,
            "Switched to the selected speaker.");
        log_i("##[BT-TX]# speaker switch complete: %s", address);
    } else if (previous && matched) {
        publishSourceUiError(
            _sourceSwitchClientId,
            "New speaker did not connect; previous speaker restored");
        log_w("##[BT-TX]# speaker switch rolled back to %s", address);
    } else {
        publishSourceUiError(
            _sourceSwitchClientId,
            "A different speaker connected during the switch; check the active device");
        log_w("##[BT-TX]# unexpected speaker during switch: %s (expected %s)",
              address, expected);
    }
}

void BluetoothPlayer::retryOrFailSourceOutputRoute(const char* reason) {
    _sourceRouteVerifyPending = false;
    if (_sourceRouteAttempts < BT_SOURCE_ROUTE_MAX_ATTEMPTS) {
        _sourceRouteApplyPending = true;
        _sourceRouteNextActionMs =
            millis() + BT_SOURCE_ROUTE_RETRY_DELAY_MS;
        log_w("##[BT-TX]# source output route retry scheduled: %s",
              reason ? reason : "unknown error");
        return;
    }

    _sourceRouteApplyPending = false;
    log_e("##[BT-TX]# source output route failed after %u attempt(s): %s",
          (unsigned)_sourceRouteAttempts,
          reason ? reason : "unknown error");
    netserver.btSourceScanStateToWs(
        _sourceRouteClientId, "error", _sourceScanDeviceCount,
        "Could not change the Bluetooth output route");
}

void BluetoothPlayer::serviceSourceDeviceControl() {
    const uint32_t now = millis();
    if (_sourceUiCommand != SOURCE_UI_COMMAND_NONE &&
        static_cast<int32_t>(now - _sourceUiCommandDeadlineMs) >= 0) {
        const sourceUiCommand_e timedOut = _sourceUiCommand;
        const uint8_t clientId = _sourceUiCommandClientId;
        _sourceUiCommand = SOURCE_UI_COMMAND_NONE;
        _sourceUiCommandDeadlineMs = 0;
        if (timedOut == SOURCE_UI_COMMAND_SET_BT_ENABLE ||
            timedOut == SOURCE_UI_COMMAND_VERIFY_BT_ENABLE) {
            retryOrFailSourceOutputRoute("BTEN command response timeout");
            publishSourceUiSnapshot(clientId);
            return;
        }
        if (timedOut == SOURCE_UI_COMMAND_SCAN ||
            timedOut == SOURCE_UI_COMMAND_STOP_SCAN) {
            _sourceScanActive = false;
            _sourceScanDeadlineMs = 0;
        }
        if (timedOut == SOURCE_UI_COMMAND_CONNECT) {
            if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET) {
                // A lost command reply is not proof that the A2DP link failed.
                // Let the connection-state deadline decide before rollback.
                log_w("##[BT-TX]# target connect reply timed out; waiting for A2DP state");
                return;
            }
            if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS) {
                log_w("##[BT-TX]# previous-speaker connect reply timed out; waiting for A2DP state");
                return;
            }
            _sourceConnectPending = false;
            _sourceManualConnectPending = false;
            _sourceConnectDeadlineMs = 0;
            _sourceConnectedAddress[0] = '\0';
            _sourceConnectedName[0] = '\0';
        }
        if (timedOut == SOURCE_UI_COMMAND_RECONNECT_LAST) {
            _sourceConnectPending = false;
            _sourceManualConnectPending = false;
            _sourceReconnectAfterEnable = false;
        }
        if (timedOut == SOURCE_UI_COMMAND_SWITCH_DISCONNECT) {
            if (_lastA2dpStatus == 1) {
                _sourceSwitchState =
                    _sourceSwitchState == SOURCE_SWITCH_DISCONNECTING_TARGET
                        ? SOURCE_SWITCH_WAIT_ROLLBACK
                        : SOURCE_SWITCH_WAIT_STANDBY;
                _sourceSwitchDeadlineMs = now + 7000U;
                return;
            }
            // DISC may have been accepted even if its terminal reply was
            // lost. Query the actual A2DP state before giving up.
            _sourceSwitchState =
                _sourceSwitchState == SOURCE_SWITCH_DISCONNECTING_TARGET
                    ? SOURCE_SWITCH_WAIT_TARGET_STANDBY
                    : SOURCE_SWITCH_WAIT_STANDBY;
            _sourceSwitchDeadlineMs = now + 7000U;
            _sourceSwitchNextStatusQueryMs = now + 500U;
            return;
        }
        if (timedOut == SOURCE_UI_COMMAND_SWITCH_QUERY_STATUS) {
            // The next bounded poll or overall switch deadline will decide.
            return;
        }
        publishSourceUiError(clientId, "Bluetooth command response timeout");
    }

    if (_sourceScanActive &&
        static_cast<int32_t>(now - _sourceScanDeadlineMs) >= 0) {
        sendAT("AT+SCAN=0");
        finishSourceScan("timeout",
                         "Scan stopped after the expected completion time");
    }

    if (_sourceConnectPending &&
        static_cast<int32_t>(now - _sourceConnectDeadlineMs) >= 0) {
        if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET) {
            queueSourceSwitchRollback("target connection timeout");
            return;
        }
        if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS) {
            _sourceSwitchState = SOURCE_SWITCH_IDLE;
            _sourceSwitchDeadlineMs = 0;
            _sourceConnectPending = false;
            _sourceManualConnectPending = false;
            _sourceConnectDeadlineMs = 0;
            _sourceConnectedAddress[0] = '\0';
            _sourceConnectedName[0] = '\0';
            publishSourceUiError(
                _sourceSwitchClientId,
                "New speaker failed; previous speaker could not be restored automatically");
            return;
        }
        const bool manualConnect = _sourceManualConnectPending;
        _sourceConnectPending = false;
        _sourceManualConnectPending = false;
        _sourceConnectDeadlineMs = 0;
        _sourceConnectedAddress[0] = '\0';
        _sourceConnectedName[0] = '\0';
        publishSourceUiError(
            0,
            manualConnect
                ? "Connection timed out: select an A2DP speaker/headset in pairing mode"
                : "Speaker connection timed out");
    }

    if (_sourceSwitchState == SOURCE_SWITCH_WAIT_STANDBY &&
        _sourceUiCommand == SOURCE_UI_COMMAND_NONE) {
        if (_lastA2dpStatus == 1 &&
            !_consoleCommandPending && !_sourceAudioEnablePending &&
            !aliasSyncBusy() && !inputUiBusy() && !roleTransitionBusy() &&
            !_sourceScanActive && !_sourceRouteApplyPending &&
            _moduleBluetoothEnabled == 1 &&
            static_cast<uint32_t>(now - _lastAtTxMs) >= BT_AT_COMMAND_GUARD_MS) {
            startSourceSwitchConnection(false);
            return;
        }
        if (static_cast<int32_t>(now - _sourceSwitchDeadlineMs) >= 0) {
            const uint8_t clientId = _sourceSwitchClientId;
            _sourceSwitchState = SOURCE_SWITCH_IDLE;
            _sourceSwitchDeadlineMs = 0;
            publishSourceUiError(
                clientId,
                "Previous speaker did not disconnect; select Connect again");
        }
    }

    if (_sourceSwitchState == SOURCE_SWITCH_WAIT_ROLLBACK &&
        _sourceUiCommand == SOURCE_UI_COMMAND_NONE) {
        if (_lastA2dpStatus == 1 &&
            !_consoleCommandPending && !_sourceAudioEnablePending &&
            !aliasSyncBusy() && !inputUiBusy() && !roleTransitionBusy() &&
            !_sourceRouteApplyPending && _moduleBluetoothEnabled == 1 &&
            static_cast<uint32_t>(now - _lastAtTxMs) >= BT_AT_COMMAND_GUARD_MS) {
            startSourceSwitchConnection(true);
            return;
        }
        if (_lastA2dpStatus >= 2 && _lastA2dpStatus <= 5 &&
            static_cast<int32_t>(now - _sourceSwitchNextStatusQueryMs) >= 0 &&
            static_cast<uint32_t>(now - _lastAtTxMs) >= BT_AT_COMMAND_GUARD_MS) {
            _sourceSwitchState = SOURCE_SWITCH_DISCONNECTING_TARGET;
            _sourceSwitchDeadlineMs = now + 7000U;
            _sourceUiCommand = SOURCE_UI_COMMAND_SWITCH_DISCONNECT;
            _sourceUiCommandClientId = _sourceSwitchClientId;
            _sourceUiCommandDeadlineMs = now + 3000U;
            sendAT("AT+A2DPDISC");
            log_w("##[BT-TX]# cancelling unfinished target connection before rollback");
            return;
        }
        if (static_cast<int32_t>(now - _sourceSwitchDeadlineMs) >= 0) {
            _sourceSwitchState = SOURCE_SWITCH_IDLE;
            publishSourceUiError(
                _sourceSwitchClientId,
                "Could not stop the new connection attempt; reconnect the previous speaker manually");
        }
    }

    if (_sourceSwitchState == SOURCE_SWITCH_DISCONNECTING_TARGET &&
        _sourceUiCommand == SOURCE_UI_COMMAND_NONE) {
        if (_lastA2dpStatus == 1) {
            _sourceSwitchState = SOURCE_SWITCH_WAIT_ROLLBACK;
            _sourceSwitchDeadlineMs = now + 7000U;
        } else if (static_cast<int32_t>(now - _sourceSwitchDeadlineMs) >= 0) {
            _sourceSwitchState = SOURCE_SWITCH_IDLE;
            publishSourceUiError(
                _sourceSwitchClientId,
                "New connection did not disconnect; reconnect the previous speaker manually");
        }
    }

    if ((_sourceSwitchState == SOURCE_SWITCH_WAIT_STANDBY ||
         _sourceSwitchState == SOURCE_SWITCH_WAIT_TARGET_STANDBY) &&
        _sourceUiCommand == SOURCE_UI_COMMAND_NONE) {
        if (_sourceSwitchState == SOURCE_SWITCH_WAIT_TARGET_STANDBY &&
            _lastA2dpStatus == 1) {
            _sourceSwitchState = SOURCE_SWITCH_WAIT_ROLLBACK;
            _sourceSwitchDeadlineMs = now + 7000U;
        } else if (_lastA2dpStatus != 1 &&
                   static_cast<int32_t>(now - _sourceSwitchDeadlineMs) >= 0) {
            const bool restoring =
                _sourceSwitchState == SOURCE_SWITCH_WAIT_TARGET_STANDBY;
            _sourceSwitchState = SOURCE_SWITCH_IDLE;
            publishSourceUiError(
                _sourceSwitchClientId,
                restoring
                    ? "New connection did not disconnect; reconnect the previous speaker manually"
                    : "Previous speaker did not disconnect; select Connect again");
        } else if (_lastA2dpStatus != 1 &&
                   static_cast<int32_t>(now - _sourceSwitchNextStatusQueryMs) >= 0 &&
                   !_consoleCommandPending &&
                   static_cast<uint32_t>(now - _lastAtTxMs) >= BT_AT_COMMAND_GUARD_MS) {
            _sourceUiCommand = SOURCE_UI_COMMAND_SWITCH_QUERY_STATUS;
            _sourceUiCommandClientId = _sourceSwitchClientId;
            _sourceUiCommandDeadlineMs = now + 1500U;
            _sourceSwitchNextStatusQueryMs = now + 2000U;
            sendAT("AT+A2DPSTAT");
        }
    }

    const uint8_t outputMode = sourceOutputMode();
    if (_moduleRole == BT_ROLE_SOURCE &&
        outputMode == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        _sourceSwitchState = SOURCE_SWITCH_IDLE;
        _sourceSwitchDeadlineMs = 0;
        _sourceReconnectAfterEnable = false;
        _sourceReconnectNextActionMs = 0;
        _sourceConnectPending = false;
        _sourceManualConnectPending = false;
        _sourceConnectDeadlineMs = 0;
        if (sourceRadioRunning()) stopSourceRadio();
        if (sourceToneRunning()) stopSourceTone();
        if (_sourceScanActive &&
            _sourceUiCommand == SOURCE_UI_COMMAND_NONE) {
            stopSourceScan(_sourceRouteClientId);
            return;
        }
    }

    // AT+BTEN=1 restores the radio but the tested firmware does not always
    // restart its automatic A2DP connection immediately. The documented
    // parameterless A2DPCONN command reconnects the last A2DP endpoint while
    // keeping the paired list and AUTOCONN setting untouched.
    if (_sourceReconnectAfterEnable && !_sourceRouteApplyPending &&
        _sourceSwitchState == SOURCE_SWITCH_IDLE &&
        _moduleRole == BT_ROLE_SOURCE &&
        outputMode != BT_SOURCE_OUTPUT_LOCAL_ONLY &&
        _moduleBluetoothEnabled == 1) {
        if (_lastA2dpStatus >= 2 && _lastA2dpStatus <= 5) {
            _sourceReconnectAfterEnable = false;
            _sourceReconnectNextActionMs = 0;
        } else if (_sourceUiCommand == SOURCE_UI_COMMAND_NONE &&
                   !_consoleCommandPending && !_sourceAudioEnablePending &&
                   !aliasSyncBusy() && !inputUiBusy() && !sinkControlBusy() &&
                   !roleTransitionBusy() && !_sourceScanActive &&
                   static_cast<int32_t>(now - _sourceReconnectNextActionMs) >= 0 &&
                   static_cast<uint32_t>(now - _lastAtTxMs) >= BT_AT_COMMAND_GUARD_MS) {
            _sourceUiCommand = SOURCE_UI_COMMAND_RECONNECT_LAST;
            _sourceUiCommandClientId = _sourceRouteClientId;
            _sourceUiCommandDeadlineMs = now + 3000U;
            _sourceConnectPending = true;
            _sourceManualConnectPending = false;
            _sourceConnectDeadlineMs = now + BT_SOURCE_CONNECT_TIMEOUT_MS;
            sendAT("AT+A2DPCONN");
            log_i("##[BT-TX]# reconnecting the last paired speaker after Local only");
            netserver.btSourceScanStateToWs(
                _sourceRouteClientId, "connecting", _sourceScanDeviceCount,
                "Reconnecting the last paired speaker...");
            return;
        }
    }

    if (!_sourceRouteApplyPending ||
        _sourceUiCommand != SOURCE_UI_COMMAND_NONE ||
        !_startupProbeComplete || _moduleRole == BT_ROLE_UNKNOWN ||
        _consoleCommandPending || _sourceAudioEnablePending ||
        aliasSyncBusy() || inputUiBusy() || sinkControlBusy() ||
        roleTransitionBusy() || _sourceScanActive) {
        return;
    }
    if (static_cast<int32_t>(now - _sourceRouteNextActionMs) < 0 ||
        static_cast<uint32_t>(now - _lastAtTxMs) < BT_AT_COMMAND_GUARD_MS) {
        return;
    }

    // Bluetooth input always needs the radio. In source/output mode only the
    // Local-only route disables it; AUTOCONN itself is never modified.
    const bool desiredBluetooth =
        _moduleRole != BT_ROLE_SOURCE ||
        outputMode != BT_SOURCE_OUTPUT_LOCAL_ONLY;
    if (!_sourceRouteVerifyPending && _moduleBluetoothEnabled >= 0 &&
        ((_moduleBluetoothEnabled != 0) == desiredBluetooth)) {
        _sourceRouteApplyPending = false;
        _sourceRouteAttempts = 0;
        netserver.btSourceStateToWs(
            _sourceRouteClientId, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        return;
    }

    _sourceBluetoothTarget = desiredBluetooth;
    _sourceUiCommandClientId = _sourceRouteClientId;
    _sourceUiCommandDeadlineMs =
        now + BT_SOURCE_ROUTE_COMMAND_TIMEOUT_MS;
    if (_sourceRouteVerifyPending || _moduleBluetoothEnabled < 0) {
        if (_moduleBluetoothEnabled < 0 && !_sourceRouteVerifyPending) {
            ++_sourceRouteAttempts;
        }
        _sourceRouteVerifyPending = false;
        _sourceUiCommand = SOURCE_UI_COMMAND_VERIFY_BT_ENABLE;
        sendAT("AT+BTEN");
        return;
    }

    char command[16];
    snprintf(command, sizeof(command), "AT+BTEN=%u",
             desiredBluetooth ? 1U : 0U);
    ++_sourceRouteAttempts;
    if (desiredBluetooth && _moduleRole == BT_ROLE_SOURCE) {
        _sourceReconnectAfterEnable = true;
        _sourceReconnectNextActionMs = now + 500U;
    }
    _sourceUiCommand = SOURCE_UI_COMMAND_SET_BT_ENABLE;
    sendAT(command);
    log_i("##[BT-TX]# %s module Bluetooth for source output route (attempt %u/%u)",
          desiredBluetooth ? "enabling" : "disabling",
          (unsigned)_sourceRouteAttempts,
          (unsigned)BT_SOURCE_ROUTE_MAX_ATTEMPTS);
}

// --- non-blocking line reader, called from main loop() --------------
void BluetoothPlayer::loop() {
    handleLabConsole();

    if (_consoleCommandPending &&
        static_cast<int32_t>(millis() - _consoleCommandDeadlineMs) >= 0) {
        Serial.println("##[BT-LAB]# response timeout");
        _consoleCommandPending = false;
    }

    serviceStartupProbe();
    serviceSourceDeviceControl();
    serviceSourceRadioStats();
    serviceSourceUiPlaybackState();

    // Periodic read-only health/state poll. Unsolicited events remain the
    // primary state source; this query only repairs a missed UART event.
    static uint32_t lastPing = 0;
    uint32_t now = millis();
    if (_startupProbeComplete && !_consoleCommandPending &&
        !aliasSyncBusy() && !inputUiBusy() && !sinkControlBusy() &&
        !_sourceAudioEnablePending &&
        !_sourceScanActive && _sourceUiCommand == SOURCE_UI_COMMAND_NONE &&
        _sourceSwitchState == SOURCE_SWITCH_IDLE &&
        !roleTransitionBusy() && now - lastPing > 10000) {
        lastPing = now;
        sendAT("AT+STAT");
    }

    while (_uart.available()) {
        char c = (char)_uart.read();
        if (c == '\0') continue;  // observed once immediately after AT+REBOOT
        if (c == '\n' || c == '\r') {
            if (_lineOverflow) {
                log_w("##[BT]# dropped overlong UART line");
            } else if (_lineLen > 0) {
                _lineBuf[_lineLen] = '\0';
                parseLine(_lineBuf);
            }
            _lineLen = 0;
            _lineOverflow = false;
            continue;
        }
        if (!_lineOverflow && _lineLen < sizeof(_lineBuf) - 1) {
            _lineBuf[_lineLen++] = c;
        } else {
            _lineOverflow = true;
        }
    }

    serviceSinkControl();
    serviceRoleTransition();
    serviceAliasSync();
    serviceInputUiControl();
    serviceSinkStreamInfo();
    serviceSourceRadioAutostart();
    serviceSourceAudioEnable();

    // WEB/SD playback is stopped through the player queue. Start the BT
    // bridge only after that stop has really completed, otherwise both audio
    // producers can touch the same I2S TX channel during a mode change.
    if (_bridgeStartPending) {
        if (config.getMode() != PM_BLUETOOTH) {
            _bridgeStartPending = false;
        } else if (_moduleRole == BT_ROLE_SOURCE) {
            _bridgeStartPending = false;
            log_w("##[BT-TX]# pending RX bridge cancelled: module is source");
        } else if (_moduleRole == BT_ROLE_UNKNOWN) {
            // Wait for the read-only AT+MODCFG response before assigning a
            // direction to the module-side I2S pins.
        } else if (!player.isRunning() && player.status() == STOPPED &&
                   player.readyForWebStation()) {
            _bridgeStartPending = false;
            if (!startBridgeNow()) {
                log_e("##[BT]# deferred bridge start failed");
            } else {
                log_i("##[BT]# deferred bridge started");
            }
        }
    }

    // Player::_stop() completes asynchronously during WEB -> BT switching and
    // can write the localized [stop] title after PLAYSTAT=1 has already been
    // handled. A recently forwarded non-silent PCM block is the authoritative
    // fallback signal: while audio is flowing and no metadata exists, keep the
    // first title row on "Playing". Real TRACKINFO always takes precedence.
    const bool sinkAudioActive = audioActive();
    if (_moduleRole == BT_ROLE_SINK && sinkAudioActive) {
        activateSinkMetaGeneration();
    }
    if (config.getMode() == PM_BLUETOOTH &&
        _moduleRole == BT_ROLE_SINK && !roleTransitionBusy() &&
        !config.isClockTTS && _title.length() == 0 &&
        _artist.length() == 0) {
        if (_state == BT_STATE_PLAYING) {
            setFallbackTitle(sinkAudioActive ? "Playing"
                                             : LANG::const_PlReady);
        } else if (_state == BT_STATE_CONNECTED) {
            setFallbackTitle(LANG::const_PlReady);
        }
    }

    // Ha érkezett új cím/előadó, küldjük a display-re.
    // Feltétel: connected legyen (BT_PA vagy BT_STOP is jó — kézi trackváltásnál
    // a metaadat BT_STOP állapotban érkezik, BT_PA csak utána jön)
    // Cache AVRCP metadata in every mode, but publish it only while Bluetooth
    // is selected. Otherwise a connected phone or PC overwrites WEB/SD/DLNA.
    if (_metaUpdated && playing() && config.getMode() == PM_BLUETOOTH) {
        publishSinkPlaybackTitle();
    }
}

void BluetoothPlayer::publishSinkPlaybackTitle() {
    if (config.getMode() != PM_BLUETOOTH) return;

    // YouTube exposes the channel name through AVRCP as both artist and
    // album. It arrives after the real video title, so treating it as a
    // musical artist changes an already-correct title into
    // "Channel - Video title" and also poisons cover lookup. Real music
    // services normally provide distinct artist and album fields.
    const bool artistIsChannel =
        _artist.length() > 0 && _album.length() > 0 &&
        _artist.equalsIgnoreCase(_album);
    String info;
    if (artistIsChannel && _title.length() > 0) {
        // Only normalize a separator when the reported channel itself is the
        // exact prefix. Colons, dashes and pipes elsewhere remain untouched:
        // they are too ambiguous in mix/compilation titles.
        info = normalizeChannelPrefixedTitle(_title, _artist);
    } else if (_artist.length() > 0 && _title.length() > 0) {
        info = _artist + " - " + _title;
    } else if (_title.length() > 0) {
        info = _title;
    } else if (_artist.length() > 0) {
        info = _artist;
    } else {
        info = audioActive() ? "Playing" : LANG::const_PlReady;
    }

    _metaUpdated = false;
    if (!strcmp(config.station.title, info.c_str())) return;
    if (artistIsChannel) {
        log_i("##[BT-META]# YouTube channel metadata normalized: '%s' -> '%s'",
              _title.c_str(), info.c_str());
    }
    config.setTitle(info.c_str());
    display.putRequest(NEWTITLE);
    netserver.requestOnChange(TITLE, 0);
}

void BluetoothPlayer::beginSinkMetaGeneration(bool rememberCurrentTitle) {
    if (rememberCurrentTitle && _title.length() > 0) {
        _previousSinkTitle = _title;
    }
    _pendingSinkTitle = "";
    _pendingSinkArtist = "";
    _pendingSinkAlbum = "";
    _sinkPlaybackSeen = false;
    _sinkMetaFreshAccepted = false;
    _sinkStaleMetaLogged = false;
    _lastSinkPlayStatus = -1;
    clearMeta();
    _state = BT_STATE_DISC;
    _lastPcmMs = 0;
    _vuLeft = 0;
    _vuRight = 0;
    if (config.getMode() == PM_BLUETOOTH && !roleTransitionBusy()) {
        config.setTitle("");
    }
}

void BluetoothPlayer::activateSinkMetaGeneration() {
    if (_moduleRole != BT_ROLE_SINK || _sinkPlaybackSeen) return;
    _sinkPlaybackSeen = true;

    const bool pendingIsFresh = _pendingSinkTitle.length() > 0 &&
        (_previousSinkTitle.length() == 0 ||
         _pendingSinkTitle != _previousSinkTitle);
    if (pendingIsFresh) {
        _sinkMetaFreshAccepted = true;
        applyTrackInfo(_pendingSinkTitle, _pendingSinkArtist,
                       _pendingSinkAlbum);
    } else if (config.getMode() == PM_BLUETOOTH) {
        if (_pendingSinkTitle.length() > 0 &&
            _pendingSinkTitle == _previousSinkTitle &&
            !_sinkStaleMetaLogged) {
            log_i("##[BT]# suppressing previous-session TRACKINFO at playback start");
            _sinkStaleMetaLogged = true;
        }
        setFallbackTitle(audioActive() ? "Playing" : LANG::const_PlReady);
    }

    _pendingSinkTitle = "";
    _pendingSinkArtist = "";
    _pendingSinkAlbum = "";
}

void BluetoothPlayer::clearMeta() {
    _title      = "";
    _artist     = "";
    _album      = "";
    _genre      = "";
    _trackNum   = 0;
    _trackTotal = 0;
    _trackMs    = 0;
    _posMs      = 0;
    _streamCodec[0] = '\0';
    _streamBits = BT_I2S_BITS;
    _metaUpdated = false;
    _streamInfoRequested = false;
}

void BluetoothPlayer::applyTrackInfo(const String& newTitle,
                                     const String& newArtist,
                                     const String& newAlbum) {
    bool changed = false;
    if (newTitle.length() > 0 && newTitle != _title) {
        _title = newTitle;
        // BT1058 metadata is delivered in stages. A new title with empty
        // trailing fields starts a new generation, so stale artist/album data
        // from the previous track must not survive.
        _artist = "";
        _album = "";
        _posMs = 0;
        _trackMs = 0;
        changed = true;
    }
    if (newArtist.length() > 0 && newArtist != _artist) {
        _artist = newArtist;
        changed = true;
    }
    if (newAlbum.length() > 0 && newAlbum != _album) {
        _album = newAlbum;
        changed = true;
    }
    if (changed) _metaUpdated = true;
}

void BluetoothPlayer::setFallbackTitle(const char* title) {
    if (config.getMode() != PM_BLUETOOTH) return;
    if (_title.length() > 0 || _artist.length() > 0) return;
    if (!strcmp(config.station.title, title)) return;
    config.setTitle(title);
}

void BluetoothPlayer::requestStreamInfo(bool force) {
    if (force) {
        sendAT("AT+A2DPRES");
        sendAT("AT+I2SCFG");
    }

    // Outside active playback AT+A2DPDEC may return the module's idle SBC
    // placeholder rather than the codec selected for a real stream.
    if (!playing()) return;
    if (!force && _streamInfoRequested) return;

    _streamInfoRequested = true;
    sendAT("AT+A2DPDEC");
}

void BluetoothPlayer::handleTrackInfo(const char* value) {
    if (!value) return;

    String newTitle;
    String newArtist;
    String newAlbum;
    const char* firstComma = strchr(value, ',');
    if (!firstComma) {
        newTitle = value;
    } else {
        newTitle = String(value).substring(0, firstComma - value);
        const char* secondComma = strchr(firstComma + 1, ',');
        if (!secondComma) {
            newArtist = firstComma + 1;
        } else {
            newArtist = String(firstComma + 1).substring(0, secondComma - firstComma - 1);
            newAlbum = secondComma + 1;
        }
    }
    newTitle.trim();
    newArtist.trim();
    newAlbum.trim();

    // The module reports this placeholder during profile connection and when
    // the remote application supplies no AVRCP metadata. Treat that as a new
    // metadata generation: keeping the previous track here is more misleading
    // than temporarily showing the neutral Playing fallback.
    const bool metadataUnavailable = newTitle.equalsIgnoreCase("Not Provided") ||
                                     (newTitle.length() == 0 &&
                                      newArtist.length() == 0 &&
                                      newAlbum.length() == 0);
    if (metadataUnavailable) {
        const bool hadText = _title.length() > 0 || _artist.length() > 0 ||
                             _album.length() > 0;
        if (_title.length() > 0) _previousSinkTitle = _title;
        _title = "";
        _artist = "";
        _album = "";
        _pendingSinkTitle = "";
        _pendingSinkArtist = "";
        _pendingSinkAlbum = "";
        _posMs = 0;
        _trackMs = 0;
        _metaUpdated = false;
        _sinkMetaFreshAccepted = false;
        if (config.getMode() == PM_BLUETOOTH &&
            (hadText || playing() ||
             (_lastPcmMs != 0 && millis() - _lastPcmMs < 1000U))) {
            config.setTitle(playing()
                                ? (audioActive() ? "Playing"
                                                 : LANG::const_PlReady)
                                : "");
        }
        return;
    }

    if (_moduleRole == BT_ROLE_SINK && !_sinkPlaybackSeen) {
        _pendingSinkTitle = newTitle;
        _pendingSinkArtist = newArtist;
        _pendingSinkAlbum = newAlbum;
        return;
    }

    if (_moduleRole == BT_ROLE_SINK && !_sinkMetaFreshAccepted &&
        _previousSinkTitle.length() > 0 &&
        newTitle == _previousSinkTitle) {
        if (!_sinkStaleMetaLogged) {
            log_i("##[BT]# suppressing previous-session TRACKINFO until a fresh title arrives");
            _sinkStaleMetaLogged = true;
        }
        return;
    }

    if (_moduleRole == BT_ROLE_SINK) _sinkMetaFreshAccepted = true;
    applyTrackInfo(newTitle, newArtist, newAlbum);
}

void BluetoothPlayer::handlePlayStatus(uint32_t status) {
    const int8_t previousPlayStatus = _lastSinkPlayStatus;
    switch (status) {
        case 0:  // stopped
        case 2:  // paused
            // PAUSE normally resumes the same item, so its cached metadata is
            // useful. A real STOP is also emitted by YouTube while replacing
            // one video with another: start a fresh generation there so the
            // previous video's title cannot be restored on the next PLAY.
            if (status == 0 && _moduleRole == BT_ROLE_SINK &&
                previousPlayStatus != 0) {
                beginSinkMetaGeneration(true);
            }
            _lastSinkPlayStatus = static_cast<int8_t>(status);
            _state = BT_STATE_PAUSED;
            _streamInfoRequested = false;
            if (_moduleRole == BT_ROLE_SINK &&
                config.getMode() == PM_BLUETOOTH &&
                strcmp(config.station.title, LANG::const_PlStopped)) {
                // Keep the cached AVRCP metadata so PLAY can restore it
                // immediately, but do not leave the old title visible while
                // the remote stream is stopped or paused.
                config.setTitle(LANG::const_PlStopped);
                display.putRequest(NEWTITLE);
                netserver.requestOnChange(TITLE, 0);
            }
            if (_moduleRole == BT_ROLE_SINK &&
                config.getMode() == PM_BLUETOOTH) {
                display.putRequest(PSTOP);
            }
            break;
        case 1:  // playing
        case 3:  // fast-forward
        case 4:  // rewind
            _lastSinkPlayStatus = static_cast<int8_t>(status);
            _state = BT_STATE_PLAYING;
            activateSinkMetaGeneration();
            publishSinkPlaybackTitle();
            if (_moduleRole == BT_ROLE_SINK &&
                config.getMode() == PM_BLUETOOTH) {
                display.putRequest(PSTART);
            }
            requestStreamInfo();
            break;
        default:
            break;
    }
}

void BluetoothPlayer::handleTrackStatus(const char* value) {
    if (!value) return;

    unsigned long status = 0;
    unsigned long positionMs = 0;
    unsigned long durationMs = 0;
    if (sscanf(value, "%lu,%lu,%lu", &status, &positionMs, &durationMs) != 3) {
        log_w("##[BT]# malformed TRACKSTAT: %s", value);
        return;
    }
    _posMs = static_cast<uint32_t>(positionMs);
    if (durationMs > 0) _trackMs = static_cast<uint32_t>(durationMs);
    handlePlayStatus(static_cast<uint32_t>(status));
}

void BluetoothPlayer::handleSourceScanResult(const char* value) {
    if (!value || !_sourceScanActive) return;

    char fields[5][16] = {};
    const char* cursor = value;
    for (uint8_t field = 0; field < 5; ++field) {
        const char* comma = strchr(cursor, ',');
        if (!comma) {
            if (field == 4) {
                const size_t length = min(strlen(cursor),
                                          sizeof(fields[field]) - 1U);
                memcpy(fields[field], cursor, length);
                fields[field][length] = '\0';
                cursor += strlen(cursor);
                break;
            }
            log_w("##[BT-TX]# malformed scan result: %s", value);
            return;
        }
        const size_t length = min(static_cast<size_t>(comma - cursor),
                                  sizeof(fields[field]) - 1U);
        memcpy(fields[field], cursor, length);
        fields[field][length] = '\0';
        cursor = comma + 1;
    }

    const uint32_t addressType = strtoul(fields[1], nullptr, 10);
    if (addressType != 2 || !validBluetoothAddress(fields[2])) {
        return; // WebUI source pairing currently lists BR/EDR endpoints only.
    }
    const long parsedRssi = strtol(fields[3], nullptr, 10);
    const size_t reportedNameLength = strtoul(fields[4], nullptr, 10);
    const size_t availableNameLength = strlen(cursor);

    // Param7 is an optional six-character hexadecimal Bluetooth Class of
    // Device. A source-mode target must normally advertise the Audio/Video
    // major class (0x04). Keep results that omit/leave the class uncategorized,
    // but hide definite phones, computers and other non-audio device classes.
    // This remains a conservative filter because older speakers do not always
    // include Param7 in their inquiry result.
    uint32_t classOfDevice = 0;
    bool classOfDeviceKnown = false;
    if (reportedNameLength <= availableNameLength) {
        const char* classField = cursor + reportedNameLength;
        if (*classField == ',' && strlen(classField + 1) >= 6U) {
            char classText[7] = {};
            memcpy(classText, classField + 1, 6);
            char* classEnd = nullptr;
            classOfDevice = strtoul(classText, &classEnd, 16);
            classOfDeviceKnown = classEnd == classText + 6;
        }
    }
    if (classOfDeviceKnown) {
        const uint8_t majorDeviceClass =
            static_cast<uint8_t>((classOfDevice >> 8) & 0x1FU);
        if (majorDeviceClass != 0U && majorDeviceClass != 0x04U) {
            log_i("##[BT-TX]# scan device ignored: %s name=%.*s COD=%06lX major=%u (not Audio/Video)",
                  fields[2], static_cast<int>(min(reportedNameLength,
                                                 availableNameLength)),
                  cursor, static_cast<unsigned long>(classOfDevice),
                  static_cast<unsigned>(majorDeviceClass));
            return;
        }
    }

    const size_t copyNameLength = min(
        min(reportedNameLength, availableNameLength),
        sizeof(_sourceScanDevices[0].name) - 1U);

    uint8_t target = _sourceScanDeviceCount;
    for (uint8_t i = 0; i < _sourceScanDeviceCount; ++i) {
        if (strcasecmp(_sourceScanDevices[i].address, fields[2]) == 0) {
            target = i;
            break;
        }
    }
    if (target == _sourceScanDeviceCount) {
        if (_sourceScanDeviceCount >= BT_SOURCE_SCAN_MAX_DEVICES) {
            log_w("##[BT-TX]# scan cache full; ignoring %s", fields[2]);
            return;
        }
        ++_sourceScanDeviceCount;
    }

    btSourceScanDevice_t& device = _sourceScanDevices[target];
    strlcpy(device.address, fields[2], sizeof(device.address));
    device.rssi = static_cast<int16_t>(constrain(parsedRssi, -127L, 0L));
    if (copyNameLength > 0) {
        memcpy(device.name, cursor, copyNameLength);
        device.name[copyNameLength] = '\0';
    } else if (!device.name[0]) {
        strlcpy(device.name, "Unknown device", sizeof(device.name));
    }

    netserver.btSourceScanDeviceToWs(0, device.address, device.name,
                                     device.rssi);
    netserver.btSourceScanStateToWs(0, "scanning", _sourceScanDeviceCount,
                                    nullptr);
    if (classOfDeviceKnown) {
        log_i("##[BT-TX]# scan device: %s RSSI=%d name=%s COD=%06lX",
              device.address, static_cast<int>(device.rssi), device.name,
              static_cast<unsigned long>(classOfDevice));
    } else {
        log_i("##[BT-TX]# scan device: %s RSSI=%d name=%s COD=unknown",
              device.address, static_cast<int>(device.rssi), device.name);
    }
}

void BluetoothPlayer::handleSourceConnectedDevice(const char* value,
                                                   bool hasName) {
    if (!value) return;
    char address[13] = {};
    const char* comma = strchr(value, ',');
    const size_t addressLength = comma
        ? static_cast<size_t>(comma - value) : strlen(value);
    if (addressLength != 12) return;
    memcpy(address, value, 12);
    address[12] = '\0';
    if (!validBluetoothAddress(address)) return;

    const bool addressChanged = !_sourceConnectedAddress[0] ||
        strcasecmp(_sourceConnectedAddress, address) != 0;
    strlcpy(_sourceConnectedAddress, address,
            sizeof(_sourceConnectedAddress));
    if (remoteBatteryLevel() >= 0 && !_remoteBatteryAddress[0]) {
        strlcpy(_remoteBatteryAddress, address,
                sizeof(_remoteBatteryAddress));
    }
    if (hasName && comma && comma[1] != '\0') {
        strlcpy(_sourceConnectedName, comma + 1,
                sizeof(_sourceConnectedName));
    } else {
        const btSourceScanDevice_t* device = findSourceScanDevice(address);
        if (device) {
            strlcpy(_sourceConnectedName, device->name,
                    sizeof(_sourceConnectedName));
        }
    }
    if (addressChanged && _inputUiCommand != INPUT_UI_REFRESH_RSSI) {
        _inputRefreshPending = true;
    }
    // Only +A2DPDEV confirms the connected endpoint. +A2DPCONN merely
    // announces an attempt and must not complete the switch prematurely.
    if (hasName &&
        (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET ||
         _sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS ||
         _sourceSwitchState == SOURCE_SWITCH_WAIT_ROLLBACK)) {
        _sourceSwitchAddressReported = true;
        completeSourceSwitchConnection(address);
    }

    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
}

void BluetoothPlayer::handleInputConnectedDevice(const char* value,
                                                  bool hasName) {
    if (!value) return;
    const char* comma = strchr(value, ',');
    const size_t addressLength = comma
        ? static_cast<size_t>(comma - value) : strlen(value);
    if (addressLength != 12) return;

    char address[13] = {};
    memcpy(address, value, 12);
    if (!validBluetoothAddress(address)) return;
    const bool addressChanged = !_inputConnectedAddress[0] ||
        strcasecmp(_inputConnectedAddress, address) != 0;
    strlcpy(_inputConnectedAddress, address,
            sizeof(_inputConnectedAddress));
    if (remoteBatteryLevel() >= 0 && !_remoteBatteryAddress[0]) {
        strlcpy(_remoteBatteryAddress, address,
                sizeof(_remoteBatteryAddress));
    }
    if (hasName && comma && comma[1]) {
        strlcpy(_inputConnectedName, comma + 1,
                sizeof(_inputConnectedName));
    }
    if (addressChanged && _inputUiCommand != INPUT_UI_REFRESH_RSSI) {
        _inputRefreshPending = true;
    }
    publishInputUiSnapshot(0);
}

void BluetoothPlayer::handleA2dpStatus(uint32_t status) {
    const uint32_t previousStatus = _lastA2dpStatus;
    const bool sinkConnectionEstablished =
        _moduleRole == BT_ROLE_SINK && status >= 3U && status <= 5U &&
        (previousStatus < 3U || previousStatus > 5U);
    const bool manualSourceConnectFailed =
        _moduleRole == BT_ROLE_SOURCE && status == 1U &&
        previousStatus == 2U && _sourceConnectPending &&
        _sourceManualConnectPending;
    const bool switchConnectionPending =
        _sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET ||
        _sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS;
    _lastA2dpStatus = status;
    if (_moduleRole == BT_ROLE_SOURCE) {
        if (status >= 2 && status <= 5) {
            _sourceReconnectAfterEnable = false;
            _sourceReconnectNextActionMs = 0;
        }
        if (status < 3 || status > 5) {
            _sourceAudioPathReady = false;
            _sourceAudioEnablePending = false;
            _sourceAudioEnableDeadlineMs = 0;
            _sourceAudioEnableAttempts = 0;
            _nextSourceAudioEnableMs = 0;
        } else if (previousStatus < 3 || previousStatus > 5) {
            _sourceAudioPathReady = false;
            _sourceAudioEnablePending = false;
            _sourceAudioEnableDeadlineMs = 0;
            _sourceAudioEnableAttempts = 0;
            _nextSourceAudioEnableMs = millis() + 250U;
        }
    }
    switch (status) {
        case 1:  // standby / no A2DP connection
            if (_moduleRole == BT_ROLE_SINK && previousStatus != 1) {
                beginSinkMetaGeneration(true);
            } else if (connected()) {
                clearMeta();
            }
            _state = BT_STATE_DISC;
            _streamInfoRequested = false;
            _remoteRssi = -127;
            _remoteRssiAddress[0] = '\0';
            if (_moduleRole == BT_ROLE_SINK) {
                if (_sinkControlQueue) xQueueReset(_sinkControlQueue);
                __atomic_store_n(&_sinkVolumePending, false,
                                 __ATOMIC_RELEASE);
                _inputConnectedAddress[0] = '\0';
                _inputConnectedName[0] = '\0';
                _inputCodec[0] = '\0';
                _inputCodecSampleRate = 0;
            }
            if (_moduleRole == BT_ROLE_SOURCE && previousStatus >= 2 &&
                previousStatus <= 5) {
                _sourceConnectPending = false;
                _sourceManualConnectPending = false;
                _sourceConnectDeadlineMs = 0;
                _sourceConnectedAddress[0] = '\0';
                _sourceConnectedName[0] = '\0';
                _streamCodec[0] = '\0';
                _sourceCodecSampleRate = 0;
                if (manualSourceConnectFailed && !switchConnectionPending) {
                    netserver.btSourceScanStateToWs(
                        0, "error", _sourceScanDeviceCount,
                        "Connection failed: the selected device did not accept A2DP audio. Choose a speaker/headset in pairing mode.");
                    log_w("##[BT-TX]# manual source connection rejected or unsupported by remote device");
                } else if (!switchConnectionPending) {
                    netserver.btSourceScanStateToWs(
                        0, "disconnected", _sourceScanDeviceCount,
                        "Speaker disconnected");
                }
            }
            break;
        case 2:  // connecting
            break;
        case 3:  // connected
            if (previousStatus < 3 || previousStatus > 5) {
                _inputRefreshPending = true;
            }
            if (!switchConnectionPending) {
                _sourceConnectPending = false;
                _sourceManualConnectPending = false;
                _sourceConnectDeadlineMs = 0;
            }
            if (_moduleRole == BT_ROLE_SOURCE && previousStatus < 3 &&
                !switchConnectionPending) {
                netserver.btSourceScanStateToWs(
                    0, "connected", _sourceScanDeviceCount,
                    "Speaker connected");
            }
            if (!playing()) _state = BT_STATE_CONNECTED;
            break;
        case 4:  // active A2DP transport
            if (previousStatus < 3 || previousStatus > 5) {
                _inputRefreshPending = true;
            }
            if (_moduleRole == BT_ROLE_SINK &&
                (previousStatus != 4 || !_inputCodec[0])) {
                _streamInfoRequested = false;
            }
            if (!switchConnectionPending) {
                _sourceConnectPending = false;
                _sourceManualConnectPending = false;
                _sourceConnectDeadlineMs = 0;
            }
            if (!connected()) _state = BT_STATE_CONNECTED;
            break;
        case 5:  // firmware also reports this around media start/pause
            if (previousStatus < 3 || previousStatus > 5) {
                _inputRefreshPending = true;
            }
            if (!switchConnectionPending) {
                _sourceConnectPending = false;
                _sourceManualConnectPending = false;
                _sourceConnectDeadlineMs = 0;
            }
            if (!connected()) _state = BT_STATE_CONNECTED;
            break;
        default:
            break;
    }
    if (switchConnectionPending) {
        if (status == 1U && previousStatus >= 2U &&
            previousStatus <= 5U) {
            if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET) {
                queueSourceSwitchRollback("A2DP returned to standby");
            } else if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS) {
                _sourceSwitchState = SOURCE_SWITCH_IDLE;
                _sourceSwitchDeadlineMs = 0;
                _sourceConnectPending = false;
                _sourceManualConnectPending = false;
                _sourceConnectDeadlineMs = 0;
                publishSourceUiError(
                    _sourceSwitchClientId,
                    "New speaker failed; previous speaker could not be restored automatically");
            }
        } else if (status >= 3U && status <= 5U &&
                   _sourceSwitchAddressReported) {
            completeSourceSwitchConnection(_sourceConnectedAddress);
        }
    }
    if (_sourceSwitchState == SOURCE_SWITCH_WAIT_ROLLBACK &&
        status >= 3U && status <= 5U && _sourceSwitchAddressReported) {
        completeSourceSwitchConnection(_sourceConnectedAddress);
    }
    if (sinkConnectionEstablished && config.getMode() == PM_BLUETOOTH &&
        !playing()) {
        // A2DP is now genuinely connected, but no playback metadata has been
        // validated yet. Replace the waiting text with the same localized
        // idle acknowledgement used by the normal player.
        setFallbackTitle(LANG::const_PlReady);
    }
    netserver.btSourceStateToWs(
        0, moduleRoleName(_moduleRole),
        static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
        _sourceConnectPending, _sourceConnectedAddress,
        _sourceConnectedName);
    if (_moduleRole == BT_ROLE_SINK && previousStatus != status) {
        publishInputUiSnapshot(0);
    }
}

void BluetoothPlayer::parseLine(char* line) {
    log_d("##[BT]# RX: %s", line);

    // The normal parser intentionally keeps routine OK/ERROR and unsupported
    // query replies out of the INFO log. Mirror only the response to a command
    // entered manually through the laboratory USB console.
    if (_consoleCommandPending) {
        Serial.printf("##[BT-LAB]# RX: %s\r\n", line);
        if (!strcmp(line, "OK") || !strcmp(line, "ERROR")) {
            _consoleCommandPending = false;
        }
    }

    if (aliasSyncBusy() &&
        (!strcmp(line, "OK") || !strcmp(line, "ERROR"))) {
        const bool accepted = !strcmp(line, "OK");
        _aliasSyncDeadlineMs = 0;

        if (_aliasSyncState == ALIAS_SYNC_WAIT_SET_RESPONSE) {
            if (!accepted) {
                retryOrFailAliasSync("module rejected AT+NAME");
            } else {
                _aliasSyncState = ALIAS_SYNC_WAIT_VERIFY_DELAY;
                _aliasSyncNextActionMs = millis() + BT_ALIAS_VERIFY_DELAY_MS;
            }
            return;
        }

        if (_aliasSyncState == ALIAS_SYNC_WAIT_VERIFY_RESPONSE) {
            if (accepted && _moduleName[0] &&
                strcmp(config.store.btAlias, _moduleName) == 0) {
                [[maybe_unused]] const uint8_t attempts = _aliasSyncAttempts;
                _aliasSyncState = ALIAS_SYNC_IDLE;
                _aliasSyncAttempts = 0;
                _aliasSyncNextActionMs = 0;
                log_i("##[BT-NAME]# alias verified: '%s' after %u attempt(s)",
                      _moduleName, (unsigned)attempts);
                publishInputUiSnapshot(0, "Bluetooth alias applied and verified.");
            } else {
                retryOrFailAliasSync(accepted ? "read-back mismatch"
                                               : "AT+NAME query rejected");
            }
            return;
        }
    }

    if (inputUiBusy() &&
        (!strcmp(line, "OK") || !strcmp(line, "ERROR"))) {
        const bool accepted = !strcmp(line, "OK");
        const inputUiCommand_e completed = _inputUiCommand;
        _inputUiCommandDeadlineMs = 0;

        if (completed == INPUT_UI_REFRESH_PAIR) {
            // DEVSTAT is documented and exposes BR/EDR discoverability in
            // bit 1. Continue with PLIST even if that read failed.
            if (!accepted) _moduleDiscoverable = -1;
            memset(_inputPairedRefreshDevices, 0,
                   sizeof(_inputPairedRefreshDevices));
            _inputPairedRefreshDeviceCount = 0;
            _inputPairedRefreshSawEnd = false;
            log_i("##[BT-PLIST]# refresh started: role=%s cached=%u",
                  moduleRoleName(_moduleRole),
                  (unsigned)_inputPairedDeviceCount);
            _inputUiCommand = INPUT_UI_REFRESH_LIST;
            _inputUiCommandDeadlineMs = millis() + BT_INPUT_COMMAND_TIMEOUT_MS;
            sendAT("AT+PLIST");
            return;
        }

        if (completed == INPUT_UI_REFRESH_LIST) {
            if (accepted && _inputPairedRefreshSawEnd) {
                const bool inputRole = _moduleRole == BT_ROLE_SINK;
                memset(_inputPairedDevices, 0,
                       sizeof(_inputPairedDevices));
                memcpy(_inputPairedDevices, _inputPairedRefreshDevices,
                       sizeof(_inputPairedDevices));
                _inputPairedDeviceCount =
                    _inputPairedRefreshDeviceCount;
                log_i("##[BT-PLIST]# refresh committed: role=%s count=%u",
                      moduleRoleName(_moduleRole),
                      (unsigned)_inputPairedDeviceCount);
                publishInputPairedList(_inputUiClientId);
                if (_moduleRole == BT_ROLE_SOURCE) {
                    // A scan result may have been hidden by the browser's old
                    // paired-device cache. Replay the source cache only after
                    // the authoritative PLIST commit so newly-unpaired/lost
                    // records immediately reappear as pairable devices.
                    publishSourceUiSnapshot(_inputUiClientId);
                }
                _inputRssiProbeIndex = 0;
                _inputRssiProbeMatched = false;
                _inputRssiProbeAddress[0] = '\0';
                if (beginNextInputRssiProbe()) {
                    publishInputUiSnapshot(
                        _inputUiClientId,
                        "Identifying the connected device and refreshing RSSI...");
                    return;
                }
                finishInputUiCommand(
                    true, inputRole ? "Bluetooth input settings refreshed."
                                    : "Paired-device list refreshed.");
            } else {
                log_w("##[BT-PLIST]# refresh rejected/incomplete: accepted=%u end=%u; retaining cached=%u",
                      accepted ? 1U : 0U,
                      _inputPairedRefreshSawEnd ? 1U : 0U,
                      (unsigned)_inputPairedDeviceCount);
                netserver.btInputPairedStateToWs(
                    _inputUiClientId, "error", _inputPairedDeviceCount,
                    "Could not read a complete paired-device list; previous data was retained.");
                finishInputUiCommand(
                    false,
                    "Paired-device refresh incomplete; previous data retained.");
            }
            return;
        }

        if (completed == INPUT_UI_REFRESH_RSSI) {
            if (_inputRssiProbeMatched) {
                const uint8_t clientId = _inputUiClientId;
                finishInputUiCommand(
                    true,
                    "Connected Bluetooth device and RSSI refreshed.");
                if (_moduleRole == BT_ROLE_SOURCE) {
                    publishSourceUiSnapshot(clientId);
                }
            } else if (beginNextInputRssiProbe()) {
                // Query the next paired address. A zero RSSI or ERROR means
                // that the preceding record has no live BR/EDR link.
            } else {
                const uint8_t clientId = _inputUiClientId;
                _remoteRssi = -127;
                _remoteRssiAddress[0] = '\0';
                finishInputUiCommand(
                    true,
                    "Paired devices refreshed; live RSSI is unavailable.");
                if (_moduleRole == BT_ROLE_SOURCE) {
                    publishSourceUiSnapshot(clientId);
                }
            }
            return;
        }

        if (completed == INPUT_UI_SET_DISCOVERABLE) {
            if (!accepted) {
                finishInputUiCommand(false,
                                     "The module rejected the discoverability setting.");
            } else {
                _inputUiCommand = INPUT_UI_VERIFY_DISCOVERABLE;
                _inputUiCommandDeadlineMs =
                    millis() + BT_INPUT_COMMAND_TIMEOUT_MS;
                sendAT("AT+DEVSTAT");
            }
            return;
        }

        if (completed == INPUT_UI_VERIFY_DISCOVERABLE) {
            const bool verified = accepted && _moduleDiscoverable >= 0 &&
                ((_moduleDiscoverable != 0) == _requestedDiscoverable);
            finishInputUiCommand(
                verified,
                verified
                    ? (_requestedDiscoverable
                           ? "Bluetooth input is discoverable."
                           : "Bluetooth input is hidden from new devices.")
                    : "Discoverability read-back did not match.");
            return;
        }

        if (completed == INPUT_UI_CANCEL_RECONNECT_FOR_FORGET) {
            if (!accepted) {
                // A reconnect can fall back to standby before A2DPDISC is
                // parsed, in which case some firmware revisions answer ERROR.
                // The subsequent address-specific PLIST deletion is still the
                // authoritative operation, so try it once.
                log_w("##[BT]# reconnect cancellation rejected before Forget; trying deletion anyway");
            }

            const char* connectedAddress = _moduleRole == BT_ROLE_SOURCE
                                               ? _sourceConnectedAddress
                                               : _inputConnectedAddress;
            if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5 &&
                (!connectedAddress[0] ||
                 strcasecmp(_inputForgetAddress, connectedAddress) == 0)) {
                netserver.btInputPairedStateToWs(
                    _inputUiClientId, "error", _inputPairedDeviceCount,
                    "The device connected before it could be removed; disconnect it first.");
                finishInputUiCommand(
                    false,
                    "The device connected before it could be removed; disconnect it first.");
            } else {
                const uint8_t clientId = _inputUiClientId;
                char address[sizeof(_inputForgetAddress)];
                strlcpy(address, _inputForgetAddress, sizeof(address));
                beginForgetInputPairedDevice(address, clientId);
            }
            return;
        }

        if (completed == INPUT_UI_FORGET_DEVICE) {
            if (!accepted) {
                netserver.btInputPairedStateToWs(
                    _inputUiClientId, "error", _inputPairedDeviceCount,
                    "The module rejected the forget command.");
                finishInputUiCommand(false,
                                     "Paired device could not be removed.");
            } else {
                const uint8_t clientId = _inputUiClientId;
                _inputUiCommand = INPUT_UI_COMMAND_NONE;
                _inputUiCommandDeadlineMs = 0;
                _inputForgetAddress[0] = '\0';
                _inputRefreshPending = true;
                netserver.btInputPairedStateToWs(
                    clientId, "forgetting", _inputPairedDeviceCount,
                    "Device removed; refreshing the paired list...");
                publishInputUiSnapshot(clientId,
                                       "Paired device removed.");
            }
            return;
        }
    }

    if (_sinkControlInFlight != BT_SINK_CONTROL_NONE &&
        (!strcmp(line, "OK") || !strcmp(line, "ERROR"))) {
        const btSinkControl_e completed = _sinkControlInFlight;
        const bool accepted = !strcmp(line, "OK");
        _sinkControlInFlight = BT_SINK_CONTROL_NONE;
        _sinkControlDeadlineMs = 0;
        if (accepted) {
            log_i("##[BT-CTRL]# module accepted AVRCP command: %s",
                  sinkControlName(completed));
        } else {
            log_w("##[BT-CTRL]# module rejected AVRCP command: %s",
                  sinkControlName(completed));
        }
        return;
    }

    // serviceSourceAudioEnable() owns a single guarded AT transaction. The
    // startup, health, WebUI and console paths cannot overlap it, so this
    // terminal response belongs to AT+A2DPAUDIO=1.
    if (_sourceAudioEnablePending &&
        (!strcmp(line, "OK") || !strcmp(line, "ERROR"))) {
        const bool accepted = !strcmp(line, "OK");
        _sourceAudioEnablePending = false;
        _sourceAudioEnableDeadlineMs = 0;
        if (accepted) {
            _sourceAudioPathReady = true;
            _nextSourceAudioEnableMs = 0;
            log_i("##[BT-TX]# A2DP source audio path accepted after %u attempt(s)",
                  (unsigned)_sourceAudioEnableAttempts);
        } else {
            _sourceAudioPathReady = false;
            _nextSourceAudioEnableMs =
                millis() + BT_SOURCE_AUDIO_ENABLE_RETRY_MS;
            log_w("##[BT-TX]# A2DP source audio enable rejected; retry scheduled");
        }
        return;
    }

    if (_sourceUiCommand != SOURCE_UI_COMMAND_NONE &&
        (!strcmp(line, "OK") || !strcmp(line, "ERROR"))) {
        const sourceUiCommand_e completedCommand = _sourceUiCommand;
        const uint8_t clientId = _sourceUiCommandClientId;
        const bool failed = !strcmp(line, "ERROR");
        _sourceUiCommand = SOURCE_UI_COMMAND_NONE;
        _sourceUiCommandDeadlineMs = 0;

        if (completedCommand == SOURCE_UI_COMMAND_SET_BT_ENABLE) {
            if (failed) {
                retryOrFailSourceOutputRoute(
                    "module rejected AT+BTEN setting");
            } else {
                _sourceRouteVerifyPending = true;
                _sourceRouteApplyPending = true;
                _sourceRouteNextActionMs =
                    millis() + BT_SOURCE_ROUTE_VERIFY_DELAY_MS;
            }
            publishSourceUiSnapshot(clientId);
            return;
        }

        if (completedCommand == SOURCE_UI_COMMAND_VERIFY_BT_ENABLE) {
            const bool verified = !failed &&
                _moduleBluetoothEnabled >= 0 &&
                ((_moduleBluetoothEnabled != 0) ==
                 _sourceBluetoothTarget);
            if (!verified) {
                retryOrFailSourceOutputRoute(
                    failed ? "AT+BTEN read-back rejected"
                           : "BTEN read-back mismatch");
            } else {
                const bool desiredNow =
                    _moduleRole != BT_ROLE_SOURCE ||
                    sourceOutputMode() != BT_SOURCE_OUTPUT_LOCAL_ONLY;
                _sourceRouteApplyPending =
                    desiredNow != _sourceBluetoothTarget;
                _sourceRouteVerifyPending = false;
                _sourceRouteAttempts = 0;
                _sourceRouteNextActionMs = millis();
                if (!_sourceBluetoothTarget) {
                    // A disabled Bluetooth radio cannot have a live A2DP or
                    // AVRCP link. Normalize the cached UI state even if this
                    // firmware revision omits the corresponding unsolicited
                    // disconnect events after BTEN=0.
                    _lastAvrcpStatus = 1;
                    handleA2dpStatus(1);
                }
                log_i("##[BT-TX]# module Bluetooth verified %s for source output route",
                      _sourceBluetoothTarget ? "enabled" : "disabled");
            }
            publishSourceUiSnapshot(clientId);
            return;
        }

        if (completedCommand == SOURCE_UI_COMMAND_RECONNECT_LAST) {
            _sourceReconnectAfterEnable = false;
            _sourceReconnectNextActionMs = 0;
            if (failed) {
                _sourceConnectPending = false;
                _sourceManualConnectPending = false;
                _sourceConnectDeadlineMs = 0;
                publishSourceUiError(
                    clientId,
                    "The last paired speaker could not be reconnected; scan or connect it manually");
            } else {
                netserver.btSourceScanStateToWs(
                    clientId, "connecting", _sourceScanDeviceCount,
                    "Connecting to the last paired speaker...");
                netserver.btSourceStateToWs(
                    clientId, moduleRoleName(_moduleRole),
                    static_cast<uint8_t>(_lastA2dpStatus),
                    _sourceScanActive, _sourceConnectPending,
                    _sourceConnectedAddress, _sourceConnectedName);
            }
            return;
        }

        if (completedCommand ==
            SOURCE_UI_COMMAND_CANCEL_RECONNECT_FOR_SCAN) {
            if (failed) {
                // Some firmware reports ERROR when the transient connecting
                // state has already returned to standby. A manual scan is
                // still the correct next operation, so try it once and let
                // its own response provide the authoritative result.
                log_w("##[BT-TX]# reconnect cancellation was rejected; trying requested scan anyway");
            }
            if (_moduleRole != BT_ROLE_SOURCE || !_startupProbeComplete ||
                (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5)) {
                publishSourceUiError(
                    clientId,
                    "A speaker connected before the scan could start");
            } else {
                beginSourceScan(_sourceScanRequestedSeconds, clientId);
            }
            return;
        }

        if (completedCommand == SOURCE_UI_COMMAND_SWITCH_DISCONNECT) {
            const bool cancellingTarget =
                _sourceSwitchState == SOURCE_SWITCH_DISCONNECTING_TARGET;
            if ((_sourceSwitchState != SOURCE_SWITCH_DISCONNECTING &&
                 !cancellingTarget) ||
                sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
                return; // Output route changed while DISC was in flight.
            }
            if (failed) {
                if (_lastA2dpStatus == 1) {
                    _sourceSwitchState = cancellingTarget
                        ? SOURCE_SWITCH_WAIT_ROLLBACK
                        : SOURCE_SWITCH_WAIT_STANDBY;
                    _sourceSwitchDeadlineMs = millis() + 7000U;
                } else {
                    _sourceSwitchState = SOURCE_SWITCH_IDLE;
                    _sourceSwitchDeadlineMs = 0;
                    publishSourceUiError(
                        clientId,
                        cancellingTarget
                            ? "Could not cancel the new connection; reconnect the previous speaker manually"
                            : "Bluetooth module rejected the disconnect; current speaker was kept");
                }
            } else {
                _sourceSwitchState = cancellingTarget
                    ? SOURCE_SWITCH_WAIT_TARGET_STANDBY
                    : SOURCE_SWITCH_WAIT_STANDBY;
                _sourceSwitchDeadlineMs = millis() + 7000U;
                _sourceSwitchNextStatusQueryMs = millis() + 1000U;
            }
            return;
        }

        if (completedCommand == SOURCE_UI_COMMAND_SWITCH_QUERY_STATUS) {
            if (failed) {
                log_w("##[BT-TX]# switch A2DP status query rejected; waiting for state event");
            }
            return;
        }

        if (completedCommand == SOURCE_UI_COMMAND_CONNECT &&
            _sourceSwitchState == SOURCE_SWITCH_WAIT_ROLLBACK) {
            // The target already failed on an unsolicited A2DP state edge.
            // Consume its late terminal response before starting rollback.
            return;
        }

        if (completedCommand == SOURCE_UI_COMMAND_CONNECT &&
            (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET ||
             _sourceSwitchState == SOURCE_SWITCH_CONNECTING_PREVIOUS)) {
            if (failed) {
                if (_sourceSwitchState == SOURCE_SWITCH_CONNECTING_TARGET) {
                    queueSourceSwitchRollback("module rejected target connection");
                } else {
                    _sourceSwitchState = SOURCE_SWITCH_IDLE;
                    _sourceSwitchDeadlineMs = 0;
                    _sourceConnectPending = false;
                    _sourceManualConnectPending = false;
                    _sourceConnectDeadlineMs = 0;
                    publishSourceUiError(
                        clientId,
                        "New speaker failed; previous speaker could not be restored automatically");
                }
            }
            return;
        }

        if (failed) {
            if (completedCommand == SOURCE_UI_COMMAND_SCAN ||
                completedCommand == SOURCE_UI_COMMAND_STOP_SCAN) {
                finishSourceScan("error",
                                 "Bluetooth module rejected the scan command");
            } else {
                if (completedCommand == SOURCE_UI_COMMAND_CONNECT) {
                    if (_lastA2dpStatus >= 3 && _lastA2dpStatus <= 5) {
                        // A connection event can precede the command's final
                        // response. Do not erase a confirmed live endpoint.
                        log_w("##[BT-TX]# connect reply rejected after A2DP connected; retaining live device");
                        publishSourceUiSnapshot(clientId);
                        return;
                    }
                    _sourceConnectPending = false;
                    _sourceManualConnectPending = false;
                    _sourceConnectDeadlineMs = 0;
                    _sourceConnectedAddress[0] = '\0';
                    _sourceConnectedName[0] = '\0';
                }
                publishSourceUiError(
                    clientId,
                    completedCommand == SOURCE_UI_COMMAND_DISCONNECT
                        ? "Bluetooth module rejected the disconnect command"
                        : "Bluetooth module rejected the connection request");
            }
        } else if (completedCommand == SOURCE_UI_COMMAND_STOP_SCAN) {
            finishSourceScan("cancelled", "Scan stopped");
        } else {
            netserver.btSourceStateToWs(
                clientId, moduleRoleName(_moduleRole),
                static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
                _sourceConnectPending, _sourceConnectedAddress,
                _sourceConnectedName);
        }
    }

    if (!strncmp(line, "+BTEN=", 6)) {
        _moduleBluetoothEnabled =
            parseUnsignedValue(line) != 0 ? 1 : 0;
        log_i("##[BT-TX]# module Bluetooth radio: %s",
              _moduleBluetoothEnabled ? "enabled" : "disabled");
        const bool desiredBluetooth =
            _moduleRole != BT_ROLE_SOURCE ||
            sourceOutputMode() != BT_SOURCE_OUTPUT_LOCAL_ONLY;
        if (_sourceUiCommand != SOURCE_UI_COMMAND_SET_BT_ENABLE &&
            _sourceUiCommand != SOURCE_UI_COMMAND_VERIFY_BT_ENABLE &&
            ((_moduleBluetoothEnabled != 0) != desiredBluetooth)) {
            _sourceRouteApplyPending = true;
            _sourceRouteNextActionMs = millis();
        }
        return;
    }
    if (!strncmp(line, "+PAIR=", 6)) {
        _moduleDiscoverable = parseUnsignedValue(line) != 0 ? 1 : 0;
        publishInputUiSnapshot(0);
        return;
    }
    if (!strncmp(line, "+RSSI=", 6)) {
        handleRemoteRssi(line + 6);
        return;
    }
    if (!strncmp(line, "+PLIST=", 7)) {
        handlePairedListEntry(line + 7);
        return;
    }
    if (!strncmp(line, "+TRACKINFO=", 11)) {
        handleTrackInfo(line + 11);
        return;
    }
    if (!strncmp(line, "+TRACKSTAT=", 11)) {
        handleTrackStatus(line + 11);
        return;
    }
    if (!strncmp(line, "+PLAYSTAT=", 10)) {
        handlePlayStatus(parseUnsignedValue(line));
        return;
    }
    if (!strncmp(line, "+A2DPSTAT=", 10)) {
        const uint32_t status = parseUnsignedValue(line);
        log_i("##[BT]# A2DP state: %u", (unsigned)status);
        handleA2dpStatus(status);
        return;
    }
    if (!strncmp(line, "+AVRCPSTAT=", 11)) {
        _lastAvrcpStatus = parseUnsignedValue(line);
        log_i("##[BT]# AVRCP state: %u", (unsigned)_lastAvrcpStatus);
        if (_moduleRole == BT_ROLE_SINK && _lastAvrcpStatus != 3 &&
            _sinkControlQueue) {
            xQueueReset(_sinkControlQueue);
            __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
        }
        return;
    }
    if (!strncmp(line, "+HFPSTAT=", 9)) {
        _lastHfpStatus = parseUnsignedValue(line);
        if (_lastHfpStatus < 3) {
            __atomic_store_n(&_remoteBatteryLevel, static_cast<int8_t>(-1),
                             __ATOMIC_RELEASE);
            _remoteBatteryAddress[0] = '\0';
        }
        log_i("##[BT]# HFP state: %u", (unsigned)_lastHfpStatus);
        publishInputUiSnapshot(0);
        netserver.btSourceStateToWs(
            0, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        return;
    }
    if (!strncmp(line, "+HFPBATT=", 9)) {
        const int battery = constrain(
            static_cast<int>(parseUnsignedValue(line)), 0, 5);
        if (!_remoteBatteryAddress[0]) {
            const char* activeAddress =
                _moduleRole == BT_ROLE_SOURCE ? _sourceConnectedAddress :
                _moduleRole == BT_ROLE_SINK ? _inputConnectedAddress : nullptr;
            if (activeAddress && activeAddress[0]) {
                strlcpy(_remoteBatteryAddress, activeAddress,
                        sizeof(_remoteBatteryAddress));
            }
        }
        __atomic_store_n(&_remoteBatteryLevel,
                         static_cast<int8_t>(battery), __ATOMIC_RELEASE);
        log_i("##[BT]# remote HFP battery level: %d/5 device=%s role=%s",
              battery,
              _remoteBatteryAddress[0] ? _remoteBatteryAddress : "unknown",
              moduleRoleName(_moduleRole));
        publishInputUiSnapshot(0);
        netserver.btSourceStateToWs(
            0, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        return;
    }
    if (!strncmp(line, "+A2DPDEC=", 9)) {
        static const char* const codecNames[] = {
            "unknown", "SBC", "AAC", "LDAC", "aptX", "aptX HD", "aptX Adaptive"
        };
        const uint32_t codec = parseUnsignedValue(line);
        const char* name = codec < (sizeof(codecNames) / sizeof(codecNames[0]))
                               ? codecNames[codec]
                               : codecNames[0];
        strlcpy(_streamCodec, name, sizeof(_streamCodec));
        strlcpy(_inputCodec, name, sizeof(_inputCodec));
        _streamBits = BT_I2S_BITS;
        log_i("##[BT]# A2DP decoder: %s (%u)%s", name, (unsigned)codec,
              playing() ? "" : " [idle/not streaming]");
        publishInputUiSnapshot(0);
        display.putRequest(DBITRATE);
        return;
    }
    if (!strncmp(line, "+A2DPENC=", 9)) {
        static const char* const encoderNames[] = {
            "unknown", "SBC", "aptX", "aptX HD", "aptX Adaptive"
        };
        const uint32_t codec = parseUnsignedValue(line);
        const char* name = codec < (sizeof(encoderNames) / sizeof(encoderNames[0]))
                               ? encoderNames[codec]
                               : encoderNames[0];
        strlcpy(_streamCodec, name, sizeof(_streamCodec));
        log_i("##[BT-TX]# A2DP encoder: %s (%u)", name, (unsigned)codec);
        display.putRequest(DBITRATE);
        return;
    }
    if (!strncmp(line, "+A2DPSR=", 8)) {
        // This is the radio codec rate. The PCM bridge remains fixed at 48 kHz
        // because A2DP resampling is enabled in the module.
        _inputCodecSampleRate = parseUnsignedValue(line);
        log_i("##[BT]# A2DP codec sample rate: %u (I2S output fixed at %u)",
              (unsigned)_inputCodecSampleRate, (unsigned)BT_I2S_SAMPLE_RATE);
        publishInputUiSnapshot(0);
        display.putRequest(DBITRATE);
        return;
    }
    if (!strncmp(line, "+I2SCFG=", 8)) {
        const uint32_t value = parseUnsignedValue(line);
        _moduleI2sCfg = static_cast<int16_t>(value);
        const bool validSourceLabCfg = _moduleRole == BT_ROLE_SOURCE &&
                                       (value == 1 || value == 3);
        const bool validSinkCfg = _moduleRole == BT_ROLE_SINK &&
                                  value == BT1058_EXPECTED_I2SCFG;
        if (_moduleRole != BT_ROLE_UNKNOWN && !validSourceLabCfg && !validSinkCfg) {
            log_w("##[BT]# module I2SCFG=%u, expected %u for %s mode",
                  (unsigned)value,
                  (unsigned)(_moduleRole == BT_ROLE_SOURCE
                                 ? BT1058_EXPECTED_SOURCE_I2SCFG
                                 : BT1058_EXPECTED_I2SCFG),
                  _moduleRole == BT_ROLE_SOURCE ? "source" : "sink");
        } else {
            log_i("##[BT]# module I2S configuration: %u%s",
                  (unsigned)value,
                  _moduleRole == BT_ROLE_SOURCE
                      ? (value == 1 ? " (module master / ESP slave)"
                                    : " (module slave / ESP master)")
                      : "");
        }
        return;
    }
    if (!strncmp(line, "+AUXCFG=", 8)) {
        const uint32_t value = parseUnsignedValue(line);
        _moduleAuxCfg = static_cast<int16_t>(value);
        log_i("##[BT]# module audio input: %u", (unsigned)value);
        return;
    }
    if (!strncmp(line, "+A2DPRES=", 9)) {
        const uint32_t value = parseUnsignedValue(line);
        if (value != BT1058_EXPECTED_A2DPRES) {
            log_w("##[BT]# module A2DPRES=%u, expected %u for fixed-rate I2S",
                  (unsigned)value, (unsigned)BT1058_EXPECTED_A2DPRES);
        }
        return;
    }
    if (!strncmp(line, "+PROFILE=", 9)) {
        _moduleProfile = static_cast<int32_t>(parseUnsignedValue(line));
        log_i("##[BT]# module profile bitmap: %ld", (long)_moduleProfile);
        return;
    }
    if (!strncmp(line, "+NAME=", 6)) {
        const char* reportedName = line + 6;
        if (_nameBeforeRoleChange[0] &&
            strcmp(_nameBeforeRoleChange, reportedName) != 0) {
            log_w("##[BT-NAME]# module name changed across role transition: '%s' -> '%s'",
                  _nameBeforeRoleChange, reportedName);
        } else {
            log_i("##[BT-NAME]# module BR/EDR name: %s", reportedName);
        }
        strlcpy(_moduleName, reportedName, sizeof(_moduleName));
        if (!config.store.btAlias[0] && _moduleName[0]) {
            config.saveValue(config.store.btAlias, _moduleName,
                             sizeof(config.store.btAlias));
            log_i("##[BT-NAME]# adopted initial module name as saved alias: '%s'",
                  config.store.btAlias);
            publishInputUiSnapshot(0, "Module name loaded as the Bluetooth alias.");
        }
        return;
    }
    if (!strncmp(line, "+AVRCPCFG=", 10)) {
        _moduleAvrcpCfg = static_cast<int16_t>(parseUnsignedValue(line));
        log_i("##[BT]# AVRCP configuration bitmap: %d%s",
              (int)_moduleAvrcpCfg,
              (_moduleAvrcpCfg & 1) ? " (metadata enabled)" : " (metadata disabled)");
        return;
    }
    if (!strncmp(line, "+STAT=", 6)) {
        unsigned long dev = 0, spp = 0, gatt = 0, hfp = 0;
        unsigned long a2dp = 0, avrcp = 0, bis = 0, cis = 0;
        const int count = sscanf(line + 6, "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
                                 &dev, &spp, &gatt, &hfp, &a2dp, &avrcp, &bis, &cis);
        if (count >= 1) _moduleDiscoverable = (dev & (1UL << 1)) ? 1 : 0;
        if (count >= 5) handleA2dpStatus(static_cast<uint32_t>(a2dp));
        if (count >= 6) _lastAvrcpStatus = static_cast<uint32_t>(avrcp);
        if (_moduleRole == BT_ROLE_SINK) publishInputUiSnapshot(0);
        return;
    }
    if (!strncmp(line, "+DEVSTAT=", 9)) {
        const uint32_t dev = parseUnsignedValue(line);
        _moduleDiscoverable = (dev & (1UL << 1)) ? 1 : 0;
        publishInputUiSnapshot(0);
        return;
    }
    if (!strncmp(line, "+MODCFG=", 8)) {
        const uint32_t role = parseUnsignedValue(line);
        const btModuleRole_e reportedRole =
            role == 0 ? BT_ROLE_SINK
                      : role == 1 ? BT_ROLE_SOURCE : BT_ROLE_UNKNOWN;
        __atomic_store_n(&_moduleRole, reportedRole, __ATOMIC_RELEASE);
        if (!roleTransitionBusy()) {
            __atomic_store_n(&_requestedRole, reportedRole,
                             __ATOMIC_RELEASE);
        }
        _sourceAudioPathReady = false;
        _sourceAudioEnablePending = false;
        _sourceAudioEnableDeadlineMs = 0;
        _sourceAudioEnableAttempts = 0;
        _nextSourceAudioEnableMs = 0;
        log_i("##[BT]# module role: %s (%u)",
              _moduleRole == BT_ROLE_SINK ? "sink" :
              _moduleRole == BT_ROLE_SOURCE ? "source" : "unknown",
              (unsigned)role);
        netserver.btSourceStateToWs(
            0, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        if (_moduleRole == BT_ROLE_SOURCE && bridgeRunningOrPending()) {
            stopBridge();
            log_i("##[BT-TX]# receive bridge stopped after source-role detection");
        }
        if (_moduleRole != BT_ROLE_SOURCE && (sourceToneRunning() || sourceRadioRunning())) {
            stopSourceRadio();
            stopSourceTone();
            log_i("##[BT-TX]# source output stopped after sink-role detection");
        }
        if (_moduleRole == BT_ROLE_SINK) sendAT("AT+A2DPRES");
        return;
    }
    if (!strncmp(line, "+DEVMOD:", 8)) {
        const bool sourceOutputWasActive = sourceToneRunning() || sourceRadioRunning();
        if (sourceOutputWasActive) {
            stopSourceRadio();
            stopSourceTone();
            log_i("##[BT-TX]# source output stopped for module reboot; restart it after the probe");
        }

        // This event is emitted after every module boot. Any previously
        // cached persistent configuration may now be stale, so restart the
        // read-only staged probe before allowing a direction-sensitive I2S
        // test to start.
        _moduleI2sCfg = -1;
        _moduleAuxCfg = -1;
        _moduleProfile = -1;
        _moduleAvrcpCfg = -1;
        _moduleBluetoothEnabled = -1;
        _lastA2dpStatus = 0;
        _lastAvrcpStatus = 0;
        _lastHfpStatus = 0;
        __atomic_store_n(&_remoteBatteryLevel, static_cast<int8_t>(-1),
                         __ATOMIC_RELEASE);
        _remoteBatteryAddress[0] = '\0';
        _remoteRssi = -127;
        _remoteRssiAddress[0] = '\0';
        _inputRssiProbeIndex = 0;
        _inputRssiProbeMatched = false;
        _inputRssiProbeAddress[0] = '\0';
        _inputConnectedAddress[0] = '\0';
        _inputConnectedName[0] = '\0';
        _sourceConnectedAddress[0] = '\0';
        _sourceConnectedName[0] = '\0';
        _sourceSwitchState = SOURCE_SWITCH_IDLE;
        _sourceSwitchDeadlineMs = 0;
        _sourceCodecSampleRate = 0;
        _sourceAudioPathReady = false;
        _sourceAudioEnablePending = false;
        _sourceAudioEnableDeadlineMs = 0;
        _sourceAudioEnableAttempts = 0;
        _nextSourceAudioEnableMs = 0;
        if (_sinkControlQueue) xQueueReset(_sinkControlQueue);
        _sinkControlInFlight = BT_SINK_CONTROL_NONE;
        _sinkControlDeadlineMs = 0;
        __atomic_store_n(&_sinkVolumePending, false, __ATOMIC_RELEASE);
        _sourceRouteApplyPending = true;
        _sourceRouteVerifyPending = false;
        _sourceRouteAttempts = 0;
        _sourceRouteNextActionMs = 0;
        _sourceReconnectAfterEnable = false;
        _sourceReconnectNextActionMs = 0;
        _startupProbeStep = 0;
        _nextStartupProbeMs = millis() + 500U;
        _startupProbeComplete = false;
        if (!strcmp(line + 8, "MASTER")) {
            __atomic_store_n(&_moduleRole, BT_ROLE_SOURCE, __ATOMIC_RELEASE);
        }
        if (!strcmp(line + 8, "SLAVE")) {
            __atomic_store_n(&_moduleRole, BT_ROLE_SINK, __ATOMIC_RELEASE);
        }
        if (!roleTransitionBusy() && _moduleRole != BT_ROLE_UNKNOWN) {
            __atomic_store_n(&_requestedRole, _moduleRole,
                             __ATOMIC_RELEASE);
        }
        _state = BT_STATE_DISC;
        _lastPcmMs = 0;
        _vuLeft = 0;
        _vuRight = 0;
        if (_moduleRole == BT_ROLE_SINK) {
            const bool sinkRequestAlreadyCaptured = roleTransitionBusy() &&
                _requestedRole == BT_ROLE_SINK;
            beginSinkMetaGeneration(!sinkRequestAlreadyCaptured);
        }
        log_i("##[BT]# module boot role: %s",
              _moduleRole == BT_ROLE_SOURCE ? "source" :
              _moduleRole == BT_ROLE_SINK ? "sink" : line + 8);
        netserver.btSourceStateToWs(
            0, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        if (_moduleRole == BT_ROLE_SOURCE && bridgeRunningOrPending()) {
            stopBridge();
            log_i("##[BT-TX]# receive bridge stopped after source-role reboot");
        }
        return;
    }
    if (!strcmp(line, "+SCAN=E")) {
        if (_sourceScanActive) {
            finishSourceScan("done", nullptr);
            log_i("##[BT-TX]# source device scan finished: %u device(s)",
                  static_cast<unsigned>(_sourceScanDeviceCount));
        }
        return;
    }
    if (!strncmp(line, "+SCAN=", 6)) {
        handleSourceScanResult(line + 6);
        return;
    }
    if (!strncmp(line, "+I2SSR=", 7)) {
        const uint32_t sampleRate = parseUnsignedValue(line);
        log_i("##[BT-TX]# %s", line);
        // I2SSR often arrives during A2DP reconnect, before the local relay
        // task has started. It describes the module/remote audio path, not the
        // lifetime of our relay task, so retain it independently.
        if (_moduleRole == BT_ROLE_SOURCE &&
            sampleRate == BT_I2S_SAMPLE_RATE) {
            if (!_sourceAudioPathReady) {
                log_i("##[BT-TX]# A2DP source audio path ready");
            }
            _sourceAudioPathReady = true;
            _sourceCodecSampleRate = sampleRate;
        }
        if (_moduleRole == BT_ROLE_SINK && _inputCodecSampleRate == 0) {
            _inputCodecSampleRate = sampleRate;
            publishInputUiSnapshot(0);
        }
        display.putRequest(DBITRATE);
        return;
    }
    if (!strncmp(line, "+A2DPDEV=", 9)) {
        if (_moduleRole == BT_ROLE_SOURCE) {
            handleSourceConnectedDevice(line + 9, true);
        } else if (_moduleRole == BT_ROLE_SINK) {
            handleInputConnectedDevice(line + 9, true);
        }
        log_i("##[BT-TX]# %s", line);
        return;
    }
    if (!strncmp(line, "+HFPDEV=", 8)) {
        const char* value = line + 8;
        const char* comma = strchr(value, ',');
        const size_t addressLength = comma
            ? static_cast<size_t>(comma - value) : strlen(value);
        if (addressLength == 12) {
            char address[13] = {};
            memcpy(address, value, 12);
            if (validBluetoothAddress(address)) {
                if (_remoteBatteryAddress[0] &&
                    strcasecmp(_remoteBatteryAddress, address) != 0) {
                    __atomic_store_n(&_remoteBatteryLevel,
                                     static_cast<int8_t>(-1),
                                     __ATOMIC_RELEASE);
                }
                strlcpy(_remoteBatteryAddress, address,
                        sizeof(_remoteBatteryAddress));
            }
        }
        if (_moduleRole == BT_ROLE_SOURCE) {
            if (!_sourceConnectedAddress[0]) {
                handleSourceConnectedDevice(value, true);
            }
        } else if (_moduleRole == BT_ROLE_SINK) {
            if (!_inputConnectedAddress[0]) {
                handleInputConnectedDevice(value, true);
            }
        }
        publishInputUiSnapshot(0);
        netserver.btSourceStateToWs(
            0, moduleRoleName(_moduleRole),
            static_cast<uint8_t>(_lastA2dpStatus), _sourceScanActive,
            _sourceConnectPending, _sourceConnectedAddress,
            _sourceConnectedName);
        log_i("##[BT]# %s", line);
        return;
    }
    if (!strncmp(line, "+A2DPCONN=", 10)) {
        if (_moduleRole == BT_ROLE_SOURCE) {
            handleSourceConnectedDevice(line + 10, false);
        } else if (_moduleRole == BT_ROLE_SINK) {
            handleInputConnectedDevice(line + 10, false);
        }
        log_i("##[BT-TX]# %s", line);
        return;
    }
    if (!strncmp(line, "+PAIRED=", 8)) {
        if (_moduleRole == BT_ROLE_SOURCE) {
            handleSourceConnectedDevice(line + 8, false);
            netserver.btSourceScanStateToWs(0, "paired",
                                            _sourceScanDeviceCount,
                                            "Speaker paired successfully");
        } else if (_moduleRole == BT_ROLE_SINK) {
            handleInputConnectedDevice(line + 8, false);
            _inputRefreshPending = true;
            netserver.btInputPairedStateToWs(
                0, "refreshing", _inputPairedDeviceCount,
                "Phone or PC paired; refreshing the list...");
        }
        log_i("##[BT-TX]# %s", line);
        return;
    }
    if (!strncmp(line, "+A2DPAUDIO=", 11)) {
        log_i("##[BT-TX]# %s", line);
        return;
    }

    // In source mode the connected loudspeaker is the AVRCP Controller and
    // the BT1058 is the AVRCP Target. Convert its documented UART events to a
    // typed command before dispatching them to the normal Edge player API.
    struct BtRemoteEvent {
        const char* wireEvent;
        const char* action;
        btRemoteCommand_e command;
    };
    static const BtRemoteEvent remoteEvents[] = {
        {"+CTPLAY",  "PLAY",        BT_REMOTE_PLAY},
        {"+CTPAUSE", "PAUSE",       BT_REMOTE_PAUSE},
        {"+CTSTOP",  "STOP",        BT_REMOTE_STOP},
        {"+CTFWD",   "NEXT",        BT_REMOTE_NEXT},
        {"+CTBACK",  "PREVIOUS",    BT_REMOTE_PREVIOUS},
        {"+CTVOLUP", "VOLUME UP",   BT_REMOTE_VOLUME_UP},
        {"+CTVOLDN", "VOLUME DOWN", BT_REMOTE_VOLUME_DOWN},
    };
    for (const BtRemoteEvent& event : remoteEvents) {
        if (!strcmp(line, event.wireEvent)) {
            handleRemoteControlEvent(event.command, event.wireEvent,
                                     event.action);
            return;
        }
    }
    if (!strncmp(line, "+SPKVOL=", 8)) {
        log_i("##[BT-REMOTE]# speaker absolute volume: %u (%s), role=%s",
              (unsigned)parseUnsignedValue(line), line,
              moduleRoleName(_moduleRole));
        return;
    }

    // Connection identity, HFP, AVRCP, pairing, battery and network events
    // are intentionally retained in the debug log but do not mutate the
    // player state in this first receive-mode milestone.
    if (!strcmp(line, "OK") || !strcmp(line, "ERROR")) return;
}

// =====================================================================
//  I2S RX (module -> ESP32) on the second I2S peripheral
// =====================================================================

void BluetoothPlayer::resetI2SPins() {
    // ESP-IDF normally rewires the GPIO matrix when a channel is deleted and
    // recreated. Explicitly return every module-side signal to a floating
    // input as well: this makes repeated master-TX <-> slave-RX direction
    // changes deterministic and prevents the old ESP master drive from
    // fighting the module's BCK/LRCK after it becomes the I2S master.
    const gpio_num_t pins[] = {
        static_cast<gpio_num_t>(BT_I2S_BCK),
        static_cast<gpio_num_t>(BT_I2S_LRCK),
        static_cast<gpio_num_t>(BT_I2S_RX_DATA),
        static_cast<gpio_num_t>(BT_I2S_TX_DATA),
    };
    for (gpio_num_t pin : pins) {
        gpio_reset_pin(pin);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
}

bool BluetoothPlayer::initI2SRx() {
    resetI2SPins();
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_1, BT_I2S_RX_MASTER ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    // The tested FSC-BT1058 drives I2S as master output. The ESP RX side is a
    // slave and only samples BCK/LRCK/DATA from the module.
    esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &_rxHandle);
    if (err != ESP_OK) {
        log_e("##[BT][ERROR]# i2s_new_channel rx failed: %d", err);
        return false;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BT_I2S_SAMPLE_RATE),
#if BT_I2S_FORMAT == 1
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            BT_I2S_DATA_WIDTH,
            I2S_SLOT_MODE_STEREO),
#elif BT_I2S_FORMAT == 2
        .slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(
            BT_I2S_DATA_WIDTH,
            I2S_SLOT_MODE_STEREO),
#else
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            BT_I2S_DATA_WIDTH,
            I2S_SLOT_MODE_STEREO),
#endif
        .gpio_cfg =
            {
//                .mclk = I2S_GPIO_UNUSED,
#if defined(BT_I2S_MCLK) && BT_I2S_USE_MCLK
                .mclk = (gpio_num_t)BT_I2S_MCLK,
#else
                .mclk = I2S_GPIO_UNUSED,
#endif
                .bclk = (gpio_num_t)BT_I2S_BCK,
                .ws = (gpio_num_t)BT_I2S_LRCK,
                .dout = I2S_GPIO_UNUSED,
                .din = (gpio_num_t)BT_I2S_RX_DATA,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = BT_I2S_BCLK_INV,
                        .ws_inv = BT_I2S_WS_INV,
                    },
            },
    };

#if BT_I2S_BITS == 24
    stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
#endif

    log_i("##[BT]# RX config: role=%s rate=%u bits=%u fmt=%u mclk=%u bclkInv=%u wsInv=%u shift=%u",
          BT_I2S_RX_MASTER ? "master" : "slave",
          (unsigned)BT_I2S_SAMPLE_RATE,
          (unsigned)BT_I2S_BITS,
          (unsigned)BT_I2S_FORMAT,
          (unsigned)BT_I2S_USE_MCLK,
          (unsigned)BT_I2S_BCLK_INV,
          (unsigned)BT_I2S_WS_INV,
          (unsigned)BT_I2S_32_SHIFT);

    err = i2s_channel_init_std_mode(_rxHandle, &stdCfg);
    if (err != ESP_OK) {
        log_e("##[BT][ERROR]# i2s_channel_init_std_mode rx failed: %d", err);
        i2s_del_channel(_rxHandle);
        _rxHandle = nullptr;
        return false;
    }
    return true;
}

void BluetoothPlayer::deinitI2SRx() {
    if (_rxHandle) {
        if (_rxChannelEnabled) {
            i2s_channel_disable(_rxHandle);
            _rxChannelEnabled = false;
        }
        i2s_del_channel(_rxHandle);
        _rxHandle = nullptr;
    }
    resetI2SPins();
}

// =====================================================================
//  Bridge task: BT I2S RX -> existing DAC I2S TX (audio.i2sWriteExt)
// =====================================================================

bool BluetoothPlayer::startBridge() {
    if (_bridgeTaskHandle) return true;
    if (_moduleRole == BT_ROLE_UNKNOWN) {
        _bridgeStartPending = true;
        log_i("##[BT]# RX bridge deferred until module role is known");
        return true;
    }
    if (_moduleRole == BT_ROLE_SOURCE) {
        log_w("##[BT]# RX bridge refused: module is in source mode");
        return false;
    }
    if (sourceToneRunning() || sourceRadioRunning() || _txHandle) {
        log_w("##[BT]# RX bridge refused while a source output owns I2S1");
        return false;
    }
    if (!_rxHandle && !initI2SRx()) return false;

    if (player.isRunning() || player.status() == PLAYING ||
        !player.readyForWebStation()) {
        _bridgeStartPending = true;
        log_i("##[BT]# bridge start deferred until decoder stops");
        return true;
    }

    return startBridgeNow();
}

bool BluetoothPlayer::startBridgeNow() {
    if (_bridgeTaskHandle || !_rxHandle) return false;

    // Read-only verification. The module's persistent settings were already
    // established during isolated bring-up; the Edge lab must not rewrite
    // them automatically.
    requestStreamInfo(true);

    if (_bridgeTaskHandle || !_rxHandle) return false;

    // Match the DAC TX channel to the module's verified fixed 48 kHz PCM
    // output. The narrow display DMA guard in Audio protects the short TX
    // reconfiguration without holding a display transaction during streaming.
    _bridgeSampleRate = BT_I2S_SAMPLE_RATE;

    // setSampleRate() performs the complete disable -> reconfigure -> enable
    // cycle in this Audio implementation. Calling i2sEnableExt() afterwards
    // would try to preload an already-running TX channel and produces the IDF
    // "data can only be preloaded when the channel is READY" error.
    if (!player.setSampleRate(_bridgeSampleRate)) {
        log_e("##[BT]# DAC sample-rate reconfiguration failed");
        return false;
    }
    spectrumAnalyzer.setSampleRate(_bridgeSampleRate);

    if (i2s_channel_enable(_rxHandle) != ESP_OK) return false;
    _rxChannelEnabled = true;
    _bridgeStopRequest = false;
    // Core 0 keeps the bridge separate from the regular Audio task.
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        bridgeTaskWrapper, "bt_i2s_bridge", 4096, this, 3,
        &_bridgeTaskHandle, 0);
    if (taskCreated != pdPASS) {
        _bridgeTaskHandle = nullptr;
        i2s_channel_disable(_rxHandle);
        _rxChannelEnabled = false;
        player.setOutputSampleRate(Audio::SR_ORIGIN);
        return false;
    }
    return true;
}

void BluetoothPlayer::stopBridge() {
    _bridgeStartPending = false;
    _lastPcmMs = 0;
    _vuLeft = 0;
    _vuRight = 0;
    if (!_bridgeTaskHandle) return;
    _bridgeStopRequest = true;
    // task deletes itself and clears the handle on the way out
    while (_bridgeTaskHandle) { delay(2); }
    if (_rxHandle && _rxChannelEnabled) {
        i2s_channel_disable(_rxHandle);
        _rxChannelEnabled = false;
    }

    // Vissza az eredeti (forrás szerinti) mintavételi rátára, hogy a
    // következő WEB/SD lejátszás a szokásos módon, felesleges resample
    // nélkül induljon.
    player.setOutputSampleRate(Audio::SR_ORIGIN);
}

void BluetoothPlayer::bridgeTaskWrapper(void* param) { static_cast<BluetoothPlayer*>(param)->bridgeTask(); }

void BluetoothPlayer::bridgeTask() {
    static uint8_t rxBuf[2048];
    static int32_t tx32Buf[2048 / 2];
    static int32_t spectrumBuf[2048 / 2];
    size_t         bytesRead    = 0;
    size_t         bytesWritten = 0;

    uint32_t lastErrorLogMs = 0;
    uint32_t rxTimeouts = 0;
    uint32_t i2sIdleSinceMs = 0;
    bool waitingForI2sClocks = false;
    bool i2sIdleReported = false;
    bool firstPcmBlockLogged = false;
    uint8_t cachedVolumeStep = 0xFFU;
    int8_t cachedBalance = INT8_MIN;
    float cachedCurveDb = 1.0f;
    int32_t targetGainLeftQ15 = 0;
    int32_t targetGainRightQ15 = 0;
    int32_t currentGainLeftQ15 = 0;
    int32_t currentGainRightQ15 = 0;
    bool gainInitialized = false;

    log_i("##[BT]# bridge task started (core %d, rxBits=%u, rate=%u)",
          xPortGetCoreID(), (unsigned)BT_I2S_BITS, (unsigned)_bridgeSampleRate);

    while (!_bridgeStopRequest) {
        esp_err_t err = i2s_channel_read(_rxHandle, rxBuf, sizeof(rxBuf), &bytesRead, pdMS_TO_TICKS(50));
        if (err == ESP_OK && bytesRead > 0) {
            if (i2sIdleReported) {
                log_i("##[BT]# module I2S clocks active after %u ms idle (%u read timeouts)",
                      (unsigned)(millis() - i2sIdleSinceMs),
                      (unsigned)rxTimeouts);
            }
            waitingForI2sClocks = false;
            i2sIdleReported = false;
            i2sIdleSinceMs = 0;
            rxTimeouts = 0;
            size_t frames = 0;
            size_t txBytes = 0;
            const uint8_t* txData = nullptr;
            uint32_t rawPeakLeft = 0;
            uint32_t rawPeakRight = 0;

            const uint8_t volumeStep = __atomic_load_n(
                &_sinkVolumeStep, __ATOMIC_ACQUIRE);
            const int8_t balance = config.store.balance;
            const float curveDb = volumeStep == 0
                ? -76.0f
                : player.getVolumeCurveDbPoint(volumeStep);
            if (volumeStep != cachedVolumeStep || balance != cachedBalance ||
                curveDb != cachedCurveDb) {
                calculateSinkDacGains(volumeStep, balance, curveDb,
                                      targetGainLeftQ15,
                                      targetGainRightQ15);
                cachedVolumeStep = volumeStep;
                cachedBalance = balance;
                cachedCurveDb = curveDb;
                if (!gainInitialized) {
                    currentGainLeftQ15 = targetGainLeftQ15;
                    currentGainRightQ15 = targetGainRightQ15;
                    gainInitialized = true;
                }
            }

            if (BT_I2S_BITS == 24 || BT_I2S_BITS == 32) {
                const int32_t* src = reinterpret_cast<const int32_t*>(rxBuf);
                frames = bytesRead / (2 * sizeof(int32_t));
                const int64_t leftGainStep = frames
                    ? (static_cast<int64_t>(targetGainLeftQ15 - currentGainLeftQ15) * 65536LL) /
                          static_cast<int64_t>(frames)
                    : 0;
                const int64_t rightGainStep = frames
                    ? (static_cast<int64_t>(targetGainRightQ15 - currentGainRightQ15) * 65536LL) /
                          static_cast<int64_t>(frames)
                    : 0;
                int64_t leftGain = static_cast<int64_t>(currentGainLeftQ15) * 65536LL;
                int64_t rightGain = static_cast<int64_t>(currentGainRightQ15) * 65536LL;
                for (size_t frame = 0; frame < frames; ++frame) {
                    leftGain += leftGainStep;
                    rightGain += rightGainStep;
                    const int16_t left = static_cast<int16_t>(
                        src[frame * 2] >> BT_I2S_32_SHIFT);
                    const int16_t right = static_cast<int16_t>(
                        src[frame * 2 + 1] >> BT_I2S_32_SHIFT);
                    const uint32_t absLeft = left < 0
                        ? static_cast<uint32_t>(-static_cast<int32_t>(left))
                        : static_cast<uint32_t>(left);
                    const uint32_t absRight = right < 0
                        ? static_cast<uint32_t>(-static_cast<int32_t>(right))
                        : static_cast<uint32_t>(right);
                    if (absLeft > rawPeakLeft) rawPeakLeft = absLeft;
                    if (absRight > rawPeakRight) rawPeakRight = absRight;
                    const int32_t rawLeft = static_cast<int32_t>(left) * 65536;
                    const int32_t rawRight = static_cast<int32_t>(right) * 65536;
                    spectrumBuf[frame * 2] = scaleSpectrumSample(rawLeft);
                    spectrumBuf[frame * 2 + 1] = scaleSpectrumSample(rawRight);
                    const int32_t scaledLeft = static_cast<int32_t>(
                        (static_cast<int64_t>(left) * (leftGain >> 16)) >> 15);
                    const int32_t scaledRight = static_cast<int32_t>(
                        (static_cast<int64_t>(right) * (rightGain >> 16)) >> 15);
                    tx32Buf[frame * 2] = scaledLeft * 65536;
                    tx32Buf[frame * 2 + 1] = scaledRight * 65536;
                }
                currentGainLeftQ15 = targetGainLeftQ15;
                currentGainRightQ15 = targetGainRightQ15;
                txData = reinterpret_cast<const uint8_t*>(tx32Buf);
                txBytes = frames * 2 * sizeof(int32_t);
            } else {
                const int16_t* src = reinterpret_cast<const int16_t*>(rxBuf);
                frames = bytesRead / (2 * sizeof(int16_t));
                const int64_t leftGainStep = frames
                    ? (static_cast<int64_t>(targetGainLeftQ15 - currentGainLeftQ15) * 65536LL) /
                          static_cast<int64_t>(frames)
                    : 0;
                const int64_t rightGainStep = frames
                    ? (static_cast<int64_t>(targetGainRightQ15 - currentGainRightQ15) * 65536LL) /
                          static_cast<int64_t>(frames)
                    : 0;
                int64_t leftGain = static_cast<int64_t>(currentGainLeftQ15) * 65536LL;
                int64_t rightGain = static_cast<int64_t>(currentGainRightQ15) * 65536LL;
                for (size_t frame = 0; frame < frames; ++frame) {
                    leftGain += leftGainStep;
                    rightGain += rightGainStep;
                    const int16_t left = src[frame * 2];
                    const int16_t right = src[frame * 2 + 1];
                    const uint32_t absLeft = left < 0
                        ? static_cast<uint32_t>(-static_cast<int32_t>(left))
                        : static_cast<uint32_t>(left);
                    const uint32_t absRight = right < 0
                        ? static_cast<uint32_t>(-static_cast<int32_t>(right))
                        : static_cast<uint32_t>(right);
                    if (absLeft > rawPeakLeft) rawPeakLeft = absLeft;
                    if (absRight > rawPeakRight) rawPeakRight = absRight;
                    const int32_t rawLeft = static_cast<int32_t>(left) * 65536;
                    const int32_t rawRight = static_cast<int32_t>(right) * 65536;
                    spectrumBuf[frame * 2] = scaleSpectrumSample(rawLeft);
                    spectrumBuf[frame * 2 + 1] = scaleSpectrumSample(rawRight);
                    const int32_t scaledLeft = static_cast<int32_t>(
                        (static_cast<int64_t>(left) * (leftGain >> 16)) >> 15);
                    const int32_t scaledRight = static_cast<int32_t>(
                        (static_cast<int64_t>(right) * (rightGain >> 16)) >> 15);
                    tx32Buf[frame * 2] = scaledLeft * 65536;
                    tx32Buf[frame * 2 + 1] = scaledRight * 65536;
                }
                currentGainLeftQ15 = targetGainLeftQ15;
                currentGainRightQ15 = targetGainRightQ15;
                txData = reinterpret_cast<const uint8_t*>(tx32Buf);
                txBytes = frames * 2 * sizeof(int32_t);
            }

            bool ok = player.i2sWriteExt(txData, txBytes, &bytesWritten, 50);
            if ((!ok || bytesWritten != txBytes) && millis() - lastErrorLogMs >= 2000) {
                log_w("##[BT]# I2S TX write failed: expected=%u written=%u",
                      (unsigned)txBytes, (unsigned)bytesWritten);
                lastErrorLogMs = millis();
            }

            // Match normal player semantics: the VU shows the incoming
            // programme level, independently of local volume and spectrum
            // visualization gain.
            const uint8_t peakLeft = rawPeakLeft > 32767U
                ? 255U : static_cast<uint8_t>(rawPeakLeft >> 7);
            const uint8_t peakRight = rawPeakRight > 32767U
                ? 255U : static_cast<uint8_t>(rawPeakRight >> 7);
            _vuLeft = peakLeft >= _vuLeft ? peakLeft :
                      (_vuLeft > 4 ? static_cast<uint8_t>(_vuLeft - 4) : peakLeft);
            _vuRight = peakRight >= _vuRight ? peakRight :
                       (_vuRight > 4 ? static_cast<uint8_t>(_vuRight - 4) : peakRight);

            // The module may keep I2S clocks and silent PCM running while
            // idle. Only mark audio as present when a successfully forwarded
            // block also contains a signal visible to the existing VU meter.
            if (ok && bytesWritten == txBytes && (peakLeft > 0 || peakRight > 0)) {
                _lastPcmMs = millis();
            }

            if (!firstPcmBlockLogged) {
                firstPcmBlockLogged = true;
                log_i("##[BT]# first PCM block: rxBytes=%u frames=%u txBytes=%u peakL=%u peakR=%u",
                      (unsigned)bytesRead, (unsigned)frames, (unsigned)bytesWritten,
                      (unsigned)peakLeft, (unsigned)peakRight);
            }

            spectrumAnalyzer.pushSamples(spectrumBuf, (int16_t)frames);
        } else if (err == ESP_ERR_TIMEOUT) {
            ++rxTimeouts;
            if (!waitingForI2sClocks) {
                waitingForI2sClocks = true;
                i2sIdleSinceMs = millis();
            }
            // A single 50 ms read timeout is normal around packet/clock
            // boundaries. Report only a sustained clock loss, once, then a
            // single recovery line. This keeps sink playback logs usable.
            if (!i2sIdleReported &&
                millis() - i2sIdleSinceMs >= 1000U) {
                i2sIdleReported = true;
                log_i("##[BT]# I2S RX idle; waiting for module BCK/LRCK (BCK=%u WS=%u DIN=%u)",
                      (unsigned)BT_I2S_BCK,
                      (unsigned)BT_I2S_LRCK, (unsigned)BT_I2S_RX_DATA);
            }
        } else if (err != ESP_OK && millis() - lastErrorLogMs >= 2000U) {
            log_w("##[BT]# I2S RX read failed: %d", err);
            lastErrorLogMs = millis();
        }
    }

    _vuLeft = 0;
    _vuRight = 0;
    _lastPcmMs = 0;
    _bridgeTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

// =====================================================================
//  Isolated source proof: ESP32 I2S TX -> module I2S_IN
// =====================================================================

bool BluetoothPlayer::initI2STx() {
    if (_txHandle) return true;

    // I2S_NUM_1 is initially prepared as RX before the module role is known.
    // Release that dormant channel, then claim the peripheral as TX. I2S0,
    // owned by the normal player/DAC path, remains completely independent.
    deinitI2SRx();
    resetI2SPins();

    // Complement the role selected in the module's I2SCFG bit field:
    //   1 = module master -> ESP slave (diagnostics only on this firmware)
    //   3 = module slave  -> ESP master (hardware-verified source path)
    const bool espTxMaster = _moduleI2sCfg == 3;
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_1, espTxMaster ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    esp_err_t err = i2s_new_channel(&chanCfg, &_txHandle, nullptr);
    if (err != ESP_OK) {
        _txHandle = nullptr;
        log_e("##[BT-TX]# i2s_new_channel tx failed: %d", err);
        return false;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BT_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BT_I2S_BCK,
            .ws = (gpio_num_t)BT_I2S_LRCK,
            .dout = (gpio_num_t)BT_I2S_TX_DATA,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(_txHandle, &stdCfg);
    if (err != ESP_OK) {
        log_e("##[BT-TX]# I2S TX init failed: %d", err);
        i2s_del_channel(_txHandle);
        _txHandle = nullptr;
        return false;
    }
    err = i2s_channel_enable(_txHandle);
    if (err != ESP_OK) {
        log_e("##[BT-TX]# I2S TX enable failed: %d", err);
        i2s_del_channel(_txHandle);
        _txHandle = nullptr;
        return false;
    }
    return true;
}

void BluetoothPlayer::deinitI2STx() {
    if (_txHandle) {
        i2s_channel_disable(_txHandle);
        i2s_del_channel(_txHandle);
        _txHandle = nullptr;
    }
    resetI2SPins();
}

bool BluetoothPlayer::startSourceTone() {
    if (_sourceToneTaskHandle) return true;
    if (sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        log_w("##[BT-TX]# tone refused in Local only output mode");
        return false;
    }
    if (_moduleRole != BT_ROLE_SOURCE) {
        log_w("##[BT-TX]# tone refused: module role is not source");
        return false;
    }
    if (_moduleAuxCfg != 3 || (_moduleI2sCfg != 1 && _moduleI2sCfg != 3)) {
        log_w("##[BT-TX]# tone refused: expected AUXCFG=3 and I2SCFG=1 or 3, got %d/%d",
              (int)_moduleAuxCfg, (int)_moduleI2sCfg);
        return false;
    }
    if (_bridgeTaskHandle || _bridgeStartPending) {
        log_w("##[BT-TX]# tone refused while RX bridge is active or pending");
        return false;
    }
    if (sourceRadioRunning()) stopSourceRadio();
    if (!initI2STx()) return false;

    _sourceToneStopRequest = false;
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        sourceToneTaskWrapper, "bt_source_tone", 3072, this, 2,
        &_sourceToneTaskHandle, 0);
    if (taskCreated != pdPASS) {
        _sourceToneTaskHandle = nullptr;
        deinitI2STx();
        log_e("##[BT-TX]# source tone task creation failed");
        return false;
    }

    log_i("##[BT-TX]# I2S source tone configured: ESP role=%s, %u Hz PCM, %u Hz tone, 16-bit stereo, pins BCK=%u WS=%u DATA=%u",
          _moduleI2sCfg == 3 ? "master" : "slave",
          (unsigned)BT_I2S_SAMPLE_RATE, (unsigned)BT_SOURCE_TONE_HZ,
          (unsigned)BT_I2S_BCK, (unsigned)BT_I2S_LRCK, (unsigned)BT_I2S_TX_DATA);
    return true;
}

void BluetoothPlayer::stopSourceTone() {
    if (!_sourceToneTaskHandle) return;
    _sourceToneStopRequest = true;
    while (_sourceToneTaskHandle) delay(2);
    deinitI2STx();
}

void BluetoothPlayer::sourceToneTaskWrapper(void* param) {
    static_cast<BluetoothPlayer*>(param)->sourceToneTask();
}

void BluetoothPlayer::sourceToneTask() {
    static constexpr size_t framesPerBlock = 256;
    static constexpr size_t samplesPerPeriod = BT_I2S_SAMPLE_RATE / BT_SOURCE_TONE_HZ;
    static_assert(BT_SOURCE_TONE_HZ > 0 &&
                  BT_I2S_SAMPLE_RATE % BT_SOURCE_TONE_HZ == 0,
                  "BT source tone must divide the I2S sample rate exactly");
    static int16_t toneLut[samplesPerPeriod];
    static int16_t txBuf[framesPerBlock * 2];

    for (size_t i = 0; i < samplesPerPeriod; ++i) {
        const float phase = 6.28318530718f * static_cast<float>(i) /
                            static_cast<float>(samplesPerPeriod);
        toneLut[i] = static_cast<int16_t>(sinf(phase) * BT_SOURCE_TONE_AMPLITUDE);
    }

    size_t toneIndex = 0;
    uint32_t lastStatsMs = millis();
    uint32_t lastWriteErrorMs = 0;
    uint32_t blocks = 0;
    log_i("##[BT-TX]# source tone task started on core %d", xPortGetCoreID());

    while (!_sourceToneStopRequest) {
        for (size_t frame = 0; frame < framesPerBlock; ++frame) {
            const int16_t sample = toneLut[toneIndex];
            toneIndex = (toneIndex + 1) % samplesPerPeriod;
            txBuf[frame * 2] = sample;
            txBuf[frame * 2 + 1] = sample;
        }

        size_t bytesWritten = 0;
        const esp_err_t err = i2s_channel_write(
            _txHandle, txBuf, sizeof(txBuf), &bytesWritten, 100);
        if (err != ESP_OK || bytesWritten != sizeof(txBuf)) {
            if (lastWriteErrorMs == 0 || millis() - lastWriteErrorMs >= 2000U) {
                if (err == ESP_ERR_TIMEOUT) {
                    log_w("##[BT-TX]# tone TX waiting for external BCK/WS (I2S slave timeout)");
                } else {
                    log_w("##[BT-TX]# tone write failed: err=%d expected=%u written=%u",
                          err, (unsigned)sizeof(txBuf), (unsigned)bytesWritten);
                }
                lastWriteErrorMs = millis();
            }
            delay(10);
        } else {
            ++blocks;
        }

        if (millis() - lastStatsMs >= 5000U) {
            log_i("##[BT-TX]# tone alive: blocks=%u stack_hwm=%u",
                  (unsigned)blocks, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            blocks = 0;
            lastStatsMs = millis();
        }
    }

    log_i("##[BT-TX]# source tone task stopped");
    _sourceToneTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

// =====================================================================
//  Experimental player PCM tap -> FSC-BT1058 A2DP source relay
// =====================================================================

uint32_t BluetoothPlayer::sourcePcmBufferedFrames() const {
    const uint32_t writeFrame =
        __atomic_load_n(&_sourcePcmWriteFrame, __ATOMIC_ACQUIRE);
    const uint32_t readFrame =
        __atomic_load_n(&_sourcePcmReadFrame, __ATOMIC_ACQUIRE);
    const uint32_t buffered = writeFrame - readFrame;
    return buffered <= BT_SOURCE_PCM_RING_FRAMES ? buffered
                                                 : BT_SOURCE_PCM_RING_FRAMES;
}

void BluetoothPlayer::pushSourcePcm(const int32_t* stereo32, size_t frames) {
    if (!stereo32 || !_sourcePcmRing || frames == 0 ||
        !__atomic_load_n(&_sourceRadioEnabled, __ATOMIC_ACQUIRE)) {
        return;
    }

    const uint32_t writeFrame =
        __atomic_load_n(&_sourcePcmWriteFrame, __ATOMIC_RELAXED);
    const uint32_t readFrame =
        __atomic_load_n(&_sourcePcmReadFrame, __ATOMIC_ACQUIRE);
    uint32_t used = writeFrame - readFrame;
    if (used > BT_SOURCE_PCM_RING_FRAMES) used = BT_SOURCE_PCM_RING_FRAMES;
    const size_t freeFrames = BT_SOURCE_PCM_RING_FRAMES - used;
    const size_t accepted = frames < freeFrames ? frames : freeFrames;
    const uint32_t ringMask = BT_SOURCE_PCM_RING_FRAMES - 1U;
    uint16_t peakLeft = 0;
    uint16_t peakRight = 0;
    uint32_t nonZeroSamples = 0;

    for (size_t i = 0; i < accepted; ++i) {
        const uint32_t dst = (writeFrame + static_cast<uint32_t>(i)) & ringMask;
        const int16_t left = static_cast<int16_t>(stereo32[i * 2] >> 16);
        const int16_t right = static_cast<int16_t>(stereo32[i * 2 + 1] >> 16);
        _sourcePcmRing[dst * 2] = left;
        _sourcePcmRing[dst * 2 + 1] = right;

        const uint16_t absLeft = static_cast<uint16_t>(
            left < 0 ? -static_cast<int32_t>(left) : left);
        const uint16_t absRight = static_cast<uint16_t>(
            right < 0 ? -static_cast<int32_t>(right) : right);
        if (absLeft > peakLeft) peakLeft = absLeft;
        if (absRight > peakRight) peakRight = absRight;
        if (left != 0) ++nonZeroSamples;
        if (right != 0) ++nonZeroSamples;
    }

    if (accepted > 0) {
        __atomic_store_n(&_sourcePcmWriteFrame,
                         writeFrame + static_cast<uint32_t>(accepted),
                         __ATOMIC_RELEASE);
        __atomic_fetch_add(&_sourcePcmFramesIn,
                           static_cast<uint32_t>(accepted),
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&_sourcePcmNonZeroSamples,
                           nonZeroSamples, __ATOMIC_RELAXED);

        if (peakLeft > __atomic_load_n(&_sourcePcmPeakLeft, __ATOMIC_RELAXED)) {
            __atomic_store_n(&_sourcePcmPeakLeft, peakLeft, __ATOMIC_RELAXED);
        }
        if (peakRight > __atomic_load_n(&_sourcePcmPeakRight, __ATOMIC_RELAXED)) {
            __atomic_store_n(&_sourcePcmPeakRight, peakRight, __ATOMIC_RELAXED);
        }

        const uint32_t level = used + static_cast<uint32_t>(accepted);
        const uint32_t highWater =
            __atomic_load_n(&_sourcePcmHighWaterFrames, __ATOMIC_RELAXED);
        // Only the Audio task writes the high-water mark.
        if (level > highWater) {
            __atomic_store_n(&_sourcePcmHighWaterFrames, level, __ATOMIC_RELAXED);
        }
    }

    if (accepted < frames) {
        __atomic_fetch_add(&_sourcePcmDroppedFrames,
                           static_cast<uint32_t>(frames - accepted),
                           __ATOMIC_RELAXED);
    }
}

bool BluetoothPlayer::popSourcePcmElastic(int16_t* stereo16,
                                          size_t outputFrames,
                                          uint32_t stepQ16,
                                          uint32_t& phaseQ16,
                                          size_t& consumedFrames) {
    consumedFrames = 0;
    if (!stereo16 || outputFrames == 0 || stepQ16 == 0) return false;

    const uint32_t readFrame =
        __atomic_load_n(&_sourcePcmReadFrame, __ATOMIC_RELAXED);
    const uint32_t writeFrame =
        __atomic_load_n(&_sourcePcmWriteFrame, __ATOMIC_ACQUIRE);
    uint32_t available = writeFrame - readFrame;
    if (available > BT_SOURCE_PCM_RING_FRAMES) {
        available = BT_SOURCE_PCM_RING_FRAMES;
    }
    const uint64_t finalPhase = static_cast<uint64_t>(phaseQ16) +
        static_cast<uint64_t>(stepQ16) * (outputFrames - 1U);
    const size_t requiredFrames =
        static_cast<size_t>(finalPhase >> 16) + 2U;
    if (available < requiredFrames) return false;

    const uint32_t ringMask = BT_SOURCE_PCM_RING_FRAMES - 1U;
    uint64_t phase = phaseQ16;
    for (size_t i = 0; i < outputFrames; ++i) {
        const uint32_t offset = static_cast<uint32_t>(phase >> 16);
        const uint32_t src0 = (readFrame + offset) & ringMask;
        const uint32_t src1 = (src0 + 1U) & ringMask;
        // Eight interpolation bits are ample for the sub-percent clock
        // correction and keep the relay task's math in fast 32-bit range.
        const int32_t fraction = static_cast<int32_t>((phase >> 8) & 0xFFU);
        const int32_t left0 = _sourcePcmRing[src0 * 2];
        const int32_t right0 = _sourcePcmRing[src0 * 2 + 1];
        const int32_t left1 = _sourcePcmRing[src1 * 2];
        const int32_t right1 = _sourcePcmRing[src1 * 2 + 1];
        stereo16[i * 2] = static_cast<int16_t>(
            left0 + (((left1 - left0) * fraction) >> 8));
        stereo16[i * 2 + 1] = static_cast<int16_t>(
            right0 + (((right1 - right0) * fraction) >> 8));
        phase += stepQ16;
    }

    consumedFrames = static_cast<size_t>(phase >> 16);
    phaseQ16 = static_cast<uint32_t>(phase & 0xFFFFU);
    __atomic_store_n(&_sourcePcmReadFrame,
                     readFrame + static_cast<uint32_t>(consumedFrames),
                     __ATOMIC_RELEASE);
    __atomic_fetch_add(&_sourcePcmFramesOut,
                       static_cast<uint32_t>(consumedFrames),
                       __ATOMIC_RELAXED);
    return true;
}

bool BluetoothPlayer::startSourceRadio() {
    if (_sourceRadioTaskHandle) return true;
    if (sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY) {
        log_w("##[BT-TX]# radio relay refused in Local only output mode");
        return false;
    }
    if (_moduleRole != BT_ROLE_SOURCE) {
        log_w("##[BT-TX]# radio relay refused: module role is not source");
        return false;
    }
    // This firmware did not generate clocks as an I2S master in source mode.
    // Keep the real-audio relay on the hardware-verified topology only.
    if (_moduleAuxCfg != 3 || _moduleI2sCfg != 3) {
        log_w("##[BT-TX]# radio relay refused: expected AUXCFG=3/I2SCFG=3, got %d/%d",
              (int)_moduleAuxCfg, (int)_moduleI2sCfg);
        return false;
    }
    if (_bridgeTaskHandle || _bridgeStartPending) {
        log_w("##[BT-TX]# radio relay refused while RX bridge is active or pending");
        return false;
    }

    if (sourceToneRunning()) stopSourceTone();

    const size_t ringBytes =
        BT_SOURCE_PCM_RING_FRAMES * 2U * sizeof(int16_t);
    _sourcePcmRing = static_cast<int16_t*>(
        heap_caps_malloc(ringBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_sourcePcmRing) {
        log_e("##[BT-TX]# could not allocate %u-byte PCM ring in PSRAM",
              (unsigned)ringBytes);
        return false;
    }
    memset(_sourcePcmRing, 0, ringBytes);

    if (!initI2STx()) {
        heap_caps_free(_sourcePcmRing);
        _sourcePcmRing = nullptr;
        return false;
    }

    __atomic_store_n(&_sourcePcmWriteFrame, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmReadFrame, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmFramesIn, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmFramesOut, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmDroppedFrames, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmRebufferEvents, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmUnderrunBlocks, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmWriteErrors, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmHighWaterFrames, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmNonZeroSamples, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourceRadioBlocks, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmRateAdjustPpm, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmPeakLeft, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmPeakRight, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourceRadioPrimed, false, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourceRadioEverPrimed, false, __ATOMIC_RELAXED);
    _sourceRadioLastStatsMs = millis();
    // Do not clear a valid I2SSR observation made during A2DP reconnect before
    // this relay task was created. A disconnected/new link resets it in
    // handleA2dpStatus(), and a module reboot resets it in parseLine().
    if (!_sourceAudioPathReady) {
        _sourceAudioEnableAttempts = 0;
        _nextSourceAudioEnableMs = millis() + 250U;
    }
    _sourceRadioStopRequest = false;

    // The module consumes fixed 48 kHz PCM. Audio's existing resampler feeds
    // both the normal DAC and our post-DSP tap with that exact rate.
    if (!player.setOutputSampleRate(Audio::SR_48000)) {
        deinitI2STx();
        heap_caps_free(_sourcePcmRing);
        _sourcePcmRing = nullptr;
        log_e("##[BT-TX]# radio relay could not switch player output to 48 kHz");
        return false;
    }
    __atomic_store_n(&_sourceRadioEnabled, true, __ATOMIC_RELEASE);

    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        sourceRadioTaskWrapper, "bt_source_radio", 4096, this, 3,
        &_sourceRadioTaskHandle, 0);
    if (taskCreated != pdPASS) {
        _sourceRadioTaskHandle = nullptr;
        __atomic_store_n(&_sourceRadioEnabled, false, __ATOMIC_RELEASE);
        player.setOutputSampleRate(Audio::SR_ORIGIN);
        deinitI2STx();
        heap_caps_free(_sourcePcmRing);
        _sourcePcmRing = nullptr;
        log_e("##[BT-TX]# radio relay task creation failed");
        return false;
    }

    log_i("##[BT-TX]# player PCM relay configured: ESP master, %u Hz, 16-bit stereo, ring=%u frames, prefill=%u",
          (unsigned)BT_I2S_SAMPLE_RATE,
          (unsigned)BT_SOURCE_PCM_RING_FRAMES,
          (unsigned)BT_SOURCE_PCM_PREFILL_FRAMES);
    if (!player.isRunning()) {
        log_w("##[BT-TX]# radio relay is armed but the player is not producing audio");
    }
    return true;
}

void BluetoothPlayer::stopSourceRadio() {
    const bool wasActive = _sourceRadioTaskHandle != nullptr ||
        __atomic_load_n(&_sourceRadioEnabled, __ATOMIC_ACQUIRE);
    if (!wasActive) return;

    __atomic_store_n(&_sourceRadioEnabled, false, __ATOMIC_RELEASE);
    // Stopping only the local relay does not tear down the module's A2DP audio
    // path. Preserve the last I2SSR result so a same-link relay restart does
    // not spam AT+A2DPAUDIO=1.
    _sourceAudioEnablePending = false;
    _sourceAudioEnableDeadlineMs = 0;
    _sourceRadioStopRequest = true;
    while (_sourceRadioTaskHandle) delay(2);
    deinitI2STx();
    // This takes Audio's task mutex. Once it returns, no callback that saw
    // _sourceRadioEnabled=true can still be touching the PSRAM ring.
    player.setOutputSampleRate(Audio::SR_ORIGIN);
    __atomic_store_n(&_sourcePcmWriteFrame, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&_sourcePcmReadFrame, 0U, __ATOMIC_RELAXED);
    heap_caps_free(_sourcePcmRing);
    _sourcePcmRing = nullptr;
}

void BluetoothPlayer::sourceRadioTaskWrapper(void* param) {
    static_cast<BluetoothPlayer*>(param)->sourceRadioTask();
}

void BluetoothPlayer::sourceRadioTask() {
    static int16_t txBuf[BT_SOURCE_PCM_BLOCK_FRAMES * 2];
    uint32_t lastWriteErrorMs = 0;
    uint32_t lastPrimeLogMs = 0;
    uint32_t sourcePhaseQ16 = 0;
    bool primed = false;
    bool producerWasRunning = player.isRunning();
    uint32_t producerStableSinceMs =
        producerWasRunning ? millis() : 0U;

    log_i("##[BT-TX]# radio relay task started on core %d", xPortGetCoreID());

    while (!_sourceRadioStopRequest) {
        const bool producerRunning = player.isRunning();
        const uint32_t now = millis();
        if (producerRunning != producerWasRunning) {
            producerWasRunning = producerRunning;
            producerStableSinceMs = producerRunning ? now : 0U;
        }
        const bool steadyPlayback = producerRunning &&
            static_cast<uint32_t>(now - producerStableSinceMs) >=
                BT_SOURCE_UNDERRUN_GRACE_MS;
        const uint32_t buffered = sourcePcmBufferedFrames();
        if (!primed && buffered >= BT_SOURCE_PCM_PREFILL_FRAMES) {
            primed = true;
            sourcePhaseQ16 = 0;
            __atomic_store_n(&_sourceRadioPrimed, true, __ATOMIC_RELAXED);
            __atomic_store_n(&_sourceRadioEverPrimed, true, __ATOMIC_RELAXED);
            if (lastPrimeLogMs == 0 ||
                static_cast<uint32_t>(now - lastPrimeLogMs) >= 2000U) {
                log_i("##[BT-TX]# PCM buffer primed: %u frames",
                      (unsigned)buffered);
                lastPrimeLogMs = now;
            }
        }

        bool havePcm = false;
        if (primed) {
            const int32_t fillError = static_cast<int32_t>(buffered) -
                static_cast<int32_t>(BT_SOURCE_PCM_TARGET_FRAMES);
            int32_t correction = fillError /
                (1L << BT_SOURCE_PCM_RATE_RESPONSE_SHIFT);
            const int32_t maxCorrection =
                static_cast<int32_t>(BT_SOURCE_PCM_MAX_STEP_CORRECTION_Q16);
            if (correction < -maxCorrection) correction = -maxCorrection;
            if (correction > maxCorrection) correction = maxCorrection;
            const uint32_t stepQ16 = static_cast<uint32_t>(65536L + correction);
            const int32_t ppm = static_cast<int32_t>(
                (static_cast<int64_t>(correction) * 1000000LL) / 65536LL);
            __atomic_store_n(&_sourcePcmRateAdjustPpm, ppm,
                             __ATOMIC_RELAXED);
            size_t consumedFrames = 0;
            havePcm = popSourcePcmElastic(
                txBuf, BT_SOURCE_PCM_BLOCK_FRAMES, stepQ16,
                sourcePhaseQ16, consumedFrames);
        }
        if (!havePcm) {
            if (primed) {
                primed = false;
                sourcePhaseQ16 = 0;
                __atomic_store_n(&_sourcePcmRateAdjustPpm, 0,
                                 __ATOMIC_RELAXED);
                __atomic_store_n(&_sourceRadioPrimed, false, __ATOMIC_RELAXED);
                if (producerRunning) {
                    __atomic_fetch_add(&_sourcePcmRebufferEvents, 1U,
                                       __ATOMIC_RELAXED);
                }
                if (steadyPlayback) {
                    __atomic_fetch_add(&_sourcePcmUnderrunBlocks, 1U,
                                       __ATOMIC_RELAXED);
                }
            }
            memset(txBuf, 0, sizeof(txBuf));
        }

        size_t bytesWritten = 0;
        const esp_err_t err = i2s_channel_write(
            _txHandle, txBuf, sizeof(txBuf), &bytesWritten, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesWritten != sizeof(txBuf)) {
            __atomic_fetch_add(&_sourcePcmWriteErrors, 1U, __ATOMIC_RELAXED);
            if (lastWriteErrorMs == 0 || millis() - lastWriteErrorMs >= 2000U) {
                log_w("##[BT-TX]# radio PCM write failed: err=%d expected=%u written=%u",
                      err, (unsigned)sizeof(txBuf), (unsigned)bytesWritten);
                lastWriteErrorMs = millis();
            }
            delay(10);
        } else {
            __atomic_fetch_add(&_sourceRadioBlocks, 1U, __ATOMIC_RELAXED);
        }
    }

    __atomic_store_n(&_sourceRadioPrimed, false, __ATOMIC_RELAXED);
    log_i("##[BT-TX]# radio relay task stopped");
    _sourceRadioTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

#endif // USE_BLUETOOTH
