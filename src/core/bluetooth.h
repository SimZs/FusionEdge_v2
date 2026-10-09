#pragma once
#include "options.h"
#include "bluetooth_config.h"

#ifdef USE_BLUETOOTH
/*
  FusionEdge laboratory - FSC-BT1058 AT driver + isolated I2S tests
  --------------------------------------------------------------
  UART: HardwareSerial(2) on BT_UART_TX / BT_UART_RX (myoptions.h)
  I2S:  Second peripheral (I2S_NUM_1), one direction at a time:
        sink: module I2S_OUT -> dedicated RX data pin -> bridge -> DAC
        source: player PCM tap -> dedicated TX data pin -> module I2S_IN
*/
#include <Arduino.h>
#include <driver/i2s_std.h>

enum btState_e : uint8_t {
    BT_STATE_UNKNOWN    = 0,
    BT_STATE_CONNECTED  = 1,  // BT_CN
    BT_STATE_PLAYING    = 2,  // BT_PA
    BT_STATE_PAUSED     = 3,  // BT_STOP
    BT_STATE_DISC       = 4,  // BT_DISC - disconnected
};

enum btModuleRole_e : uint8_t {
    BT_ROLE_UNKNOWN = 0,
    BT_ROLE_SINK,
    BT_ROLE_SOURCE,
};

enum btSourceOutputMode_e : uint8_t {
    BT_SOURCE_OUTPUT_BLUETOOTH_ONLY = 0,
    BT_SOURCE_OUTPUT_LOCAL_AND_BLUETOOTH = 1,
    BT_SOURCE_OUTPUT_LOCAL_ONLY = 2,
};

enum btRoleTransitionState_e : uint8_t {
    BT_ROLE_TRANSITION_IDLE = 0,
    BT_ROLE_TRANSITION_ASSESS,
    BT_ROLE_TRANSITION_WAIT_DISCONNECT,
    BT_ROLE_TRANSITION_WAIT_REBOOT,
    BT_ROLE_TRANSITION_APPLY_AUX,
    BT_ROLE_TRANSITION_APPLY_I2S,
    BT_ROLE_TRANSITION_REBOOT_CONFIG,
    BT_ROLE_TRANSITION_READY,
    BT_ROLE_TRANSITION_FAILED,
};

struct btSourceScanDevice_t {
    char    address[13] = {0};
    char    name[64] = {0};
    int16_t rssi = -127;
};

struct btPairedDevice_t {
    uint8_t  index = 0;
    uint16_t profile = 0;
    char     address[13] = {0};
    char     name[64] = {0};
};

struct btDisplayDevice_t {
    uint16_t profile = 0;
    char     address[13] = {0};
    char     name[64] = {0};
    int16_t  rssi = -127;
    bool     connected = false;
};

class BluetoothPlayer {
  public:
    void begin();
    void end();
    void loop();

    // --- transport controls ---
    void play();
    void pause();
    void toggle();
    void next();
    void prev();
    void volUp();
    void volDown();
    void setVol(uint8_t step0to21);

    // --- status / metadata ---
    bool       connected()  const {
        return _state == BT_STATE_CONNECTED || _state == BT_STATE_PLAYING ||
               _state == BT_STATE_PAUSED;
    }
    bool       playing()    const { return _state == BT_STATE_PLAYING; }
    btState_e  state()      const { return _state; }
    btModuleRole_e moduleRole() const {
        return __atomic_load_n(&_moduleRole, __ATOMIC_ACQUIRE);
    }
    String     title()      const { return _title; }
    String     artist()     const { return _artist; }
    String     album()      const { return _album; }
    String     genre()      const { return _genre; }
    uint8_t    trackNum()   const { return _trackNum; }
    uint8_t    trackTotal() const { return _trackTotal; }
    uint32_t   trackMs()    const { return _trackMs; }
    uint32_t   posMs()      const { return _posMs; }
    const char* streamCodec() const { return _streamCodec; }
    uint32_t   streamSampleRate() const { return _bridgeSampleRate; }
    uint32_t   inputCodecSampleRate() const { return _inputCodecSampleRate; }
    uint32_t   sourceCodecSampleRate() const { return _sourceCodecSampleRate; }
    int8_t     remoteBatteryLevel() const {
        return __atomic_load_n(&_remoteBatteryLevel, __ATOMIC_ACQUIRE);
    }
    const char* remoteBatteryAddress() const { return _remoteBatteryAddress; }
    uint8_t    streamBits() const { return _streamBits; }

    // The module itself persists MODCFG. On entry to Bluetooth input, Edge
    // exposes the BT player screen as soon as the previous decoder has safely
    // stopped; the I2S bridge is still started only after role/configuration
    // verification has completed.
    // When leaving Bluetooth input, targetPlaybackMode preserves the exact
    // WEB/SD playback destination selected by the caller. A negative value
    // keeps the laboratory-console default of returning to WEB.
    bool requestRole(btModuleRole_e role, int8_t targetPlaybackMode = -1);
    bool roleTransitionBusy() const;
    const char* roleTransitionStateName() const;

    // --- source speaker discovery / connection ---
    bool startSourceScan(uint8_t seconds, uint8_t clientId = 0);
    bool stopSourceScan(uint8_t clientId = 0);
    bool connectSourceDevice(const char* address, uint8_t clientId = 0);
    bool disconnectSourceDevice(uint8_t clientId = 0);
    void publishSourceUiSnapshot(uint8_t clientId = 0) const;
    // True only after the source role, audio route and the shared paired-device
    // cache have all reached their verified idle state.  The WebUI uses this
    // single gate for both Refresh and Scan so it cannot imply partial
    // readiness while the post-role-switch setup is still running.
    bool sourceUiReady() const;
    bool setSourceOutputMode(uint8_t mode, uint8_t clientId = 0);
    // Apply the persisted local-DAC route before player.init()/smart-start can
    // produce PCM. begin() later performs the full module/radio setup.
    void prepareSourceOutputMode(uint8_t mode) {
        if (mode > BT_SOURCE_OUTPUT_LOCAL_ONLY) {
            mode = BT_SOURCE_OUTPUT_LOCAL_AND_BLUETOOTH;
        }
        __atomic_store_n(&_sourceOutputMode, mode, __ATOMIC_RELEASE);
        __atomic_store_n(
            &_sourceLocalDacEnabled,
            mode != BT_SOURCE_OUTPUT_BLUETOOTH_ONLY,
            __ATOMIC_RELEASE);
    }
    uint8_t sourceOutputMode() const {
        return __atomic_load_n(&_sourceOutputMode, __ATOMIC_ACQUIRE);
    }
    int8_t moduleBluetoothEnabled() const {
        return _moduleBluetoothEnabled;
    }
    bool sourceOutputRoutePending() const {
        return _sourceRouteApplyPending || _sourceRouteVerifyPending;
    }

    // --- local TFT Bluetooth view (read-only cache + deferred refresh) ---
    uint8_t displayPairedDeviceCount() const;
    bool displayPairedDeviceAt(uint8_t index, btDisplayDevice_t& out) const;
    bool displayControlsBusy() const;
    uint32_t displayFingerprint() const;
    void requestDisplayUiRefresh();

    // --- sink/input identity ---
    bool setInputAlias(const char* alias, uint8_t clientId = 0);
    void publishInputUiSnapshot(uint8_t clientId = 0,
                                const char* message = nullptr) const;
    void requestInputUiRefresh(uint8_t clientId = 0);
    bool setInputDiscoverable(bool enabled, uint8_t clientId = 0);
    bool forgetInputPairedDevice(const char* address, uint8_t clientId = 0);

    // --- I2S RX bridge ---
    bool startBridge();
    void stopBridge();
    bool bridgeRunning() const { return _bridgeTaskHandle != nullptr; }
    bool bridgeRunningOrPending() const { return _bridgeTaskHandle != nullptr || _bridgeStartPending; }
    bool sourceToneRunning() const { return _sourceToneTaskHandle != nullptr; }
    bool sourceRadioRunning() const { return _sourceRadioTaskHandle != nullptr; }
    void setSourceLocalDacEnabled(bool enabled) {
        __atomic_store_n(&_sourceLocalDacEnabled, enabled, __ATOMIC_RELEASE);
    }
    bool sourceLocalDacEnabled() const {
        return __atomic_load_n(&_sourceLocalDacEnabled, __ATOMIC_ACQUIRE);
    }
    bool silenceLocalDacForSource() const {
        const btModuleRole_e currentRole =
            __atomic_load_n(&_moduleRole, __ATOMIC_ACQUIRE);
        const btModuleRole_e requestedRole =
            __atomic_load_n(&_requestedRole, __ATOMIC_ACQUIRE);
        // "Bluetooth only" is an output-route choice, not a property of the
        // PCM relay task. Keep the local DAC silent throughout source mode,
        // including speaker reconnects and periods with no speaker attached.
        // Start muted when Bluetooth-only is stored and the module role is not
        // known yet. This prevents a few seconds of local audio leakage during
        // boot. A verified sink/input role explicitly releases the mute.
        return !sourceLocalDacEnabled() &&
               currentRole != BT_ROLE_SINK &&
               requestedRole != BT_ROLE_SINK;
    }
    // Called by Audio's post-DSP weak callback override. This is deliberately
    // non-blocking: full buffers drop new frames rather than stalling playback.
    void pushSourcePcm(const int32_t* stereo32, size_t frames);
    // PLAYSTAT may remain "playing" while the remote application sends only
    // silent PCM. Treat audio as active only after a recent non-silent block.
    bool audioActive() const {
        const uint32_t lastPcmMs = _lastPcmMs;
        return lastPcmMs != 0 &&
               static_cast<uint32_t>(millis() - lastPcmMs) < 1000U;
    }
    uint16_t vuLevel() const {
        return (static_cast<uint16_t>(_vuRight) << 8) | _vuLeft;
    }

  private:
    enum btSinkControl_e : uint8_t {
        BT_SINK_CONTROL_NONE = 0,
        BT_SINK_CONTROL_PLAY,
        BT_SINK_CONTROL_PAUSE,
        BT_SINK_CONTROL_TOGGLE,
        BT_SINK_CONTROL_NEXT,
        BT_SINK_CONTROL_PREVIOUS,
        BT_SINK_CONTROL_SET_VOLUME,
    };

    enum btRemoteCommand_e : uint8_t {
        BT_REMOTE_PLAY = 0,
        BT_REMOTE_PAUSE,
        BT_REMOTE_STOP,
        BT_REMOTE_NEXT,
        BT_REMOTE_PREVIOUS,
        BT_REMOTE_VOLUME_UP,
        BT_REMOTE_VOLUME_DOWN,
    };

    void sendAT(const char* cmd);
    void handleRemoteControlEvent(btRemoteCommand_e command,
                                  const char* wireEvent,
                                  const char* action);
    void handleLabConsole();
    void printLabHelp() const;
    void serviceStartupProbe();
    void serviceSourceRadioStats();
    void serviceSourceUiPlaybackState();
    void serviceSinkStreamInfo();
    void serviceSourceRadioAutostart();
    void serviceSourceAudioEnable();
    void serviceSourceDeviceControl();
    void retryOrFailSourceOutputRoute(const char* reason);
    void serviceRoleTransition();
    void serviceAliasSync();
    void serviceInputUiControl();
    void serviceSinkControl();
    void queueSinkControl(btSinkControl_e command);
    bool sinkControlBusy() const;
    static const char* sinkControlName(btSinkControl_e command);
    static const char* sinkControlAtCommand(btSinkControl_e command);
    void retryOrFailAliasSync(const char* reason);
    bool aliasSyncBusy() const;
    bool inputUiBusy() const;
    void publishInputPairedList(uint8_t clientId = 0) const;
    void handlePairedListEntry(const char* value);
    bool beginNextInputRssiProbe();
    void handleRemoteRssi(const char* value);
    void beginForgetInputPairedDevice(const char* address, uint8_t clientId);
    void finishInputUiCommand(bool success, const char* message);
    void invalidateStartupProbe(uint32_t delayMs);
    bool roleConfigurationMatches(btModuleRole_e role) const;
    void completeRoleTransition();
    void failRoleTransition(const char* reason);
    void parseLine(char* line);
    bool initI2SRx();
    void deinitI2SRx();
    bool startSourceTone();
    void stopSourceTone();
    bool startSourceRadio();
    void stopSourceRadio();
    bool initI2STx();
    void deinitI2STx();
    void resetI2SPins();
    void beginSinkMetaGeneration(bool rememberCurrentTitle);
    void activateSinkMetaGeneration();
    void applyTrackInfo(const String& title, const String& artist,
                        const String& album);
    void publishSinkPlaybackTitle();
    void clearMeta();
    void setFallbackTitle(const char* title);
    void requestStreamInfo(bool force = false);
    void handleTrackInfo(const char* value);
    void handleTrackStatus(const char* value);
    void handleA2dpStatus(uint32_t status);
    void handlePlayStatus(uint32_t status);
    void handleSourceScanResult(const char* value);
    void handleSourceConnectedDevice(const char* value, bool hasName);
    void handleInputConnectedDevice(const char* value, bool hasName);
    bool beginSourceScan(uint8_t seconds, uint8_t clientId);
    void finishSourceScan(const char* state, const char* message = nullptr);
    void startSourceSwitchConnection(bool restorePrevious);
    void queueSourceSwitchRollback(const char* reason);
    void completeSourceSwitchConnection(const char* address);
    void publishSourceUiError(uint8_t clientId, const char* message) const;
    const btSourceScanDevice_t* findSourceScanDevice(const char* address) const;
    static bool validBluetoothAddress(const char* address);

    enum sourceUiCommand_e : uint8_t {
        SOURCE_UI_COMMAND_NONE = 0,
        SOURCE_UI_COMMAND_SCAN,
        SOURCE_UI_COMMAND_CONNECT,
        SOURCE_UI_COMMAND_DISCONNECT,
        SOURCE_UI_COMMAND_STOP_SCAN,
        SOURCE_UI_COMMAND_CANCEL_RECONNECT_FOR_SCAN,
        SOURCE_UI_COMMAND_SET_BT_ENABLE,
        SOURCE_UI_COMMAND_VERIFY_BT_ENABLE,
        SOURCE_UI_COMMAND_RECONNECT_LAST,
        SOURCE_UI_COMMAND_SWITCH_DISCONNECT,
        SOURCE_UI_COMMAND_SWITCH_QUERY_STATUS,
    };

    enum sourceSwitchState_e : uint8_t {
        SOURCE_SWITCH_IDLE = 0,
        SOURCE_SWITCH_DISCONNECTING,
        SOURCE_SWITCH_WAIT_STANDBY,
        SOURCE_SWITCH_CONNECTING_TARGET,
        SOURCE_SWITCH_WAIT_ROLLBACK,
        SOURCE_SWITCH_DISCONNECTING_TARGET,
        SOURCE_SWITCH_WAIT_TARGET_STANDBY,
        SOURCE_SWITCH_CONNECTING_PREVIOUS,
    };

    enum aliasSyncState_e : uint8_t {
        ALIAS_SYNC_IDLE = 0,
        ALIAS_SYNC_WAIT_SET_RESPONSE,
        ALIAS_SYNC_WAIT_VERIFY_DELAY,
        ALIAS_SYNC_WAIT_VERIFY_RESPONSE,
        ALIAS_SYNC_FAILED,
    };

    enum inputUiCommand_e : uint8_t {
        INPUT_UI_COMMAND_NONE = 0,
        INPUT_UI_REFRESH_PAIR,
        INPUT_UI_REFRESH_LIST,
        INPUT_UI_REFRESH_RSSI,
        INPUT_UI_SET_DISCOVERABLE,
        INPUT_UI_VERIFY_DISCOVERABLE,
        INPUT_UI_CANCEL_RECONNECT_FOR_FORGET,
        INPUT_UI_FORGET_DEVICE,
    };

    static void bridgeTaskWrapper(void* param);
    void        bridgeTask();
    bool        startBridgeNow();
    static void sourceToneTaskWrapper(void* param);
    void        sourceToneTask();
    static void sourceRadioTaskWrapper(void* param);
    void        sourceRadioTask();
    bool        popSourcePcmElastic(int16_t* stereo16, size_t outputFrames,
                                    uint32_t stepQ16, uint32_t& phaseQ16,
                                    size_t& consumedFrames);
    uint32_t    sourcePcmBufferedFrames() const;

    HardwareSerial _uart{2};
    char           _lineBuf[512] = {0};
    uint16_t       _lineLen = 0;
    bool           _lineOverflow = false;
    char           _consoleBuf[96] = {0};
    uint8_t        _consoleLen = 0;
    bool           _consoleCommandPending = false;
    uint32_t       _consoleCommandDeadlineMs = 0;
    uint8_t        _startupProbeStep = 0;
    uint32_t       _nextStartupProbeMs = 0;
    bool           _startupProbeComplete = false;

    btState_e _state      = BT_STATE_UNKNOWN;
    btModuleRole_e _moduleRole = BT_ROLE_UNKNOWN;
    int16_t   _moduleI2sCfg = -1;
    int16_t   _moduleAuxCfg = -1;
    int32_t   _moduleProfile = -1;
    int16_t   _moduleAvrcpCfg = -1;
    String    _title;
    String    _artist;
    String    _album;
    String    _genre;
    String    _previousSinkTitle;
    String    _pendingSinkTitle;
    String    _pendingSinkArtist;
    String    _pendingSinkAlbum;
    uint8_t   _trackNum   = 0;
    uint8_t   _trackTotal = 0;
    uint32_t  _trackMs    = 0;
    uint32_t  _posMs      = 0;
    bool      _metaUpdated = false;
    bool      _sinkPlaybackSeen = false;
    bool      _sinkMetaFreshAccepted = false;
    bool      _sinkStaleMetaLogged = false;
    int8_t    _lastSinkPlayStatus = -1;
    bool      _streamInfoRequested = false;
    char      _streamCodec[16] = {};
    uint8_t   _streamBits = 0;

    i2s_chan_handle_t _rxHandle          = nullptr;
    i2s_chan_handle_t _txHandle          = nullptr;
    TaskHandle_t      _bridgeTaskHandle  = nullptr;
    TaskHandle_t      _sourceToneTaskHandle = nullptr;
    TaskHandle_t      _sourceRadioTaskHandle = nullptr;
    bool              _rxChannelEnabled = false;
    volatile bool     _bridgeStopRequest = false;
    volatile bool     _sourceToneStopRequest = false;
    volatile bool     _sourceRadioStopRequest = false;
    volatile bool     _sourceRadioEnabled = false;
    volatile bool     _sourceLocalDacEnabled = true;
    volatile bool     _bridgeStartPending = false;
    volatile uint32_t _lastPcmMs = 0;
    uint32_t          _bridgeSampleRate = 0;
    uint32_t          _sourceCodecSampleRate = 0;
    volatile uint8_t  _vuLeft = 0;
    volatile uint8_t  _vuRight = 0;
    volatile uint32_t _sourcePcmWriteFrame = 0;
    volatile uint32_t _sourcePcmReadFrame = 0;
    volatile uint32_t _sourcePcmFramesIn = 0;
    volatile uint32_t _sourcePcmFramesOut = 0;
    volatile uint32_t _sourcePcmDroppedFrames = 0;
    volatile uint32_t _sourcePcmRebufferEvents = 0;
    volatile uint32_t _sourcePcmUnderrunBlocks = 0;
    volatile uint32_t _sourcePcmWriteErrors = 0;
    volatile uint32_t _sourcePcmHighWaterFrames = 0;
    volatile uint32_t _sourcePcmNonZeroSamples = 0;
    volatile uint32_t _sourceRadioBlocks = 0;
    volatile int32_t  _sourcePcmRateAdjustPpm = 0;
    volatile uint16_t _sourcePcmPeakLeft = 0;
    volatile uint16_t _sourcePcmPeakRight = 0;
    volatile bool     _sourceRadioPrimed = false;
    volatile bool     _sourceRadioEverPrimed = false;
    uint32_t          _sourceRadioLastStatsMs = 0;
    uint32_t          _nextSourceAudioEnableMs = 0;
    uint32_t          _nextSourceRadioAutostartMs = 0;
    uint32_t          _sourceAudioEnableDeadlineMs = 0;
    uint32_t          _lastAtTxMs = 0;
    uint8_t           _sourceAudioEnableAttempts = 0;
    bool              _sourceAudioPathReady = false;
    bool              _sourceAudioEnablePending = false;
    bool              _sourceRadioManualStop = false;
    btSourceScanDevice_t _sourceScanDevices[BT_SOURCE_SCAN_MAX_DEVICES];
    uint8_t           _sourceScanDeviceCount = 0;
    bool              _sourceScanActive = false;
    uint32_t          _sourceScanDeadlineMs = 0;
    uint8_t           _sourceScanRequestedSeconds = BT_SOURCE_SCAN_SECONDS;
    sourceUiCommand_e _sourceUiCommand = SOURCE_UI_COMMAND_NONE;
    uint8_t           _sourceUiCommandClientId = 0;
    uint32_t          _sourceUiCommandDeadlineMs = 0;
    sourceSwitchState_e _sourceSwitchState = SOURCE_SWITCH_IDLE;
    uint8_t           _sourceSwitchClientId = 0;
    uint32_t          _sourceSwitchDeadlineMs = 0;
    uint32_t          _sourceSwitchNextStatusQueryMs = 0;
    char              _sourceSwitchTargetAddress[13] = {0};
    char              _sourceSwitchPreviousAddress[13] = {0};
    char              _sourceSwitchPreviousName[64] = {0};
    bool              _sourceSwitchAddressReported = false;
    bool              _sourceConnectPending = false;
    bool              _sourceManualConnectPending = false;
    uint32_t          _sourceConnectDeadlineMs = 0;
    char              _sourceConnectedAddress[13] = {0};
    char              _sourceConnectedName[64] = {0};
    volatile uint8_t  _sourceOutputMode = BT_SOURCE_OUTPUT_LOCAL_AND_BLUETOOTH;
    int8_t            _moduleBluetoothEnabled = -1;
    bool              _sourceRouteApplyPending = true;
    bool              _sourceRouteVerifyPending = false;
    bool              _sourceBluetoothTarget = true;
    uint8_t           _sourceRouteAttempts = 0;
    uint8_t           _sourceRouteClientId = 0;
    uint32_t          _sourceRouteNextActionMs = 0;
    bool              _sourceReconnectAfterEnable = false;
    uint32_t          _sourceReconnectNextActionMs = 0;
    uint32_t          _lastA2dpStatus = 0;
    uint32_t          _lastAvrcpStatus = 0;
    uint32_t          _lastHfpStatus = 0;
    volatile int8_t   _remoteBatteryLevel = -1;
    char              _remoteBatteryAddress[13] = {0};
    btModuleRole_e    _requestedRole = BT_ROLE_UNKNOWN;
    btRoleTransitionState_e _roleTransitionState = BT_ROLE_TRANSITION_IDLE;
    uint32_t          _roleTransitionNextActionMs = 0;
    uint32_t          _roleTransitionDeadlineMs = 0;
    uint32_t          _roleDisconnectDeadlineMs = 0;
    uint8_t           _roleChangeAttempts = 0;
    uint8_t           _roleConfigAttempts = 0;
    bool              _roleConfigChanged = false;
    char              _moduleName[32] = {0};
    char              _nameBeforeRoleChange[32] = {0};
    aliasSyncState_e  _aliasSyncState = ALIAS_SYNC_IDLE;
    uint32_t          _aliasSyncNextActionMs = 0;
    uint32_t          _aliasSyncDeadlineMs = 0;
    uint8_t           _aliasSyncAttempts = 0;
    inputUiCommand_e  _inputUiCommand = INPUT_UI_COMMAND_NONE;
    uint8_t           _inputUiClientId = 0;
    uint32_t          _inputUiCommandDeadlineMs = 0;
    bool              _inputRefreshPending = true;
    bool              _sourceUiInitializationPending = true;
    bool              _requestedDiscoverable = false;
    int8_t            _moduleDiscoverable = -1;
    btPairedDevice_t  _inputPairedDevices[BT_INPUT_PAIRED_MAX_DEVICES];
    uint8_t           _inputPairedDeviceCount = 0;
    btPairedDevice_t  _inputPairedRefreshDevices[BT_INPUT_PAIRED_MAX_DEVICES];
    uint8_t           _inputPairedRefreshDeviceCount = 0;
    bool              _inputPairedRefreshSawEnd = false;
    uint8_t           _inputRssiProbeIndex = 0;
    bool              _inputRssiProbeMatched = false;
    char              _inputRssiProbeAddress[13] = {0};
    int16_t           _remoteRssi = -127;
    char              _remoteRssiAddress[13] = {0};
    char              _inputForgetAddress[13] = {0};
    char              _inputConnectedAddress[13] = {0};
    char              _inputConnectedName[64] = {0};
    char              _inputCodec[16] = {0};
    uint32_t          _inputCodecSampleRate = 0;
    QueueHandle_t     _sinkControlQueue = nullptr;
    btSinkControl_e   _sinkControlInFlight = BT_SINK_CONTROL_NONE;
    uint32_t          _sinkControlDeadlineMs = 0;
    volatile bool     _sinkVolumePending = false;
    volatile uint8_t  _sinkVolumeStep = 0;
    bool              _sourceUiPlayingKnown = false;
    bool              _sourceUiPlaying = false;
    int16_t*          _sourcePcmRing = nullptr;
};

extern BluetoothPlayer bluetooth;
#endif
