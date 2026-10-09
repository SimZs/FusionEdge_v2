#include "options.h"
#if (TS_MODEL != TS_MODEL_UNDEFINED) && (DSP_MODEL != DSP_DUMMY)
#    include "Arduino.h"
#    include "touchscreen.h"
#    include "config.h"
#    include "controls.h"
#    include "display.h"
#    include "player.h"
#    include "presets.h"
#    include "../displays/display_select.h" // DspCore + extern dsp
#    include "../pluginsManager/pluginsManager.h"
#    include "../plugins/backlight/backlight.h"
#    include "commandhandler.h"
#    include "fonts.h"
#    include "netserver.h"

#    ifndef TS_STEPS
#        define TS_STEPS 40
#    endif

#    ifndef TS_AXIS_LOCK_MARGIN
#        define TS_AXIS_LOCK_MARGIN 8
#    endif

// ── Widget területek (conf_480x320.h konstansokkal szinkronban) ──────────────
// A station preset csak a META es Title1 szovegsorokrol nyithato meg.
// A koordinatak a conf_480x320.h metaConf/title1Conf teruleteit kovetik.
#    define SW_PRESETS_L        (TFT_FRAMEWDT + 124)
#    define SW_PRESETS_R        (DSP_WIDTH - TFT_FRAMEWDT)
#    define SW_META_T            0
#    define SW_META_B           36
#    define SW_TITLE1_T         38
#    define SW_TITLE1_B         70
// A presets kepernyo fejlece: presets.cpp TOP_H = 40.
#    define SW_PRESETS_HEADER_B  40

// A 120x120-as album-/allomaslogo sajat terulete a player bal felso sarkaban.
#    define SW_COVER_L           TFT_FRAMEWDT
#    define SW_COVER_R          (TFT_FRAMEWDT + 120)
#    define SW_COVER_T            7
#    define SW_COVER_B          (SW_COVER_T + 120)

// ClockWidget: a jobb szeli hangeroskala melle rendezve.
#    define SW_CLOCK_L    261
#    define SW_CLOCK_R    435
#    define SW_CLOCK_T    157
#    define SW_CLOCK_B    263

// SpectrumWidget: teljes szelessegben a lapos bufferindikator felett.
#    define SW_SPEC_L     TFT_FRAMEWDT
#    define SW_SPEC_R    (SW_SPEC_L + PLAYER_SPECTRUM_WIDTH)
#    define SW_SPEC_T    160
#    define SW_SPEC_B    230

// A Title2 alatti, lathato transportgombok pontos erintesi teruletei.
#    define SW_TRANSPORT_T       105
#    define SW_TRANSPORT_B       130
#    define SW_TRANSPORT_PREV_L  PLAYER_TRANSPORT_LEFT
#    define SW_TRANSPORT_PREV_R  (SW_TRANSPORT_PREV_L + PLAYER_TRANSPORT_BUTTON_WIDTH)
#    define SW_TRANSPORT_PLAY_L  (SW_TRANSPORT_PREV_R + PLAYER_TRANSPORT_BUTTON_GAP)
#    define SW_TRANSPORT_PLAY_R  (SW_TRANSPORT_PLAY_L + PLAYER_TRANSPORT_BUTTON_WIDTH)
#    define SW_TRANSPORT_NEXT_L  (SW_TRANSPORT_PLAY_R + PLAYER_TRANSPORT_BUTTON_GAP)
#    define SW_TRANSPORT_NEXT_R  PLAYER_TRANSPORT_RIGHT

// VOL/Playlist swipe zóna – a VOL_AREA_TOP-tól lefelé
// (VOL_AREA_TOP = 137, def. display.cpp-ben)
#    define SW_VOL_T      137

// A jobb oldali fuggoleges VolumeWidget. A valodi widget 448..470 px,
// de az ujj szamara kissé szelesebb, a weather ikonnal nem atfedo savot adunk.
#    define SW_VOLUME_L         440
#    define SW_VOLUME_R         DSP_WIDTH
#    define SW_VOLUME_T         142
#    define SW_VOLUME_B         264
#    define SW_VOLUME_LEVEL_T   152
#    define SW_VOLUME_LEVEL_B   235

// Wi-Fi informacios zona: a csatornaszam jobb szeletol a kijelzo szeleig.
// Maga az ikon 4 pixellel beljebb kerult, de a teljes szelso sav erintheto.
#    define SW_WIFI_L     415
#    define SW_WIFI_R     DSP_WIDTH
#    define SW_WIFI_T     271
#    define SW_WIFI_B     313

// Bluetooth informacios zona: a kijelzo bal szeletol a playmode widgetig.
// Maga az ikon 4 pixellel beljebb kerult, de a teljes szelso sav erintheto.
#    define SW_BT_L       0
#    define SW_BT_R       65
#    define SW_BT_T       271
#    define SW_BT_B       313

// Kodbol rajzolt beallitasgomb a footer kozepen.
#    define SW_GEAR_L     205
#    define SW_GEAR_R     275
#    define SW_GEAR_T     288
#    define SW_GEAR_B     313

// A ket szoveges footer widget sajat, egymast nem atfedo touch-zonaja.
#    define SW_PLAYMODE_L  65
#    define SW_PLAYMODE_R  175
#    define SW_PLAYMODE_T  288
#    define SW_PLAYMODE_B  313
#    define SW_STATION_L   305
#    define SW_STATION_R   415
#    define SW_STATION_T   288
#    define SW_STATION_B   313

// WeatherIconWidget: 64x64 px a player jobb also reszen.
#    define SW_WEATHER_L  370
#    define SW_WEATHER_R  (SW_WEATHER_L + 64)
#    define SW_WEATHER_T  205
#    define SW_WEATHER_B  269

void TouchScreen::init(uint16_t w, uint16_t h) {
    // A LovyanGFX a dsp.init() során már inicializálta a touch IC-t a buszon.
    _width = w;
    _height = h;
}

tsDirection_e TouchScreen::_tsDirection(uint16_t x, uint16_t y) {
    int16_t dX = x - _oldTouchX;
    int16_t dY = y - _oldTouchY;
    // Az eredeti üzleti logika szerinti 20 pixeles küszöb
    if (abs(dX) > 20 || abs(dY) > 20) {
        if (abs(dX) > abs(dY)) {
            return (dX > 0) ? TSD_RIGHT : TSD_LEFT;
        } else {
            return (dY > 0) ? TSD_DOWN : TSD_UP;
        }
    }
    return TDS_REQUEST;
}

// ── Segédfüggvények ──────────────────────────────────────────────────────────

// Megadja hogy x,y beleesik-e a [left..right) × [top..bottom) téglalapba
static inline bool _inRect(uint16_t x, uint16_t y,
                            uint16_t left, uint16_t right,
                            uint16_t top,  uint16_t bottom) {
    return (x >= left && x < right && y >= top && y < bottom);
}

// A fuggoleges hangeroskala teteje 21, az alja 0. A widget also hangszoro
// ikonja is a 0-s szinthez tartozik, ezert a teljes touch-sav hasznalhato.
static uint8_t _touchVolumeFromY(uint16_t y) {
    const int32_t clampedY = constrain(static_cast<int32_t>(y),
                                       static_cast<int32_t>(SW_VOLUME_LEVEL_T),
                                       static_cast<int32_t>(SW_VOLUME_LEVEL_B));
    const int32_t span = SW_VOLUME_LEVEL_B - SW_VOLUME_LEVEL_T;
    const int32_t value = ((SW_VOLUME_LEVEL_B - clampedY) * 21 + span / 2) / span;
    return static_cast<uint8_t>(constrain(value, 0L, 21L));
}

// Ciklikusan lépteti a clockFontStyle-t (0→1→2→3→4→5→6→0)
static void _touchCycleClockFont() {
    uint8_t style = config.store.clockFontStyle;
    style = (style + 1) % (CLOCKFONT_STYLE_SQUAREFONT + 1);
    char val[4];
    snprintf(val, sizeof(val), "%d", (int)style);
    cmd.exec("clockfont", val);
}

// Ciklikusan lépteti a vuSpecMode-ot (0→1→2→3→0)
static void _touchCycleVuStyle() {
    uint8_t mode = config.store.vuSpecMode;
    mode = (mode + 1) % 4;
    char val[4];
    snprintf(val, sizeof(val), "%d", (int)mode);
    cmd.exec("vustyle", val);
}

// vuPeakOn toggle
static void _touchToggleVuPeak() {
    uint8_t newVal = config.store.vuPeakOn ? 0 : 1;
    char val[4];
    snprintf(val, sizeof(val), "%d", (int)newVal);
    cmd.exec("vupeak", val);
}

enum touchTransportAction_e : uint8_t {
    TOUCH_TRANSPORT_PREVIOUS = 0,
    TOUCH_TRANSPORT_TOGGLE,
    TOUCH_TRANSPORT_NEXT,
};

static void _touchDispatchTransport(touchTransportAction_e action) {
    bool consumedByPlugin = false;
#    if (BRIGHTNESS_PIN != 255)
    if (config.store.fadeEnabled && backlightPlugin.isFadeControl()) {
        consumedByPlugin = true;
    }
#    endif
    if (!consumedByPlugin) {
        pm.on_user_activity(consumedByPlugin);
    }
    if (consumedByPlugin) {
        return; // az első érintés csak a fényt hozza vissza
    }

    if (action == TOUCH_TRANSPORT_TOGGLE) {
        // A közös gombútvonal az encoderrel azonos plugin- és activity-
        // kezelést biztosítja.
        onBtnClick(EVT_BTNCENTER);
        return;
    }

    // A PREV/NEXT nem valódi nyomógomb-esemény, ezért itt tartjuk ébren a
    // képernyőt, majd a közös transport routerre bízzuk az aktív lejátszót.
    config.screensaverTicks = 0;
    config.screensaverPlayingTicks = 0;
    if (action == TOUCH_TRANSPORT_PREVIOUS) {
        controlsPlaybackPrev();
    } else {
        controlsPlaybackNext();
    }
}

// PlayMode ciklikus váltás:
//   USE_SD + USE_DLNA:  WEB → SD → DLNA → WEB → ...
//   USE_SD csak:        WEB → SD → WEB → ...
//   Egyik sem:         nincs hatás
static void __attribute__((unused)) _touchCyclePlayMode() {
#    if defined(USE_SD) || defined(USE_DLNA)

    // Aktuális logikai állapot meghatározása
    // Állapot 0 = WEB, 1 = SD, 2 = DLNA
    uint8_t curState = 0;
#    ifdef USE_SD
    if (config.getMode() == PM_SDCARD) {
        curState = 1;
    } else
#    endif
    { // FusionEdge: explicit blokk, hogy a fenti "else" ne nyelje el a következő
      // utasítást, ha USE_DLNA nincs definiálva (és ez a blokk üresen maradna)
#    ifdef USE_DLNA
    if (config.store.playlistSource == PL_SRC_DLNA) {
        curState = 2;
    }
#    endif
    }

    // Következő állapot meghatározása
    uint8_t nextState;
    switch (curState) {
        case 0: // WEB → SD (ha van SD) vagy DLNA (ha nincs SD, de van DLNA) vagy marad WEB
#    ifdef USE_SD
            nextState = 1; // WEB → SD
#    elif defined(USE_DLNA)
            nextState = 2; // WEB → DLNA
#    else
            nextState = 0;
#    endif
            break;
        case 1: // SD →
#    ifdef USE_DLNA
            nextState = 2; // SD → DLNA
#    else
            nextState = 0; // SD → WEB
#    endif
            break;
        case 2: // DLNA → WEB
        default:
            nextState = 0;
            break;
    }

    // Átváltás végrehajtása az állapotnak megfelelő logikával
    if (nextState == 1) {
        // → SD
#    ifdef USE_SD
        config.changeMode(PM_SDCARD);
        netserver.requestOnChange(GETPLAYERMODE, 0);
        netserver.requestOnChange(GETINDEX, 0);
#    endif
    } else if (nextState == 2) {
        // → DLNA (PM_WEB + PL_SRC_DLNA)
#    ifdef USE_DLNA
        if (!config.hasUsableDlnaPlaylist()) {
            log_i("##[MODE]# DLNA skipped: no usable saved playlist");
            return;
        }
#        ifdef USE_SD
        if (config.getMode() == PM_SDCARD) { config.changeMode(PM_WEB); }
#        endif
        config.store.playlistSource = PL_SRC_DLNA;
        config.saveValue(&config.store.playlistSource, (uint8_t)PL_SRC_DLNA);
        config.indexDLNAPlaylist();
        config.initDLNAPlaylist();
        netserver.requestOnChange(GETINDEX, 0);
        netserver.requestOnChange(GETPLAYERMODE, 0);
#    endif
    } else {
        // → WEB (PM_WEB + PL_SRC_WEB)
        config.store.playlistSource = PL_SRC_WEB;
        config.saveValue(&config.store.playlistSource, (uint8_t)PL_SRC_WEB);
#    ifdef USE_SD
        if (config.getMode() == PM_SDCARD) { config.changeMode(PM_WEB); }
        else {
#    endif
            config.indexPlaylist();
            config.initPlaylist();
            netserver.requestOnChange(GETINDEX, 0);
#    ifdef USE_SD
        }
#    endif
        netserver.requestOnChange(GETPLAYERMODE, 0);
    }

#    endif // USE_SD || USE_DLNA
}

// ─────────────────────────────────────────────────────────────────────────────

void TouchScreen::loop() {
    uint16_t             touchX = 0, touchY = 0;
    static uint32_t      touchLongPress;
    static tsDirection_e direct;
    static uint16_t      touchVol, touchStation;
#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
    static uint32_t presetsLastActivity = 0;
    static int      presetActionDone = 0; // 0=play, 1=save, 2=del
    static int      presetHoldSlot = -1;
    static int      favHold = -1;
    static bool     favLongTriggered = false;
    static uint32_t lastTouchMs = 0;
    static bool     specLongTriggered = false;  // spectrum long-press már kisült?
    static bool     volumeTouchActive = false;
    static int8_t   volumeTouchValue = -1;
#    endif
    static bool lastStTouched = false;

    if (!_checklpdelay(20, _touchdelay)) { return; }

    // ---- LovyanGFX touch beolvasás ----
    uint16_t x = 0, y = 0;
    bool     istouched = dsp.getTouch(&x, &y);

    // CSAK AKKOR frissítsük a touchX/Y-t, ha tényleg érintés van!
    // Ha istouched hamis, akkor a touchX/Y maradjon az előző érvényes érték.
    if (istouched) {
        if(config.store.dbgtouch) {
            Serial.printf("[TOUCH] raw x=%d y=%d  screen=%dx%d\n", x, y, _width, _height);
        }
        touchX = x;
        touchY = y;

        if (config.store.xTouchMirroring) {
            touchX = _width - 1 - touchX;
        }
        if (config.store.yTouchMirroring) {
            touchY = _height - 1 - touchY;
        }

        lastTouchMs = millis();
    }

    bool stTouched = (istouched || ((uint32_t)(millis() - lastTouchMs) < 120));
    bool newTouch = (stTouched && !lastStTouched);
    bool endTouch = (!stTouched && lastStTouched);

#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
    // Auto-exit presets screen after 15s
    if (display.mode() == PRESETS) {
        if (presets_toastExpired()) { return; }
        if (presetsLastActivity == 0) { presetsLastActivity = millis(); }
        if (stTouched) {
            presetsLastActivity = millis();
        } else if ((uint32_t)(millis() - presetsLastActivity) > 15000UL) {
            display.putRequest(NEWMODE, PLAYER);
            presetsLastActivity = 0;
            direct = TSD_STAY;
            return;
        }
    } else {
        presetsLastActivity = 0;
    }
#    endif

    if (stTouched) {
        if (newTouch) { /************************** START TOUCH *************************************/
            // Screensaverből kilépés bármilyen érintésre
            if (display.mode() == SCREENSAVER || display.mode() == SCREENBLANK) {
                display.putRequest(NEWMODE, PLAYER);
                direct = TSD_STAY;
                lastStTouched = stTouched;
                return;
            }

            _oldTouchX = touchX;
            _oldTouchY = touchY;
            touchVol = touchX;
            touchStation = touchY;
            direct = TDS_REQUEST;
            touchLongPress = millis();
            specLongTriggered = false;

#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
            volumeTouchActive = false;
            volumeTouchValue = -1;
            // A fuggoleges hangeroskala tap es drag vezerlest is kap. Azonnal
            // lefoglaljuk ezt a touch-ot, hogy ne valjon belole playlist swipe.
            if (display.mode() == PLAYER && !display.isEqOpen() &&
                _inRect(touchX, touchY,
                        SW_VOLUME_L, SW_VOLUME_R,
                        SW_VOLUME_T, SW_VOLUME_B)) {
                volumeTouchActive = true;
                volumeTouchValue = static_cast<int8_t>(_touchVolumeFromY(touchY));
                player.setVol(static_cast<uint8_t>(volumeTouchValue));
                direct = TSD_STAY;
            }
#    endif

#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
            if (display.mode() == PRESETS && !presets_keyboardActive()) {
                presetHoldSlot = presets_hitTest(touchX, touchY);
                presetActionDone = 0;
                presets_setPressedSlot(presetHoldSlot);
                presets_drawPressed();
                favHold = presets_hitTestFav(touchX, touchY);
                favLongTriggered = false;
            }
#    endif
        } else { /************************** HOLD / SWIPE TOUCH *************************************/
            bool handleGesture = true;
#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
            if (volumeTouchActive) {
                if (istouched) {
                    const int8_t nextVolume =
                        static_cast<int8_t>(_touchVolumeFromY(touchY));
                    if (nextVolume != volumeTouchValue) {
                        volumeTouchValue = nextVolume;
                        player.setVol(static_cast<uint8_t>(nextVolume));
                    }
                }
                direct = TSD_STAY;
                handleGesture = false;
            }
#    endif
            if (handleGesture) {
                if (display.mode() == PRESETS) {
                    direct = TDS_REQUEST;
                } else {
                    // KRITIKUS: Csak akkor nézzük a mozgást, ha a hardver MÉG ÉRZÉKELI az ujjad (istouched)
                    if (_oldTouchY >= 50 && istouched) {

                        int16_t distX = abs(touchX - _oldTouchX);
                        int16_t distY = abs(touchY - _oldTouchY);

                    // Lock the gesture axis once per touch. Reclassifying every sample
                    // allowed a noisy vertical playlist drag to become a volume swipe.
                        if (direct == TDS_REQUEST) {
                            if (distX < 30 && distY < 30) {
                            // MODESELECT: azonnal kijelöli azt az elemet, ami az ujj alatt van
                            if (display.mode() == MODESELECT) {
                                constexpr uint16_t MS_TOP  = 137;
                                constexpr uint16_t MS_H    = DSP_HEIGHT - MS_TOP;
                                constexpr uint16_t LIST_OFF = 35;
                                uint8_t cnt = display.msModeCount();
                                if (cnt > 0 && touchY >= (MS_TOP + LIST_OFF) && touchY < (MS_TOP + MS_H)) {
                                    uint16_t itemH = (MS_H - 40) / cnt;
                                    uint8_t  hovered = (touchY - (MS_TOP + LIST_OFF)) / itemH;
                                    if (hovered < cnt) { display.msHover(hovered); }
                                }
                            }
                            if (display.mode() == SCREENSELECT) {
                                constexpr uint16_t SS_TOP = 137;
                                constexpr uint16_t SS_H = DSP_HEIGHT - SS_TOP;
                                constexpr uint16_t LIST_OFF = 35;
                                const uint8_t cnt = display.screensaverSelectorCount();
                                if (touchY >= SS_TOP + LIST_OFF &&
                                    touchY < SS_TOP + SS_H) {
                                    const uint16_t itemH = (SS_H - 40) / cnt;
                                    const uint8_t hovered =
                                        (touchY - (SS_TOP + LIST_OFF)) / itemH;
                                    if (hovered < cnt) {
                                        display.screensaverSelectorHover(hovered);
                                    }
                                }
                            }
                            } else if (abs(distX - distY) >= TS_AXIS_LOCK_MARGIN) {
                                direct = _tsDirection(touchX, touchY);
                            }
                        }

                        switch (direct) {
                        case TSD_LEFT:
                        case TSD_RIGHT: {
                            // EQ overlay drag
                            if (display.mode() == PLAYER && display.isEqOpen() &&
                                touchY >= SW_VOL_T && touchY < DSP_HEIGHT) {
                                display_eq_touch(touchX, touchY, true);
                                break;
                            }
                            touchLongPress = millis() - 5000;

                            // VOL swipe: csak a VOL_AREA zónában (y >= SW_VOL_T)
                            if ((display.mode() == PLAYER || display.mode() == VOL) &&
                                _oldTouchY >= SW_VOL_T) {
                                int16_t xDelta = map(abs(touchVol - touchX), 0, _width, 0, TS_STEPS);
                                if (xDelta >= 1) {
                                    display.putRequest(NEWMODE, VOL);
                                    controlsEvent((touchVol - touchX) < 0);
                                    touchVol = touchX;
                                }
                            }
                            break;
                        }
                        case TSD_UP:
                        case TSD_DOWN: {
                            touchLongPress = millis() - 5000;
                            // MODESELECT swipe: görgetés a módok között
                            if (display.mode() == MODESELECT) {
                                int16_t yDelta = map(abs(touchStation - touchY), 0, _height, 0, TS_STEPS);
                                if (yDelta >= 1) {
                                    display.modeSelectorScroll((touchStation - touchY) < 0 ? 1 : -1);
                                    touchStation = touchY;
                                }
                                break;
                            }
                            if (display.mode() == SCREENSELECT) {
                                int16_t yDelta = map(abs(touchStation - touchY),
                                                     0, _height, 0, TS_STEPS);
                                if (yDelta >= 1) {
                                    display.screensaverSelectorScroll(
                                        (touchStation - touchY) < 0 ? 1 : -1);
                                    touchStation = touchY;
                                }
                                break;
                            }
                            // Bluetooth parositott lista: fel/le lapozas.
                            if (display.mode() == BTINFO &&
                                _oldTouchY >= SW_VOL_T) {
                                int16_t yDelta = map(abs(touchStation - touchY),
                                                     0, _height, 0, TS_STEPS);
                                if (yDelta >= 1) {
                                    display.btInfoScroll(
                                        (touchStation - touchY) > 0 ? 1 : -1);
                                    touchStation = touchY;
                                }
                                break;
                            }
                            // A fel/le huzas alapesetben nem nyit listat. Csak
                            // a mar megnyitott playlisten gorgeti a sorokat.
                            if (display.mode() == STATIONS && _oldTouchY >= SW_VOL_T) {
                                int16_t yDelta = map(abs(touchStation - touchY), 0, _height, 0, TS_STEPS);
                                if (yDelta >= 1) {
                                    controlsEvent((touchStation - touchY) < 0);
                                    touchStation = touchY;
                                }
                            }
                            break;
                        }
                            default: break;
                        }
                    }
                }
            }
        }
/********************************** PRESETS HOLD (real-time) ************************************/
#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
        if (display.mode() == PRESETS && !presets_keyboardActive()) {
            uint32_t held = millis() - touchLongPress;

            if (favHold >= 0 && !favLongTriggered) {
                if (held >= BTN_PRESS_TICKS * 2) {
                    presets_keyboardOpen((uint8_t)favHold);
                    presets_drawScreen();
                    favLongTriggered = true;
                    direct = TSD_STAY;
                }
            }

            if (presetHoldSlot >= 0) {
                presets_drawHoldBar(held);
                presets_setPressedSlot(presetHoldSlot);
                presets_drawPressed();

                if (held >= BTN_PRESS_TICKS * 4)
                    presetActionDone = 2; // DEL
                else if (held >= BTN_PRESS_TICKS * 2)
                    presetActionDone = 1; // SAVE
                else
                    presetActionDone = 0; // PLAY
            }
        }

        // ── Spectrum widget hosszú érintés: peak toggle ──────────────────────
        if (display.mode() == PLAYER && !specLongTriggered && direct == TDS_REQUEST) {
            if (_inRect(_oldTouchX, _oldTouchY, SW_SPEC_L, SW_SPEC_R, SW_SPEC_T, SW_SPEC_B)) {
                uint32_t held = millis() - touchLongPress;
                if (held >= (uint32_t)BTN_PRESS_TICKS * 2) {
                    _touchToggleVuPeak();
                    specLongTriggered = true;
                    direct = TSD_STAY;   // ne triggerelj tap-et is
                }
            }
        }

        // ── PlayMode widget hosszú érintés: már nincs akció (tap nyitja a MODESELECT menüt) ────
#    endif
    } else {
        if (endTouch) { /**************************** END TOUCH *********************************/
            if (direct == TDS_REQUEST) {
                uint32_t pressTicks = millis() - touchLongPress;

                // ── MODESELECT: minden touch event IDE fut, más handler nem kapja meg ──
                if (display.mode() == MODESELECT) {
                    // Same full-height lower menu area as display.cpp.
                    constexpr uint16_t MS_AREA_TOP = 137;
                    constexpr uint16_t MS_AREA_H   = DSP_HEIGHT - MS_AREA_TOP;
                    if (_oldTouchY >= MS_AREA_TOP && _oldTouchY < (MS_AREA_TOP + MS_AREA_H)) {
                        uint16_t listTop = MS_AREA_TOP + 35;
                        uint8_t  cnt     = display.msModeCount();
                        if (cnt > 0 && _oldTouchY >= listTop) {
                            uint16_t itemH = (MS_AREA_H - 40) / cnt;
                            int8_t tapped  = (_oldTouchY - listTop) / itemH;
                            if (tapped >= 0 && tapped < (int8_t)cnt) {
                                display.msTapItem((uint8_t)tapped);
                                goto finish_touch;
                            }
                        }
                    }
                    display.modeSelectorClose();
                    goto finish_touch;
                }

                if (display.mode() == SCREENSELECT) {
                    constexpr uint16_t SS_AREA_TOP = 137;
                    constexpr uint16_t SS_AREA_H = DSP_HEIGHT - SS_AREA_TOP;
                    const uint16_t listTop = SS_AREA_TOP + 35;
                    const uint8_t cnt = display.screensaverSelectorCount();
                    if (_oldTouchY >= listTop &&
                        _oldTouchY < SS_AREA_TOP + SS_AREA_H) {
                        const uint16_t itemH = (SS_AREA_H - 40) / cnt;
                        const int8_t tapped = (_oldTouchY - listTop) / itemH;
                        if (tapped >= 0 && tapped < static_cast<int8_t>(cnt)) {
                            display.screensaverSelectorTap(
                                static_cast<uint8_t>(tapped));
                            goto finish_touch;
                        }
                    }
                    display.screensaverSelectorClose();
                    goto finish_touch;
                }

#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
                presets_clearPressed();
                presets_setPressedSlot(-1);

                // A hat gyorsbeallitas-gomb ugyanazt az akcioutat hasznalja,
                // mint az enkoderes kivalasztas.
                if (display.mode() == QUICKSETTINGS &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
                    const int8_t action =
                        display.quickSettingsHit(_oldTouchX, _oldTouchY);
                    if (action >= 0) {
                        display.quickSettingsActivate(static_cast<uint8_t>(action));
                    } else {
                        display.putRequest(NEWMODE, PLAYER);
                    }
                    goto finish_touch;
                }

                // Csak a BT Connect/Disconnect gomb tartja nyitva a listát;
                // a szöveges rész vagy az üres terület érintése bezárja.
                if (display.mode() == BTINFO &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
                    if (!display.btInfoTap(_oldTouchX, _oldTouchY)) {
                        display.putRequest(NEWMODE, PLAYER);
                    }
                    goto finish_touch;
                }

                // A Wi-Fi informacios nezet barmely rovid erintesre bezarhato.
                if (display.mode() == WIFI &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
                    display.putRequest(NEWMODE, PLAYER);
                    goto finish_touch;
                }

                // Az ot napos elorejelzes a tobbi kozepso inline nezethez
                // hasonloan barmely rovid erintesre bezarhato.
                if (display.mode() == FORECAST &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
                    display.putRequest(NEWMODE, PLAYER);
                    goto finish_touch;
                }

                // 1) STATION PRESETS: csak a META vagy Title1 szovegsorrol.
                const bool inPresetTextRow =
                    _inRect(_oldTouchX, _oldTouchY,
                            SW_PRESETS_L, SW_PRESETS_R, SW_META_T, SW_META_B) ||
                    _inRect(_oldTouchX, _oldTouchY,
                            SW_PRESETS_L, SW_PRESETS_R, SW_TITLE1_T, SW_TITLE1_B);
                if (display.mode() == PLAYER && inPresetTextRow &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
                    display.putRequest(NEWMODE, PRESETS);
                    presetsLastActivity = millis();
                    goto finish_touch;
                }

                // 2) PRESETS MÓD BELSŐ KEZELÉSE
                if (display.mode() == PRESETS) {
                    if (presets_keyboardActive()) {
                        if (pressTicks > 50) presets_keyboardTap(_oldTouchX, _oldTouchY);
                    } else if (_oldTouchY < SW_PRESETS_HEADER_B &&
                               pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
                        display.putRequest(NEWMODE, PLAYER);
                    } else {
                        int fav = presets_hitTestFav(_oldTouchX, _oldTouchY);
                        if (fav >= 0) {
                            if (!favLongTriggered && pressTicks > 50) presets_selectBank((uint8_t)fav);
                            presets_drawScreen();
                        } else {
                            int hit = presets_hitTest(_oldTouchX, _oldTouchY);
                            if (hit >= 0) {
                                if (presetActionDone == 2)
                                    presets_clear((uint8_t)hit);
                                else if (presetActionDone == 1)
                                    presets_save((uint8_t)hit);
                                else {
                                    if (!presets_play((uint8_t)hit)) display.putRequest(NEWMODE, PLAYER);
                                }
                            }
                        }
                    }
                    goto finish_touch;
                }

                // The EQ panel now covers the footer too. Its own BACK button
                // closes it; the hidden settings hit area must not take input.
                if (display.mode() == PLAYER && display.isEqOpen() &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2 &&
                    _oldTouchY >= SW_VOL_T && _oldTouchY < DSP_HEIGHT) {
                    display_eq_touch(_oldTouchX, _oldTouchY, false);
                    goto finish_touch;
                }

                // Ha EQ nyitva van, innentől ne kezeljük a többi widgetet
                if (display.isEqOpen()) goto finish_touch;

                // A footer kozepen levo beallitasgomb nyitja a menut.
                if (display.mode() == PLAYER &&
                    pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2 &&
                    _inRect(_oldTouchX, _oldTouchY,
                            SW_GEAR_L, SW_GEAR_R, SW_GEAR_T, SW_GEAR_B)) {
                    display.putRequest(NEWMODE, QUICKSETTINGS);
                    goto finish_touch;
                }

                // ── Widget tap-ok: csak rövid, egyértelmű érintés ────────────
                if (pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {

#    ifdef USE_COVER_SCREENSAVER
                    // A kis logo a mar meglevo nagy cover-art nezetet nyitja,
                    // a beallitott automatikus screensaver modtol fuggetlenul.
                    if (display.mode() == PLAYER &&
                        _inRect(_oldTouchX, _oldTouchY,
                                SW_COVER_L, SW_COVER_R, SW_COVER_T, SW_COVER_B)) {
                        display.putRequest(SHOWCOVERVIEW);
                        goto finish_touch;
                    }
#    endif

                    // A footer bal szelen levo, kodbol rajzolt Bluetooth ikon.
                    if (display.mode() == PLAYER &&
                        _inRect(_oldTouchX, _oldTouchY,
                                SW_BT_L, SW_BT_R, SW_BT_T, SW_BT_B)) {
                        display.putRequest(NEWMODE, BTINFO);
                        goto finish_touch;
                    }

                    // A modvalaszto a footer playmode szovegrol nyilik.
                    if (display.mode() == PLAYER &&
                        _inRect(_oldTouchX, _oldTouchY,
                                SW_PLAYMODE_L, SW_PLAYMODE_R,
                                SW_PLAYMODE_T, SW_PLAYMODE_B)) {
                        display.modeSelectorOpen();
                        goto finish_touch;
                    }

                    // A playlistet a footer Ch.:/station sorszama nyitja.
                    if (display.mode() == PLAYER &&
                        _inRect(_oldTouchX, _oldTouchY,
                                SW_STATION_L, SW_STATION_R,
                                SW_STATION_T, SW_STATION_B)) {
                        display.putRequest(NEWMODE, STATIONS);
                        goto finish_touch;
                    }

                    // A footer jobb szelen levo Wi-Fi ikon nyitja az SSID/IP/RSSI
                    // informacios nezetet. A rajzolast a display task vegzi.
                    if (display.mode() == PLAYER &&
                        _inRect(_oldTouchX, _oldTouchY, SW_WIFI_L, SW_WIFI_R, SW_WIFI_T, SW_WIFI_B)) {
                        display.putRequest(NEWMODE, WIFI);
                        goto finish_touch;
                    }

                    // Az aktualis idojaras ikon nyitja az ot napos
                    // elorejelzest a megszokott kozepso setup-teruleten.
                    if (display.mode() == PLAYER && config.store.showweather &&
                        _inRect(_oldTouchX, _oldTouchY,
                                SW_WEATHER_L, SW_WEATHER_R,
                                SW_WEATHER_T, SW_WEATHER_B)) {
                        display.putRequest(NEWMODE, FORECAST);
                        goto finish_touch;
                    }

                    // A harom ikonos gomb erintesi terulete megegyezik a
                    // kijelzon lathato lekerekitett keretekkel.
                    const bool inTransportRow =
                        display.mode() == PLAYER &&
                        _oldTouchY >= SW_TRANSPORT_T &&
                        _oldTouchY < SW_TRANSPORT_B;
                    if (inTransportRow &&
                        _oldTouchX >= SW_TRANSPORT_PREV_L &&
                        _oldTouchX < SW_TRANSPORT_PREV_R) {
                        _touchDispatchTransport(TOUCH_TRANSPORT_PREVIOUS);
                        goto finish_touch;
                    }
                    if (inTransportRow &&
                        _oldTouchX >= SW_TRANSPORT_PLAY_L &&
                        _oldTouchX < SW_TRANSPORT_PLAY_R) {
                        _touchDispatchTransport(TOUCH_TRANSPORT_TOGGLE);
                        goto finish_touch;
                    }
                    if (inTransportRow &&
                        _oldTouchX >= SW_TRANSPORT_NEXT_L &&
                        _oldTouchX < SW_TRANSPORT_NEXT_R) {
                        _touchDispatchTransport(TOUCH_TRANSPORT_NEXT);
                        goto finish_touch;
                    }

                    // STATIONS mód: playlist elem kiválasztása touch-al
                    if (display.mode() == STATIONS && _oldTouchY >= SW_VOL_T) {
                        display.tapPlaylistItem(_oldTouchY);
                        goto finish_touch;
                    }

                    // The player widgets underneath an inline menu are hidden.
                    // Do not let their old touch zones trigger through it.
                    if (display.mode() != PLAYER) { goto finish_touch; }

                    // 5) ClockWidget tap → ciklikus óra-font váltás
                    if (_inRect(_oldTouchX, _oldTouchY, SW_CLOCK_L, SW_CLOCK_R, SW_CLOCK_T, SW_CLOCK_B)) {
                        _touchCycleClockFont();
                        goto finish_touch;
                    }

                    // 6) SpectrumWidget tap → vuStyle ciklus (long press már specLongTriggered-be ment)
                    if (!specLongTriggered &&
                        _inRect(_oldTouchX, _oldTouchY, SW_SPEC_L, SW_SPEC_R, SW_SPEC_T, SW_SPEC_B)) {
                        _touchCycleVuStyle();
                        goto finish_touch;
                    }

                }
#    endif
            }

        finish_touch:
#    if (DSP_WIDTH == 480) && (DSP_HEIGHT == 320)
            presetHoldSlot = -1;
            presetActionDone = 0;
            specLongTriggered = false;
            volumeTouchActive = false;
            volumeTouchValue = -1;
#    endif
            direct = TSD_STAY;
        }
    }
    lastStTouched = stTouched;
}

bool TouchScreen::_checklpdelay(int m, uint32_t& tstamp) {
    if (millis() - tstamp > (uint32_t)m) {
        tstamp = millis();
        return true;
    }
    return false;
}

#endif
