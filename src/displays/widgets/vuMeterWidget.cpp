#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY && defined(USE_VU_METER_SCREENSAVER)
#include "../../core/config.h"
#include "../../core/fonts.h"
#include "../../core/network.h"
#include "../../core/player.h"
#ifdef USE_BLUETOOTH
#include "../../core/bluetooth.h"
#endif
#include "../display_select.h"
#include "vuMeterWidget.h"
#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstring>

static_assert(DSP_WIDTH == 480 && DSP_HEIGHT == 320,
              "USE_VU_METER_SCREENSAVER currently requires a 480x320 display");

namespace {
constexpr uint32_t ANIM_MS = 16;

constexpr float VU_MIN  = -20.0f;
constexpr float VU_MAX  = 3.0f;
constexpr float VU_SPAN = 23.0f;

constexpr float START_ANGLE = -135.0f;
constexpr float END_ANGLE   = -45.0f;
constexpr float PI_DEG      = 0.01745329251994329577f;

// Classic analog VU palette
constexpr uint16_t BLACK       = 0x0000;
constexpr uint16_t WHITE       = 0xFFFF;
constexpr uint16_t CREAM       = 0xFFDF;
constexpr uint16_t CREAM_DARK  = 0xD69A;
constexpr uint16_t FRAME_DARK  = 0x2104;
constexpr uint16_t FRAME_MID   = 0x4208;
constexpr uint16_t FRAME_LIGHT = 0x630C;
constexpr uint16_t RED         = 0xF800;
constexpr uint16_t RED_DARK    = 0x7800;
constexpr uint16_t TEXT_DARK   = 0x2104;

constexpr uint16_t PANEL_BG = BLACK;
constexpr uint16_t LABEL_BG = BLACK;

constexpr uint16_t LABEL_TEXT_STATION = WHITE;
constexpr uint16_t LABEL_TEXT_TITLE1  = WHITE;
constexpr uint16_t LABEL_TEXT_TITLE2  = WHITE;

constexpr int MARGIN = 5;
constexpr int GAP    = 6;

constexpr uint8_t SUPPORTED_VLW_SIZES[] =
    {9, 11, 12, 16, 18, 20, 22, 24, 26, 36};

uint8_t pickFontSize(float targetPx) {
    uint8_t best = SUPPORTED_VLW_SIZES[0];
    for (uint8_t s : SUPPORTED_VLW_SIZES) {
        if (s <= targetPx) best = s;
        else break;
    }
    return best;
}

float dbPos(float db) {
    if (db <= VU_MIN) return 0.0f;
    if (db >= VU_MAX) return 1.0f;
    return (db - VU_MIN) / VU_SPAN;
}

void loadFont(LGFX_Sprite& canvas, uint8_t* font) {
    if (font) {
        canvas.loadFont(font);
    } else {
        canvas.unloadFont();
        canvas.setFont(nullptr);
    }
    canvas.setTextSize(1);
}

// Ported from coverScreensaverWidget.cpp's drawClock(), adapted to draw the
// time left-aligned at leftX instead of docked to a fixed side panel.
void drawClock(LGFX_Sprite& canvas, int16_t leftX, int16_t y,
               const char* timeText) {
    int hour = 0;
    int minute = 0;
    if (!timeText || sscanf(timeText, "%d:%d", &hour, &minute) != 2) return;

    uint8_t* mainFont = nullptr;
    uint8_t* secondaryFont = nullptr;
    getClockFontStylePointers(config.store.clockFontStyle, &mainFont, &secondaryFont);
    loadFont(canvas, mainFont);

    char mainTime[8] = {};
    if (config.store.clockAmPmStyle) {
        int hour12 = hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(mainTime, sizeof(mainTime), "%2d:%02d", hour12, minute);
    } else {
        snprintf(mainTime, sizeof(mainTime), "%02d:%02d", hour, minute);
    }

    const uint16_t timeWidth = canvas.textWidth(mainTime);
    const uint16_t timeHeight = canvas.fontHeight();
    const int16_t timeX = leftX;

    canvas.setTextColor(config.theme.clock, TFT_BLACK);
    canvas.setTextDatum(top_left);
    canvas.drawString(mainTime, timeX, y);
    if (mainFont) canvas.unloadFont();

    if (!config.store.clockAmPmStyle) return;

    loadFont(canvas, font_vlw_18);
    const char* amPm = hour >= 12 ? "PM" : "AM";
    const uint16_t amPmHeight = canvas.fontHeight();
    const int16_t amPmX = timeX + timeWidth + 4;
    const int16_t amPmY = y + max<int16_t>(0, (int16_t)(timeHeight / 4) - (int16_t)(amPmHeight / 2));

    canvas.setTextColor(config.theme.seconds, TFT_BLACK);
    canvas.setTextDatum(top_left);
    canvas.drawString(amPm, amPmX, amPmY);
    if (font_vlw_18) canvas.unloadFont();
}
}

vuMeterWidget::vuMeterWidget() : Widget(), _sprite(&dsp) {}

vuMeterWidget::~vuMeterWidget() {}

void vuMeterWidget::init(WidgetConfig conf, uint16_t fgcolor, uint16_t bgcolor) {
    Widget::init(conf, fgcolor, bgcolor);
    _config.left = 0;
    _config.top = 0;
    _width = DSP_WIDTH;

    _geomReady = false;
    _spriteReady = false;
    _spriteAllocAttempted = false;
    _phaseL = _phaseR = 0.0f;
    _levelL = _levelR = 0.0f;
    _lastAnim = 0;

    _calcGeometry();
}

void vuMeterWidget::init(uint16_t bgcolor) {
    WidgetConfig conf;
    conf.left = 0;
    conf.top = 0;
    conf.align = WA_LEFT;
    init(conf, WHITE, bgcolor);
}

bool vuMeterWidget::_createSprite() {
    if (_spriteReady) return true;
    if (_spriteAllocAttempted) return false;
    _spriteAllocAttempted = true;

    _sprite.setColorDepth(16);
    _sprite.setPsram(true);
    _spriteReady = _sprite.createSprite(DSP_WIDTH, dsp.height());
    if (!_spriteReady) {
        log_w("##[VU-SS]# unable to allocate 480x320 PSRAM sprite");
    }

    return _spriteReady;
}

bool vuMeterWidget::ready() {
    return _createSprite();
}

void vuMeterWidget::_calcGeometry() {
    if (_geomReady) return;

    const int available = DSP_WIDTH - 2 * MARGIN - GAP;
    _meterW = available / 2;

    _meterH = (int)((dsp.height() - 100) * 0.8f);
    if (_meterH < 60) _meterH = 60;

    _meterXL = MARGIN;
    _meterXR = MARGIN + _meterW + GAP;
    _meterY  = 85;

    _meterVScale = (float)_meterH / 220.0f;
    _meterHScale = (float)_meterW / 232.0f;

    _geomReady = true;
}

void vuMeterWidget::setLevel(float left, float right) {
    _levelL = constrain(left, 0.0f, 1.0f);
    _levelR = constrain(right, 0.0f, 1.0f);
}

void vuMeterWidget::setStation(const char* station) {
    strlcpy(_stationText, station ? station : "", sizeof(_stationText));
}

void vuMeterWidget::setTitle(const char* title1, const char* title2) {
    strlcpy(_title1Text, title1 ? title1 : "", sizeof(_title1Text));
    strlcpy(_title2Text, title2 ? title2 : "", sizeof(_title2Text));
}

void vuMeterWidget::setTime(const char* time) {
    char minuteTime[8] = {};
    if (time) {
        int hour = 0;
        int minute = 0;
        if (sscanf(time, "%d:%d", &hour, &minute) == 2) {
            snprintf(minuteTime, sizeof(minuteTime), "%02d:%02d", hour, minute);
        }
    }
    strlcpy(_time, minuteTime, sizeof(_time));
}

void vuMeterWidget::loop() {
    if (!_active || _locked) return;

    const uint32_t now = millis();
    if (now - _lastAnim < ANIM_MS) return;
    _lastAnim = now;

    uint16_t vu = 0;
    bool audioLevelAvailable = false;
#ifdef USE_BLUETOOTH
    if (config.getMode() == PM_BLUETOOTH && bluetooth.bridgeRunning()) {
        vu = bluetooth.vuLevel();
        audioLevelAvailable = true;
    }
#endif
    if (!audioLevelAvailable && player.isRunning()) {
        vu = player.getVUlevel();
        audioLevelAvailable = true;
    }

    if (audioLevelAvailable) {
        setLevel((vu & 0xFF) / 255.0f,
                 ((vu >> 8) & 0xFF) / 255.0f);
    } else {
        setLevel(0.0f, 0.0f);
    }

    setStation(config.station.name);
    setTitle(config.station.title, "");

    // Keep the fast response, but make the movement slightly smoother.
    const float responseL = powf(_levelL, 1.65f) * 0.92f;
    const float responseR = powf(_levelR, 1.65f) * 0.92f;

    const float targetL = responseL * VU_SPAN;
    const float targetR = responseR * VU_SPAN;

    auto step = [](float& p, float t) {
        const float speed = (t > p) ? 0.78f : 0.50f;
        p += (t - p) * speed;
        if (p < 0.0f) p = 0.0f;
        if (p > VU_SPAN) p = VU_SPAN;
    };

    step(_phaseL, targetL);
    step(_phaseR, targetR);

    _draw();
}

void vuMeterWidget::_draw() {
    if (!_active || _locked) return;

    if (!_geomReady) _calcGeometry();
    if (!_createSprite()) return;

    _sprite.fillSprite(PANEL_BG);

    _drawFrame();
    _drawClock();
    _drawLabels();

    _drawMeter(_meterXL, _meterY, _meterW, _meterH, "LEFT",  _phaseL);
    _drawMeter(_meterXR, _meterY, _meterW, _meterH, "RIGHT", _phaseR);

    _drawDigital();

    _sprite.pushSprite(0, 0);
}

void vuMeterWidget::_drawClock() {
    tm timeinfo{};
    network_get_timeinfo_snapshot(&timeinfo);
    strftime(_time, sizeof(_time), "%H:%M", &timeinfo);

    if (!_time[0]) return;
    drawClock(_sprite, 10, 6, _time);
}

void vuMeterWidget::_drawFrame() {
    // Outer metal-like frame
    _sprite.drawFastHLine(0, 0, DSP_WIDTH, FRAME_LIGHT);
    _sprite.drawFastHLine(0, 1, DSP_WIDTH, FRAME_DARK);
    _sprite.drawFastHLine(0, dsp.height() - 2, DSP_WIDTH, FRAME_DARK);
    _sprite.drawFastHLine(0, dsp.height() - 1, DSP_WIDTH, FRAME_LIGHT);

    // Divider between the two meters
    const int cx = DSP_WIDTH / 2;
    _sprite.drawFastVLine(cx - 1, _meterY + 5, _meterH - 10, FRAME_DARK);
    _sprite.drawFastVLine(cx,     _meterY + 5, _meterH - 10, FRAME_MID);

    // Small center screw/details
    _sprite.fillCircle(cx, _meterY + 10, 2, FRAME_LIGHT);
}

void vuMeterWidget::_drawMeter(int x, int y, int w, int h,
                               const char* label, float phase) {
    // Black chassis
    _sprite.fillRoundRect(x, y, w, h, 8, FRAME_DARK);
    _sprite.drawRoundRect(x, y, w, h, 8, FRAME_LIGHT);

    // Inner black border
    _sprite.fillRoundRect(x + 4, y + 4, w - 8, h - 8, 6, BLACK);

    // Cream analog dial, intentionally flatter and more rectangular,
    // like the photographed vintage VU instrument.
    _sprite.fillRect(x + 8, y + 8, w - 16, h - 16, CREAM);
    _sprite.drawRect(x + 8, y + 8, w - 16, h - 16, CREAM_DARK);

    // Inner printed border.
    _sprite.drawRect(x + 12, y + 12, w - 24, h - 24, CREAM_DARK);

    _drawScale(x, y, w, h);

    const uint8_t textSz = (_meterVScale < 0.8f) ? 1 : 2;

    _sprite.setFont(nullptr);
    _sprite.setTextSize(textSz);
    _sprite.setTextDatum(middle_center);

    // Channel label only; the large "VU" is drawn by _drawScale()
    // so it stays centered relative to the analog face.
    _sprite.setTextSize(1);
    _sprite.setTextColor(TEXT_DARK, CREAM);
    _sprite.drawString(label, x + w / 2,
                       y + h - (int)(27 * _meterVScale));

    // Bottom small scale line
    _sprite.drawFastHLine(x + 18, y + h - (int)(45 * _meterVScale),
                          w - 36, CREAM_DARK);

    _drawNeedle(x, y, w, h, phase);
}

void vuMeterWidget::_drawScale(int x, int y, int w, int h) {
    const int cx = x + w / 2;

    // Low pivot: the scale and needle share the same mechanical geometry.
    const int pivotY = y + h - (int)(46 * _meterVScale);

    int radius = w / 2 - (int)(20 * _meterHScale);
    if (radius < 24) radius = 24;

    // ------------------------------------------------------------
    // Main thick arc - black from -20 to 0 dB, red from 0 to +3 dB
    // ------------------------------------------------------------
    const int arcRadius = radius - 3;

    int lastX = 0;
    int lastY = 0;
    bool haveLast = false;

    for (int i = 0; i <= 92; ++i) {
        const float pos = i / 92.0f;
        const float a =
            (START_ANGLE + pos * (END_ANGLE - START_ANGLE)) * PI_DEG;

        const int px = cx + (int)(cosf(a) * arcRadius);
        const int py = pivotY + (int)(sinf(a) * arcRadius);

        if (haveLast) {
            const uint16_t c = (pos >= dbPos(0.0f)) ? RED : TEXT_DARK;

            // Double line makes the arc look more like a printed instrument scale.
            _sprite.drawLine(lastX, lastY, px, py, c);

            if (w >= 90) {
                _sprite.drawLine(lastX, lastY + 1, px, py + 1, c);
            }
        }

        lastX = px;
        lastY = py;
        haveLast = true;
    }

    // Small red dot right at the 0 dB transition point, as in the photo.
    {
        const float a0 = (START_ANGLE +
                          dbPos(0.0f) * (END_ANGLE - START_ANGLE)) * PI_DEG;
        const int dotR = arcRadius - (int)(8 * _meterHScale);
        _sprite.fillCircle(cx + (int)(cosf(a0) * dotR),
                           pivotY + (int)(sinf(a0) * dotR),
                           2, RED);
    }

    // ------------------------------------------------------------
    // Outer dB scale
    // ------------------------------------------------------------
    const int dbMarks[] = {-20, -10, -7, -5, -3, -1, 0, 3};

    _sprite.setFont(nullptr);
    _sprite.setTextSize(1);
    _sprite.setTextDatum(middle_center);

    for (int db : dbMarks) {
        const float a =
            (START_ANGLE +
             dbPos((float)db) * (END_ANGLE - START_ANGLE)) * PI_DEG;

        const bool major =
            (db == -20 || db == -10 || db == -5 || db == 0 || db == 3);

        const int outer = radius + (int)(1 * _meterHScale);
        const int inner = outer - (major ? 13 : 9);

        const uint16_t tickColor = (db >= 0) ? RED : TEXT_DARK;

        _sprite.drawLine(
            cx + (int)(cosf(a) * outer),
            pivotY + (int)(sinf(a) * outer),
            cx + (int)(cosf(a) * inner),
            pivotY + (int)(sinf(a) * inner),
            tickColor);

        if (major) {
            _sprite.drawLine(
                cx + (int)(cosf(a) * (outer + 1)),
                pivotY + (int)(sinf(a) * (outer + 1)),
                cx + (int)(cosf(a) * (inner - 2)),
                pivotY + (int)(sinf(a) * (inner - 2)),
                tickColor);
        }

        // Put the dB number outside the main arc.
        const int labelR = radius + (int)(18 * _meterHScale);
        const int tx = cx + (int)(cosf(a) * labelR);
        const int ty = pivotY + (int)(sinf(a) * labelR);

        // A signed 32-bit value plus an optional leading '+' and the terminator
        // needs at most 12 bytes. The scale values are much smaller, but sizing
        // for the declared int type also keeps -Wformat-truncation satisfied.
        char b[12];
        snprintf(b, sizeof(b), db > 0 ? "+%d" : "%d", db);

        _sprite.setTextColor(db >= 0 ? RED_DARK : TEXT_DARK, CREAM);
        _sprite.drawString(b, tx, ty);
    }

    // ------------------------------------------------------------
    // Decorative "-" and "+" end symbols, just past the last tick on
    // each side, as in the reference photograph.
    // ------------------------------------------------------------
    {
        const float endOffsetDeg = 9.0f;
        const int labelR = radius + (int)(18 * _meterHScale);

        const float aMinus = (START_ANGLE - endOffsetDeg) * PI_DEG;
        _sprite.setTextColor(TEXT_DARK, CREAM);
        _sprite.drawString("-",
            cx + (int)(cosf(aMinus) * labelR),
            pivotY + (int)(sinf(aMinus) * labelR));

        const float aPlus = (END_ANGLE + endOffsetDeg) * PI_DEG;
        _sprite.setTextColor(RED_DARK, CREAM);
        _sprite.drawString("+",
            cx + (int)(cosf(aPlus) * labelR),
            pivotY + (int)(sinf(aPlus) * labelR));
    }

    // ------------------------------------------------------------
    // Inner percentage scale, similar to the reference photograph
    // ------------------------------------------------------------
    const int pctMarks[] = {0, 30, 50, 70, 100};

    for (int pct : pctMarks) {
        // 0..100% is mapped over the same usable arc.
        const float pos = constrain(pct / 100.0f, 0.0f, 1.0f);
        const float a =
            (START_ANGLE + pos * (END_ANGLE - START_ANGLE)) * PI_DEG;

        const int innerR = radius - (int)(19 * _meterHScale);
        const int tickR  = innerR + (int)(8 * _meterHScale);

        _sprite.drawLine(
            cx + (int)(cosf(a) * tickR),
            pivotY + (int)(sinf(a) * tickR),
            cx + (int)(cosf(a) * innerR),
            pivotY + (int)(sinf(a) * innerR),
            TEXT_DARK);

        const int labelR = radius - (int)(30 * _meterHScale);
        const int tx = cx + (int)(cosf(a) * labelR);
        const int ty = pivotY + (int)(sinf(a) * labelR);

        char b[8];
        snprintf(b, sizeof(b), "%d", pct);

        _sprite.setTextColor(TEXT_DARK, CREAM);
        _sprite.drawString(b, tx, ty);
    }

    // Small percentage sign on the lower-left, as in the reference.
    _sprite.setTextColor(TEXT_DARK, CREAM);
    _sprite.drawString("%", x + (int)(18 * _meterHScale),
                       pivotY + (int)(13 * _meterVScale));

    // ------------------------------------------------------------
    // Center VU marking
    // ------------------------------------------------------------
    _sprite.setTextSize((_meterVScale < 0.75f) ? 1 : 2);
    _sprite.setTextColor(TEXT_DARK, CREAM);
    _sprite.drawString("VU", cx,
                       y + (int)(142 * _meterVScale));
}

float vuMeterWidget::_phaseToAngle(float phase) const {
    phase = constrain(phase, 0.0f, VU_SPAN);
    return START_ANGLE +
           (phase / VU_SPAN) * (END_ANGLE - START_ANGLE);
}

void vuMeterWidget::_drawNeedle(int x, int y, int w, int h, float phase) {
    const int cx = x + w / 2;

    // Low mechanical pivot, like a classic analog VU meter.
    const int pivotY = y + h - (int)(46 * _meterVScale);

    // Match the tick-arc radius used in _drawScale so the needle tip lands
    // right at the scale instead of drifting past it on wide/tall panels.
    int radius = w / 2 - (int)(20 * _meterHScale);
    if (radius < 24) radius = 24;

    const int len = radius + (int)(6 * _meterHScale);

    float pos = phase / VU_SPAN;
    pos = constrain(pos, 0.0f, 1.0f);

    const float ang =
        (START_ANGLE + pos * (END_ANGLE - START_ANGLE)) * PI_DEG;

    const int tx = cx + (int)(cosf(ang) * len);
    const int ty = pivotY + (int)(sinf(ang) * len);

    // Dark shadow
    _sprite.drawLine(cx + 2, pivotY + 1,
                     tx + 2, ty + 1, RED_DARK);

    // Thin red pointer
    _sprite.drawLine(cx, pivotY, tx, ty, RED);
    _sprite.drawLine(cx + 1, pivotY, tx + 1, ty, RED);

    // Small lower tail
    const int tail = (int)(10 * _meterVScale);
    if (tail > 0) {
        _sprite.drawLine(cx, pivotY, cx, pivotY + tail, RED_DARK);
        _sprite.drawLine(cx + 1, pivotY, cx + 1, pivotY + tail, RED);
    }

    // No visible hub: in the reference photo the needle simply exits the
    // bottom of the dial and the mechanical pivot is hidden below the face.
}

void vuMeterWidget::_drawDigital() {
    float dbL = VU_MIN + _phaseL;
    float dbR = VU_MIN + _phaseR;

    dbL = roundf(dbL * 10.0f) / 10.0f;
    dbR = roundf(dbR * 10.0f) / 10.0f;

    char l[20], r[20];
    snprintf(l, sizeof(l), "%.1f dB", dbL);
    snprintf(r, sizeof(r), "%.1f dB", dbR);

    _sprite.setFont(nullptr);
    _sprite.setTextSize(1);
    _sprite.setTextDatum(middle_center);
    _sprite.setTextColor(WHITE, PANEL_BG);

    _sprite.fillRect(_meterXL + 18, dsp.height() - 37,
                     _meterW - 36, 16, PANEL_BG);
    _sprite.fillRect(_meterXR + 18, dsp.height() - 37,
                     _meterW - 36, 16, PANEL_BG);

    _sprite.drawString(l, _meterXL + _meterW / 2, dsp.height() - 31);
    _sprite.drawString(r, _meterXR + _meterW / 2, dsp.height() - 31);

    _sprite.fillRect(DSP_WIDTH / 2 - 72, dsp.height() - 17,
                     144, 14, PANEL_BG);

    _sprite.setTextColor(WHITE, PANEL_BG);
    _sprite.drawString("0VU = -12 dBFS",
                       DSP_WIDTH / 2, dsp.height() - 9);
}

void vuMeterWidget::_drawLabelLine(const char* text, int16_t cx,
                                   int16_t cy, int16_t maxW,
                                   uint8_t fontSize, uint16_t color) {
    if (!text || !text[0]) return;

    uint8_t* vlw = vlwBySize(fontSize);

    if (vlw) {
        _sprite.loadFont(vlw);
        _sprite.setTextSize(1);
    } else {
        _sprite.setFont(nullptr);
        _sprite.setTextSize(1);
    }

    _sprite.setTextColor(color, LABEL_BG);
    _sprite.setTextDatum(middle_center);
    _sprite.setTextWrap(false);

    char displayText[128] = {};
    const size_t sourceLen = strlen(text);

    size_t cut = sourceLen < sizeof(displayText) - 4
                   ? sourceLen
                   : sizeof(displayText) - 4;

    while (cut > 0 && ((uint8_t)text[cut] & 0xC0U) == 0x80U)
        --cut;

    bool shortened = cut < sourceLen;

    do {
        memcpy(displayText, text, cut);
        displayText[cut] = '\0';

        if (shortened)
            strlcat(displayText, "...", sizeof(displayText));

        if (_sprite.textWidth(displayText) <= maxW || cut == 0)
            break;

        shortened = true;
        --cut;

        while (cut > 0 && ((uint8_t)text[cut] & 0xC0U) == 0x80U)
            --cut;

    } while (true);

    _sprite.drawString(displayText, cx, cy);

    if (vlw)
        _sprite.unloadFont();
}

void vuMeterWidget::_drawLabels() {
    const int16_t clockZoneW = 110;   // left space reserved for the clock
    const int16_t labelLeft = clockZoneW;
    const int16_t labelTop  = 30;
    const int16_t labelW    = DSP_WIDTH - labelLeft - 10;
    const int16_t labelH    = 66;
    const int16_t lineH     = labelH / 3;
    const int16_t cx        = labelLeft + labelW / 2;
    const int16_t maxW      = labelW - 8;

    const uint8_t fsStation = pickFontSize(lineH * 0.80f);
    const uint8_t fsTitle   = pickFontSize(lineH * 0.85f);

    _drawLabelLine(_stationText, cx, labelTop + lineH / 2,
                   maxW, fsStation, LABEL_TEXT_STATION);

    _drawLabelLine(_title1Text, cx, labelTop + lineH + lineH / 2,
                   maxW, fsTitle, LABEL_TEXT_TITLE1);

    _drawLabelLine(_title2Text, cx, labelTop + lineH * 2 + lineH / 2,
                   maxW, fsTitle, LABEL_TEXT_TITLE2);
}

void vuMeterWidget::_clear() {
    if (_spriteReady) {
        _sprite.fillSprite(_bgcolor);
        _sprite.pushSprite(0, 0);
    } else {
        dsp.fillScreen(_bgcolor);
    }
}

void vuMeterWidget::_reset() {
    _phaseL = _phaseR = 0.0f;
    _levelL = _levelR = 0.0f;
    _lastAnim = 0;
}

#endif
