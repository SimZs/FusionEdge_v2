#pragma once
#include "common.h"

#if DSP_MODEL == DSP_DUMMY
#    define DUMMYDISPLAY
#endif

#ifndef DUMMYDISPLAY
class ScrollWidget;
class PlayListWidget;
class FillWidget;
class Pager;
class Page;
class SpectrumWidget;
class NumWidget;
class ClockWidget;
class TextWidget;
class textBoxWidget;
class SliderWidget;
class VolumeWidget;
class BufferWidget;
class WifiWidget;
class BluetoothWidget;
class GearWidget;
class TransportWidget;
class WeatherIconWidget;
class StationIconWidget;
class DateWidget;
class StatusWidget;
class EqWidget;
class CassetteWidget;
#ifdef USE_COVER_SCREENSAVER
class CoverScreensaverWidget;
#endif
#ifdef USE_VU_METER_SCREENSAVER
class vuMeterWidget;
#endif

class Display {
  public:
    uint16_t      currentPlItem;
    uint16_t      numOfNextStation;
    displayMode_e _mode;

  public:
    Display() {};
    ~Display();
    displayMode_e mode() { return _mode; }
    void          mode(displayMode_e m) { _mode = m; }
    void          init();
    void          loop();
    void          _start();
    bool          ready() { return _bootStep == 2; }
    void          resetQueue();
    void          purgeQueuedRequestType(displayRequestType_e type);
    void          beginContentChange();
    bool          waitQueueEmpty(uint32_t timeoutMs = 300);
    bool          waitContentReady(uint32_t timeoutMs = 300);
    void          putRequest(displayRequestType_e type, int payload = 0);
    void          flip();
    void          invert();
    bool          deepsleep();
    void          wakeup();
    void          setContrast();
    void          lock();
    void          unlock();
    bool          isLocked() const { return _locked; }
    void          i2sReconfigBegin(); // Display DMA guard az I2S rövid újrakonfigurálásához
    void          i2sReconfigEnd();
    void          waitDMA();        // LGFX DMA transzfer befejezésének megvárása (SD/SPI szinkron)
    uint16_t      width();
    uint16_t      height();
    void          setBrightnessPercent(uint8_t percent);
    uint8_t       effectiveBrightnessPercent(uint8_t normalPercent) const;
    bool          clockScreensaverBrightnessActive() const { return _clockScreensaverBrightnessActive; }
    void          applyVuModeChange();
    void          invalidateThemeWidgets();  // Refresh all widgets when theme colors change
    bool          eqToggle();               // EQ overlay toggle, visszaadja az új állapotot
    bool          isEqOpen() const;         // EQ overlay nyitva van-e
    void          eqForceClose();           // EQ overlay kényszerbezárás
    void          eqEncoderRotate(int8_t delta);
    void          eqEncoderConfirm();
    void          _eqInlineDraw();          // EQ overlay újrarajzolása (display_eq_touch hívja)
    bool          tapPlaylistItem(uint16_t y); // Touch tap a playlist területén → kiválasztás
    bool          btInfoTap(uint16_t x, uint16_t y);
    void          btInfoScroll(int direction);
    int8_t        quickSettingsHit(uint16_t x, uint16_t y) const;
    void          quickSettingsRedraw();
    void          quickSettingsOpenEqualizer();
    void          quickSettingsScroll(int dir);
    void          quickSettingsMove(int8_t horizontal, int8_t vertical);
    void          quickSettingsConfirm();
    void          quickSettingsActivate(uint8_t index);

    // --- Mód-választó overlay (MODESELECT) ---
    void          modeSelectorOpen();
    void          modeSelectorScroll(int dir);
    void          modeSelectorConfirm();
    void          modeSelectorClose();
    void          modeSelectorTick();
    // Touchscreen számára szükséges elérés
    uint8_t       msModeCount() const { return _msCount; }
    void          msTapItem(uint8_t idx) { _msIdx = idx; modeSelectorConfirm(); }
    void          msHover(uint8_t idx) {
                      if (_mode != MODESELECT || idx == _msIdx) return;
                      _msIdx = idx;
                      putRequest(DRAWMODESELECT);
                  }

    // --- Screensaver-választó overlay (SCREENSELECT) ---
    void          screensaverSelectorOpen();
    void          screensaverSelectorScroll(int dir);
    void          screensaverSelectorConfirm();
    void          screensaverSelectorClose();
    void          screensaverSelectorTick();
    uint8_t       screensaverSelectorCount() const { return 4; }
    void          screensaverSelectorTap(uint8_t idx) {
                      if (idx >= screensaverSelectorCount()) return;
                      _ssIdx = idx;
                      screensaverSelectorConfirm();
                  }
    void          screensaverSelectorHover(uint8_t idx) {
                      if (_mode != SCREENSELECT || idx >= screensaverSelectorCount() || idx == _ssIdx) return;
                      _ssIdx = idx;
                      putRequest(DRAWSCREENSELECT);
                  }
#    ifdef NAMEDAYS_FILE
    void loopDate(bool force = false);
#    endif

  private:
    void _updateStationIcon();
    void _updateFooterPlaybackText();
    void _updateAudioInfoText();
#    ifdef USE_COVER_SCREENSAVER
    void _updateCoverScreensaver();
    void _updateCoverScreensaverText();
#    endif
    bool _clockScreensaverBrightnessActive = false;
    bool _manualCoverView = false;
    ScrollWidget *  _meta, *_title1, *_plcurrent, *_weather, *_title2;
    PlayListWidget* _plwidget;
    FillWidget *    _metabackground, *_plbackground;
    SliderWidget *  _volbar;
    Pager*          _pager;
    Page*           _footer;
    SpectrumWidget* _spectrum;
    NumWidget*      _nums;
    ClockWidget*    _clock;
    Page*           _boot;
    TextWidget *    _volip, *_voltxt, *_rssi, *_bitrate, *_chtxt;
    textBoxWidget * _ipbox = nullptr, *_rssibox = nullptr, *_bootstring;
    textBoxWidget*  _footerPlayMode = nullptr;
    textBoxWidget*  _footerStation = nullptr;
    textBoxWidget*  _audioInfoText = nullptr;
    VolumeWidget*   _volwidget = nullptr;
    BufferWidget*   _bufferwidget = nullptr;
    WifiWidget*     _wifiwidget = nullptr;
    BluetoothWidget* _bluetoothwidget = nullptr;
    GearWidget*     _gearwidget = nullptr;
    TransportWidget* _transportwidget = nullptr;
    WeatherIconWidget*  _weatherIcon;
    StationIconWidget*  _stationIcon;
    DateWidget*     _datewidget;
    StatusWidget*   _statuswidget = nullptr;
    EqWidget*       _eqwidget = nullptr;
#    ifdef USE_CASSETTE_SCREENSAVER
    CassetteWidget* _cassettewidget = nullptr;
#    endif
#    ifdef USE_COVER_SCREENSAVER
    CoverScreensaverWidget* _coverScreensaverWidget = nullptr;
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
    vuMeterWidget* _vuMeterWidget = nullptr;
#    endif

    bool     _locked = false;
    bool     _panelAwake = true;
    uint8_t  _bootStep;
    int      _lastRssiText = 9999;
    uint32_t _lastRssiTextMs = 0;
    uint8_t  _btInfoFirst = 0;
    uint32_t _btInfoFingerprint = 0;
    uint32_t _btInfoPollMs = 0;
    uint8_t  _quickSettingsIndex = 0;
    uint32_t _quickSettingsLastActivityMs = 0;
    uint32_t _forecastLastActivityMs = 0;

    // --- Mód-választó overlay state (publikus, touchscreen.cpp éri el) ---
    static constexpr uint8_t MS_MAX_MODES = 5;
    uint8_t  _msCount      = 0;
    uint8_t  _msIdx        = 0;
    int8_t   _msValues[MS_MAX_MODES];     // mód értékek (-1 = DLNA)
    const char* _msLabels[MS_MAX_MODES];  // mód feliratok
    uint32_t _msAutoCloseMs = 0;          // auto-bezárás időpontja
    displayMode_e _msReturnMode = PLAYER;
    void     _msBuildList();              // elérhető módok listájának összeállítása
    void     _msDraw();                   // overlay rajzolása
    uint8_t  _ssIdx = 0;
    uint32_t _ssAutoCloseMs = 0;
    void     _ssDraw();
    void     _time(bool redraw = false);
    void     _apScreen();
    void     _swichMode(displayMode_e newmode);
    void     _drawPlaylist();
    void     _volume();
    void     _ssUpdateDate();
    void     _volInlineShow();
    void     _volInlineHide();
    void     _volInlineDraw();
    void     _eqInlineShow();
    void     _eqInlineHide();
    void     _title();
    void     _station();
    void     _drawNextStationNum(uint16_t num);
    void     _createDspTask();
    void     _showDialog(const char* title);
    void     _wifiInlineShow();           // SSID/IP/RSSI nezet a kozepso savban
    void     _wifiInlineHide();
    void     _drawWifiInfo();
    void     _bluetoothInlineShow();
    void     _bluetoothInlineHide();
    void     _drawBluetoothInfo();
    void     _updateBluetoothWidget();
    void     _quickSettingsInlineShow();
    void     _quickSettingsInlineHide();
    void     _drawQuickSettings();
    void     _quickSettingsTick();
    void     _forecastInlineShow();
    void     _forecastInlineHide();
    void     _drawForecast();
    void     _forecastTick();
    void     _updateGearWidget();
    void     _sdProgressDraw(int count);   // SD indexelés progress a VOL_AREA sávban
    void     _buildPager();
    void     _bootScreen();
    void     _layoutChange(bool played);
    void     _setRSSI(int rssi);
    void     _updateBootSprite(int ssidIndex);
    void     _refreshThemeColors();
    void     _applyRssiMode();
};

#else

class Display {
  public:
    uint16_t      currentPlItem;
    uint16_t      numOfNextStation;
    displayMode_e _mode;

  public:
    Display() {};
    displayMode_e mode() { return _mode; }
    void          mode(displayMode_e m) { _mode = m; }
    void          init();
    void          _start();
    void          putRequest(displayRequestType_e type, int payload = 0);
    void          loop() {}
    bool          ready() { return true; }
    void          resetQueue() {}
    void          purgeQueuedRequestType(displayRequestType_e type) {}
    void          beginContentChange() {}
    bool          waitQueueEmpty(uint32_t timeoutMs = 0) { return true; }
    bool          waitContentReady(uint32_t timeoutMs = 0) { return true; }
    void          centerText(const char* text, uint8_t y, uint16_t fg, uint16_t bg) {}
    void          rightText(const char* text, uint8_t y, uint16_t fg, uint16_t bg) {}
    void          flip() {}
    void          invert() {}
    void          setContrast() {}
    void          applyVuModeChange() {}
    bool          deepsleep() { return true; }
    void          wakeup() {}
    void          printPLitem(uint8_t pos, const char* item) {}
    void          lock() {}
    void          unlock() {}
    bool          isLocked() const { return false; }
    uint16_t      width() { return 0; }
    uint16_t      height() { return 0; }
    void          setBrightnessPercent(uint8_t percent) {}
    uint8_t       effectiveBrightnessPercent(uint8_t normalPercent) const { return normalPercent > 100 ? 100 : normalPercent; }
    bool          clockScreensaverBrightnessActive() const { return false; }
    bool          isEqOpen() const { return false; }
    void          eqEncoderRotate(int8_t delta) { (void)delta; }
    void          eqEncoderConfirm() {}

  private:
    void _createDspTask();
};

#endif

void display_show_maintenance_screen();
void display_eq_touch(uint16_t x, uint16_t y, bool drag);

// FusionEdge: Auto EQ – genre-name alapú EQ preset keresés (case-insensitive).
// A preset nevek/értékek a display.cpp-ben élnek (eqPresetNames/eqPresetValues).
// Visszatér true-val és kitölti bass/mid/treb-et, ha talál egyezést, egyébként
// false-szal (a kimeneti paraméterek ilyenkor nem módosulnak).
bool findEqPresetByGenre(const char* genre, int8_t& bass, int8_t& mid, int8_t& treb);

extern Display display;
