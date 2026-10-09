#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY
#include "../../core/config.h"
#include "../display_select.h"
#include "bufferWidget.h"

BufferWidget::BufferWidget(yoDisplay* dsp, const VolumeWidgetConfig& conf) : _dsp(dsp), _spr(dsp) {
    _conf   = conf;
    _filled = -1;

    Widget::init({_conf.left, _conf.top, 0, WA_LEFT},
                 0,
                 config.theme.buff_bg);

    _spr.setPsram(true);
    _spr.setColorDepth(16);
    _spr.createSprite(_conf.width, _conf.height);
}

void BufferWidget::clear() {
    dsp.fillRect(_conf.left, _conf.top, _conf.width, _conf.height, config.theme.background);
}

void BufferWidget::setValue(uint32_t val, uint32_t maxVal) {
    if (!_active) return;
    if (maxVal == 0) maxVal = 1;

    int8_t filled = (int8_t)(((uint32_t)_conf.segments * val) / maxVal);
    if (filled > _conf.segments) filled = _conf.segments;
    if (filled == _filled) return;

    _filled = filled;
    _draw();
}

void BufferWidget::_draw() {
    if (!_active || _locked) return;

    _spr.fillSprite(config.theme.buff_bg);

    // Lapos strip mod: a spektrum alatt csak egy vekony felso elvalaszto
    // es alatta a teljes spektrumszelessegre elosztott bufferblokkok jelennek meg.
    // A nulla ikonmeret mellett a nagyon szeles/alacsony geometria kapcsolja
    // be, igy a tobbi kijelzomeret hagyomanyos BufferWidgetje valtozatlan.
    const bool strip = _conf.iconW == 0 && _conf.iconH == 0 &&
                       _conf.width >= _conf.height * 4;
    if (strip) {
        for (uint8_t i = 0; i < _conf.border && i < _conf.height; i++) {
            _spr.drawFastHLine(0, i, _conf.width, config.theme.buff_border);
        }

        const int startY = max((int)_conf.border + 1,
                               (int)_conf.height - (int)_conf.segHeight);
        // Spread the blocks evenly across the full strip. With fixed integer
        // block widths they previously stopped short of the spectrum edge.
        if (_conf.segments > 0) {
            const int span = (int)_conf.width + (int)_conf.segGap;
            for (uint8_t i = 0; i < _conf.segments; ++i) {
                const int x = (i * span) / _conf.segments;
                const int next = ((i + 1) * span) / _conf.segments;
                const int w = next - x - _conf.segGap;
                const uint16_t color = (i < _filled) ? config.theme.buff
                                                     : config.theme.buff_inactive;
                if (w > 0) { _spr.fillRect(x, startY, w, _conf.segHeight, color); }
            }
        }
        _spr.pushSprite(_conf.left, _conf.top);
        return;
    }

    // Háttér
    if (_conf.radius > 0)
        _spr.fillRoundRect(0, 0, _conf.width, _conf.height, _conf.radius, config.theme.buff_bg);
    else
        _spr.fillRect(0, 0, _conf.width, _conf.height, config.theme.buff_bg);

    // Keret – 1px border, nincs nagy belső margó
    if (_conf.border > 0) {
        for (uint8_t i = 0; i < _conf.border; i++) {
            if (_conf.radius > 0)
                _spr.drawRoundRect(i, i, _conf.width - i*2, _conf.height - i*2, _conf.radius, config.theme.buff_border);
            else
                _spr.drawRect(i, i, _conf.width - i*2, _conf.height - i*2, config.theme.buff_border);
        }
    }

    // Tartalom: szorosan a border melle, 2px padding.
    const int pad = _conf.border + 2;
    const int innerW = _conf.width - pad * 2;
    const int innerH = _conf.height - pad * 2;
    const bool vertical = _conf.height > _conf.width;

    if (vertical) {
        const int segmentsH = (_conf.segments * _conf.segHeight) +
                              ((_conf.segments - 1) * _conf.segGap);
        const bool hasIcon = _conf.iconW > 0 && _conf.iconH > 0;
        const int totalContentH = segmentsH + (hasIcon ? 5 + _conf.iconH : 0);
        const int offsetY = pad + max(0, (innerH - totalContentH) / 2);
        _drawVerticalSegments((_conf.width - _conf.segWidth) / 2, offsetY);
        if (hasIcon) {
            _drawIcon((_conf.width - _conf.iconW) / 2,
                      offsetY + segmentsH + 5, _conf.iconH);
        }
    } else {
        const int segmentsW = (_conf.segments * _conf.segWidth) +
                              ((_conf.segments - 1) * _conf.segGap);
        const bool hasIcon = _conf.iconW > 0 && _conf.iconH > 0;
        const int totalContentW = segmentsW + (hasIcon ? _conf.iconW + 3 : 0);
        const int offsetX = pad + (innerW - totalContentW) / 2;
        if (hasIcon) { _drawIcon(offsetX, pad, innerH); }
        _drawSegments(offsetX + (hasIcon ? _conf.iconW + 3 : 0), pad, innerH);
    }

    _spr.pushSprite(_conf.left, _conf.top);
}

// ── IKON: henger-stack (3 réteg, adatbázis/buffer stílus) ──────────────────
// Fusion-féle implementáció: minden réteg felső+alsó ellipszis ív + oldalsó vonalak
void BufferWidget::_drawIcon(int startX, int startY, int areaH) {
    uint16_t c  = config.theme.buff_icon;
    int      y  = startY + (areaH - _conf.iconH) / 2;

    int cx     = startX + _conf.iconW / 2;
    int rx     = _conf.iconW / 2;
    int ry     = max(1, (int)(_conf.iconH / 10));
    int layers = 3;
    int layerH = (_conf.iconH - ry) / layers;

    for (int lv = 0; lv < layers; lv++) {
        int topY = y + lv * layerH;
        int botY = topY + layerH;

        // Oldalsó vonalak
        _spr.drawLine(cx - rx, topY + ry, cx - rx, botY + ry, c);
        _spr.drawLine(cx + rx, topY + ry, cx + rx, botY + ry, c);

        // Felső ív (felső félellipszis)
        for (int dx = -rx; dx <= rx; dx++) {
            float t  = (float)dx / rx;
            int   ey = (int)(ry - ry * sqrtf(1.0f - t * t));
            _spr.drawPixel(cx + dx, topY + ey, c);
        }

        // Alsó ív (teljes ellipszis – látszik az "alap")
        for (int dx = -rx; dx <= rx; dx++) {
            float t  = (float)dx / rx;
            int   ey = (int)(ry * sqrtf(1.0f - t * t));
            _spr.drawPixel(cx + dx, botY + ry + ey, c);
            _spr.drawPixel(cx + dx, botY + ry - ey, c);
        }
    }
}

// ── SZEGMENSEK ──────────────────────────────────────────────────────────────
void BufferWidget::_drawSegments(int startX, int startY, int areaH) {
    int y = startY + (areaH - _conf.segHeight) / 2;

    for (uint8_t i = 0; i < _conf.segments; i++) {
        int      x   = startX + i * (_conf.segWidth + _conf.segGap);
        uint16_t col = (i < _filled) ? config.theme.buff : config.theme.buff_inactive;
        _spr.fillRect(x, y, _conf.segWidth, _conf.segHeight, col);
    }
}

void BufferWidget::_drawVerticalSegments(int startX, int startY) {
    for (uint8_t i = 0; i < _conf.segments; i++) {
        const int y = startY +
                      (_conf.segments - 1 - i) * (_conf.segHeight + _conf.segGap);
        const uint16_t col =
            (i < _filled) ? config.theme.buff : config.theme.buff_inactive;
        _spr.fillRect(startX, y, _conf.segWidth, _conf.segHeight, col);
    }
}

#endif // DSP_MODEL != DSP_DUMMY
