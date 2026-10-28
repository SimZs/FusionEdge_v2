#include "Arduino.h"
#include <LittleFS.h>
#include "options.h"
#include "WiFi.h"
#include "time.h"
#include "config.h"
#include "display.h"
#include "presets.h"
#include "player.h"
#include "network.h"
#include "netserver.h"
#include "timekeeper.h"
#include "../pluginsManager/pluginsManager.h"
#include "../displays/display_select.h"
#include "../displays/widgets/widgets.h"
#include "../displays/widgets/iconWidgets.h"
#include "../displays/widgets/spectrumWidget.h"
#include "../core/spectrumAnalyzer.h"
#include "../displays/widgets/eqWidget.h"
#include "../displays/widgets/pages.h"
#include "../displays/tools/language.h"
#include "fonts.h"
#include "speaker_bitmaps.h"
#include "freertos/semphr.h"
#ifdef USE_BLUETOOTH
#    include "bluetooth.h"
#endif
#ifdef NAMEDAYS_FILE
#    include "namedays.h"
#endif
#if BRIGHTNESS_PIN != 255
#    include "../plugins/backlight/backlight.h"
#endif
#include "coverart.h"
#include "battery.h"

// All inline menus cover the complete lower player area, including the footer.
#define VOL_AREA_TOP    137
#define VOL_AREA_HEIGHT (DSP_HEIGHT - VOL_AREA_TOP)
#define VOL_AREA_LEFT     0
#define VOL_AREA_WIDTH  DSP_WIDTH
#define INLINE_MENU_IDLE_TIMEOUT_MS 5000UL
#include "serial_littlefs.h"
#include "touchscreen.h"
// #define LGFX_USE_PNG

Display display;
#ifdef USE_NEXTION
#    include "../displays/nextion.h"
Nextion nextion;
#endif

#ifndef CORE_STACK_SIZE
#    define CORE_STACK_SIZE 1024 * 8 // 4
#endif
#ifndef DSP_TASK_PRIORITY
#    if DSP_MODEL == DSP_AXS15231B
#        define DSP_TASK_PRIORITY 2
#    else
#        define DSP_TASK_PRIORITY 3 //"task_prioritas"
#    endif
#endif
#ifndef DSP_TASK_CORE_ID
#    define DSP_TASK_CORE_ID 0
#endif
#ifndef DSP_TASK_DELAY
#    if DSP_MODEL == DSP_AXS15231B
#        define DSP_TASK_DELAY pdMS_TO_TICKS(5)
#    else
#        define DSP_TASK_DELAY pdMS_TO_TICKS(30) // cap for 50 fps
#    endif
#endif
#ifndef DISPLAY_QUEUE_LENGTH
#    define DISPLAY_QUEUE_LENGTH 20
#endif

#ifndef RSSI_TEXT_HYSTERESIS_DB
#    define RSSI_TEXT_HYSTERESIS_DB 3
#endif
#ifndef RSSI_TEXT_FORCE_UPDATE_MS
#    define RSSI_TEXT_FORCE_UPDATE_MS 30000UL
#endif

#define DSP_QUEUE_TICKS 0

#ifndef DSQ_SEND_DELAY
#    define DSQ_SEND_DELAY pdMS_TO_TICKS(200)
#endif

QueueHandle_t displayQueue;
static SemaphoreHandle_t displayMutex = nullptr;
static uint16_t displayContentGeneration = 1;
static bool displayContentChanging = false;

class DisplayMutexGuard {
  public:
    explicit DisplayMutexGuard(TickType_t ticks) {
        _taken = (displayMutex == nullptr) || (xSemaphoreTakeRecursive(displayMutex, ticks) == pdTRUE);
    }
    ~DisplayMutexGuard() {
        if (_taken && displayMutex != nullptr) { xSemaphoreGiveRecursive(displayMutex); }
    }
    bool taken() const { return _taken; }

  private:
    bool _taken;
};

static void purgeQueuedRequestType(displayRequestType_e type) {
    if (displayQueue == NULL) { return; }
    requestParams_t keep[DISPLAY_QUEUE_LENGTH];
    size_t          keepCount = 0;
    requestParams_t item;
    while (xQueueReceive(displayQueue, &item, 0) == pdTRUE) {
        if (item.type != type && keepCount < (sizeof(keep) / sizeof(keep[0]))) { keep[keepCount++] = item; }
    }
    for (size_t i = 0; i < keepCount; i++) { xQueueSend(displayQueue, &keep[i], 0); }
}

void Display::purgeQueuedRequestType(displayRequestType_e type) {
    ::purgeQueuedRequestType(type);
}

static bool isContentRequest(displayRequestType_e type, int payload = 0) {
    switch (type) {
        case NEWTITLE:
        case NEWSTATION:
        case NEWCOVER:
        case DBITRATE:
        case PSTOP:
        case PSTART:
        case PLAYERREBUILD:
            return true;
        case NEWMODE:
            return payload == PLAYER;
        default:
            return false;
    }
}

static bool isBackgroundRequest(displayRequestType_e type) {
    switch (type) {
        case CLOCK:
        case DSPRSSI:
        case AUDIOINFO:
            return true;
        default:
            return false;
    }
}

static bool isSplitPlayerRequest(displayRequestType_e type, int payload) {
    switch (type) {
        case NEWTITLE:
        case NEWSTATION:
        case NEWCOVER:
        case DBITRATE:
            return true;
        case NEWMODE:
            return payload == PLAYER;
        default:
            return false;
    }
}

static const char* displayCodecName(BitrateFormat format) {
    switch (format) {
        case BF_MP3:  return "MP3";
        case BF_AAC:  return "AAC";
        case BF_FLAC: return "FLAC";
        case BF_OGG:  return "OGG";
        case BF_WAV:  return "WAV";
        case BF_VOR:  return "VORBIS";
        case BF_OPU:  return "OPUS";
        default:      return "";
    }
}

static void appendAudioInfoToken(char* destination, size_t destinationSize,
                                 const char* token) {
    if (!destination || !token || !token[0]) { return; }
    if (destination[0]) { strlcat(destination, " - ", destinationSize); }
    strlcat(destination, token, destinationSize);
}

void Display::beginContentChange() {
    DisplayMutexGuard guard(portMAX_DELAY);
    displayContentGeneration++;
    if (displayContentGeneration == 0) { displayContentGeneration = 1; }
    displayContentChanging = true;
    purgeQueuedRequestType(NEWTITLE);
    purgeQueuedRequestType(NEWSTATION);
    purgeQueuedRequestType(NEWCOVER);
    purgeQueuedRequestType(DBITRATE);
    purgeQueuedRequestType(PLAYERREBUILD);
    purgeQueuedRequestType(PSTART);
    purgeQueuedRequestType(PSTOP);
    purgeQueuedRequestType(CLOCK);
    purgeQueuedRequestType(DSPRSSI);
    purgeQueuedRequestType(AUDIOINFO);
}

bool Display::waitQueueEmpty(uint32_t timeoutMs) {
    if (displayQueue == NULL) { return true; }
    const uint32_t startMs = millis();
    while (uxQueueMessagesWaiting(displayQueue) > 0) {
        if ((millis() - startMs) >= timeoutMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

bool Display::waitContentReady(uint32_t timeoutMs) {
    const uint32_t startMs = millis();
    while (displayContentChanging) {
        if ((millis() - startMs) >= timeoutMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

static void loopDspTask(void* pvParameters) {
    while (true) {
#ifndef DUMMYDISPLAY
        if (displayQueue == NULL) { break; }
        if (timekeeper.loop0()) {
            {
                DisplayMutexGuard guard(0);
                if (guard.taken()) {
#    if (TS_MODEL != TS_MODEL_UNDEFINED) && (DSP_MODEL != DSP_DUMMY)
                    if ((network.status == CONNECTED || network.status == SDREADY) && !display.isLocked()) touchscreen.loop();
#    endif
#    ifdef NAMEDAYS_FILE
                    display.loopDate(false);
#    endif
                    display.loop();
                }
            }
#    ifndef NETSERVER_LOOP1
            netserver.loop();
#    endif
        }
#else
        timekeeper.loop0();
#    ifndef NETSERVER_LOOP1
        netserver.loop();
#    endif
#endif
        vTaskDelay(DSP_TASK_DELAY);
    }
    vTaskDelete(NULL);
}

void Display::_createDspTask() {
    xTaskCreatePinnedToCore(loopDspTask, "DspTask", CORE_STACK_SIZE, NULL, DSP_TASK_PRIORITY, NULL, DSP_TASK_CORE_ID); //"task_prioritas"
}

#ifndef DUMMYDISPLAY
//============================================================================================================================
DspCore dsp;

Page* pages[] = {new Page(), new Page(), new Page(), new Page(), new Page()}; // "presets" +1 page

#    if !((DSP_MODEL == DSP_ST7735 && DTYPE == INITR_BLACKTAB) || DSP_MODEL == DSP_ST7789 || DSP_MODEL == DSP_ST7796 || DSP_MODEL == DSP_ILI9488 || DSP_MODEL == DSP_ILI9486 || \
          DSP_MODEL == DSP_ILI9341 || DSP_MODEL == DSP_ILI9225 || DSP_MODEL == DSP_ST7789_170 || DSP_MODEL == DSP_SSD1322)
#        undef BITRATE_FULL
#        define BITRATE_FULL false
#    endif

void returnPlayer() {
    display.putRequest(NEWMODE, PLAYER);
}

Display::~Display() {
    delete _pager;
    delete _footer;
    delete _plwidget;
    delete _nums;
    delete _clock;
    delete _meta;
    delete _title1;
    delete _title2;
    delete _plcurrent;
#ifdef USE_COVER_SCREENSAVER
    delete _coverScreensaverWidget;
#endif
#ifdef USE_VU_METER_SCREENSAVER
    delete _vuMeterWidget;
#endif
}

void Display::init() {
    Serial.print("##[BOOT]#\tdisplay.init\t\n");
    if (displayMutex == nullptr) {
        displayMutex = xSemaphoreCreateRecursiveMutex();
        if (displayMutex == nullptr) { log_e("##[DSP]# display mutex create failed"); }
    }
#    ifdef USE_NEXTION
    nextion.begin();
#    endif
#    if LIGHT_SENSOR != 255
    analogSetAttenuation(ADC_0db);
#    endif
    _bootStep = 0;
    // --- HARDVER INIT ---
    dsp.initDisplay(); // void DspCore::initDisplay() - Ez a függvény hívja meg a display inicializálását, ami a DspCore osztályban van definiálva. Ez a függvény felelős a kijelző beállításáért és
                       // előkészítéséért a használatra.

    if (!loadFonts()) {
        Serial.println("[FONT] ERROR: One or more binding fonts are missing from LittleFS!");
    } else {
        Serial.println("[FONT] Binding fonts successfully loaded.");
    }

    // --- QUEUE ---
    displayQueue = xQueueCreate(DISPLAY_QUEUE_LENGTH, sizeof(requestParams_t)); // Increased from 5 to 20 for better handling of rapid channel switches
    while (displayQueue == NULL) { delay(1); }
    _pager = new Pager();
    _footer = new Page();
    _plwidget = new PlayListWidget();
    _nums = new NumWidget();
    _clock = new ClockWidget();
    _meta = new ScrollWidget();
    _title1 = new ScrollWidget();
    _plcurrent = new ScrollWidget();
    _createDspTask();
    while (_bootStep == 0) { delay(10); }
    Serial.println("done");
}

uint16_t Display::width() {
    return dsp.width();
}

uint16_t Display::height() {
    return dsp.height();
}

void Display::lock() {
    if (displayMutex != nullptr) { xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); }
    _locked = true;
}

void Display::unlock() {
    _locked = false;
    if (displayMutex != nullptr) { xSemaphoreGiveRecursive(displayMutex); }
}

void Display::_bootScreen() {
    if (!_pager) return;
    _boot = new Page();
    _boot->addWidget(new ProgressWidget(bootWdtConf, bootPrgConf, 0xFFFF, 0));
    // The boot screen is always black; its border must stay visible even if
    // the user's divider theme color is dark or black.
    _bootstring = (textBoxWidget*)&_boot->addWidget(
        new textBoxWidget(bootstrConf, 50, true, 0xFFFF, 0, 0xBDF7));
    _pager->addPage(_boot);
    _pager->setPage(_boot, true);
    dsp.drawLogo(bootLogoTop);
    _bootStep = 1;
}

void Display::_buildPager() {
    _meta->init("*", metaConf, config.theme.meta, config.theme.metabg);
    _title1->init("*", title1Conf, config.theme.title1, config.theme.background);
    _clock->init(clockConf, 0, 0);
    _plcurrent->init("*", playlistConf, config.theme.plcurrent, config.theme.plcurrentbg); // scrollwidget
    _plwidget->init(_plcurrent);
    _plcurrent->moveTo({TFT_FRAMEWDT, (uint16_t)(_plwidget->currentTop()), (int16_t)playlistConf.width});

#    ifndef HIDE_TITLE2
    _title2 = new ScrollWidget("*", title2Conf, config.theme.title2, config.theme.background);
#    endif

    _plbackground = new FillWidget(playlBGConf, config.theme.plcurrentfill);

#    if DSP_INVERT_TITLE || defined(DSP_OLED)
    _metabackground = new FillWidget(metaBGConf, config.theme.metafill);
#    else
    _metabackground = new FillWidget(metaBGConfInv, config.theme.metafill);
#    endif

    // ── SpectrumWidget ────────────────────────────────────────────────────
    // A spektrum a teljes bal oldali savot hasznalja. Egy LED + gap magassaggal
    // rovidebb, az igy felszabadult sorban fut alatta a lapos bufferindikator.
    // Ez alatt kulon sor marad az audioformatum adatai szamara.
    {
        SpectrumConfig sc;
        sc.left      = TFT_FRAMEWDT;
        sc.top       = 160;
        sc.width     = PLAYER_SPECTRUM_WIDTH;
        sc.height    = 70;
        sc.bands     = PLAYER_SPECTRUM_BANDS;
        sc.barGap    = PLAYER_SPECTRUM_GAP;
        sc.ledGap    = 3;
        sc.barHeight = 4;
        sc.midPct    = config.store.vuMidPctDef;
        sc.highPct   = config.store.vuHighPctDef;
        sc.peakColor = 0xFFFF;  // fehér
        sc.bgColor   = config.theme.background;
        sc.showPeak  = (config.store.vuPeakOn != 0);
        _spectrum = new SpectrumWidget(sc);
        spectrumAnalyzer.begin(sc.bands);
        pages[PG_PLAYER]->addWidget(_spectrum);
    }

    const textBoxConfig audioInfoConf = {
        TFT_FRAMEWDT, 241, PLAYER_SPECTRUM_WIDTH, 25, 16, WA_LEFT, 0, 0, false, 0, 0};
    _audioInfoText = new textBoxWidget(
        audioInfoConf, 64, false, config.theme.bitrate,
        config.theme.background, config.theme.bitrate);
    _updateAudioInfoText();
    pages[PG_PLAYER]->addWidget(_audioInfoText);

#    ifndef HIDE_HEAPBAR
#    endif

#    ifndef HIDE_VOL
    _volwidget = new VolumeWidget(&dsp, volConf);

    // BufferWidget: a spektrum alatti, elvalasztovonalas lapos szintjelzo.
    VolumeWidgetConfig buffConf; memcpy_P(&buffConf, &bufferWidgetConf, sizeof(VolumeWidgetConfig));
    _bufferwidget = new BufferWidget(&dsp, buffConf);
    pages[PG_PLAYER]->addWidget(_volwidget);
    pages[PG_PLAYER]->addWidget(_bufferwidget);
#    endif

    // Az IP-cim, az SSID es a szoveges RSSI a Wi-Fi ikonrol megnyithato
    // informacios nezetbe kerult; a footer kozepere kulon beallitas ikon kerul.
    _wifiwidget = new WifiWidget(&dsp, &wifiConf);
    _bluetoothwidget = new BluetoothWidget(
        &dsp, {TFT_FRAMEWDT + 4, 280, 0, WA_LEFT});
    _gearwidget = new GearWidget(&dsp, {205, 288, 0, WA_LEFT});

    // A ket szoveges statusz es a szelesebb beallitasgomb egyenletesen
    // tolti ki a Bluetooth- es Wi-Fi ikon kozotti helyet.
    const textBoxConfig footerModeConf = {
        65, 288, 110, 25, 16, WA_CENTER, 1, 4, false, 0, 0};
    const textBoxConfig footerStationConf = {
        PLAYER_STATION_NUMBER_LEFT, 288, PLAYER_STATION_NUMBER_WIDTH,
        25, 16, WA_CENTER, 1, 4, false, 0, 0};
    _footerPlayMode = new textBoxWidget(
        footerModeConf, 8, false, config.theme.pmode,
        config.theme.background, config.theme.pmode);
    _footerStation = new textBoxWidget(
        footerStationConf, 16, false, config.theme.pmode,
        config.theme.background, config.theme.pmode);
    _updateFooterPlaybackText();

    _nums->init(numConf, 10, false, config.theme.digit, config.theme.background);

#    ifndef HIDE_WEATHER
    _weather = new ScrollWidget("\007", weatherConf, config.theme.weather, config.theme.background);
#    endif


    if (_bluetoothwidget) { _footer->addWidget(_bluetoothwidget); }
    if (_footerPlayMode) { _footer->addWidget(_footerPlayMode); }
    if (_gearwidget) { _footer->addWidget(_gearwidget); }
    if (_footerStation) { _footer->addWidget(_footerStation); }
    if (_ipbox) { _footer->addWidget(_ipbox); }
    if (_wifiwidget) { _footer->addWidget(_wifiwidget); }
    if (_rssibox) { _footer->addWidget(_rssibox); }
    if (_metabackground) { pages[PG_PLAYER]->addWidget(_metabackground); }
    pages[PG_PLAYER]->addWidget(_meta);
    pages[PG_PLAYER]->addWidget(_title1);
    if (_title2) { pages[PG_PLAYER]->addWidget(_title2); }
    if (_weather) { pages[PG_PLAYER]->addWidget(_weather); }

    // A korabbi playmode/station es bitrate dobozok helyen harom
    // ikonos transportgomb kap helyet.
    static_assert(TransportWidget::BUTTON_WIDTH == PLAYER_TRANSPORT_BUTTON_WIDTH &&
                      TransportWidget::BUTTON_GAP == PLAYER_TRANSPORT_BUTTON_GAP &&
                      TransportWidget::WIDTH == PLAYER_TRANSPORT_RIGHT - PLAYER_TRANSPORT_LEFT,
                  "Transport buttons and scroll width must match");
    _transportwidget = new TransportWidget(&dsp, {PLAYER_TRANSPORT_LEFT, 105, 0, WA_LEFT});
    _transportwidget->setPlaying(config.isPlaybackActive());
    _transportwidget->setActive(true, false);
    pages[PG_PLAYER]->addWidget(_transportwidget);

    // Az EQ/volume-presets allapota tovabbra is a meglevo widgetben el, de a
    // regi, kulon fokepernyos gombot mar nem rajzoljuk ki. A fogaskerek menu
    // VOLUME / PRESETS sora nyitja meg ugyanazt az inline EQ panelt.
    WidgetConfig _eqc = { 301, 105, 16, WA_LEFT };
    _eqwidget = new EqWidget();
    _eqwidget->init(_eqc, config.theme.background);

#    ifdef NAMEDAYS_FILE
    // DateWidget: dátum + névnap összefűzve, scrollozható
    // A ScrollConfig itt van definiálva, mert a conf_480x320.h körkörös include miatt
    // nem érhető el a display.cpp scope-jában fordítási időben.
    // Minden dateWidget paraméter a conf_480x320.h dateConf-ból jön
    WidgetConfig _dc; memcpy_P(&_dc, &dateConf, sizeof(WidgetConfig));
    static ScrollConfig dateWidgetConf = {{ 0, 0, 0, WA_LEFT }, 192, false, MAX_WIDTH - 20, 3000, 4, 40 };
    dateWidgetConf.widget.left     = _dc.left;
    dateWidgetConf.widget.top      = _dc.top;
    dateWidgetConf.widget.textsize = _dc.textsize;
    dateWidgetConf.widget.align    = _dc.align;
#    if DSP_WIDTH == 480 && DSP_HEIGHT == 320
    // Keep the scrolling date (including namedays) clear of the volume widget.
    dateWidgetConf.width = PLAYER_DATE_RIGHT - _dc.left;
#    endif
    // ── Icon widgetek ──────────────────────────────────────────────────────
    WidgetConfig _wic = {
        PLAYER_WEATHER_ICON_LEFT, PLAYER_WEATHER_ICON_TOP, 18, WA_RIGHT };
    _weatherIcon = new WeatherIconWidget();
    _weatherIcon->init(_wic, config.theme.background);
    _weatherIcon->lock(!config.store.showweather);
    _weatherIcon->setActive(true, false);
    pages[PG_PLAYER]->addWidget(_weatherIcon);

    WidgetConfig _sic = { TFT_FRAMEWDT, 7, 0, WA_LEFT }; // 120x120 PLAYER cover/playmode ikon
    _stationIcon = new StationIconWidget();
    _stationIcon->init(_sic, config.theme.background);
    _stationIcon->setActive(true, false);
    pages[PG_PLAYER]->addWidget(_stationIcon);

    _datewidget = new DateWidget();
    _datewidget->init(dateWidgetConf, config.theme.date, config.theme.background);
    _datewidget->setActive(true, false);
    pages[PG_PLAYER]->addWidget(_datewidget);

#    endif
    pages[PG_PLAYER]->addWidget(_clock);
#    ifdef USE_CASSETTE_SCREENSAVER
    _cassettewidget = new CassetteWidget();
    _cassettewidget->init({0, 0, 0, WA_LEFT}, config.theme.meta, config.theme.background);
    _cassettewidget->setTextColors(config.theme.title1, config.theme.title2);
    _cassettewidget->lock(true);
    pages[PG_SCREENSAVER]->addWidget(_cassettewidget);
#    endif
#    ifdef USE_COVER_SCREENSAVER
    _coverScreensaverWidget = new CoverScreensaverWidget();
    _coverScreensaverWidget->init({0, 0, 0, WA_LEFT}, config.theme.background);
    _coverScreensaverWidget->lock(true);
    pages[PG_SCREENSAVER]->addWidget(_coverScreensaverWidget);
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
    _vuMeterWidget = new vuMeterWidget();
    _vuMeterWidget->init(config.theme.background);
    _vuMeterWidget->lock(true);
    pages[PG_SCREENSAVER]->addWidget(_vuMeterWidget);
#    endif
    pages[PG_SCREENSAVER]->addWidget(_clock);
    pages[PG_PLAYER]->addPage(_footer);
    // _metabackground NEM kerül PG_DIALOG-ra (SDCHANGE overlay-nél nem kell a vonal)
    pages[PG_DIALOG]->addWidget(_meta);
    pages[PG_DIALOG]->addWidget(_nums);
    pages[PG_DIALOG]->addPage(_footer);
    pages[PG_PLAYLIST]->addWidget(_plcurrent); // scrollwidget (legacy)
    pages[PG_PLAYLIST]->addWidget(_plwidget);  // legacy
    pages[PG_PLAYER]->addWidget(_plcurrent);   // inline playlist mode
    pages[PG_PLAYER]->addWidget(_plwidget);    // inline playlist mode
    _plwidget->lock(true);   // alapból inaktív PG_PLAYER-en
    _plcurrent->lock(true);  // alapból inaktív PG_PLAYER-en
    for (const auto& p : pages) { _pager->addPage(p); }
}

void Display::_apScreen() {
    if (_boot) { _pager->removePage(_boot); }
    _boot = new Page();
#    if DSP_INVERT_TITLE || defined(DSP_OLED)
    _boot->addWidget(new FillWidget(metaBGConf, config.theme.metafill));
#    else
    _boot->addWidget(new FillWidget(metaBGConfInv, config.theme.metafill));
#    endif
    ScrollWidget* bootTitle = (ScrollWidget*)&_boot->addWidget(new ScrollWidget("*", apTitleConf, config.theme.meta, config.theme.metabg));
    bootTitle->setText("FusionEdge AP Mode");
    TextWidget* apname = (TextWidget*)&_boot->addWidget(new TextWidget(apNameConf, 30, false, config.theme.title1, config.theme.background));
    apname->setText(LANG::apNameTxt);
    TextWidget* apname2 = (TextWidget*)&_boot->addWidget(new TextWidget(apName2Conf, 30, false, config.theme.clock, config.theme.background));
    apname2->setText(apSsid);
    TextWidget* appass = (TextWidget*)&_boot->addWidget(new TextWidget(apPassConf, 30, false, config.theme.title1, config.theme.background));
    appass->setText(LANG::apPassTxt);
    TextWidget* appass2 = (TextWidget*)&_boot->addWidget(new TextWidget(apPass2Conf, 30, false, config.theme.clock, config.theme.background));
    appass2->setText(apPassword);
    ScrollWidget* bootSett = (ScrollWidget*)&_boot->addWidget(new ScrollWidget("*", apSettConf, config.theme.title2, config.theme.background));
    bootSett->setText(config.ipToStr(WiFi.softAPIP()), LANG::apSettFmt);
    _pager->addPage(_boot);
    _pager->setPage(_boot);
}

void Display::_start() {
    if (_boot) { _pager->removePage(_boot); }
#    ifdef USE_NEXTION
    nextion.wake();
#    endif
    if (network.status != CONNECTED && network.status != SDREADY) {
        _apScreen();
#    ifdef USE_NEXTION
        nextion.apScreen();
#    endif
        _bootStep = 2;
        return;
    }
#    ifdef USE_NEXTION
    nextion.start();
#    endif
    _buildPager();
    _mode = PLAYER;
    config.setTitle(LANG::const_PlReady);

    _pager->setPage(pages[PG_PLAYER]);

    if (_bufferwidget) {
        _bufferwidget->lock(!config.store.audioinfo);
        if (!config.store.audioinfo) {
            _bufferwidget->clear();
        } else {
            uint32_t bm = psramInit() ? 300000 : 1600 * config.store.abuff;
            _bufferwidget->setValue(player.inBufferFilled(), bm);
        }
    }

    if (_weather) { _weather->lock(!config.store.showweather); }
    if (_weather && config.store.showweather) { _weather->setText(LANG::const_getWeather); } // Üres string.
    if (_wifiwidget) { _wifiwidget->setRSSI(WiFi.RSSI()); }
    _updateBluetoothWidget();
    _updateGearWidget();
#    ifndef HIDE_RSSI
    _applyRssiMode();
#    endif
#    ifndef HIDE_IP
    if (_ipbox) { _ipbox->setText(config.ipToStr(WiFi.localIP()), iptxtFmt); }
#    endif
    _volume();
    _station();
    _time(false);
    _bootStep = 2;
    pm.on_display_player();
} // _start vége

void Display::_showDialog(const char* title) {
    dsp.setScrollId(NULL);
    _pager->setPage(pages[PG_DIALOG]);
#    ifdef META_MOVE
    _meta->moveTo(metaMove);
#    endif
    _meta->setAlign(WA_CENTER);
    _meta->setText(title);
} // _showDialog vége

void Display::_drawWifiInfo() {
    if (_mode != WIFI) { return; }

    const bool connected = (WiFi.status() == WL_CONNECTED);
    const String ssid = connected ? WiFi.SSID() : String("-");
    const String ip   = connected ? WiFi.localIP().toString() : String("-");

    char rssiText[32];
    if (connected) {
        snprintf(rssiText, sizeof(rssiText), "%d dBm", WiFi.RSSI());
    } else {
        strlcpy(rssiText, "not connected", sizeof(rssiText));
    }

    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    if (!canvas.createSprite(VOL_AREA_WIDTH, VOL_AREA_HEIGHT)) { return; }

    const uint16_t bg     = config.theme.background;
    const uint16_t label  = config.theme.ip_border;
    const uint16_t value  = config.theme.ip;

    canvas.fillSprite(bg);

    constexpr int16_t WIFI_HEADER_Y = 20;
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(config.theme.meta, bg);
    if (font_vlw_20) {
        canvas.loadFont(font_vlw_20);
        canvas.drawString("WI-FI", VOL_AREA_WIDTH / 2, WIFI_HEADER_Y);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString("WI-FI", VOL_AREA_WIDTH / 2, WIFI_HEADER_Y);
    }
    const int16_t labelX = 28;
    const int16_t valueX = 112;
    const int16_t valueMaxWidth = VOL_AREA_WIDTH - valueX - 20;
    const int16_t rowY[] = { 61, 103, 145 };
    const char* labels[] = { "SSID", "IP", "RSSI" };
    const char* values[] = { ssid.c_str(), ip.c_str(), rssiText };

    for (uint8_t i = 0; i < 3; ++i) {
        canvas.setTextDatum(ML_DATUM);
        canvas.setTextColor(label, bg);
        if (font_vlw_16) {
            canvas.loadFont(font_vlw_16);
            canvas.drawString(labels[i], labelX, rowY[i]);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(2);
            canvas.drawString(labels[i], labelX, rowY[i]);
        }

        canvas.setTextColor(value, bg);
        char fittedValue[96] = {};
        strlcpy(fittedValue, values[i], sizeof(fittedValue));
        if (font_vlw_20) {
            canvas.loadFont(font_vlw_20);
            if (canvas.textWidth(fittedValue) > valueMaxWidth) {
                size_t cut = strlen(fittedValue);
                while (cut > 0) {
                    do {
                        --cut;
                    } while (cut > 0 && (static_cast<uint8_t>(fittedValue[cut]) & 0xC0U) == 0x80U);
                    fittedValue[cut] = '\0';
                    strlcat(fittedValue, "...", sizeof(fittedValue));
                    if (canvas.textWidth(fittedValue) <= valueMaxWidth) { break; }
                    fittedValue[cut] = '\0';
                }
            }
            canvas.drawString(fittedValue, valueX, rowY[i]);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(2);
            canvas.drawString(fittedValue, valueX, rowY[i]);
        }
    }

    canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
    canvas.deleteSprite();
}

void Display::_wifiInlineShow() {
    eqForceClose();
    if (_spectrum)    { _spectrum->lock(true); }
    if (_volwidget)   { _volwidget->lock(true); }
    if (_bufferwidget){ _bufferwidget->lock(true); }
    if (_audioInfoText) { _audioInfoText->lock(true); }
    if (_clock)       { _clock->lock(true); }
    if (_datewidget)  { _datewidget->lock(true); }
    if (_weather)     { _weather->lock(true); }
    if (_weatherIcon) { _weatherIcon->lock(true); }
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    _drawWifiInfo();
}

void Display::_wifiInlineHide() {
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    if (_spectrum)    { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
    if (_volwidget)   { _volwidget->lock(false); }
    if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
    if (_audioInfoText) { _audioInfoText->lock(false); }
    if (_clock)       { _clock->lock(false); }
    if (_datewidget)  { _datewidget->lock(false); }
    if (_weather)     { _weather->lock(!config.store.showweather); }
    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
}

void Display::_drawForecast() {
    if (_mode != FORECAST) { return; }

    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    if (!canvas.createSprite(VOL_AREA_WIDTH, VOL_AREA_HEIGHT)) { return; }

    const uint16_t bg = config.theme.background;
    const uint16_t secondary = config.theme.status_inactive;
    canvas.fillSprite(bg);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(config.theme.meta, bg);
    if (font_vlw_20) {
        canvas.loadFont(font_vlw_20);
        canvas.drawString("5-DAY FORECAST", VOL_AREA_WIDTH / 2, 14);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString("5-DAY FORECAST", VOL_AREA_WIDTH / 2, 14);
    }
    canvas.drawFastHLine(20, 29, VOL_AREA_WIDTH - 40, config.theme.div);

    ForecastDay days[5] = {};
    const uint8_t count = timekeeper.forecastSnapshot(days, 5);
    if (count == 0) {
        const bool loading = timekeeper.forecastLoading() || timekeeper.forceWeather;
        const char* message = network.status != CONNECTED
                                  ? "WI-FI NOT CONNECTED"
                                  : loading ? "FORECAST LOADING..."
                                            : "NO FORECAST DATA";
        canvas.setTextColor(secondary, bg);
        if (font_vlw_18) {
            canvas.loadFont(font_vlw_18);
            canvas.drawString(message, VOL_AREA_WIDTH / 2, 105);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(2);
            canvas.drawString(message, VOL_AREA_WIDTH / 2, 105);
        }
        canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
        canvas.deleteSprite();
        return;
    }

    constexpr int16_t MARGIN = 10;
    constexpr int16_t COLUMN_W = 92;
    constexpr int16_t ICON_SIZE = 64;
    for (uint8_t i = 0; i < count; ++i) {
        const int16_t left = MARGIN + i * COLUMN_W;
        const int16_t center = left + COLUMN_W / 2;
        if (i > 0) {
            canvas.drawFastVLine(left, 35, 136, config.theme.div);
        }

        const char* dayName = static_cast<const char*>(
            pgm_read_ptr(&LANG::dow[days[i].weekday % 7]));
        char dayLabel[20] = {};
        snprintf(dayLabel, sizeof(dayLabel), "%s %u", dayName,
                 static_cast<unsigned>(days[i].day));
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(config.theme.date, bg);
        if (font_vlw_16) {
            canvas.loadFont(font_vlw_16);
            canvas.drawString(dayLabel, center, 46);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(2);
            canvas.drawString(dayLabel, center, 46);
        }

        char path[48] = {};
        snprintf(path, sizeof(path), "/images/weather/%s.png", days[i].icon);
        if (!canvas.drawPngFile(LittleFS, path, center - ICON_SIZE / 2, 60,
                                ICON_SIZE, ICON_SIZE)) {
            canvas.setTextColor(secondary, bg);
            canvas.setTextSize(2);
            canvas.drawString("--", center, 92);
        }

        char temperature[32] = {};
        snprintf(temperature, sizeof(temperature), "%.0f\xC2\xB0 / %.0f\xC2\xB0",
                 days[i].maxTemp, days[i].minTemp);
        canvas.setTextColor(config.theme.weatherIconTxt, bg);
        if (font_vlw_12) {
            canvas.loadFont(font_vlw_12);
            canvas.drawString(temperature, center, 149);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(1);
            canvas.drawString(temperature, center, 149);
        }
    }

    canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
    canvas.deleteSprite();
}

void Display::_forecastInlineShow() {
    eqForceClose();
    _forecastLastActivityMs = millis();
    if (_spectrum)    { _spectrum->lock(true); }
    if (_volwidget)   { _volwidget->lock(true); }
    if (_bufferwidget){ _bufferwidget->lock(true); }
    if (_audioInfoText) { _audioInfoText->lock(true); }
    if (_clock)       { _clock->lock(true); }
    if (_datewidget)  { _datewidget->lock(true); }
    if (_weather)     { _weather->lock(true); }
    if (_weatherIcon) { _weatherIcon->lock(true); }
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    timekeeper.requestForecastRefresh();
    _drawForecast();
}

void Display::_forecastInlineHide() {
    _forecastLastActivityMs = 0;
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    if (_spectrum)    { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
    if (_volwidget)   { _volwidget->lock(false); }
    if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
    if (_audioInfoText) { _audioInfoText->lock(false); }
    if (_clock)       { _clock->lock(false); }
    if (_datewidget)  { _datewidget->lock(false); }
    if (_weather)     { _weather->lock(!config.store.showweather); }
    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
}

void Display::_forecastTick() {
    if (_mode != FORECAST || _forecastLastActivityMs == 0) { return; }
    if (static_cast<uint32_t>(millis() - _forecastLastActivityMs) <
        INLINE_MENU_IDLE_TIMEOUT_MS) {
        return;
    }
    _forecastLastActivityMs = 0;
    putRequest(NEWMODE, PLAYER);
}

void Display::_updateBluetoothWidget() {
    if (!_bluetoothwidget) { return; }
#ifdef USE_BLUETOOTH
    const bool disabled =
        bluetooth.moduleRole() == BT_ROLE_SOURCE &&
        bluetooth.sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY;
    _bluetoothwidget->setState(
        disabled ? BT_WIDGET_DISABLED
                 : bluetooth.connected() ? BT_WIDGET_ACTIVE
                                         : BT_WIDGET_INACTIVE);
#else
    _bluetoothwidget->setState(BT_WIDGET_DISABLED);
#endif
}

void Display::_drawBluetoothInfo() {
    if (_mode != BTINFO) { return; }

    constexpr int16_t HEADER_Y = 14;
    constexpr int16_t ROW_TOP = 33;

    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    if (!canvas.createSprite(VOL_AREA_WIDTH, VOL_AREA_HEIGHT)) { return; }
    canvas.fillSprite(config.theme.background);

    const uint16_t bg = config.theme.background;
    const uint16_t secondary = config.theme.status_inactive;

    char header[48] = "BLUETOOTH";
#ifdef USE_BLUETOOTH
    constexpr int16_t ROW_H = 47;
    constexpr uint8_t ROW_COUNT = 3;
    constexpr int16_t ACTION_X = 360;
    constexpr int16_t ACTION_W = 108;
    constexpr int16_t ACTION_H = 27;
    const uint16_t primary = config.theme.title1;
    const uint16_t accent = config.theme.wifi_border;
    constexpr uint16_t disconnectColor = 0xF800;

    const btModuleRole_e role = bluetooth.moduleRole();
    const bool localOnly = role == BT_ROLE_SOURCE &&
                           bluetooth.sourceOutputMode() ==
                               BT_SOURCE_OUTPUT_LOCAL_ONLY;
    if (localOnly) {
        strlcpy(header, "BLUETOOTH  LOCAL ONLY", sizeof(header));
    } else if (role == BT_ROLE_SOURCE) {
        strlcpy(header, "BLUETOOTH OUTPUT", sizeof(header));
    } else if (role == BT_ROLE_SINK) {
        strlcpy(header, "BLUETOOTH INPUT", sizeof(header));
    }
    const uint8_t deviceCount = bluetooth.displayPairedDeviceCount();
    const uint8_t lastPageFirst = deviceCount == 0
                                      ? 0
                                      : static_cast<uint8_t>(
                                            ((deviceCount - 1U) / ROW_COUNT) *
                                            ROW_COUNT);
    if (_btInfoFirst > lastPageFirst) { _btInfoFirst = lastPageFirst; }
#endif

    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(config.theme.meta, bg);
    if (font_vlw_20) {
        canvas.loadFont(font_vlw_20);
        canvas.drawString(header, VOL_AREA_WIDTH / 2, HEADER_Y);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString(header, VOL_AREA_WIDTH / 2, HEADER_Y);
    }
    canvas.drawFastHLine(20, 29, VOL_AREA_WIDTH - 40, config.theme.div);

#ifdef USE_BLUETOOTH
    if (deviceCount > ROW_COUNT) {
        char pageText[12];
        const uint8_t page = static_cast<uint8_t>(_btInfoFirst / ROW_COUNT + 1U);
        const uint8_t pages = static_cast<uint8_t>(
            (deviceCount + ROW_COUNT - 1U) / ROW_COUNT);
        snprintf(pageText, sizeof(pageText), "%u/%u", (unsigned)page,
                 (unsigned)pages);
        canvas.setTextDatum(MR_DATUM);
        canvas.setTextColor(secondary, bg);
        if (font_vlw_12) {
            canvas.loadFont(font_vlw_12);
            canvas.drawString(pageText, VOL_AREA_WIDTH - 22, HEADER_Y);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(1);
            canvas.drawString(pageText, VOL_AREA_WIDTH - 22, HEADER_Y);
        }
    }

    if (deviceCount == 0) {
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(secondary, bg);
        const char* emptyText = bluetooth.displayControlsBusy()
                                    ? "LOADING PAIRED DEVICES..."
                                    : "NO PAIRED DEVICES";
        if (font_vlw_18) {
            canvas.loadFont(font_vlw_18);
            canvas.drawString(emptyText, VOL_AREA_WIDTH / 2,
                              ROW_TOP + (VOL_AREA_HEIGHT - ROW_TOP) / 2);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(2);
            canvas.drawString(emptyText, VOL_AREA_WIDTH / 2,
                              ROW_TOP + (VOL_AREA_HEIGHT - ROW_TOP) / 2);
        }
    } else {
        const bool sourceRole = role == BT_ROLE_SOURCE;
        const bool sourceReady = sourceRole && bluetooth.sourceUiReady();
        const bool controlsBusy = bluetooth.displayControlsBusy();

        for (uint8_t row = 0; row < ROW_COUNT; ++row) {
            const uint8_t index = static_cast<uint8_t>(_btInfoFirst + row);
            if (index >= deviceCount) { break; }
            btDisplayDevice_t device;
            if (!bluetooth.displayPairedDeviceAt(index, device)) { continue; }

            const int16_t y = ROW_TOP + row * ROW_H;
            const int16_t cy = y + ROW_H / 2;
            if (row > 0) {
                canvas.drawFastHLine(12, y, VOL_AREA_WIDTH - 24,
                                     config.theme.div);
            }

            char displayName[72] = {};
            strlcpy(displayName, device.name, sizeof(displayName));
            canvas.setTextDatum(ML_DATUM);
            canvas.setTextColor(device.connected ? accent : primary, bg);
            if (font_vlw_16) {
                canvas.loadFont(font_vlw_16);
                if (canvas.textWidth(displayName) > 220) {
                    size_t cut = strlen(displayName);
                    while (cut > 0) {
                        do {
                            --cut;
                        } while (cut > 0 &&
                                 (static_cast<uint8_t>(displayName[cut]) &
                                  0xC0U) == 0x80U);
                        displayName[cut] = '\0';
                        strlcat(displayName, "...", sizeof(displayName));
                        if (canvas.textWidth(displayName) <= 220) { break; }
                        displayName[cut] = '\0';
                    }
                }
                canvas.drawString(displayName, 14, cy);
                canvas.unloadFont();
            } else {
                canvas.setTextSize(2);
                canvas.drawString(displayName, 14, cy);
            }

            char rssiText[16];
            if (device.rssi < 0 && device.rssi > -127) {
                snprintf(rssiText, sizeof(rssiText), "%d dBm",
                         static_cast<int>(device.rssi));
            } else {
                strlcpy(rssiText, "--", sizeof(rssiText));
            }
            canvas.setTextDatum(MC_DATUM);
            canvas.setTextColor(device.connected ? accent : secondary, bg);
            if (font_vlw_12) {
                canvas.loadFont(font_vlw_12);
                canvas.drawString(rssiText, 297, cy);
                canvas.unloadFont();
            } else {
                canvas.setTextSize(1);
                canvas.drawString(rssiText, 297, cy);
            }

            const bool outputCapable = device.profile == 0 ||
                                       (device.profile & (1U << 6)) != 0;
            const bool showDisconnect = sourceRole && device.connected;
            const bool showConnect = sourceRole && !device.connected &&
                                     outputCapable;
            if (showDisconnect || showConnect) {
                const bool enabled = sourceReady && !localOnly && !controlsBusy;
                const uint16_t buttonColor = enabled
                    ? (showDisconnect ? disconnectColor : accent)
                    : secondary;
                const int16_t actionY = y + (ROW_H - ACTION_H) / 2;
                canvas.drawRoundRect(ACTION_X, actionY, ACTION_W, ACTION_H, 4,
                                     buttonColor);
                canvas.setTextDatum(MC_DATUM);
                canvas.setTextColor(buttonColor, bg);
                const char* actionText = showDisconnect ? "DISCONNECT" : "CONNECT";
                if (font_vlw_12) {
                    canvas.loadFont(font_vlw_12);
                    canvas.drawString(actionText, ACTION_X + ACTION_W / 2,
                                      actionY + ACTION_H / 2);
                    canvas.unloadFont();
                } else {
                    canvas.setTextSize(1);
                    canvas.drawString(actionText, ACTION_X + ACTION_W / 2,
                                      actionY + ACTION_H / 2);
                }
            } else {
                canvas.setTextDatum(MC_DATUM);
                canvas.setTextColor(secondary, bg);
                canvas.setTextSize(1);
                canvas.drawString("--", ACTION_X + ACTION_W / 2, cy);
            }
        }
    }
    _btInfoFingerprint = bluetooth.displayFingerprint();
#else
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(secondary, bg);
    if (font_vlw_18) {
        canvas.loadFont(font_vlw_18);
        canvas.drawString("BLUETOOTH DISABLED", VOL_AREA_WIDTH / 2,
                          ROW_TOP + (VOL_AREA_HEIGHT - ROW_TOP) / 2);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString("BLUETOOTH DISABLED", VOL_AREA_WIDTH / 2,
                          ROW_TOP + (VOL_AREA_HEIGHT - ROW_TOP) / 2);
    }
#endif

    canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
    canvas.deleteSprite();
    _btInfoPollMs = millis();
}

void Display::_bluetoothInlineShow() {
    eqForceClose();
    if (_spectrum)    { _spectrum->lock(true); }
    if (_volwidget)   { _volwidget->lock(true); }
    if (_bufferwidget){ _bufferwidget->lock(true); }
    if (_audioInfoText) { _audioInfoText->lock(true); }
    if (_clock)       { _clock->lock(true); }
    if (_datewidget)  { _datewidget->lock(true); }
    if (_weather)     { _weather->lock(true); }
    if (_weatherIcon) { _weatherIcon->lock(true); }
    _btInfoFirst = 0;
    _btInfoFingerprint = 0;
#ifdef USE_BLUETOOTH
    bluetooth.requestDisplayUiRefresh();
#endif
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    _drawBluetoothInfo();
}

void Display::_bluetoothInlineHide() {
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    if (_spectrum)    { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
    if (_volwidget)   { _volwidget->lock(false); }
    if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
    if (_audioInfoText) { _audioInfoText->lock(false); }
    if (_clock)       { _clock->lock(false); }
    if (_datewidget)  { _datewidget->lock(false); }
    if (_weather)     { _weather->lock(!config.store.showweather); }
    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
}

bool Display::btInfoTap(uint16_t x, uint16_t y) {
    if (_mode != BTINFO) { return false; }
    constexpr uint16_t ROW_TOP_ABS = VOL_AREA_TOP + 33;
    constexpr uint16_t ROW_H = 47;
    constexpr uint16_t ROW_COUNT = 3;
    constexpr uint16_t ACTION_X = 360;
    constexpr uint16_t ACTION_W = 108;
    constexpr uint16_t ACTION_H = 27;

    if (y < ROW_TOP_ABS || y >= ROW_TOP_ABS + ROW_H * ROW_COUNT) {
        return false;
    }
    const uint8_t row = static_cast<uint8_t>((y - ROW_TOP_ABS) / ROW_H);
#ifdef USE_BLUETOOTH
    const uint8_t index = static_cast<uint8_t>(_btInfoFirst + row);
    btDisplayDevice_t device;
    if (!bluetooth.displayPairedDeviceAt(index, device)) { return false; }

    const bool sourceRole = bluetooth.moduleRole() == BT_ROLE_SOURCE;
    const bool outputCapable = device.profile == 0 ||
                               (device.profile & (1U << 6)) != 0;
    const bool actionVisible = sourceRole &&
                               (device.connected || outputCapable);
    const uint16_t actionTop = ROW_TOP_ABS + row * ROW_H +
                               (ROW_H - ACTION_H) / 2;
    if (!actionVisible || x < ACTION_X || x >= ACTION_X + ACTION_W ||
        y < actionTop || y >= actionTop + ACTION_H) {
        return false; // a lista szoveges resze bezarja a menut
    }

    // A látható, de átmenetileg inaktív gomb nem zárja be a listát.
    if (!bluetooth.sourceUiReady() ||
        bluetooth.sourceOutputMode() == BT_SOURCE_OUTPUT_LOCAL_ONLY ||
        bluetooth.displayControlsBusy()) {
        return true;
    }
    if (device.connected) {
        bluetooth.disconnectSourceDevice(0);
    } else {
        bluetooth.connectSourceDevice(device.address, 0);
    }
    _btInfoFingerprint = 0;
    _drawBluetoothInfo();
    return true;
#else
    (void)x;
    (void)row;
    return false;
#endif
}

void Display::btInfoScroll(int direction) {
    if (_mode != BTINFO || direction == 0) { return; }
#ifdef USE_BLUETOOTH
    constexpr uint8_t ROW_COUNT = 3;
    const uint8_t count = bluetooth.displayPairedDeviceCount();
    const uint8_t lastPageFirst = count == 0
                                      ? 0
                                      : static_cast<uint8_t>(
                                            ((count - 1U) / ROW_COUNT) *
                                            ROW_COUNT);
    if (direction > 0 && _btInfoFirst < lastPageFirst) {
        _btInfoFirst = min(static_cast<uint8_t>(_btInfoFirst + ROW_COUNT),
                           lastPageFirst);
    } else if (direction < 0 && _btInfoFirst > 0) {
        _btInfoFirst = _btInfoFirst >= ROW_COUNT
                           ? static_cast<uint8_t>(_btInfoFirst - ROW_COUNT)
                           : 0;
    } else {
        return;
    }
    _drawBluetoothInfo();
#endif
}

void Display::_updateGearWidget() {
    if (!_gearwidget) { return; }
    _gearwidget->setSelected(_mode == QUICKSETTINGS || _mode == MODESELECT ||
                             _mode == SCREENSELECT || isEqOpen());
}

namespace {
constexpr int16_t QS_HEADER_Y = 15;
constexpr int16_t QS_BUTTON_TOP = 34;
constexpr int16_t QS_BUTTON_W = 140;
constexpr int16_t QS_BUTTON_H = 36;
constexpr int16_t QS_BUTTON_GAP_X = 8;
constexpr int16_t QS_BUTTON_GAP_Y = 20;
constexpr int16_t QS_BUTTON_LEFT = 22;
constexpr int16_t QS_INFO_LINE_Y = 145;
constexpr int16_t QS_INFO_TEXT_Y = 160;
constexpr uint8_t QS_BUTTON_COUNT = 6;
}

void Display::_drawQuickSettings() {
    if (_mode != QUICKSETTINGS) { return; }

    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    if (!canvas.createSprite(VOL_AREA_WIDTH, VOL_AREA_HEIGHT)) { return; }

    const uint16_t bg = config.theme.background;
    const uint16_t accent = config.theme.status_active;
    const uint16_t inactive = config.theme.status_inactive;
    canvas.fillSprite(bg);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(config.theme.meta, bg);
    if (font_vlw_20) {
        canvas.loadFont(font_vlw_20);
        canvas.drawString("SETTINGS", VOL_AREA_WIDTH / 2, QS_HEADER_Y);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString("SETTINGS", VOL_AREA_WIDTH / 2, QS_HEADER_Y);
    }
    const char* playMode = "WEB";
#ifdef USE_SD
    if (config.getMode() == PM_SDCARD) { playMode = "SD"; }
#endif
#ifdef USE_DLNA
    if (config.getMode() == PM_WEB &&
        config.store.playlistSource == static_cast<uint8_t>(PL_SRC_DLNA)) {
        playMode = "DLNA";
    }
#endif
#ifdef USE_BLUETOOTH
    if (config.getMode() == PM_BLUETOOTH) { playMode = "BT"; }
#endif

    const uint8_t modes[QS_BUTTON_COUNT] = {
        1,
        static_cast<uint8_t>(config.store.fadeEnabled ? 2 : 0),
        static_cast<uint8_t>(!config.store.clockTtsEnabled
                                  ? 0
                                  : (config.store.clockTtsOnlyWhenNoStream ? 2 : 1)),
        static_cast<uint8_t>(!config.store.lsEnabled
                                  ? 0
                                  : (config.store.lsSsEnabled ? 2 : 1)),
        1,
        static_cast<uint8_t>(config.store.screensaverPlayingEnabled ? 2 : 0),
    };
    const char* labels[QS_BUTTON_COUNT] = {
        "MODE", "FADE", "TTS", "RGB", "VOLUME", "SCREENSAVER"
    };
    const char* states[QS_BUTTON_COUNT] = {
        playMode,
        config.store.fadeEnabled ? "ON" : "OFF",
        !config.store.clockTtsEnabled
            ? "OFF"
            : (config.store.clockTtsOnlyWhenNoStream ? "NO STREAM" : "ON"),
        !config.store.lsEnabled
            ? "OFF"
            : (config.store.lsSsEnabled ? "SCREENSAVER" : "ON"),
        "PRESETS",
        !config.store.screensaverPlayingEnabled
            ? "OFF"
            : (config.store.screensaverMode == SCREENSAVER_CASSETTE
                   ? "CASSETTE"
                   : (config.store.screensaverMode == SCREENSAVER_VUMETER
                          ? "VU METER" : "COVERART")),
    };

    for (uint8_t i = 0; i < QS_BUTTON_COUNT; ++i) {
        const uint8_t col = i % 3;
        const uint8_t row = i / 3;
        const int16_t x = QS_BUTTON_LEFT + col * (QS_BUTTON_W + QS_BUTTON_GAP_X);
        const int16_t y = QS_BUTTON_TOP + row * (QS_BUTTON_H + QS_BUTTON_GAP_Y);
        const bool filled = modes[i] == 2;
        const uint16_t border = modes[i] == 0 ? inactive : accent;
        const uint16_t buttonBg = filled ? accent : bg;
        const uint16_t textColor = filled ? bg : border;
        canvas.fillRoundRect(x, y, QS_BUTTON_W, QS_BUTTON_H, 4, buttonBg);
        canvas.drawRoundRect(x, y, QS_BUTTON_W, QS_BUTTON_H, 4, border);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(textColor, buttonBg);
        if (font_vlw_16) {
            canvas.loadFont(font_vlw_16);
            canvas.drawString(labels[i], x + QS_BUTTON_W / 2, y + 11);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(1);
            canvas.drawString(labels[i], x + QS_BUTTON_W / 2, y + 11);
        }
        if (font_vlw_12) {
            canvas.loadFont(font_vlw_12);
            canvas.drawString(states[i], x + QS_BUTTON_W / 2, y + 27);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(1);
            canvas.drawString(states[i], x + QS_BUTTON_W / 2, y + 27);
        }
        if (i == _quickSettingsIndex) {
            canvas.fillRect(x + 12, y + QS_BUTTON_H + 3,
                            QS_BUTTON_W - 24, 2, config.theme.digit);
        }
    }

    canvas.drawFastHLine(20, QS_INFO_LINE_Y, VOL_AREA_WIDTH - 40,
                          config.theme.ip_border);

    char ipText[32];
    char rssiText[20];
    if (WiFi.status() == WL_CONNECTED) {
        const IPAddress ip = WiFi.localIP();
        snprintf(ipText, sizeof(ipText), "IP: %u.%u.%u.%u",
                 ip[0], ip[1], ip[2], ip[3]);
        snprintf(rssiText, sizeof(rssiText), "RSSI: %d dBm", WiFi.RSSI());
    } else {
        strlcpy(ipText, "IP: --", sizeof(ipText));
        strlcpy(rssiText, "RSSI: --", sizeof(rssiText));
    }
    canvas.setTextColor(config.theme.ip, bg);
    if (font_vlw_16) {
        canvas.loadFont(font_vlw_16);
    } else {
        canvas.setTextSize(2);
    }

    int16_t ipLeft = 22;
    if (batteryMonitor.enabled()) {
        const BatteryReading battery = batteryMonitor.reading();
        // Reuse the four Wi-Fi signal colors for the battery level.
        const uint16_t color = !battery.valid ? config.theme.ip
                               : battery.percent >= 70 ? config.theme.wifi_high
                               : battery.percent >= 40 ? config.theme.wifi_mid
                               : battery.percent >= 15 ? config.theme.wifi_low_mid
                                                       : config.theme.wifi_low;
        canvas.drawRoundRect(22, QS_INFO_TEXT_Y - 7, 20, 13, 2, color);
        canvas.fillRect(42, QS_INFO_TEXT_Y - 3, 3, 5, color);
        if (battery.valid) {
            const uint8_t fill = (battery.percent * 16 + 50) / 100;
            if (fill) canvas.fillRect(24, QS_INFO_TEXT_Y - 5, fill, 9, color);
        }
        char batteryText[24];
        if (batteryMonitor.preview()) {
            snprintf(batteryText, sizeof(batteryText), "TEST %u%%", battery.percent);
        } else if (battery.valid) {
            const uint16_t mv = battery.voltageMv;
            snprintf(batteryText, sizeof(batteryText), "%u%%  %u.%02uV",
                     battery.percent, mv / 1000, (mv % 1000) / 10);
        } else {
            strlcpy(batteryText, "--%", sizeof(batteryText));
        }
        canvas.setTextColor(color, bg);
        canvas.setTextDatum(ML_DATUM);
        canvas.drawString(batteryText, 51, QS_INFO_TEXT_Y);
        ipLeft = 170;
    } else {
        char ssidText[64] = "SSID: ";
        if (WiFi.status() == WL_CONNECTED) {
            const String ssid = WiFi.SSID();
            strlcat(ssidText, ssid.length() ? ssid.c_str() : "-", sizeof(ssidText));
        } else {
            strlcat(ssidText, "--", sizeof(ssidText));
        }
        constexpr int16_t ssidMaxWidth = 138;
        if (canvas.textWidth(ssidText) > ssidMaxWidth) {
            size_t cut = strlen(ssidText);
            while (cut > 0) {
                do {
                    --cut;
                } while (cut > 0 &&
                         (static_cast<uint8_t>(ssidText[cut]) & 0xC0U) == 0x80U);
                ssidText[cut] = '\0';
                strlcat(ssidText, "...", sizeof(ssidText));
                if (canvas.textWidth(ssidText) <= ssidMaxWidth) { break; }
                ssidText[cut] = '\0';
            }
        }
        canvas.setTextDatum(ML_DATUM);
        canvas.drawString(ssidText, 22, QS_INFO_TEXT_Y);
        ipLeft = 170;
    }
    canvas.setTextColor(config.theme.ip, bg);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString(ipText, ipLeft, QS_INFO_TEXT_Y);
    canvas.setTextDatum(MR_DATUM);
    canvas.drawString(rssiText, VOL_AREA_WIDTH - 20, QS_INFO_TEXT_Y);
    if (font_vlw_16) canvas.unloadFont();

    canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
    canvas.deleteSprite();
}

void Display::_quickSettingsInlineShow() {
    eqForceClose();
    _quickSettingsIndex = 0;
    _quickSettingsLastActivityMs = millis();
    if (_spectrum)    { _spectrum->lock(true); }
    if (_volwidget)   { _volwidget->lock(true); }
    if (_bufferwidget){ _bufferwidget->lock(true); }
    if (_audioInfoText) { _audioInfoText->lock(true); }
    if (_clock)       { _clock->lock(true); }
    if (_datewidget)  { _datewidget->lock(true); }
    if (_weather)     { _weather->lock(true); }
    if (_weatherIcon) { _weatherIcon->lock(true); }
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    _drawQuickSettings();
    _updateGearWidget();
}

void Display::_quickSettingsInlineHide() {
    _quickSettingsLastActivityMs = 0;
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                 config.theme.background);
    if (_spectrum)    { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
    if (_volwidget)   { _volwidget->lock(false); }
    if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
    if (_audioInfoText) { _audioInfoText->lock(false); }
    if (_clock)       { _clock->lock(false); }
    if (_datewidget)  { _datewidget->lock(false); }
    if (_weather)     { _weather->lock(!config.store.showweather); }
    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
}

void Display::_quickSettingsTick() {
    if (_mode != QUICKSETTINGS || _quickSettingsLastActivityMs == 0) { return; }
    if (static_cast<uint32_t>(millis() - _quickSettingsLastActivityMs) <
        INLINE_MENU_IDLE_TIMEOUT_MS) {
        return;
    }
    _quickSettingsLastActivityMs = 0;
    putRequest(NEWMODE, PLAYER);
}

int8_t Display::quickSettingsHit(uint16_t x, uint16_t y) const {
    if (_mode != QUICKSETTINGS || y < VOL_AREA_TOP ||
        y >= VOL_AREA_TOP + VOL_AREA_HEIGHT) {
        return -1;
    }
    const int16_t cy = static_cast<int16_t>(y - VOL_AREA_TOP);
    for (uint8_t i = 0; i < QS_BUTTON_COUNT; ++i) {
        const uint8_t col = i % 3;
        const uint8_t row = i / 3;
        const int16_t left = QS_BUTTON_LEFT + col * (QS_BUTTON_W + QS_BUTTON_GAP_X);
        const int16_t top = QS_BUTTON_TOP + row * (QS_BUTTON_H + QS_BUTTON_GAP_Y);
        if (x >= left && x < left + QS_BUTTON_W &&
            cy >= top && cy < top + QS_BUTTON_H) {
            return i;
        }
    }
    return -1;
}

void Display::quickSettingsRedraw() {
    if (_mode == QUICKSETTINGS) { putRequest(DRAWQUICKSETTINGS); }
}

void Display::quickSettingsOpenEqualizer() {
    if (_mode != QUICKSETTINGS) { return; }
    putRequest(OPENQUICKSETTINGSEQ);
}

void Display::quickSettingsScroll(int dir) {
    if (_mode != QUICKSETTINGS || dir == 0) { return; }
    _quickSettingsLastActivityMs = millis();
    if (dir > 0) {
        _quickSettingsIndex = (_quickSettingsIndex + 1) % QS_BUTTON_COUNT;
    } else {
        _quickSettingsIndex =
            (_quickSettingsIndex + QS_BUTTON_COUNT - 1) % QS_BUTTON_COUNT;
    }
    putRequest(DRAWQUICKSETTINGS);
}

void Display::quickSettingsMove(int8_t horizontal, int8_t vertical) {
    if (_mode != QUICKSETTINGS || (horizontal == 0 && vertical == 0)) { return; }
    _quickSettingsLastActivityMs = millis();
    const uint8_t column = _quickSettingsIndex % 3;
    const uint8_t row = _quickSettingsIndex / 3;
    const uint8_t nextColumn = (column + 3 + horizontal) % 3;
    const uint8_t nextRow = (row + 2 + vertical) % 2;
    _quickSettingsIndex = nextRow * 3 + nextColumn;
    putRequest(DRAWQUICKSETTINGS);
}

void Display::quickSettingsConfirm() {
    if (_mode != QUICKSETTINGS) { return; }
    quickSettingsActivate(_quickSettingsIndex);
}

void Display::quickSettingsActivate(uint8_t index) {
    if (_mode != QUICKSETTINGS || index >= QS_BUTTON_COUNT) { return; }
    _quickSettingsLastActivityMs = millis();
    _quickSettingsIndex = index;
    switch (index) {
        case 0:
            modeSelectorOpen();
            return;
        case 1: {
            const uint8_t enabled = config.store.fadeEnabled ? 0 : 1;
            config.saveValue(&config.store.fadeEnabled, enabled, true, false);
#if BRIGHTNESS_PIN != 255
            backlightPlugin.setEnabled(enabled != 0);
#endif
            putRequest(INVALIDATETHEMEWIDGETS);
            break;
        }
        case 2: {
            const bool enabled = config.store.clockTtsEnabled;
            const bool onlyNoStream = config.store.clockTtsOnlyWhenNoStream;
            if (!enabled) {
                config.saveValue(&config.store.clockTtsEnabled, true, false);
                config.saveValue(&config.store.clockTtsOnlyWhenNoStream, false);
            } else if (!onlyNoStream) {
                config.saveValue(&config.store.clockTtsOnlyWhenNoStream, true);
            } else {
                config.saveValue(&config.store.clockTtsEnabled, false, false);
                config.saveValue(&config.store.clockTtsOnlyWhenNoStream, false);
            }
            putRequest(INVALIDATETHEMEWIDGETS);
            break;
        }
        case 3: {
            const uint8_t enabled = config.store.lsEnabled;
            const uint8_t duringScreensaver = config.store.lsSsEnabled;
            if (!enabled) {
                config.saveValue(&config.store.lsEnabled, static_cast<uint8_t>(1), false);
                config.saveValue(&config.store.lsSsEnabled, static_cast<uint8_t>(0));
            } else if (!duringScreensaver) {
                config.saveValue(&config.store.lsSsEnabled, static_cast<uint8_t>(1));
            } else {
                config.saveValue(&config.store.lsEnabled, static_cast<uint8_t>(0), false);
                config.saveValue(&config.store.lsSsEnabled, static_cast<uint8_t>(0));
            }
            putRequest(INVALIDATETHEMEWIDGETS);
            break;
        }
        case 4:
            quickSettingsOpenEqualizer();
            return;
        case 5:
            screensaverSelectorOpen();
            return;
        default:
            return;
    }
    // A beallitas utan a Settings nyitva marad. quickSettingsActivate() elejen
    // ujrainditott inaktivitas-időzítő csak akkor zarja be, ha 5 masodpercig
    // sem forgatas, sem ujabb valasztas nem tortenik.
}

void Display::_refreshThemeColors() {
    if (_meta) { _meta->setColors(config.theme.meta, config.theme.metabg); }
    if (_title1) { _title1->setColors(config.theme.title1, config.theme.background); }
    if (_title2) { _title2->setColors(config.theme.title2, config.theme.background); }
#    ifdef USE_CASSETTE_SCREENSAVER
    if (_cassettewidget) {
        _cassettewidget->setColors(config.theme.meta, config.theme.background);
        _cassettewidget->setTextColors(config.theme.title1, config.theme.title2);
    }
#    endif
#    ifdef USE_COVER_SCREENSAVER
    if (_coverScreensaverWidget) {
        _coverScreensaverWidget->setColors(config.theme.date, config.theme.background);
    }
#    endif
    if (_metabackground) { _metabackground->setColors(config.theme.metafill, config.theme.metafill); }
    if (_weather) { _weather->setColors(config.theme.weather, config.theme.background); }
    if (_nums) { _nums->setColors(config.theme.digit, config.theme.background); }
    if (_plcurrent) { _plcurrent->setColors(config.theme.plcurrent, config.theme.plcurrentbg); }
    if (_plbackground) { _plbackground->setColors(config.theme.plcurrentfill, config.theme.plcurrentfill); }
    if (_ipbox) { _ipbox->setColors(config.theme.ip, config.theme.ip_bg, config.theme.ip_border); }
    if (_rssibox) { _rssibox->setColors(config.theme.rssi, config.theme.rssi_bg, config.theme.rssi_border); }
    if (_footerPlayMode) {
        _footerPlayMode->setColors(config.theme.pmode,
                                   config.theme.background,
                                   config.theme.pmode);
    }
    if (_footerStation) {
        _footerStation->setColors(config.theme.pmode,
                                  config.theme.background,
                                  config.theme.pmode);
    }
    if (_audioInfoText) {
        _audioInfoText->setColors(config.theme.bitrate,
                                  config.theme.background,
                                  config.theme.bitrate);
    }
}

void Display::_applyRssiMode() {
#    ifndef HIDE_RSSI
    // A footerben marado Wi-Fi ikon kezdeti allapota.
    const int currentRssi = WiFi.RSSI();
    if (_rssibox) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d dBm", currentRssi);
        _lastRssiText = currentRssi;
        _lastRssiTextMs = millis();
        _rssibox->setText(buf);
    }
    if (_wifiwidget) {
        _wifiwidget->setRSSI(currentRssi);
        _wifiwidget->lock(false);
    }
#    endif
}

static bool isLowerMenuMode(displayMode_e mode) {
    return mode == VOL || mode == WIFI || mode == BTINFO ||
           mode == QUICKSETTINGS || mode == FORECAST || mode == STATIONS ||
           mode == MODESELECT || mode == SCREENSELECT;
}

void Display::_swichMode(displayMode_e newmode) {
    // A timer request may already be queued when the EQ overlay opens.
    // Never replace an active tone adjustment with the screensaver.
    if ((newmode == SCREENSAVER || newmode == SCREENBLANK) &&
        _mode == PLAYER && isEqOpen()) {
        config.screensaverTicks = 0;
        config.screensaverPlayingTicks = 0;
        return;
    }
#    ifdef USE_NEXTION
    nextion.putRequest({NEWMODE, newmode});
#    endif
    // Az inline informacios nezetek kapcsolat nelkul is legyenek
    // megnyithatok, es onnan mindig vissza lehessen lepni a playerre.
    const bool inlineUiTransition =
        (newmode == WIFI || _mode == WIFI ||
         newmode == BTINFO || _mode == BTINFO ||
         newmode == QUICKSETTINGS || _mode == QUICKSETTINGS ||
         newmode == MODESELECT || _mode == MODESELECT ||
         newmode == SCREENSELECT || _mode == SCREENSELECT ||
         newmode == FORECAST || _mode == FORECAST);
    if (newmode == _mode ||
        (!inlineUiTransition && network.status != CONNECTED && network.status != SDREADY)) {
        return;
    }
    const displayMode_e prevMode = _mode;
    // The EQ/preset panel is also an inline overlay, although it does not
    // change _mode. Close it before hiding the shared player footer.
    if (newmode != PLAYER && isEqOpen()) { eqForceClose(); }
    if (_clockScreensaverBrightnessActive && newmode != SCREENSAVER && newmode != SCREENBLANK && config.store.dspon) {
        setBrightnessPercent(config.store.brightness);
    }
    _clockScreensaverBrightnessActive = false;
    _mode = newmode;
    if (isLowerMenuMode(newmode) && _footer) { _footer->setActive(false); }
    if (newmode != SCREENSAVER) { _manualCoverView = false; }
    dsp.setScrollId(NULL);
    if (newmode != PLAYER && prevMode == WIFI) { _wifiInlineHide(); }
    if (newmode != PLAYER && prevMode == BTINFO) { _bluetoothInlineHide(); }
    if (newmode != PLAYER && prevMode == QUICKSETTINGS) {
        _quickSettingsInlineHide();
    }
    if (newmode != PLAYER && prevMode == FORECAST) { _forecastInlineHide(); }
    if (newmode == PLAYER) {
#    ifdef USE_CASSETTE_SCREENSAVER
        if (prevMode == SCREENSAVER || prevMode == SCREENBLANK) {
            if (_cassettewidget) { _cassettewidget->lock(true); }
            if (_clock) { _clock->unlock(); }
        }
#    endif
#    ifdef USE_COVER_SCREENSAVER
        if (prevMode == SCREENSAVER || prevMode == SCREENBLANK) {
            if (_coverScreensaverWidget) { _coverScreensaverWidget->lock(true); }
            if (_clock) { _clock->unlock(); }
        }
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
        if (prevMode == SCREENSAVER || prevMode == SCREENBLANK) {
            if (_vuMeterWidget) { _vuMeterWidget->lock(true); }
            if (_clock) { _clock->unlock(); }
        }
#    endif
        if (prevMode == NUMBERS) {
            purgeQueuedRequestType(NEXTSTATION);
            purgeQueuedRequestType(NEWTITLE); // Clear stale title updates to avoid delayed metadata display
        }
        if (prevMode == VOL) {
            // VOL inline módból visszatérés: unlock widgetek
            _volInlineHide();
        }
        if (prevMode == WIFI) {
            // Wi-Fi informacios inline nezetbol visszateres.
            _wifiInlineHide();
        }
        if (prevMode == BTINFO) {
            // Bluetooth eszkozlista inline nezetbol visszateres.
            _bluetoothInlineHide();
        }
        if (prevMode == QUICKSETTINGS) {
            _quickSettingsInlineHide();
        }
        if (prevMode == FORECAST) {
            _forecastInlineHide();
        }
        if (prevMode == MODESELECT || prevMode == SCREENSELECT) {
            // Lista-overlayből visszatérés: unlock widgetek.
            if (_spectrum)    { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
            if (_volwidget)   { _volwidget->lock(false); }
            if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
            if (_audioInfoText) { _audioInfoText->lock(false); }
            if (_clock)       { _clock->lock(false); }
            if (_datewidget)  { _datewidget->lock(false); }
            if (_weather)     { _weather->lock(!config.store.showweather); }
            if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
        }
        if (prevMode == SDCHANGE) {
            // PG_DIALOG-ból visszatérés: a _pager->setPage(PG_PLAYER) már megtörtént
            // fentebb (_refreshThemeColors + _pager->setPage hívódik a PLAYER ágban)
            // Nincs külön widget unlock szükség
        }
        if (prevMode == STATIONS) {
            // Playlist inline módból visszatérés
            _plwidget->lock(true);    // playlist elrejtése
            _plcurrent->lock(true);   // scroll elrejtése
            if (_spectrum)    { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
            if (_volwidget)   { _volwidget->lock(false); }
            if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
            if (_audioInfoText) { _audioInfoText->lock(false); }
            if (_clock)       { _clock->lock(false); }
            if (_datewidget)  { _datewidget->lock(false); }
            if (_weather)     { _weather->lock(!config.store.showweather); }
            if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
            // DrawArea visszaállítása teljes képernyőre
            _plwidget->setDrawArea(0, dsp.height());
        }
        _clock->setZoom(1.0f);  // Zoom visszaállítása
        _clock->setSsDate("");  // Dátum törlése
        _clock->moveBack();
        _refreshThemeColors();
        _updateGearWidget();
        numOfNextStation = 0;
        _meta->setAlign(metaConf.widget.align);
        config.isScreensaver = false;
        if (prevMode == VOL || prevMode == WIFI || prevMode == BTINFO ||
            prevMode == QUICKSETTINGS || prevMode == FORECAST) {
            // Az inline nezetek a PG_PLAYER oldalon maradnak, ezert visszatereskor
            // nem aktivaljuk ujra es nem toroljuk a teljes oldalt.
        } else {
            _pager->setPage(pages[PG_PLAYER]);
        }
        _station();
        _title();
        _updateStationIcon();
        // Inline menus stay on PG_PLAYER, so Pager::setPage() cannot restore
        // the footer for them. Draw it after station data has been refreshed.
        if (_footer && !_footer->isActive()) { _footer->setActive(true); }
        if (_nums) { _nums->setText(""); }
        if (_spectrum) { _spectrum->lock(displayContentChanging || !config.store.vumeter); }
        if (_bluetoothwidget) { _bluetoothwidget->invalidate(); }
        if (_wifiwidget) { _wifiwidget->invalidate(); }
        config.setDspOn(config.store.dspon, false);
        pm.on_display_player();
    }

    if (newmode == SCREENSAVER || newmode == SCREENBLANK) {
        eqForceClose();
        config.isScreensaver = true;
        bool showCassette = false;
        bool showCover = false;
        bool showVuMeter = false;
#    ifdef USE_CASSETTE_SCREENSAVER
        showCassette = newmode == SCREENSAVER && !_manualCoverView &&
                       config.isPlaybackActive() &&
                       config.store.screensaverMode == SCREENSAVER_CASSETTE;
        if (_cassettewidget) { _cassettewidget->lock(!showCassette); }
#    endif
#    ifdef USE_COVER_SCREENSAVER
        showCover = newmode == SCREENSAVER &&
                    (_manualCoverView ||
                     (config.isPlaybackActive() &&
                      config.store.screensaverMode == SCREENSAVER_COVERART)) &&
                    _coverScreensaverWidget && _coverScreensaverWidget->ready();
        if (_coverScreensaverWidget) { _coverScreensaverWidget->lock(!showCover); }
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
        showVuMeter = newmode == SCREENSAVER && !_manualCoverView &&
                      config.isPlaybackActive() &&
                      config.store.screensaverMode == SCREENSAVER_VUMETER &&
                      _vuMeterWidget && _vuMeterWidget->ready();
        if (_vuMeterWidget) { _vuMeterWidget->lock(!showVuMeter); }
#    endif
        _clockScreensaverBrightnessActive =
            newmode == SCREENSAVER && !showCassette && !showCover && !showVuMeter;
        if (_clock) {
            if (_clockScreensaverBrightnessActive) {
                _clock->setZoom(2.0f);
                _clock->unlock();
            } else {
                _clock->lock(true);
            }
        }
#    ifdef USE_COVER_SCREENSAVER
        if (showCover) {
            _updateCoverScreensaverText();
            _updateCoverScreensaver();
            _time(true);
            if (timekeeper.weatherIcon[0] != '\0') {
                _coverScreensaverWidget->setWeather(timekeeper.weatherIcon, timekeeper.tempC);
            }
        }
#    endif
        _pager->setPage(pages[PG_SCREENSAVER]);
        if (newmode == SCREENBLANK) {
#    ifdef USE_CASSETTE_SCREENSAVER
            if (_cassettewidget) { _cassettewidget->lock(true); }
#    endif
#    ifdef USE_COVER_SCREENSAVER
            if (_coverScreensaverWidget) { _coverScreensaverWidget->lock(true); }
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
            if (_vuMeterWidget) { _vuMeterWidget->lock(true); }
#    endif
            if (_clock) { _clock->lock(true); _clock->clear(); }
            config.setDspOn(false, false);
        } else if (_clockScreensaverBrightnessActive) {
            setBrightnessPercent(effectiveBrightnessPercent(config.store.brightness));
        }
    } else {
        config.screensaverTicks = SCREENSAVERSTARTUPDELAY;
        config.screensaverPlayingTicks = SCREENSAVERSTARTUPDELAY;
        config.isScreensaver = false;
#    if PWR_AMP != 255 // "PWR_AMP"
        digitalWrite(PWR_AMP, HIGH);
#    endif
    }

    if (newmode == VOL) {
        // Inline volume megjelenítés - nincs teljes képernyőtörlés
        _volInlineShow();
    }
    if (newmode == NUMBERS) { _showDialog(""); }
    if (newmode == UPDATING) { _showDialog(LANG::const_DlgUpdate); }
    if (newmode == SLEEPING) {
        _showDialog("SLEEPING");
        delay(2000);
        dsp.clearDsp();
        config.doSleepW();
    }
    if (newmode == SDCHANGE) {
        // The SD progress overlay uses the same lower area. PG_DIALOG would
        // reactivate the shared footer, so hide it again until PLAYER returns.
        dsp.setScrollId(NULL);
        _pager->setPage(pages[PG_DIALOG]);
        if (_footer) { _footer->setActive(false); }
        _meta->setAlign(WA_CENTER);
        _meta->setText("");   // canvas-ban jelenítjük meg a szöveget
        if (_nums) { _nums->setText(""); }
        _sdProgressDraw(0);
    }
    if (newmode == INFO || newmode == SETTINGS || newmode == TIMEZONE) { _showDialog(LANG::const_DlgNextion); }
    if (newmode == WIFI) {
        _wifiInlineShow();
    }
    if (newmode == BTINFO) {
        _bluetoothInlineShow();
    }
    if (newmode == QUICKSETTINGS) {
        _quickSettingsInlineShow();
    }
    if (newmode == FORECAST) {
        _forecastInlineShow();
    }
    if (newmode == NUMBERS) { _showDialog(""); }
#    if (DSP_MODEL == DSP_ILI9488) || (DSP_MODEL == DSP_ST7796) || (DSP_MODEL == DSP_AXS15231B)
    if (newmode == PRESETS) {
        _pager->setPage(pages[PG_PRESETS], true);
        presets_drawScreen();
    }
#    endif
    if (newmode == STATIONS) {
        _refreshThemeColors();
        // Drop stale player-mode draw requests so they cannot paint one extra old frame.
        resetQueue();
        // Lock all widgets covered by the lower menu area.
        if (_spectrum)    { _spectrum->lock(true); }
        if (_volwidget)   { _volwidget->lock(true); }
        if (_bufferwidget){ _bufferwidget->lock(true); }
        if (_audioInfoText) { _audioInfoText->lock(true); }
        if (_clock)       { _clock->lock(true); }
        if (_datewidget)  { _datewidget->lock(true); }
        if (_weather)     { _weather->lock(true); }
        if (_weatherIcon) { _weatherIcon->lock(true); }
        // Sáv törlése
        dsp.fillRect(0, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT, config.theme.background);
        // Playlist sávra korlátozása
        _plwidget->setDrawArea(VOL_AREA_TOP, VOL_AREA_HEIGHT);
        // Prevent ScrollWidget pre-draw during page activation to avoid an empty-row flash.
        _plcurrent->lock(true);
        // Ensure no stale current-row text is visible before the fresh playlist draw.
        _plcurrent->setText("");
        // Maradunk PG_PLAYER-en - nem váltunk page-et, nincs clearDsp
        _plwidget->lock(false);   // playlist widget aktiválása
        currentPlItem = config.lastStation();
        // Átadjuk a scrollwidgetet, ha eddig nem tettük meg
        _plwidget->init(_plcurrent);
        _plcurrent->unlock();
        _drawPlaylist();
    }
    if (newmode == MODESELECT) {
        _refreshThemeColors();
        resetQueue();
        // Widget lockolás — STATIONS mintájára
        if (_spectrum)    { _spectrum->lock(true); }
        if (_volwidget)   { _volwidget->lock(true); }
        if (_bufferwidget){ _bufferwidget->lock(true); }
        if (_audioInfoText) { _audioInfoText->lock(true); }
        if (_clock)       { _clock->lock(true); }
        if (_datewidget)  { _datewidget->lock(true); }
        if (_weather)     { _weather->lock(true); }
        if (_weatherIcon) { _weatherIcon->lock(true); }
        // Sáv törlése
        dsp.fillRect(0, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT, config.theme.background);
        _msBuildList();
        _msDraw();
    }
    if (newmode == SCREENSELECT) {
        _refreshThemeColors();
        resetQueue();
        if (_spectrum)    { _spectrum->lock(true); }
        if (_volwidget)   { _volwidget->lock(true); }
        if (_bufferwidget){ _bufferwidget->lock(true); }
        if (_audioInfoText) { _audioInfoText->lock(true); }
        if (_clock)       { _clock->lock(true); }
        if (_datewidget)  { _datewidget->lock(true); }
        if (_weather)     { _weather->lock(true); }
        if (_weatherIcon) { _weatherIcon->lock(true); }
        dsp.fillRect(0, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT,
                     config.theme.background);
        _ssIdx = !config.store.screensaverPlayingEnabled
                     ? 0
                     : (config.store.screensaverMode == SCREENSAVER_CASSETTE
                            ? 2
                            : (config.store.screensaverMode == SCREENSAVER_VUMETER
                                   ? 3 : 1));
        _ssDraw();
    }
} // _swichMode vége

void Display::resetQueue() {
    if (displayQueue != NULL) { xQueueReset(displayQueue); }
}

bool Display::tapPlaylistItem(uint16_t y) {
    if (!_plwidget) return false;
    const uint16_t itemH  = _plwidget->itemHeight();   // 30px
    const uint16_t midY   = _plwidget->currentTop();   // a kiválasztott (középső) sor Y koordinátája
    const int      total  = (int)config.playlistLength();
    if (itemH == 0 || total == 0) return false;

    // Melyik sort érintette? A középső = currentPlItem, felfelé/lefelé offset
    int rowOffset  = ((int)y - (int)midY) / (int)itemH;
    int tappedItem = (int)currentPlItem + rowOffset;   // 1-alapú index
    if (tappedItem < 1)     tappedItem = 1;
    if (tappedItem > total) tappedItem = total;

    currentPlItem = (uint16_t)tappedItem;
    putRequest(CLOSEPLAYLIST, currentPlItem);
    return true;
}


void Display::_drawPlaylist() {
    _plwidget->drawPlaylist(currentPlItem);
    timekeeper.waitAndReturnPlayer(config.store.stationsListReturnTime);
}

void Display::_drawNextStationNum(uint16_t num) {
    timekeeper.waitAndReturnPlayer(config.store.stationsListReturnTime); // Visszatérési idő a főképernyőre.
    _meta->setText(config.stationByNum(num));
    _nums->setText(num, "%d");
}

void Display::putRequest(displayRequestType_e type, int payload) {
    if (displayQueue == NULL) { return; }
    if (displayContentChanging && isSplitPlayerRequest(type, payload)) {
        return;
    }
    if (displayContentChanging && isBackgroundRequest(type)) {
        return;
    }
    if (type == PSTART) { purgeQueuedRequestType(PSTOP); }
    if (type == PSTOP) { purgeQueuedRequestType(PSTART); }
    switch (type) {
        case DRAWPLAYLIST:        case NEWTITLE:
        case NEWSTATION:
        case NEWCOVER:
        case DBITRATE:
        case PLAYERREBUILD:
        case PSTART:
        case PSTOP:
        case AUDIOINFO:
        case DSPRSSI:
        case DRAWVOL:
        case DRAWMODESELECT:
        case DRAWSCREENSELECT:
        case DRAWQUICKSETTINGS:
        case DRAWEQ:
        case OPENQUICKSETTINGSEQ:
        case NEWFORECAST:
            purgeQueuedRequestType(type);
            break;
        default:
            break;
    }
    requestParams_t request;
    request.type = type;
    request.payload = payload;
    request.generation = displayContentGeneration;
    xQueueSend(displayQueue, &request, DSQ_SEND_DELAY);
#    ifdef USE_NEXTION
    nextion.putRequest(request);
#    endif
}

void Display::_layoutChange(bool played) {
    if (_transportwidget) { _transportwidget->setPlaying(played); }
    if (!_spectrum) { return; }
    spectrumAnalyzer.resetSmooth();
    if (_mode != PLAYER || isEqOpen()) { return; }
    if (config.store.vumeter) {
        if (played) {
            _spectrum->lock(displayContentChanging);
        } else {
            _spectrum->reset();
        }
    } else {
        // Diagnostic/off mode: do not touch the spectrum drawing area on
        // playback start. reset() clears the full spectrum rectangle and would
        // make a "spectrum disabled" A/B test dirty.
        _spectrum->lock(true);
    }
}

void Display::applyVuModeChange() {
    spectrumAnalyzer.resetSmooth();
    if (_mode != PLAYER || isEqOpen()) { return; }
    if (_spectrum) { _spectrum->reset(); }
    _layoutChange(player.isRunning());
}
void Display::invalidateThemeWidgets() {
    if (!_wifiwidget || _locked || config.isScreensaver) { return; }
    _wifiwidget->invalidate();
    if (_bluetoothwidget) { _bluetoothwidget->invalidate(); }
    if (_gearwidget) { _gearwidget->invalidate(); }
    if (_transportwidget) { _transportwidget->invalidate(); }
    if (_datewidget)  { _datewidget->setColors(config.theme.date, config.theme.background); }
    if (_mode == QUICKSETTINGS) { _drawQuickSettings(); }
    if (_mode == WIFI) { _drawWifiInfo(); }
    if (_mode == FORECAST) { _drawForecast(); }
}

void Display::loop() {
    DisplayMutexGuard guard(0);
    if (!guard.taken()) { return; }
    if (_bootStep == 0) {
        _pager->begin();
        _bootScreen();
        return;
    }
    // Lista-overlay auto-bezárás timerek.
    modeSelectorTick();
    screensaverSelectorTick();
    _quickSettingsTick();
    _forecastTick();
    _updateBluetoothWidget();
    _updateGearWidget();
#ifdef USE_BLUETOOTH
    if (_mode == BTINFO &&
        static_cast<uint32_t>(millis() - _btInfoPollMs) >= 250U) {
        const uint32_t fingerprint = bluetooth.displayFingerprint();
        if (fingerprint != _btInfoFingerprint) { _drawBluetoothInfo(); }
        else { _btInfoPollMs = millis(); }
    }
#endif
#if DSP_MODEL != DSP_AXS15231B
    if (_mode == STATIONS) {
        _plcurrent->loop(); // Ez hajtja az X irányú görgetést a lejátszási listában, ahol a hosszabb szövegek vannak.
    }
#endif
    if (displayQueue == NULL || _locked) { return; }
#    ifdef USE_NEXTION
    nextion.loop();
#    endif
    requestParams_t request;
    bool handledRequest = false;
    if (xQueueReceive(displayQueue, &request, DSP_QUEUE_TICKS)) {
        handledRequest = true;
        // A free-running widget frame may still be flushing through LGFX DMA.
        // Starting a title/station/icon redraw before that transfer is fully
        // settled can corrupt the display transaction state on rare timing.
#if DSP_MODEL != DSP_AXS15231B
        dsp.waitDMA();
#endif
        bool processRequest = true;
        if (isContentRequest(request.type, request.payload) && request.generation != displayContentGeneration) {
            processRequest = false;
        }
        bool pm_result = true;
        if (processRequest) { pm.on_display_queue(request, pm_result); }
        if (processRequest && pm_result) {
            switch (request.type) {
                case NEWMODE: lock(); _swichMode((displayMode_e)request.payload); unlock(); break;
                case SHOWCOVERVIEW:
#ifdef USE_COVER_SCREENSAVER
                    if (_mode == PLAYER && !displayContentChanging &&
                        _coverScreensaverWidget &&
                        _coverScreensaverWidget->ready()) {
                        _manualCoverView = true;
                        lock();
                        _swichMode(SCREENSAVER);
                        unlock();
                    }
#endif
                    break;
                case CLOSEPLAYLIST: {
                    dsp.setTextDatum(top_left);
                    player.sendCommand({PR_PLAY, request.payload});
                } break;

                case CLOCK:
                    if (_mode == PLAYER || _mode == SCREENSAVER) { _time(request.payload == 1); }
                    /*#ifdef USE_NEXTION
  if(_mode==TIMEZONE) nextion.localTime(network.timeinfo);
  if(_mode==INFO)     nextion.rssi();
#endif*/
                    break;

                case NEWTITLE:
                    if (_mode == PLAYER) {
                        _title();
                        _updateStationIcon();
                    }
#ifdef USE_COVER_SCREENSAVER
                    else if (_mode == SCREENSAVER) {
                        _title();
                        _updateCoverScreensaver();
                    }
#endif
                    break;
                case NEWCOVER:
                    if (_mode == PLAYER) { _updateStationIcon(); }
#ifdef USE_COVER_SCREENSAVER
                    else if (_mode == SCREENSAVER) { _updateCoverScreensaver(); }
#endif
                    break;
                case PLAYERREBUILD:
                    if (request.payload > 0 && request.payload != config.lastStation()) {
                        break;
                    }
                    if (_mode != PLAYER) {
                        lock();
                        _swichMode(PLAYER);
                        unlock();
                    }
                    if (_mode == PLAYER) {
                        _title();
                        if (request.payload > 0 && request.payload != config.lastStation()) {
                            break;
                        }
                        if (_bitrate) {
                            char buf[20];
                            snprintf(buf, 20, bitrateFmt, config.station.bitrate);
                            _bitrate->setText(config.station.bitrate == 0 ? "" : buf);
                        }
                        _updateFooterPlaybackText();
                        _updateAudioInfoText();
                        if (request.payload > 0 && request.payload != config.lastStation()) {
                            break;
                        }
                        _station();
                        if (request.payload > 0 && request.payload != config.lastStation()) {
                            break;
                        }
                        _updateStationIcon();
                    }
                    break;
                case NEWSTATION:
                    if (_mode == PLAYER) {
                        _station();
                        _updateStationIcon();
                    }
#ifdef USE_COVER_SCREENSAVER
                    else if (_mode == SCREENSAVER) {
                        _station();
                        _updateCoverScreensaverText();
                        _updateCoverScreensaver();
                    }
#endif
                    break;
                case NEXTSTATION:
                    if (_mode == NUMBERS) { _drawNextStationNum(request.payload); }
                    break;
                case DRAWPLAYLIST: _drawPlaylist(); break;
                case DRAWVOL: _volume(); break;
                case DRAWMODESELECT:
                    if (_mode == MODESELECT) { _msDraw(); }
                    break;
                case DRAWSCREENSELECT:
                    if (_mode == SCREENSELECT) { _ssDraw(); }
                    break;
                case DRAWQUICKSETTINGS:
                    if (_mode == QUICKSETTINGS) { _drawQuickSettings(); }
                    break;
                case DRAWEQ:
                    if (_mode == PLAYER && isEqOpen()) { _eqInlineDraw(); }
                    break;
                case CLOSEEQ:
                    eqForceClose();
                    break;
                case OPENQUICKSETTINGSEQ:
                    if (_mode == QUICKSETTINGS) {
                        lock();
                        _swichMode(PLAYER);
                        unlock();
                        eqToggle();
                        _updateGearWidget();
                    }
                    break;

                case DBITRATE: {
                    if (_mode == PLAYER) { // csak a lejátszás képernyőn frissíti a bitrateWidgetet
                        char buf[20];
                        snprintf(buf, 20, bitrateFmt, config.station.bitrate);

                        if (_bitrate) { _bitrate->setText(config.station.bitrate == 0 ? "" : buf); }

                        _updateFooterPlaybackText();
                        _updateAudioInfoText();
                        // Beállítja a csatorna számát a widgeten
                    }
                } break;

                case AUDIOINFO:
                    if (isLowerMenuMode(_mode) || isEqOpen()) { break; }
                    if (_bufferwidget) {
                        _bufferwidget->lock(!config.store.audioinfo);
                        if (!config.store.audioinfo) {
                            _bufferwidget->clear();
                        } else {
                            uint32_t bm = psramInit() ? 300000 : 1600 * config.store.abuff;
                            _bufferwidget->setValue(player.isRunning() ? player.inBufferFilled() : 0, bm);
                        }
                    }
                    break;

                case SHOWVUMETER:
                    if (_mode != PLAYER || isEqOpen()) {
                        spectrumAnalyzer.resetSmooth();
                        break;
                    }
                    if (_spectrum) {
                        if (config.store.vumeter) {
                            _spectrum->lock(false);
                            _layoutChange(player.isRunning());
                        } else {
                            _spectrum->lock(true);
                            spectrumAnalyzer.resetSmooth();
                            _spectrum->reset();
                        }
                    }
                    break;

                case SWITCHVUMODE:applyVuModeChange(); break;

                case SHOWWEATHER:
                    if (isLowerMenuMode(_mode) || isEqOpen()) { break; }
                    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); if (!config.store.showweather) _weatherIcon->clearArea(); }
                    if (_weather) { _weather->lock(!config.store.showweather); }
                    if (!config.store.showweather) {
#    ifndef HIDE_IP
                        if (_ipbox) { _ipbox->setText(config.ipToStr(WiFi.localIP()), iptxtFmt); }
#    endif
                    } else {
                        if (_weather) { _weather->setText(LANG::const_getWeather); } // Üres string
                    }
                    break;

                case NEWWEATHER:
                    if (_weatherIcon && timekeeper.weatherIcon[0] != '\0') {
                        _weatherIcon->setWeather(timekeeper.weatherIcon, timekeeper.tempC);
                    }
#ifdef USE_COVER_SCREENSAVER
                    if (_coverScreensaverWidget && timekeeper.weatherIcon[0] != '\0') {
                        _coverScreensaverWidget->setWeather(timekeeper.weatherIcon, timekeeper.tempC);
                    }
#endif
                    if (_weather && timekeeper.weatherBuf) {
                        _weather->resetText(); // short/long váltáskor az oldtext cache-t invalidáljuk
                        _weather->setText(timekeeper.weatherBuf);
                        // A weather refresh must not keep an old scroll owner.
                        // The normal page order will then choose station first, then metadata,
                        // and weather only if the higher-priority rows do not need scrolling.
                        ScrollWidget::releaseScrollOwner();
                    }
                    break;

                case NEWFORECAST:
                    if (_mode == FORECAST) { _drawForecast(); }
                    break;

                case SHOWRSSIMODE:
#    ifndef HIDE_RSSI
                    _applyRssiMode();
#    endif
                    break;

                case INVALIDATETHEMEWIDGETS: invalidateThemeWidgets(); break;

                case WIFISCANNING:
                    if (_bootStep == 1 && _bootstring) {
                        _bootstring->setText("Scan Wi-Fi...");
                    }
                    break;
                case BOOTSTRING:
                    if (_bootstring) { _bootstring->setText(config.ssids[request.payload].ssid, LANG::bootstrFmt); }
                    /*#ifdef USE_NEXTION
 char buf[50];
 snprintf(buf, 50, bootstrFmt, config.ssids[request.payload].ssid);
 nextion.bootString(buf);
#endif*/
                    break;

                case WAITFORSD:
                    if (_bootstring) { _bootstring->setText(LANG::const_waitForSD); }
                    break;

                case SDFILEINDEX:
                    if (_mode == SDCHANGE) { _sdProgressDraw(request.payload); }
                    break;

                case DSPRSSI:
                    if (_mode == PLAYER || _mode == WIFI) {
                        _setRSSI(request.payload);
                        if (_mode == PLAYER && _bufferwidget && config.store.audioinfo) {
                            uint32_t bm = psramInit() ? 300000 : 1600 * config.store.abuff;
                            _bufferwidget->setValue(player.isRunning() ? player.inBufferFilled() : 0, bm);
                        }
                    }
                    break;

                case PSTART:
                    if (request.payload > 0 && request.payload != config.lastStation()) {
                        break;
                    }
                    displayContentChanging = false;
                    _layoutChange(true);
                    _updateAudioInfoText();
#ifdef USE_CASSETTE_SCREENSAVER
                    if (_mode == SCREENSAVER && !_manualCoverView &&
                        config.store.screensaverMode == SCREENSAVER_CASSETTE &&
                        _cassettewidget) {
                        if (_clockScreensaverBrightnessActive && config.store.dspon) {
                            setBrightnessPercent(config.store.brightness);
                        }
                        _clockScreensaverBrightnessActive = false;
                        _clock->lock(true);
#    ifdef USE_COVER_SCREENSAVER
                        if (_coverScreensaverWidget) { _coverScreensaverWidget->lock(true); }
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
                        if (_vuMeterWidget) { _vuMeterWidget->lock(true); }
#    endif
                        // The selected full-screen widget must draw last: locking
                        // another active screensaver clears the whole panel.
                        _cassettewidget->lock(false);
                    }
#endif
#ifdef USE_COVER_SCREENSAVER
                    if (_mode == SCREENSAVER &&
                        (_manualCoverView ||
                         config.store.screensaverMode == SCREENSAVER_COVERART) &&
                        _coverScreensaverWidget && _coverScreensaverWidget->ready()) {
                        if (_clockScreensaverBrightnessActive && config.store.dspon) {
                            setBrightnessPercent(config.store.brightness);
                        }
                        _clockScreensaverBrightnessActive = false;
                        _updateCoverScreensaverText();
                        _updateCoverScreensaver();
                        if (timekeeper.weatherIcon[0] != '\0') {
                            _coverScreensaverWidget->setWeather(timekeeper.weatherIcon, timekeeper.tempC);
                        }
                        _clock->lock(true);
#    ifdef USE_CASSETTE_SCREENSAVER
                        if (_cassettewidget) { _cassettewidget->lock(true); }
#    endif
#    ifdef USE_VU_METER_SCREENSAVER
                        if (_vuMeterWidget) { _vuMeterWidget->lock(true); }
#    endif
                        _coverScreensaverWidget->lock(false);
                        _time(true);
                    }
#endif
#ifdef USE_VU_METER_SCREENSAVER
                    if (_mode == SCREENSAVER && !_manualCoverView &&
                        config.store.screensaverMode == SCREENSAVER_VUMETER &&
                        _vuMeterWidget && _vuMeterWidget->ready()) {
                        if (_clockScreensaverBrightnessActive && config.store.dspon) {
                            setBrightnessPercent(config.store.brightness);
                        }
                        _clockScreensaverBrightnessActive = false;
                        if (_clock) { _clock->lock(true); }
#    ifdef USE_CASSETTE_SCREENSAVER
                        if (_cassettewidget) { _cassettewidget->lock(true); }
#    endif
#    ifdef USE_COVER_SCREENSAVER
                        if (_coverScreensaverWidget) { _coverScreensaverWidget->lock(true); }
#    endif
                        _vuMeterWidget->lock(false);
                    }
#endif
                    break;
                case PSTOP:
                    _layoutChange(false);
                    _updateAudioInfoText();
                    displayContentChanging = false;
#ifdef USE_CASSETTE_SCREENSAVER
                    if (_mode == SCREENSAVER && !_manualCoverView && _cassettewidget) {
                        _cassettewidget->lock(true);
                    }
#endif
#ifdef USE_COVER_SCREENSAVER
                    if (_mode == SCREENSAVER && !_manualCoverView &&
                        _coverScreensaverWidget) {
                        _coverScreensaverWidget->lock(true);
                    }
#endif
#ifdef USE_VU_METER_SCREENSAVER
                    if (_mode == SCREENSAVER && !_manualCoverView && _vuMeterWidget) {
                        _vuMeterWidget->lock(true);
                    }
#endif
                    if (_mode == SCREENSAVER && !_manualCoverView) {
                        _clockScreensaverBrightnessActive = true;
                        _clock->setZoom(2.0f);
                        _clock->lock(false);
                        setBrightnessPercent(effectiveBrightnessPercent(config.store.brightness));
                    }
                    break;
                case DSP_START: _start(); break;

                case NEWIP:
#    ifndef HIDE_IP
                    if (_ipbox) { _ipbox->setText(config.ipToStr(WiFi.localIP()), iptxtFmt); }
#    endif
                    if (_mode == WIFI) { _drawWifiInfo(); }
                    break;

                default:
                    break;
            }
        }
    }

    // After an explicit display request, let the bus/DMA settle and process
    // free-running widget frames on the next display tick. This avoids mixing
    // a mode/title/station redraw with a spectrum/scroll/clock frame.
    if (handledRequest) {
#if DSP_MODEL != DSP_AXS15231B
        dsp.waitDMA();
#endif
        return;
    }

#if DSP_MODEL != DSP_AXS15231B
    // VOL overlay aktív → ne futtassuk a widget loop-okat (spektrum belerajzolna a VOL területbe)
    dsp.waitDMA();
    if (_mode != VOL) { _pager->loop(); }
    dsp.waitDMA();
#endif

#if DSP_MODEL == DSP_AXS15231B
    _pager->loop();
#endif
    dsp.loop();
} // loop vége

void Display::_setRSSI(int rssi) {
    const uint32_t now = millis();
    const bool     firstUpdate = (_lastRssiText == 9999);
    const bool     changedEnough = abs(rssi - _lastRssiText) >= RSSI_TEXT_HYSTERESIS_DB;
    const bool     forceUpdate = !firstUpdate && RSSI_TEXT_FORCE_UPDATE_MS > 0 &&
                                 (now - _lastRssiTextMs >= RSSI_TEXT_FORCE_UPDATE_MS);

    if (firstUpdate || changedEnough || forceUpdate) {
        _lastRssiText = rssi;
        _lastRssiTextMs = now;
        if (_rssibox) { _rssibox->setText(rssi, "%d dBm"); }
        if (_mode == WIFI) { _drawWifiInfo(); }
    }
    if (_wifiwidget) {
        _wifiwidget->setRSSI(rssi);
        return;
    }
    if (!_rssi) { return; }
#    if RSSI_DIGIT
    _rssi->setText(rssi, rssiFmt);
    return;
#    endif
    char rssiG[3];
    int  rssi_steps[] = {RSSI_STEPS};
    if (rssi >= rssi_steps[0]) { strlcpy(rssiG, "\004\006", 3); }
    if (rssi >= rssi_steps[1] && rssi < rssi_steps[0]) { strlcpy(rssiG, "\004\005", 3); }
    if (rssi >= rssi_steps[2] && rssi < rssi_steps[1]) { strlcpy(rssiG, "\004\002", 3); }
    if (rssi >= rssi_steps[3] && rssi < rssi_steps[2]) { strlcpy(rssiG, "\003\002", 3); }
    if (rssi < rssi_steps[3] || rssi >= 0) { strlcpy(rssiG, "\001\002", 3); }
    _rssi->setText(rssiG);
}

void Display::_updateFooterPlaybackText() {
    const bool bluetoothMode = config.getMode() == PM_BLUETOOTH;
    const char* modeText = bluetoothMode ? "BT"
                         : config.getMode() == PM_SDCARD ? "SD"
                         : config.store.playlistSource == PL_SRC_DLNA ? "DLNA"
                         : "WEB";
    if (_footerPlayMode) { _footerPlayMode->setText(modeText); }

    char stationText[16];
    const uint16_t station = bluetoothMode ? 0 : config.lastStation();
    if (station > 0) {
        snprintf(stationText, sizeof(stationText), "Ch.: %u", station);
    } else {
        strlcpy(stationText, "Ch.: -", sizeof(stationText));
    }
    if (_footerStation) { _footerStation->setText(stationText); }
}

void Display::_updateAudioInfoText() {
    if (!_audioInfoText) { return; }
    if (!config.isPlaybackActive()) {
        _audioInfoText->setText("");
        return;
    }

    char codec[20] = {};
    uint32_t bitrateKbps = 0;
    uint32_t sampleRate = 0;
    uint8_t bitsPerSample = 0;

#ifdef USE_BLUETOOTH
    if (config.getMode() == PM_BLUETOOTH) {
        const char* btCodec = bluetooth.streamCodec();
        if (btCodec && strcmp(btCodec, "unknown") != 0) {
            strlcpy(codec, btCodec, sizeof(codec));
        }
        sampleRate = bluetooth.inputCodecSampleRate();
        if (!sampleRate && codec[0]) {
            sampleRate = bluetooth.streamSampleRate();
        }
        bitsPerSample = bluetooth.streamBits();
    } else
#endif
    {
        strlcpy(codec, displayCodecName(config.configFmt), sizeof(codec));
        if (!codec[0]) {
            const int codecIndex = player.getCodec();
            if (codecIndex > 0 && codecIndex <= 8) {
                strlcpy(codec, player.getCodecname(), sizeof(codec));
            }
        }
        bitrateKbps = config.station.bitrate;
        if (!bitrateKbps) {
            const uint32_t bitrate = player.getBitRate();
            if (bitrate) { bitrateKbps = (bitrate + 500U) / 1000U; }
        }
        sampleRate = player.getSampleRate();
        bitsPerSample = player.getBitsPerSample();
    }

    char info[64] = {};
    char token[24] = {};
    appendAudioInfoToken(info, sizeof(info), codec);
    if (bitrateKbps) {
        snprintf(token, sizeof(token), "%lukbps",
                 static_cast<unsigned long>(bitrateKbps));
        appendAudioInfoToken(info, sizeof(info), token);
    }
    if (sampleRate) {
        if (sampleRate % 1000U == 0U) {
            snprintf(token, sizeof(token), "%lukHz",
                     static_cast<unsigned long>(sampleRate / 1000U));
        } else {
            snprintf(token, sizeof(token), "%lu.%lukHz",
                     static_cast<unsigned long>(sampleRate / 1000U),
                     static_cast<unsigned long>((sampleRate % 1000U) / 100U));
        }
        appendAudioInfoToken(info, sizeof(info), token);
    }
    if (bitsPerSample) {
        snprintf(token, sizeof(token), "%ubit",
                 static_cast<unsigned>(bitsPerSample));
        appendAudioInfoToken(info, sizeof(info), token);
    }
    _audioInfoText->setText(info);
}

void Display::_station() {
    _updateFooterPlaybackText();
    _updateAudioInfoText();
    _meta->setAlign(metaConf.widget.align);
    char nameSnapshot[BUFLEN];
    strlcpy(nameSnapshot, config.station.name, sizeof(nameSnapshot));
    if (nameSnapshot[0] == '.') {
        _meta->setText(nameSnapshot + 1);
    } else {
        _meta->setText(nameSnapshot);
    }
    // When station text changes, let the next display loop pick the
    // highest-priority long text again: station -> artist/title -> weather.
    ScrollWidget::releaseScrollOwner();

    /*#ifdef USE_NEXTION
    nextion.newNameset(config.station.name);
    nextion.bitrate(config.station.bitrate);
    nextion.bitratePic(ICON_NA);
  #endif*/
}

void Display::_updateStationIcon() {
    if (!_stationIcon) return;

    uint8_t* coverData = nullptr;
    size_t coverSize = 0;
    bool coverIsJpeg = false;
    uint32_t coverGeneration = 0;
    if (coverArt.copyReady(coverData, coverSize,
                           coverIsJpeg, coverGeneration)) {
        _stationIcon->setCover(coverData, coverSize, coverIsJpeg, coverGeneration);
        return;
    }

    const uint8_t pm = (config.getMode() == PM_SDCARD) ? DPS_SDCARD
                     : (config.getMode() == PM_BLUETOOTH) ? DPS_BLUETOOTH
                     : (config.store.playlistSource == PL_SRC_DLNA) ? DPS_DLNA : DPS_WEB;
    const char* iconLookupName = (config.getMode() == PM_BLUETOOTH)
        ? ""
        : (config.station.iconName[0] != '\0') ? config.station.iconName : config.station.name;
    _stationIcon->setStation(iconLookupName, pm);
}

#ifdef USE_COVER_SCREENSAVER
void Display::_updateCoverScreensaver() {
    if (!_coverScreensaverWidget) return;

    uint8_t* coverData = nullptr;
    size_t coverSize = 0;
    bool coverIsJpeg = false;
    uint32_t coverGeneration = 0;
    if (coverArt.copyReady(coverData, coverSize,
                           coverIsJpeg, coverGeneration)) {
        _coverScreensaverWidget->setCover(coverData, coverSize,
                                          coverIsJpeg, coverGeneration);
        return;
    }

    const uint8_t playMode = (config.getMode() == PM_SDCARD) ? DPS_SDCARD
                           : (config.getMode() == PM_BLUETOOTH) ? DPS_BLUETOOTH
                           : (config.store.playlistSource == PL_SRC_DLNA) ? DPS_DLNA
                                                                          : DPS_WEB;
    const char* iconLookupName = (config.getMode() == PM_BLUETOOTH)
        ? ""
        : (config.station.iconName[0] != '\0') ? config.station.iconName
                                                : config.station.name;
    _coverScreensaverWidget->setStation(iconLookupName, playMode);
}

void Display::_updateCoverScreensaverText() {
    if (!_coverScreensaverWidget) return;

    char title[BUFLEN];
    strlcpy(title, config.station.title[0] ? config.station.title
                                          : config.station.name,
            sizeof(title));
    char* track = strstr(title, " - ");
    if (track) {
        *track = '\0';
        track += 3;
        _coverScreensaverWidget->setTrack(title, track);
    } else {
        _coverScreensaverWidget->setTrack("", title);
    }
}
#endif

char* split(char* str, const char* delim) {
    char* dmp = strstr(str, delim);
    if (dmp == NULL) { return NULL; }
    *dmp = '\0';
    return dmp + strlen(delim);
}

void Display::_title() {
    // Ha üres a title, használja a playlistben tárolt nevet.
    char titleSnapshot[BUFLEN];
    char nameSnapshot[BUFLEN];
    strlcpy(titleSnapshot, config.station.title, sizeof(titleSnapshot));
    strlcpy(nameSnapshot, config.station.name, sizeof(nameSnapshot));

    const char* titleText = titleSnapshot[0] ? titleSnapshot : nameSnapshot;
    if (titleText[0] != '\0') {
        char tmpbuf[BUFLEN];
        strlcpy(tmpbuf, titleText, sizeof(tmpbuf));
        char* stitle = split(tmpbuf, " - ");
        if (stitle && _title2) {
            _title1->setText(tmpbuf);
            _title2->setText(stitle);
        } else {
            _title1->setText(titleText);
            if (_title2) { _title2->setText(""); }
        }
    } else {
        _title1->setText("");
        if (_title2) { _title2->setText(""); }
    }
#ifdef USE_COVER_SCREENSAVER
    _updateCoverScreensaverText();
#endif
    // New metadata should be able to pre-empt weather scrolling.
    ScrollWidget::releaseScrollOwner();
    if (player_on_track_change) { player_on_track_change(); }
    pm.on_track_change();
}

void Display::_time(bool redraw) {
    tm displayTime{};
    network_get_timeinfo_snapshot(&displayTime);

#    if LIGHT_SENSOR != 255
    if (config.store.dspon) {
        const int sensedBrightness = AUTOBACKLIGHT(analogRead(LIGHT_SENSOR));
        config.store.brightness = static_cast<uint8_t>(constrain(sensedBrightness, 0, 100));
        bool applySensorBrightness = true;
#        if BRIGHTNESS_PIN != 255
        applySensorBrightness = !config.store.fadeEnabled ||
                                (!backlightPlugin.isFading() && !backlightPlugin.isDimmed());
#        endif
        if (applySensorBrightness) {
            setBrightnessPercent(effectiveBrightnessPercent(config.store.brightness));
        }
    }
#    endif
#ifdef USE_COVER_SCREENSAVER
    if (_mode == SCREENSAVER && _coverScreensaverWidget &&
        !_coverScreensaverWidget->locked()) {
        char timeBuffer[8];
        snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d",
                 displayTime.tm_hour, displayTime.tm_min);
        // A CLOCK,1 kérés nemcsak időváltozást, hanem óra-font/AM-PM
        // beállításváltozást is jelezhet, ezért ilyenkor azonos percen belül is
        // újra kell rajzolni a cover-art képernyővédő információs paneljét.
        _coverScreensaverWidget->setTime(timeBuffer, redraw);
        _ssUpdateDate();
        return;
    }
#endif
    if (_clock && _clock->locked()) return;   // EQ / VOL overlay aktív — ne rajzoljon felé
    if (config.isScreensaver) {
        _ssUpdateDate();
        _clock->draw(redraw);
    } else {
        _clock->draw(redraw);
    }
}

void Display::_volume() {
#    ifndef HIDE_VOL
    if (_volwidget) { _volwidget->setVolume(config.store.volume); }
#    endif
    if (_mode == VOL) {
        timekeeper.waitAndReturnPlayer(2);
        _volInlineDraw();  // Frissítjük az inline volume overlay-t
    }
    /*#ifdef USE_NEXTION
      nextion.setVol(config.store.volume, _mode == VOL);
    #endif*/
}

// ---- Screensaver dátum kirajzolása a zoom-olt óra alá ----
void Display::_ssUpdateDate() {
    // Dátum szöveg frissítése a ClockWidget-ben (az óra canvas-ba rajzolja bele)
    time_t now = time(nullptr);
    tm displayTime{};
    localtime_r(&now, &displayTime);

    char datebuf[64];
    switch (config.store.dateFormat) {
        case 0:  snprintf(datebuf, sizeof(datebuf), "%d. %s %2d. %s",     displayTime.tm_year+1900, LANG::mnths[displayTime.tm_mon], displayTime.tm_mday, LANG::dowf[displayTime.tm_wday]); break;
        case 1:  snprintf(datebuf, sizeof(datebuf), "%2d %s %d",           displayTime.tm_mday, LANG::mnths[displayTime.tm_mon], displayTime.tm_year+1900); break;
        case 2:  snprintf(datebuf, sizeof(datebuf), "%s %2d %s %d",        LANG::dowf[displayTime.tm_wday], displayTime.tm_mday, LANG::mnths[displayTime.tm_mon], displayTime.tm_year+1900); break;
        case 3:  snprintf(datebuf, sizeof(datebuf), "%s - %02d. %s. %04d", LANG::dowf[displayTime.tm_wday], displayTime.tm_mday, LANG::mnths[displayTime.tm_mon], displayTime.tm_year+1900); break;
        default: snprintf(datebuf, sizeof(datebuf), "%s - %02d. %s. %d",   LANG::dowf[displayTime.tm_wday], displayTime.tm_mday, LANG::mnths[displayTime.tm_mon], displayTime.tm_year+1900); break;
    }
#ifdef USE_COVER_SCREENSAVER
    if (_coverScreensaverWidget) {
        char coverDate[192];
        strlcpy(coverDate, datebuf, sizeof(coverDate));
#    ifdef NAMEDAYS_FILE
        if (config.store.nameday) {
            char nameday[128] = {};
            if (namedays_get_str(static_cast<uint8_t>(displayTime.tm_mon + 1),
                                  static_cast<uint8_t>(displayTime.tm_mday),
                                  nameday, sizeof(nameday)) && nameday[0]) {
                strlcat(coverDate, "  |  ", sizeof(coverDate));
                strlcat(coverDate, nameday, sizeof(coverDate));
            }
        }
#    endif
        _coverScreensaverWidget->setDate(coverDate);
    }
#endif
    _clock->setSsDate(datebuf);
}

// ---- Inline volume overlay (137px → 286px sáv) ----

void Display::_volInlineShow() {
    // Ha EQ overlay nyitva van, zárjuk be
    eqForceClose();
    // Lock: spektrum, óra, dátum, időjárás widgetek
    if (_spectrum)    { _spectrum->lock(true); }
    if (_volwidget)   { _volwidget->lock(true); }
    if (_bufferwidget){ _bufferwidget->lock(true); }
    if (_audioInfoText) { _audioInfoText->lock(true); }
    if (_clock)       { _clock->lock(true); }
    if (_datewidget)  { _datewidget->lock(true); }
    if (_weather)     { _weather->lock(true); }
    if (_weatherIcon) { _weatherIcon->lock(true); }
    // Sáv törlése
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT, config.theme.background);
    // Kirajzolás
    _volInlineDraw();
}

void Display::_volInlineHide() {
    // Clear the overlay first, then unlock widgets so their redraw is not erased.
    dsp.fillRect(VOL_AREA_LEFT, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT, config.theme.background);

    // Unlock widgetek - a _pager->setPage(PG_PLAYER) után újrarajzolódnak
    if (_spectrum)    { _spectrum->lock(!config.store.vumeter); }
    if (_volwidget)   { _volwidget->lock(false); }
    if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
    if (_audioInfoText) { _audioInfoText->lock(false); }
    if (_clock)       { _clock->lock(false); }
    if (_datewidget)  { _datewidget->lock(false); }
    if (_weather)     { _weather->lock(!config.store.showweather); }
    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
}

void Display::_volInlineDraw() {
    // Canvas sprite a villogásmentes rajzoláshoz
    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(VOL_AREA_WIDTH, VOL_AREA_HEIGHT);
    canvas.fillSprite(config.theme.background);

    uint8_t vol = config.store.volume;

    // --- "VOLUME" felirat - vlw20 font ---
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(config.theme.meta, config.theme.background);
    if (font_vlw_20) {
        canvas.loadFont(font_vlw_20);
        canvas.setTextSize(1);
        canvas.drawString("VOLUME", VOL_AREA_WIDTH / 2, 14);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString("VOLUME", VOL_AREA_WIDTH / 2, 14);
    }

    // --- Nagy szám - clock font ---
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", vol);
    canvas.setTextColor(config.theme.digit, config.theme.background);
    if (font_vlw_clock) {
        canvas.loadFont(font_vlw_clock);
        canvas.setTextSize(1);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(buf, VOL_AREA_WIDTH / 2, 75);
        canvas.unloadFont();
    } else {
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextSize(7);
        canvas.drawString(buf, VOL_AREA_WIDTH / 2, 75);
    }

    // --- Hangszóró ikonok (bitmap maszk alapján) ---
    const uint16_t SPK_Y     = 30;  // canvas-on belül
    const uint16_t SPK_COLOR = config.theme.digit;
    const uint16_t SPK_LEFT  = 15;
    const uint16_t SPK_RIGHT = VOL_AREA_WIDTH - SPEAKER_W - 15;

    for (uint16_t y = 0; y < SPEAKER_H; y++) {
        for (uint16_t x = 0; x < SPEAKER_W; x++) {
            uint16_t idx = y * SPEAKER_W + x;
            if (pgm_read_byte(&speakerLeftMask[idx >> 3]) & (0x80 >> (idx & 7)))
                canvas.drawPixel(SPK_LEFT + x, SPK_Y + y, SPK_COLOR);
            if (pgm_read_byte(&speakerRightMask[idx >> 3]) & (0x80 >> (idx & 7)))
                canvas.drawPixel(SPK_RIGHT + x, SPK_Y + y, SPK_COLOR);
        }
    }

    // --- Szegmens bar a sáv aljára ---
    const uint8_t  SEG_COUNT = 21;
    const uint16_t SEG_GAP  = 5;
    const uint16_t SEG_W    = 14;
    const uint16_t BAR_LEFT = (VOL_AREA_WIDTH - (SEG_COUNT * (SEG_W + SEG_GAP) - SEG_GAP)) / 2;
    const uint16_t BAR_TOP  = VOL_AREA_HEIGHT - 28;  // canvas-on belül
    const uint16_t SEG_H    = 14;

    for (uint8_t i = 0; i < SEG_COUNT; i++) {
        uint16_t x = BAR_LEFT + i * (SEG_W + SEG_GAP);
        uint16_t col;
        if (i < vol) {
            float ratio = (float)i / SEG_COUNT;
            if (ratio < 0.6f)       col = config.theme.vol_low;
            else if (ratio < 0.85f) col = config.theme.vol_mid;
            else                    col = config.theme.vol_high;
        } else {
            col = config.theme.vol_inactive;
        }
        canvas.fillRect(x, BAR_TOP, SEG_W, SEG_H, col);
    }

    // Egy lépésben kirakjuk a sávba
    canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
    canvas.deleteSprite();
}

// ── SD indexelés progress overlay (VOL_AREA sávban, PG_PLAYER-en maradva) ──
void Display::_sdProgressDraw(int count) {
    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(VOL_AREA_WIDTH, VOL_AREA_HEIGHT);
    canvas.fillSprite(config.theme.background);

    canvas.setTextDatum(MC_DATUM);

    // Fejléc: "INDEX SD"
    canvas.setTextColor(config.theme.meta, config.theme.background);
    if (font_vlw_20) {
        canvas.loadFont(font_vlw_20);
        canvas.setTextSize(1);
        canvas.drawString(LANG::const_waitForSD, VOL_AREA_WIDTH / 2, 20);
        canvas.unloadFont();
    } else {
        canvas.setTextSize(2);
        canvas.drawString(LANG::const_waitForSD, VOL_AREA_WIDTH / 2, 20);
    }

    // Nagy szám: indexelt fájlok száma
    if (count > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", count);
        canvas.setTextColor(config.theme.digit, config.theme.background);
        if (font_vlw_clock) {
            canvas.loadFont(font_vlw_clock);
            canvas.setTextSize(1);
            canvas.drawString(buf, VOL_AREA_WIDTH / 2, 90);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(7);
            canvas.drawString(buf, VOL_AREA_WIDTH / 2, 90);
        }
    }

    canvas.pushSprite(VOL_AREA_LEFT, VOL_AREA_TOP);
    canvas.deleteSprite();
}
// ══════════════════════════════════════════════════════════════════════════════

// EQ overlay konstansok
#define EQ_OVERLAY_TOP    VOL_AREA_TOP
#define EQ_OVERLAY_H      VOL_AREA_HEIGHT
#define EQ_OVERLAY_LEFT   VOL_AREA_LEFT
#define EQ_OVERLAY_W      VOL_AREA_WIDTH

// EQ preset nevek és értékek: { bass, middle, trebble }
static const char* const eqPresetNames[]    = { "Flat","Pop","Blues","Rock","Metal","Jazz","Country","Alternative" };
static const int8_t      eqPresetValues[][3] = {
    {  0,  0,  0 },   // Flat
    { -1,  5,  5 },   // Pop
    {  4,  6,  0 },   // Blues
    {  3,  0,  5 },   // Rock
    {  6, -3,  5 },   // Metal
    { -2,  0,  3 },   // Jazz
    {  1,  2,  4 },   // Country
    {  4, -2,  4 },   // Alternative
};
static constexpr uint8_t EQ_PRESET_COUNT = 8;
static constexpr uint8_t EQ_SLIDER_COUNT = 4;
static constexpr uint8_t EQ_SLIDER_FOCUS_COUNT = 6; // four sliders, PRESETS, BACK
static constexpr uint8_t EQ_PRESET_FOCUS_COUNT = EQ_PRESET_COUNT + 1; // BACK

// FusionEdge: Auto EQ – genre-name alapú EQ preset keresés (case-insensitive).
// A playlist CSV "genre" mezőjét veti össze az eqPresetNames[] tömb elemeivel.
// Pl. genre="metal" -> "Metal" preset egyezik.
bool findEqPresetByGenre(const char* genre, int8_t& bass, int8_t& mid, int8_t& treb) {
    if (!genre || genre[0] == '\0') { return false; }
    for (uint8_t i = 0; i < EQ_PRESET_COUNT; i++) {
        if (strcasecmp(eqPresetNames[i], genre) == 0) {
            bass = eqPresetValues[i][0];
            mid  = eqPresetValues[i][1];
            treb = eqPresetValues[i][2];
            return true;
        }
    }
    return false;
}


// Belső állapot: preset nézet aktív-e
static bool _eqPresetView = false;
static uint8_t _eqEncoderFocus = 0;
static bool _eqEncoderEditing = false;
static bool _eqClosePending = false;

// Slider érintési állapot
static int8_t __attribute__((unused)) _eqTouchSlider = -1;  // 0=bass,1=mid,2=treb,3=bal; -1=nincs

bool Display::eqToggle() {
    if (!_eqwidget) return false;
    bool nowActive = _eqwidget->toggle();
    if (nowActive) {
        _eqPresetView = false;
        _eqEncoderFocus = 0;
        _eqEncoderEditing = false;
        _eqClosePending = false;
        _eqInlineShow();
    } else {
        _eqPresetView = false;
        _eqEncoderEditing = false;
        _eqClosePending = false;
        _eqInlineHide();
    }
    return nowActive;
}

bool Display::isEqOpen() const {
    return _eqwidget && _eqwidget->isEqActive();
}

void Display::eqForceClose() {
    _eqClosePending = false;
    if (!_eqwidget || !_eqwidget->isEqActive()) return;
    _eqPresetView = false;
    _eqEncoderEditing = false;
    _eqwidget->forceClose();
    _eqInlineHide();
}

void Display::eqEncoderRotate(int8_t delta) {
    if (!isEqOpen() || _eqClosePending || delta == 0) return;
    if (!_eqPresetView && _eqEncoderEditing && _eqEncoderFocus < EQ_SLIDER_COUNT) {
        const int16_t maximum = _eqEncoderFocus == 3 ? 16 : 6;
        const int16_t current = _eqEncoderFocus == 0 ? config.store.bass
                              : _eqEncoderFocus == 1 ? config.store.middle
                              : _eqEncoderFocus == 2 ? config.store.trebble
                                                     : config.store.balance;
        int16_t value = current + delta;
        if (value > maximum) value = maximum;
        if (value < -maximum) value = -maximum;
        if (value != current) {
            const int8_t adjusted = static_cast<int8_t>(value);
            switch (_eqEncoderFocus) {
                case 0: config.setTone(adjusted, config.store.middle, config.store.trebble); break;
                case 1: config.setTone(config.store.bass, adjusted, config.store.trebble); break;
                case 2: config.setTone(config.store.bass, config.store.middle, adjusted); break;
                case 3: config.setBalance(adjusted); break;
            }
        }
    } else {
        const int16_t count = _eqPresetView ? EQ_PRESET_FOCUS_COUNT : EQ_SLIDER_FOCUS_COUNT;
        // Navigation uses one item per detent, with wrap-around in both views.
        const int16_t next = (_eqEncoderFocus + (delta > 0 ? 1 : count - 1)) % count;
        _eqEncoderFocus = static_cast<uint8_t>(next);
    }
    putRequest(DRAWEQ);
}

void Display::eqEncoderConfirm() {
    if (!isEqOpen() || _eqClosePending) return;
    if (_eqPresetView) {
        if (_eqEncoderFocus < EQ_PRESET_COUNT) {
            const int8_t* tone = eqPresetValues[_eqEncoderFocus];
            config.setTone(tone[0], tone[1], tone[2]);
        }
        _eqPresetView = false;
        _eqEncoderFocus = EQ_SLIDER_COUNT; // return focus to PRESETS
    } else if (_eqEncoderFocus < EQ_SLIDER_COUNT) {
        _eqEncoderEditing = !_eqEncoderEditing;
    } else if (_eqEncoderFocus == EQ_SLIDER_COUNT) {
        _eqPresetView = true;
        _eqEncoderEditing = false;
        _eqEncoderFocus = 0;
    } else {
        _eqClosePending = true;
        putRequest(CLOSEEQ);
        return;
    }
    putRequest(DRAWEQ);
}

void Display::_eqInlineShow() {
    // Lock: spektrum, óra, dátum, időjárás — ugyanaz mint VOL
    if (_footer) { _footer->setActive(false); }
    if (_spectrum)    { _spectrum->lock(true); }
    if (_volwidget)   { _volwidget->lock(true); }
    if (_bufferwidget){ _bufferwidget->lock(true); }
    if (_audioInfoText) { _audioInfoText->lock(true); }
    if (_clock)       { _clock->lock(true); }
    if (_datewidget)  { _datewidget->lock(true); }
    if (_weather)     { _weather->lock(true); }
    if (_weatherIcon) { _weatherIcon->lock(true); }
    // Sáv törlése
    dsp.fillRect(EQ_OVERLAY_LEFT, EQ_OVERLAY_TOP, EQ_OVERLAY_W, EQ_OVERLAY_H, config.theme.background);
    _eqInlineDraw();
}

void Display::_eqInlineHide() {
    // Clear first; unlocking an active widget redraws its cached content.
    dsp.fillRect(EQ_OVERLAY_LEFT, EQ_OVERLAY_TOP, EQ_OVERLAY_W, EQ_OVERLAY_H, config.theme.background);

    // Unlock
    if (_spectrum)    { _spectrum->lock(!config.store.vumeter); }
    if (_volwidget)   { _volwidget->lock(false); }
    if (_bufferwidget){ _bufferwidget->lock(!config.store.audioinfo); }
    if (_audioInfoText) { _audioInfoText->lock(false); }
    if (_clock)       { _clock->lock(false); }
    if (_datewidget)  { _datewidget->lock(false); }
    if (_weather)     { _weather->lock(!config.store.showweather); }
    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
    if (_footer && _mode == PLAYER) { _footer->setActive(true); }
}

void Display::_eqInlineDraw() {
    LGFX_Sprite canvas(&dsp);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(EQ_OVERLAY_W, EQ_OVERLAY_H);
    canvas.fillSprite(config.theme.background);

    const uint16_t bg   = config.theme.background;
    const uint16_t fg   = config.theme.status_active;
    const uint16_t dim  = config.theme.status_inactive;
    // Button focus needs a stronger outline than the larger slider rows.
    const auto drawButtonFocus = [&](int16_t x, int16_t y, int16_t w, int16_t h) {
        for (int16_t inset = 1; inset <= 3; ++inset) {
            canvas.drawRoundRect(x + inset, y + inset, w - 2 * inset,
                                 h - 2 * inset, 4, config.theme.digit);
        }
    };

    if (_eqPresetView) {
        // ── PRESET NÉZET ─────────────────────────────────────────────────────
        // 2 sor × 4 gomb, az egész sávban középre igazítva
        const uint8_t  COLS = 4, ROWS = 2;
        const uint16_t BTN_W = 100, BTN_H = 34, BTN_GAP_X = 8, BTN_GAP_Y = 8;
        const uint16_t gridW = COLS * BTN_W + (COLS - 1) * BTN_GAP_X;
        const uint16_t gridH = ROWS * BTN_H + (ROWS - 1) * BTN_GAP_Y;
        const uint16_t offX  = (EQ_OVERLAY_W - gridW) / 2;
        const uint16_t offY  = (EQ_OVERLAY_H - gridH) / 2;

        for (uint8_t i = 0; i < EQ_PRESET_COUNT; i++) {
            uint8_t  col  = i % COLS;
            uint8_t  row  = i / COLS;
            uint16_t bx   = offX + col * (BTN_W + BTN_GAP_X);
            uint16_t by   = offY + row * (BTN_H + BTN_GAP_Y);
            canvas.fillRoundRect(bx, by, BTN_W, BTN_H, 4, fg);
            canvas.drawRoundRect(bx, by, BTN_W, BTN_H, 4, bg);
            //canvas.setTextColor(fg, dim);
            canvas.setTextColor(bg, fg);
            canvas.setTextDatum(MC_DATUM);
            if (font_vlw_16) {
                canvas.loadFont(font_vlw_16);
                canvas.drawString(eqPresetNames[i], bx + BTN_W / 2, by + BTN_H / 2);
                canvas.unloadFont();
            } else {
                canvas.setTextSize(2);
                canvas.drawString(eqPresetNames[i], bx + BTN_W / 2, by + BTN_H / 2);
            }
            if (i == _eqEncoderFocus) {
                drawButtonFocus(bx, by, BTN_W, BTN_H);
            }
        }
        const uint16_t BACK_W = 58;
        const uint16_t BACK_H = 26;
        const uint16_t BACK_X = EQ_OVERLAY_W - BACK_W - 8;
        canvas.fillRoundRect(BACK_X, 4, BACK_W, BACK_H, 4, fg);
        canvas.setTextColor(bg, fg);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextSize(1);
        canvas.drawString("BACK", BACK_X + BACK_W / 2, 4 + BACK_H / 2);
        if (_eqEncoderFocus == EQ_PRESET_COUNT) {
            drawButtonFocus(BACK_X, 4, BACK_W, BACK_H);
        }
    } else {
        // ── SLIDER NÉZET ─────────────────────────────────────────────────────
        // 4 vízszintes slider: BASS / MID / TREB / BAL
        // bass/mid/treb: -6..+6 (WebUI alapján), balance: -16..+16
        // Elrendezés: 4 sor egyenletesen, jobb oldalon PRESETS gomb

        const int8_t vals[4] = {
            config.store.bass,
            config.store.middle,
            config.store.trebble,
            config.store.balance
        };
        const char* labels[4] = { "BAS", "MID", "TRB", "BAL" };
        // Per-slider max (szimmetrikus: -max..+max)
        const int8_t slMax[4] = { 6, 6, 6, 16 };

        // PRESETS and BACK share the right-hand button column.
        const uint16_t PRESETS_BTN_W = 70;
        const uint16_t PRESETS_BTN_H = (EQ_OVERLAY_H - 30) / 2;
        const uint16_t PRESETS_BTN_X = EQ_OVERLAY_W - PRESETS_BTN_W - 8;
        const uint16_t PRESETS_BTN_Y = 10;
        const uint16_t BACK_BTN_Y = PRESETS_BTN_Y + PRESETS_BTN_H + 10;
        canvas.fillRoundRect(PRESETS_BTN_X, PRESETS_BTN_Y, PRESETS_BTN_W, PRESETS_BTN_H, 4, fg);
        canvas.drawRoundRect(PRESETS_BTN_X, PRESETS_BTN_Y, PRESETS_BTN_W, PRESETS_BTN_H, 4, bg);
        canvas.setTextColor(bg, fg);
        canvas.setTextDatum(MC_DATUM);
        if (font_vlw_16) {
            canvas.loadFont(font_vlw_16);
            canvas.drawString("PRE", PRESETS_BTN_X + PRESETS_BTN_W / 2, PRESETS_BTN_Y + PRESETS_BTN_H / 2 - 9);
            canvas.drawString("SETS", PRESETS_BTN_X + PRESETS_BTN_W / 2, PRESETS_BTN_Y + PRESETS_BTN_H / 2 + 9);
            canvas.unloadFont();
        } else {
            canvas.setTextSize(1);
            canvas.drawString("PRE",  PRESETS_BTN_X + PRESETS_BTN_W / 2, PRESETS_BTN_Y + PRESETS_BTN_H / 2 - 8);
            canvas.drawString("SETS", PRESETS_BTN_X + PRESETS_BTN_W / 2, PRESETS_BTN_Y + PRESETS_BTN_H / 2 + 8);
        }
        canvas.fillRoundRect(PRESETS_BTN_X, BACK_BTN_Y, PRESETS_BTN_W, PRESETS_BTN_H, 4, fg);
        canvas.drawRoundRect(PRESETS_BTN_X, BACK_BTN_Y, PRESETS_BTN_W, PRESETS_BTN_H, 4, bg);
        canvas.setTextSize(1);
        canvas.drawString("BACK", PRESETS_BTN_X + PRESETS_BTN_W / 2,
                          BACK_BTN_Y + PRESETS_BTN_H / 2);
        if (_eqEncoderFocus == EQ_SLIDER_COUNT) {
            drawButtonFocus(PRESETS_BTN_X, PRESETS_BTN_Y,
                            PRESETS_BTN_W, PRESETS_BTN_H);
        } else if (_eqEncoderFocus == EQ_SLIDER_COUNT + 1) {
            drawButtonFocus(PRESETS_BTN_X, BACK_BTN_Y,
                            PRESETS_BTN_W, PRESETS_BTN_H);
        }

        // Slider terület (PRESETS gombtól balra)
        const uint16_t LABEL_W   = 36;
        const uint16_t VAL_W     = 22;
        const uint16_t SL_LEFT   = 8 + LABEL_W;
        const uint16_t SL_RIGHT  = PRESETS_BTN_X - 8 - VAL_W;
        const uint16_t SL_W      = SL_RIGHT - SL_LEFT;  // slider sáv szélesség
        const uint16_t ROW_H     = EQ_OVERLAY_H / 4;
        const uint8_t  SL_H      = 6;   // slider csík magasság
        const uint8_t  THUMB_W   = 10;
        const uint8_t  THUMB_H   = 18;

        for (uint8_t i = 0; i < 4; i++) {
            uint16_t rowY    = i * ROW_H;
            uint16_t centerY = rowY + ROW_H / 2;

            if (i == _eqEncoderFocus) {
                canvas.drawRoundRect(2, rowY + 2, PRESETS_BTN_X - 6,
                                     ROW_H - 4, 4, config.theme.digit);
                if (_eqEncoderEditing) {
                    canvas.drawRoundRect(4, rowY + 4, PRESETS_BTN_X - 10,
                                         ROW_H - 8, 4, config.theme.digit);
                }
            }

            // Felirat
            canvas.setTextColor(fg, bg);
            canvas.setTextDatum(ML_DATUM);
            if (font_vlw_16) {
                canvas.loadFont(font_vlw_16);
                canvas.drawString(labels[i], 8, centerY);
                canvas.unloadFont();
            } else {
                canvas.setTextSize(1);
                canvas.drawString(labels[i], 8, centerY);
            }

            // Slider háttér sáv (-6..+6 → 13 lépés)
            // Középvonal
            uint16_t midX = SL_LEFT + SL_W / 2;
            // Teljes sáv
            canvas.fillRect(SL_LEFT, centerY - SL_H / 2, SL_W, SL_H, dim);
            // Középjelző
            canvas.fillRect(midX - 1, centerY - SL_H / 2 - 2, 2, SL_H + 4, fg);

            // Aktív rész (0 ponttól a thumb felé)
            int8_t   v    = vals[i];
            int8_t   vmax = slMax[i];   // -vmax..+vmax
            // thumb X: map -vmax..+vmax → SL_LEFT..SL_LEFT+SL_W-THUMB_W
            int32_t  range = 2 * vmax;
            uint16_t thumbX = SL_LEFT + (uint16_t)((v + vmax) * (SL_W - THUMB_W) / range);
            // Aktív sáv
            if (v > 0) {
                canvas.fillRect(midX, centerY - SL_H / 2, thumbX + THUMB_W / 2 - midX, SL_H, fg);
            } else if (v < 0) {
                uint16_t fillW = midX - (thumbX + THUMB_W / 2);
                if (fillW > 0) canvas.fillRect(thumbX + THUMB_W / 2, centerY - SL_H / 2, fillW, SL_H, fg);
            }

            // Thumb
            canvas.fillRoundRect(thumbX, centerY - THUMB_H / 2, THUMB_W, THUMB_H, 3, fg);

            // Érték szám jobb oldalon
            char vbuf[6];
            snprintf(vbuf, sizeof(vbuf), "%+d", (int)v);
            canvas.setTextColor(fg, bg);
            canvas.setTextDatum(MR_DATUM);
            if (font_vlw_16) {
                canvas.loadFont(font_vlw_16);
                canvas.drawString(vbuf, SL_RIGHT + VAL_W, centerY);
                canvas.unloadFont();
            } else {
                canvas.setTextSize(1);
                canvas.drawString(vbuf, SL_RIGHT + VAL_W, centerY);
            }
        }
    }

    canvas.pushSprite(EQ_OVERLAY_LEFT, EQ_OVERLAY_TOP);
    canvas.deleteSprite();
}

// ── EQ touch helper (touchscreen.cpp hívja) ──────────────────────────────────
// Visszaadja hogy az (x,y) koordináta az EQ overlay melyik elemét érinti.
// Csak ha az EQ overlay nyitva van és nem preset nézetben.
// preset nézet: visszaadja a preset indexet (0..7) vagy -1
// slider nézet: módosítja az EQ értéket és újrarajzol
// PRESETS gomb: átváltja a preset nézetet

void display_eq_touch(uint16_t x, uint16_t y, bool drag) {
    // Csak PLAYER módban és ha EQ overlay nyitva van
    if (!display.isEqOpen()) return;

    // Canvas-on belüli koordináták
    int16_t cx = (int16_t)x - EQ_OVERLAY_LEFT;
    int16_t cy = (int16_t)y - EQ_OVERLAY_TOP;
    if (cx < 0 || cy < 0 || cx >= EQ_OVERLAY_W || cy >= EQ_OVERLAY_H) return;

    if (_eqPresetView) {
        // Preset gomb hittest (csak tap, nem drag)
        if (drag) return;
        if (cx >= EQ_OVERLAY_W - 66 && cx < EQ_OVERLAY_W - 8 &&
            cy >= 4 && cy < 30) {
            _eqPresetView = false;
            _eqEncoderFocus = EQ_SLIDER_COUNT;
            display._eqInlineDraw();
            return;
        }
        const uint8_t  COLS = 4;
        const uint16_t BTN_W = 100, BTN_H = 34, BTN_GAP_X = 8, BTN_GAP_Y = 8;
        const uint16_t gridW = COLS * BTN_W + (COLS - 1) * BTN_GAP_X;
        const uint16_t gridH = 2 * BTN_H + BTN_GAP_Y;
        const uint16_t offX  = (EQ_OVERLAY_W - gridW) / 2;
        const uint16_t offY  = (EQ_OVERLAY_H - gridH) / 2;

        for (uint8_t i = 0; i < EQ_PRESET_COUNT; i++) {
            uint8_t  col = i % COLS;
            uint8_t  row = i / COLS;
            uint16_t bx  = offX + col * (BTN_W + BTN_GAP_X);
            uint16_t by  = offY + row * (BTN_H + BTN_GAP_Y);
            if (cx >= bx && cx < bx + BTN_W && cy >= by && cy < by + BTN_H) {
                // Preset alkalmazása
                config.setTone(eqPresetValues[i][0], eqPresetValues[i][1], eqPresetValues[i][2]);
                _eqPresetView = false;
                _eqEncoderFocus = EQ_SLIDER_COUNT;
                // Újrarajzol slider nézetbe
                dsp.fillRect(EQ_OVERLAY_LEFT, EQ_OVERLAY_TOP, EQ_OVERLAY_W, EQ_OVERLAY_H, config.theme.background);
                display._eqInlineDraw();
                return;
            }
        }
        return;
    }

    // Slider nézetben: PRESETS és BACK gombok
    const uint16_t PRESETS_BTN_W = 70;
    const uint16_t PRESETS_BTN_H = (EQ_OVERLAY_H - 30) / 2;
    const uint16_t PRESETS_BTN_X = EQ_OVERLAY_W - PRESETS_BTN_W - 8;
    const uint16_t PRESETS_BTN_Y = 10;
    const uint16_t BACK_BTN_Y = PRESETS_BTN_Y + PRESETS_BTN_H + 10;
    if (!drag && cx >= PRESETS_BTN_X && cx < PRESETS_BTN_X + PRESETS_BTN_W &&
                 cy >= PRESETS_BTN_Y && cy < PRESETS_BTN_Y + PRESETS_BTN_H) {
        _eqPresetView = true;
        _eqEncoderFocus = 0;
        _eqEncoderEditing = false;
        dsp.fillRect(EQ_OVERLAY_LEFT, EQ_OVERLAY_TOP, EQ_OVERLAY_W, EQ_OVERLAY_H, config.theme.background);
        display._eqInlineDraw();
        return;
    }
    if (!drag && cx >= PRESETS_BTN_X && cx < PRESETS_BTN_X + PRESETS_BTN_W &&
                 cy >= BACK_BTN_Y && cy < BACK_BTN_Y + PRESETS_BTN_H) {
        display.eqForceClose();
        return;
    }

    // Slider hittest
    const uint16_t LABEL_W   = 36;
    const uint16_t VAL_W     = 22;
    const uint16_t SL_LEFT   = 8 + LABEL_W;
    const uint16_t SL_RIGHT  = PRESETS_BTN_X - 8 - VAL_W;
    const uint16_t SL_W      = SL_RIGHT - SL_LEFT;
    const uint16_t ROW_H     = EQ_OVERLAY_H / 4;
    const uint8_t  THUMB_W   = 10;

    if (cx >= SL_LEFT && cx < SL_RIGHT + VAL_W) {
        uint8_t sliderIdx = cy / ROW_H;
        if (sliderIdx > 3) sliderIdx = 3;
        _eqEncoderFocus = sliderIdx;
        _eqEncoderEditing = false;

        // Per-slider tartomány
        const int8_t slMax[4] = { 6, 6, 6, 16 };
        int8_t vmax = slMax[sliderIdx];

        // Érték számítása: cx → -vmax..+vmax
        int16_t raw = (int16_t)(cx - SL_LEFT);
        if (raw < 0) raw = 0;
        if (raw > (int16_t)(SL_W - THUMB_W)) raw = SL_W - THUMB_W;
        int8_t newVal = (int8_t)map(raw, 0, (int16_t)(SL_W - THUMB_W), -vmax, vmax);

        bool changed = false;
        switch (sliderIdx) {
            case 0:
                if (newVal != config.store.bass) {
                    config.setTone(newVal, config.store.middle, config.store.trebble);
                    changed = true;
                }
                break;
            case 1:
                if (newVal != config.store.middle) {
                    config.setTone(config.store.bass, newVal, config.store.trebble);
                    changed = true;
                }
                break;
            case 2:
                if (newVal != config.store.trebble) {
                    config.setTone(config.store.bass, config.store.middle, newVal);
                    changed = true;
                }
                break;
            case 3:
                if (newVal != config.store.balance) {
                    config.setBalance(newVal);
                    changed = true;
                }
                break;
        }
        if (changed) {
            display._eqInlineDraw();
        }
    }
}

void Display::flip() {
    DisplayMutexGuard guard(portMAX_DELAY);
    dsp.flip();
}

void Display::invert() {
    DisplayMutexGuard guard(portMAX_DELAY);
    dsp.invert();
}

void Display::setContrast() {}

void Display::i2sReconfigBegin() {
#if DSP_MODEL == DSP_AXS15231B
    return;
#else
    if (displayMutex != nullptr) { xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); }
    // Do not open an SPI transaction here. We only need to ensure that an
    // in-flight display DMA transfer is finished and that no new frame starts
    // while the ESP32-S3 I2S channel is being reconfigured.
    dsp.waitDMA();
#endif
}

void Display::i2sReconfigEnd() {
#if DSP_MODEL != DSP_AXS15231B
    if (displayMutex != nullptr) { xSemaphoreGiveRecursive(displayMutex); }
#endif
}

void Display::waitDMA() {
#if DSP_MODEL == DSP_AXS15231B
    return;
#else
    DisplayMutexGuard guard(portMAX_DELAY);
    // Megvárja az LGFX háttérben futó DMA transzfer befejezését.
    // SD kártya SPI olvasás előtt kell hívni, hogy a megosztott buszon
    // ne legyen aktív DMA transzfer (csíkozás / kép szétesés megelőzése).
    dsp.waitDMA();
#endif
}

bool Display::deepsleep() {
#    if defined(DSP_OLED) || BRIGHTNESS_PIN != 255
    DisplayMutexGuard guard(portMAX_DELAY);
    if (!_panelAwake) { return true; }
#if DSP_MODEL != DSP_AXS15231B
    dsp.waitDMA();
#endif
    dsp.sleep();
    _panelAwake = false;
    log_i("##[PANEL]# sleep core=%u", (unsigned)xPortGetCoreID());
    return true;
#    endif
    return false;
}

void Display::wakeup() {
#    if defined(DSP_OLED) || BRIGHTNESS_PIN != 255
    DisplayMutexGuard guard(portMAX_DELAY);
    if (_panelAwake) { return; }
    dsp.wake();
    dsp.flip();
    dsp.invert();
    _panelAwake = true;
    log_i("##[PANEL]# wake core=%u", (unsigned)xPortGetCoreID());
#    endif
}

void Display::setBrightnessPercent(uint8_t percent) {
    percent = constrain(percent, 0, 100);
#    if DSP_MODEL == DSP_SSD1322
    uint8_t master = map(percent, 0, 100, 0, 15);
    uint8_t contrast = map(percent, 0, 100, 0, 255);
    dsp.ssd1322_setMasterContrast(master);
    dsp.ssd1322_setContrast(contrast);
#    else
#        if (BRIGHTNESS_PIN != 255)
    analogWrite(BRIGHTNESS_PIN, map(percent, 0, 100, 0, 255));
#        endif
#    endif
}

uint8_t Display::effectiveBrightnessPercent(uint8_t normalPercent) const {
    normalPercent = constrain(normalPercent, 0, 100);
    if (!_clockScreensaverBrightnessActive) return normalPercent;

    const uint8_t clockPercent = constrain(config.store.screensaverIdleBrightness, 0, 100);
#    if LIGHT_SENSOR != 255
    return static_cast<uint8_t>((static_cast<uint16_t>(normalPercent) * clockPercent + 50U) / 100U);
#    else
    return clockPercent;
#    endif
}

#    ifdef NAMEDAYS_FILE
void Display::loopDate(bool force) {
    if (_datewidget)   { _datewidget->draw(force); }
}
#    endif
//============================================================================================================================
#else // !DUMMYDISPLAY
//============================================================================================================================
void Display::init() {
    _createDspTask();
#    ifdef USE_NEXTION
    nextion.begin(true);
#    endif
}
void Display::_start() {
#    ifdef USE_NEXTION
    nextion.start();
#    endif
    config.setTitle(LANG::const_PlReady);
}

void Display::putRequest(displayRequestType_e type, int payload) {
    if (type == DSP_START) { _start(); }
#    ifdef USE_NEXTION
    requestParams_t request;
    request.type = type;
    request.payload = payload;
    request.generation = 0;
    nextion.putRequest(request);
#    else
    if (type == NEWMODE) { mode((displayMode_e)payload); }
#    endif
}
//============================================================================================================================
#endif // DUMMYDISPLAY

#ifndef DUMMYDISPLAY
void display_show_maintenance_screen() {
    DisplayMutexGuard guard(portMAX_DELAY);
    dsp.initDisplay();
    dsp.wake();
#    if BRIGHTNESS_PIN != 255
    pinMode(BRIGHTNESS_PIN, OUTPUT);
    analogWrite(BRIGHTNESS_PIN, map(100, 0, 100, 0, 255));
#    endif
    dsp.fillScreen(0x0000);
    uint16_t cx = dsp.width() / 2;
    uint16_t cy = dsp.height() / 2;
    dsp.setTextSize(2);
    dsp.setTextDatum(datum_t::middle_center);
    dsp.setTextColor(0xFFFF, 0x0000);
    dsp.drawString("LittleFS Serial Service", cx, cy - 20);
    dsp.drawString("Maintenance mode active", cx, cy + 14);
}
#else
void display_show_maintenance_screen() {}
#endif

// =====================================================================
//  MODESELECT — mód-választó menü
//  A STATIONS playlist mintájára: az alsó, y=137..képernyőszél sávba rajzol,
//  a spektrum/óra/dátum/időjárás widgetek lockolva vannak.
//  Aktiválás: display.modeSelectorOpen()
//  Enkóder forgat: modeSelectorScroll(±1)
//  Megerősítés: modeSelectorConfirm()
//  Auto-bezárás 5 másodperc inaktivitás után.
// =====================================================================

#if DSP_MODEL != DSP_DUMMY

#define MS_AUTO_CLOSE_MS 5000

// --- Elérhető módok listájának összeállítása -----------------------
void Display::_msBuildList() {
    _msCount = 0;
    _msValues[_msCount] = PM_WEB;       _msLabels[_msCount++] = "WEB";
#ifdef USE_SD
    if (SDC_CS != 255) { _msValues[_msCount] = PM_SDCARD; _msLabels[_msCount++] = "SD"; }
#endif
#ifdef USE_DLNA
    _msValues[_msCount] = -1;           _msLabels[_msCount++] = "DLNA";
#endif
#ifdef USE_BLUETOOTH
    _msValues[_msCount] = PM_BLUETOOTH; _msLabels[_msCount++] = "BT";
#endif

    // Aktuális mód kijelölése
    int8_t curVal = (config.getMode() == PM_WEB &&
                     config.store.playlistSource == (uint8_t)PL_SRC_DLNA)
                    ? -1 : (int8_t)config.getMode();
    _msIdx = 0;
    for (uint8_t i = 0; i < _msCount; i++) {
        if (_msValues[i] == curVal) { _msIdx = i; break; }
    }
}

// --- Módlista kirajzolása a VOL_AREA sávba -------------------------
// Mintája: _drawPlaylist() — direkt dsp hívások, nincs sprite overlay
void Display::_msDraw() {
    if (_mode != MODESELECT) return;

    const uint16_t bg   = config.theme.background;
    const uint16_t fg   = config.theme.meta;
    const uint16_t sel  = config.theme.digit;
    const uint16_t brd  = config.theme.pmode;

    // Sáv törlése
    dsp.fillRect(0, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT, bg);

    // Fejléc — "SELECT MODE"
    dsp.setTextDatum(TC_DATUM);
    dsp.setTextColor(fg, bg);
    if (font_vlw_16) {
        dsp.loadFont(font_vlw_16);
        dsp.drawString("SELECT MODE", VOL_AREA_WIDTH / 2, VOL_AREA_TOP + 6);
        dsp.unloadFont();
    } else {
        dsp.setTextSize(2);
        dsp.drawString("SELECT MODE", VOL_AREA_WIDTH / 2, VOL_AREA_TOP + 6);
    }

    // Elválasztó vonal a fejléc alatt
    dsp.drawFastHLine(20, VOL_AREA_TOP + 28, VOL_AREA_WIDTH - 40, brd);

    // Módok listája — függőlegesen elosztva a maradék helyen
    const uint16_t listTop  = VOL_AREA_TOP + 35;
    const uint16_t listH    = VOL_AREA_HEIGHT - 40;
    const uint16_t itemH    = listH / _msCount;

    // Aktuális mód meghatározása az aktív jelölőhöz
    int8_t curVal = (config.getMode() == PM_WEB &&
                     config.store.playlistSource == (uint8_t)PL_SRC_DLNA)
                    ? -1 : (int8_t)config.getMode();

    for (uint8_t i = 0; i < _msCount; i++) {
        uint16_t y    = listTop + i * itemH;
        bool  isSelected = (i == _msIdx);
        bool  isActive   = (_msValues[i] == curVal);

        // Kijelölt elem háttere
        if (isSelected) {
            dsp.fillRoundRect(10, y, VOL_AREA_WIDTH - 20, itemH - 2, 4, sel);
            dsp.setTextColor(bg, sel);
        } else {
            dsp.setTextColor(fg, bg);
        }

        // Módnév
        dsp.setTextDatum(ML_DATUM);
        if (font_vlw_20) {
            dsp.loadFont(font_vlw_20);
            dsp.drawString(_msLabels[i], 28, y + itemH / 2);
            dsp.unloadFont();
        } else {
            dsp.setTextSize(2);
            dsp.drawString(_msLabels[i], 28, y + itemH / 2);
        }

        // Aktív mód jelölő (kis kör a jobb oldalon)
        if (isActive) {
            uint16_t dotColor = isSelected ? bg : sel;
            dsp.fillCircle(VOL_AREA_WIDTH - 22, y + itemH / 2, 5, dotColor);
        }
    }
}

// --- Megnyitás (FŐTASK-BÓL HÍVHATÓ) -------------------------------
void Display::modeSelectorOpen() {
    if (_mode == MODESELECT) return;
    _msReturnMode = _mode == QUICKSETTINGS ? QUICKSETTINGS : PLAYER;
    _msAutoCloseMs = millis() + MS_AUTO_CLOSE_MS;
    putRequest(NEWMODE, MODESELECT); // display task: widget lock + _msBuildList + _msDraw
}

// --- Görgetés (FŐTASK-BÓL HÍVHATÓ) --------------------------------
void Display::modeSelectorScroll(int dir) {
    if (_mode != MODESELECT) return;
    if (_msCount == 0) return;
    _msAutoCloseMs = millis() + MS_AUTO_CLOSE_MS;
    if (dir > 0) { _msIdx = (_msIdx + 1) % _msCount; }
    else         { _msIdx = (_msIdx + _msCount - 1) % _msCount; }
    putRequest(DRAWMODESELECT); // display task: _msDraw()
}

// --- Megerősítés ---------------------------------------------------
// Touch esetén a DspTask hívja, miközben a display mutex még fogva van.
// A teljes changeMode() (különösen az SD indítása/indexelése) ezért csak
// halasztott NetServer-kérésből, a display mutex elengedése után futhat.
void Display::modeSelectorConfirm() {
    if (_mode != MODESELECT) return;
    int8_t val = _msValues[_msIdx];
    purgeQueuedRequestType(DRAWMODESELECT);
    putRequest(NEWMODE, PLAYER); // bezárás
    if (val == -1) {
        // DLNA is an explicit selector entry, not a request to advance the
        // mode cycle. This is especially important when leaving Bluetooth
        // input: retain DLNA as the requested destination across the module's
        // sink -> source transition.
#ifdef USE_DLNA
        config.newConfigMode = PM_WEB;
        config.newConfigPlaylistSource = PL_SRC_DLNA;
        netserver.requestOnChange(CHANGEMODE, 0);
#endif
    } else {
#ifdef USE_DLNA
        // DLNA is represented as PM_WEB + PL_SRC_DLNA. Selecting WEB must
        // therefore switch the logical playlist source as well as the mode.
        config.newConfigPlaylistSource =
            val == PM_WEB ? (int)PL_SRC_WEB : -1;
#endif
        config.newConfigMode = val;
        netserver.requestOnChange(CHANGEMODE, 0);
    }
}

// --- Bezárás módváltás nélkül (FŐTASK-BÓL HÍVHATÓ) ---------------
void Display::modeSelectorClose() {
    if (_mode != MODESELECT) return;
    purgeQueuedRequestType(DRAWMODESELECT);
    putRequest(NEWMODE, _msReturnMode);
}

// --- Auto-bezárás (DISPLAY LOOP()-BÓL — display task kontextus) ---
void Display::modeSelectorTick() {
    if (_mode != MODESELECT) return;
    if (millis() < _msAutoCloseMs) return;
    // _mode = PLAYER-t NEM állítjuk itt közvetlenül — ha megtennénk,
    // akkor putRequest(NEWMODE, PLAYER) feldolgozásakor _swichMode() azt
    // látná hogy prevMode = PLAYER (nem MODESELECT), és a MODESELECT
    // cleanup blokk (widget unlock + teljes player refresh) nem futna le
    // → maradna a "kosz" a képernyőn.
    // A _swichMode(PLAYER) fogja a prevMode=MODESELECT ágban kezelni az
    // unlock-ot és a teljes player nézet újrarajzolását.
//    if (_spectrum)    { _spectrum->lock(!config.store.vumeter); }
//    if (_clock)       { _clock->lock(false); }
//    if (_datewidget)  { _datewidget->lock(false); }
//    if (_weather)     { _weather->lock(!config.store.showweather); }
//    if (_weatherIcon) { _weatherIcon->lock(!config.store.showweather); }
//    _mode = PLAYER;
    resetQueue();
    putRequest(NEWMODE, _msReturnMode);
    if (_msReturnMode == PLAYER) {
        putRequest(NEWSTATION);
        putRequest(DBITRATE);
    }
}

// =====================================================================
//  SCREENSELECT — while-playing screensaver: Off / CoverArt / Cassette / VU
// =====================================================================

#define SS_AUTO_CLOSE_MS 5000

void Display::_ssDraw() {
    if (_mode != SCREENSELECT) return;

    const uint16_t bg  = config.theme.background;
    const uint16_t fg  = config.theme.meta;
    const uint16_t sel = config.theme.digit;
    const uint16_t brd = config.theme.pmode;
    static const char* const labels[4] = {"OFF", "COVERART", "CASSETTE", "ANALOG VU"};

    dsp.fillRect(0, VOL_AREA_TOP, VOL_AREA_WIDTH, VOL_AREA_HEIGHT, bg);
    dsp.setTextDatum(TC_DATUM);
    dsp.setTextColor(fg, bg);
    if (font_vlw_16) {
        dsp.loadFont(font_vlw_16);
        dsp.drawString("WHILE PLAYING", VOL_AREA_WIDTH / 2,
                       VOL_AREA_TOP + 6);
        dsp.unloadFont();
    } else {
        dsp.setTextSize(2);
        dsp.drawString("WHILE PLAYING", VOL_AREA_WIDTH / 2,
                       VOL_AREA_TOP + 6);
    }
    dsp.drawFastHLine(20, VOL_AREA_TOP + 28, VOL_AREA_WIDTH - 40, brd);

    const uint16_t listTop = VOL_AREA_TOP + 35;
    const uint16_t itemH = (VOL_AREA_HEIGHT - 40) / 4;
    const uint8_t active = !config.store.screensaverPlayingEnabled
                               ? 0
                               : (config.store.screensaverMode ==
                                          SCREENSAVER_CASSETTE
                                      ? 2
                                      : (config.store.screensaverMode ==
                                                 SCREENSAVER_VUMETER
                                             ? 3 : 1));
    for (uint8_t i = 0; i < 4; ++i) {
        const uint16_t y = listTop + i * itemH;
        const bool selected = i == _ssIdx;
        if (selected) {
            dsp.fillRoundRect(10, y, VOL_AREA_WIDTH - 20, itemH - 2, 4, sel);
            dsp.setTextColor(bg, sel);
        } else {
            dsp.setTextColor(fg, bg);
        }
        dsp.setTextDatum(ML_DATUM);
        if (font_vlw_16) {
            dsp.loadFont(font_vlw_16);
            dsp.drawString(labels[i], 28, y + itemH / 2);
            dsp.unloadFont();
        } else {
            dsp.setTextSize(2);
            dsp.drawString(labels[i], 28, y + itemH / 2);
        }
        if (i == active) {
            dsp.fillCircle(VOL_AREA_WIDTH - 22, y + itemH / 2, 5,
                           selected ? bg : sel);
        }
    }
}

void Display::screensaverSelectorOpen() {
    if (_mode == SCREENSELECT) return;
    _ssAutoCloseMs = millis() + SS_AUTO_CLOSE_MS;
    putRequest(NEWMODE, SCREENSELECT);
}

void Display::screensaverSelectorScroll(int dir) {
    if (_mode != SCREENSELECT || dir == 0) return;
    _ssAutoCloseMs = millis() + SS_AUTO_CLOSE_MS;
    if (dir > 0) {
        _ssIdx = (_ssIdx + 1U) % 4U;
    } else {
        _ssIdx = (_ssIdx + 3U) % 4U;
    }
    putRequest(DRAWSCREENSELECT);
}

void Display::screensaverSelectorConfirm() {
    if (_mode != SCREENSELECT) return;
    if (_ssIdx == 0) {
        config.saveValue(&config.store.screensaverPlayingEnabled, false);
    } else {
        const uint8_t mode = _ssIdx == 2 ? SCREENSAVER_CASSETTE
                           : _ssIdx == 3 ? SCREENSAVER_VUMETER
                                         : SCREENSAVER_COVERART;
        config.setScreensaverMode(mode);
        config.saveValue(&config.store.screensaverPlayingEnabled, true);
    }
    purgeQueuedRequestType(DRAWSCREENSELECT);
    // A valasztas utan visszaterunk a Settings menube; annak sajat 5
    // masodperces inaktivitas-időzítője dont a tenyleges kilepesrol.
    putRequest(NEWMODE, QUICKSETTINGS);
}

void Display::screensaverSelectorClose() {
    if (_mode != SCREENSELECT) return;
    purgeQueuedRequestType(DRAWSCREENSELECT);
    putRequest(NEWMODE, QUICKSETTINGS);
}

void Display::screensaverSelectorTick() {
    if (_mode != SCREENSELECT || millis() < _ssAutoCloseMs) return;
    resetQueue();
    putRequest(NEWMODE, QUICKSETTINGS);
}

#else // DSP_DUMMY stubs
void Display::modeSelectorOpen()         {}
void Display::modeSelectorScroll(int)    {}
void Display::modeSelectorConfirm()      {}
void Display::modeSelectorClose()        {}
void Display::modeSelectorTick()         {}
void Display::screensaverSelectorOpen()  {}
void Display::screensaverSelectorScroll(int) {}
void Display::screensaverSelectorConfirm() {}
void Display::screensaverSelectorClose() {}
void Display::screensaverSelectorTick()  {}
#endif
