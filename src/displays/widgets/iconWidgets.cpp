#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY

#include <LittleFS.h>
#include "../../core/config.h"
#include "../../core/fonts.h"
#include "../../core/stationlogo.h"
#include "../display_select.h"
#include "iconWidgets.h"

// ═══════════════════════════════════════════════════════════════════════════
// WeatherIconWidget
// ═══════════════════════════════════════════════════════════════════════════
WeatherIconWidget::~WeatherIconWidget() {
    _freePng();
}

void WeatherIconWidget::init(WidgetConfig wconf, uint16_t bgcolor) {
    Widget::init(wconf, 0, bgcolor);
    _bgcolor     = bgcolor;
    _iconCode[0] = '\0';
    _temp[0]     = '\0';
}

void WeatherIconWidget::_freePng() {
    if (_pngBuf) { free(_pngBuf); _pngBuf = nullptr; }
    _pngSize = 0;
}

bool WeatherIconWidget::_loadPng(const char* code) {
    _freePng();
    char path[48];
    snprintf(path, sizeof(path), "/images/weather/%s.png", code);
    File f = LittleFS.open(path, "r");
    if (!f) { Serial.printf("[WICON] not found: %s\n", path); return false; }
    size_t sz = f.size();
    uint8_t* buf = (uint8_t*)ps_malloc(sz);
    if (!buf) { f.close(); Serial.println("[WICON] ps_malloc failed"); return false; }
    f.read(buf, sz);
    f.close();
    _pngBuf  = buf;
    _pngSize = sz;
    return true;
}

void WeatherIconWidget::setIcon(const char* code) {
    if (!code || code[0] == '\0') return;
    if (strncmp(_iconCode, code, sizeof(_iconCode)) == 0 && _pngBuf) return;
    strlcpy(_iconCode, code, sizeof(_iconCode));
    _loadPng(code);
    if (_active && !_locked) { _clear(); _draw(); }
}

void WeatherIconWidget::setWeather(const char* code, float tempC) {
    if (code && code[0] != '\0' && (strncmp(_iconCode, code, sizeof(_iconCode)) != 0 || !_pngBuf)) {
        strlcpy(_iconCode, code, sizeof(_iconCode));
        _loadPng(code);
    }
#ifdef IMPERIALUNIT
    snprintf(_temp, sizeof(_temp), "%.0f \xC2\xB0" "F", tempC);
#else
    snprintf(_temp, sizeof(_temp), "%.0f \xC2\xB0" "C", tempC);
#endif
    _hasTemp = true;
    if (_active && !_locked) { _clear(); _draw(); }
}

void WeatherIconWidget::setTemp(float tempC) {
#ifdef IMPERIALUNIT
    snprintf(_temp, sizeof(_temp), "%.0f °F", tempC);
#else
    snprintf(_temp, sizeof(_temp), "%.0f °C", tempC);
#endif
    _hasTemp = true;
    if (_active && !_locked) { _clear(); _draw(); }
}

void WeatherIconWidget::_draw() {
    if (!_active) return;

    // PNG ikon kirajzolása
    if (_pngBuf && _pngSize > 0) {
        dsp.drawPng(_pngBuf, _pngSize, _config.left, _config.top, ICO_W, ICO_H);
    }

    // Hőmérséklet szöveg az ikon mellett balra
    if (_temp[0] == '\0') return;
    uint8_t* fnt = vlwBySize(_config.textsize);
    if (fnt) { dsp.loadFont(fnt); } else { dsp.setTextSize(1); }
    uint16_t textW = dsp.textWidth(_temp);
    uint16_t textH = dsp.fontHeight();
    dsp.setTextColor(config.theme.weatherIconTxt, _bgcolor);
    dsp.setTextDatum(top_center);
    //dsp.drawString(_temp, _config.left - textW - 2, _config.top + ICO_H/2);
    dsp.drawString(_temp, _config.left - textW - 2, _config.top + ICO_H/2 - textH/2);
    if (fnt) dsp.unloadFont();
}

void WeatherIconWidget::_clear() {
    if (!_active) return;
    if (_hasTemp) {
        // Szöveg balra van az ikontól: max szövegszélesség becslése
        uint16_t maxTextW = _config.textsize * 5 + 4; // ~5 karakter szélesség + gap
        dsp.fillRect(_config.left - maxTextW, _config.top, ICO_W + maxTextW, ICO_H, _bgcolor);
    } else {
        dsp.fillRect(_config.left, _config.top, ICO_W, ICO_H, _bgcolor);
    }
}


// ═══════════════════════════════════════════════════════════════════════════
// StationIconWidget
// ═══════════════════════════════════════════════════════════════════════════
StationIconWidget::~StationIconWidget() {
    _freePng();
    _deleteSprite();
}

void StationIconWidget::init(WidgetConfig wconf, uint16_t bgcolor) {
    Widget::init(wconf, 0, bgcolor);
    _bgcolor = bgcolor;
    _ensureSprite();
}

void StationIconWidget::_ensureSprite() {
    if (_spr) return;
    _spr = new LGFX_Sprite(&dsp);
    if (!_spr) return;
    _spr->setColorDepth(16);
    _spr->setPsram(true);
    if (_spr->createSprite(ICO_W, ICO_H) == nullptr) {
        delete _spr;
        _spr = nullptr;
    }
}

void StationIconWidget::_deleteSprite() {
    if (!_spr) return;
    _spr->deleteSprite();
    delete _spr;
    _spr = nullptr;
}

void StationIconWidget::_pushSprite() {
    if (!_spr) return;
#if DSP_MODEL == DSP_AXS15231B
    auto* pixels = static_cast<uint16_t*>(_spr->getBuffer());
    if (pixels && dsp.blitFrameBlock(_config.left, _config.top, _spr->width(), _spr->height(), pixels)) { return; }
#endif
    _spr->pushSprite(_config.left, _config.top);
}

void StationIconWidget::_freePng() {
    if (_buf) { free(_buf); _buf = nullptr; }
    _sz    = 0;
    _valid = false;
    _jpeg = false;
    _imageWidth = ICO_W;
    _imageHeight = ICO_H;
    _coverGeneration = 0;
}

void StationIconWidget::_setImageSize() {
    _imageWidth = ICO_W;
    _imageHeight = ICO_H;
    if (!_buf || _sz < 24) return;

    if (!_jpeg && _buf[0] == 0x89 && _buf[1] == 'P' && _buf[2] == 'N' && _buf[3] == 'G') {
        _imageWidth = static_cast<uint16_t>((static_cast<uint32_t>(_buf[16]) << 24) |
                                            (static_cast<uint32_t>(_buf[17]) << 16) |
                                            (static_cast<uint32_t>(_buf[18]) << 8) | _buf[19]);
        _imageHeight = static_cast<uint16_t>((static_cast<uint32_t>(_buf[20]) << 24) |
                                             (static_cast<uint32_t>(_buf[21]) << 16) |
                                             (static_cast<uint32_t>(_buf[22]) << 8) | _buf[23]);
        return;
    }

    if (_jpeg && _buf[0] == 0xFF && _buf[1] == 0xD8) {
        size_t offset = 2;
        while (offset + 9 < _sz) {
            if (_buf[offset] != 0xFF) { ++offset; continue; }
            const uint8_t marker = _buf[offset + 1];
            if (marker == 0xD8 || marker == 0xD9) { offset += 2; continue; }
            if (offset + 3 >= _sz) break;
            const uint16_t segmentLength = (static_cast<uint16_t>(_buf[offset + 2]) << 8) |
                                           _buf[offset + 3];
            if (segmentLength < 2 || offset + 2 + segmentLength > _sz) break;
            const bool isSof = (marker >= 0xC0 && marker <= 0xC3) ||
                               (marker >= 0xC5 && marker <= 0xC7) ||
                               (marker >= 0xC9 && marker <= 0xCB) ||
                               (marker >= 0xCD && marker <= 0xCF);
            if (isSof && segmentLength >= 7) {
                _imageHeight = (static_cast<uint16_t>(_buf[offset + 5]) << 8) | _buf[offset + 6];
                _imageWidth = (static_cast<uint16_t>(_buf[offset + 7]) << 8) | _buf[offset + 8];
                return;
            }
            offset += 2 + segmentLength;
        }
    }
}

bool StationIconWidget::_loadPng(const char* path) {
    _freePng();
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t sz = f.size();
    if (sz == 0) { f.close(); return false; }
    uint8_t* buf = (uint8_t*)ps_malloc(sz);
    if (!buf) { f.close(); return false; }
    const size_t readSz = f.read(buf, sz);
    f.close();
    if (readSz != sz) {
        free(buf);
        return false;
    }
    const bool png = sz >= 8 && buf[0] == 0x89 && buf[1] == 'P' &&
                     buf[2] == 'N' && buf[3] == 'G' && buf[4] == 0x0D &&
                     buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A;
    const bool jpeg = sz >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF;
    if (!png && !jpeg) {
        free(buf);
        return false;
    }
    _buf   = buf;
    _sz    = sz;
    _jpeg  = jpeg;
    _valid = true;
    _dirty = true;
    _coverGeneration = 0;
    _setImageSize();
    return true;
}

void StationIconWidget::setCover(uint8_t* data, size_t size, bool jpeg, uint32_t generation) {
    if (!data || size == 0 || generation == 0) {
        if (data) free(data);
        return;
    }
    if (_coverGeneration == generation && _valid) {
        free(data);
        if (_active && !_locked) _draw();
        return;
    }

    _freePng();
    _buf = data;
    _sz = size;
    _jpeg = jpeg;
    _valid = true;
    _dirty = true;
    _coverGeneration = generation;
    _path[0] = '\0';
    _setImageSize();
    if (_active && !_locked) _draw();
}

void StationIconWidget::setStation(const char* stationName, uint8_t playMode) {
    char path[STATION_LOGO_PATH_LEN];
    if (playMode == DPS_WEB) {
        StationLogoInfo logo;
        if (!stationLogoLookup(stationName, logo) ||
            !stationLogoLocalPath(logo, path, sizeof(path))) {
            strlcpy(path, "/images/stations/plmodeweb.png", sizeof(path));
        }
    } else if (playMode == DPS_SDCARD) {
        strlcpy(path, "/images/stations/plmodesd.png", sizeof(path));
#ifdef USE_BLUETOOTH
    } else if (playMode == DPS_BLUETOOTH) {
        strlcpy(path, "/images/stations/plmodebt.png", sizeof(path));
#endif
    } else if (playMode == DPS_DLNA) {
        strlcpy(path, "/images/stations/plmodedlna.png", sizeof(path));
    } else {
        strlcpy(path, "/images/stations/plmodeweb.png", sizeof(path));
    }
    if (!_valid || _coverGeneration != 0 || strcmp(_path, path) != 0) {
        if (!_loadPng(path)) {
            _path[0] = '\0';
            return;
        }
        strlcpy(_path, path, sizeof(_path));
    }
    if (_active && !_locked && _valid) { _draw(); }
}

void StationIconWidget::clearStation() {
    _freePng();
    _path[0] = '\0';
    if (_active && !_locked) _clear();
}

void StationIconWidget::_draw() {
    if (!_active || !_valid || !_buf || _sz == 0) return;
    _ensureSprite();
    if (!_spr) return;
    if (!_dirty) {
        _pushSprite();
        return;
    }
    _spr->fillSprite(_bgcolor);
    // A helyi állomás-/fallback ikonokat és a hálózati cover-artot azonos
    // módon, képarányt tartva illesztjük a 120x120-as PLAYER widgetbe. Így a
    // képernyővédőhöz feltöltött nagyobb PNG-t nem kell külön PLAYER-méretű
    // változatban is tárolni.
    const float scaleX = _imageWidth > 0 ? static_cast<float>(ICO_W) / _imageWidth : 1.0f;
    const float scaleY = _imageHeight > 0 ? static_cast<float>(ICO_H) / _imageHeight : 1.0f;
    const float scale = scaleX < scaleY ? scaleX : scaleY;
    if (_jpeg) {
        _spr->drawJpg(_buf, _sz, 0, 0, ICO_W, ICO_H, 0, 0,
                      scale, scale, datum_t::middle_center);
    } else {
        _spr->drawPng(_buf, _sz, 0, 0, ICO_W, ICO_H, 0, 0,
                      scale, scale, datum_t::middle_center);
    }

    // Lekerekített sarok maszk - pixel-pontos módszer:
    // Csak a sarokzónában lévő pixeleket fedi le, amelyek kívül esnek a köríven
    const int16_t r  = 15;

    // A kör középpontja minden saroknál r távolságra van befelé
    // Tehát a sarok pixelétől (0,0) a kör közepe (r-1, r-1) irányban van
    for (int16_t dy = 0; dy < r; dy++) {
        for (int16_t dx = 0; dx < r; dx++) {
            int16_t cx = r - 1 - dx;  // távolság a kör középpontjától
            int16_t cy = r - 1 - dy;
            if ((cx * cx + cy * cy) > (r * r)) {
                // Bal-felső
                _spr->drawPixel(dx,           dy,           _bgcolor);
                // Jobb-felső
                _spr->drawPixel(ICO_W-1 - dx, dy,           _bgcolor);
                // Bal-alsó
                _spr->drawPixel(dx,           ICO_H-1 - dy, _bgcolor);
                // Jobb-alsó
                _spr->drawPixel(ICO_W-1 - dx, ICO_H-1 - dy, _bgcolor);
            }
        }
    }
    _dirty = false;
    _pushSprite();
}

void StationIconWidget::_clear() {
    _ensureSprite();
    if (_spr) {
        _spr->fillSprite(_bgcolor);
        _pushSprite();
    } else {
        dsp.fillRect(_config.left, _config.top, ICO_W, ICO_H, _bgcolor);
    }
}

#endif // DSP_MODEL != DSP_DUMMY
