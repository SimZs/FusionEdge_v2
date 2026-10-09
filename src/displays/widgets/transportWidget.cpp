#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY

#    include "../../core/config.h"
#    include "transportWidget.h"

TransportWidget::TransportWidget(yoDisplay* display, WidgetConfig cfg)
    : _dsp(display) {
    Widget::init(cfg, config.theme.pmode, config.theme.background);
    _width = WIDTH;
    _backMove.width = WIDTH;
}

void TransportWidget::_drawPrevious(int16_t centerX, int16_t centerY,
                                    uint16_t color) {
    _dsp->fillRect(centerX - 12, centerY - 7, 3, 15, color);
    _dsp->fillTriangle(centerX - 8, centerY,
                       centerX + 7, centerY - 8,
                       centerX + 7, centerY + 8, color);
}

void TransportWidget::_drawPlayPause(int16_t centerX, int16_t centerY,
                                     uint16_t color) {
    if (_playing) {
        _dsp->fillRoundRect(centerX - 7, centerY - 8, 4, 16, 1, color);
        _dsp->fillRoundRect(centerX + 3, centerY - 8, 4, 16, 1, color);
    } else {
        _dsp->fillTriangle(centerX - 7, centerY - 8,
                           centerX - 7, centerY + 8,
                           centerX + 9, centerY, color);
    }
}

void TransportWidget::_drawNext(int16_t centerX, int16_t centerY,
                                uint16_t color) {
    _dsp->fillTriangle(centerX - 7, centerY - 8,
                       centerX + 8, centerY,
                       centerX - 7, centerY + 8, color);
    _dsp->fillRect(centerX + 9, centerY - 7, 3, 15, color);
}

void TransportWidget::_drawButton(uint8_t index, uint16_t color) {
    const int16_t x = _config.left + index * (BUTTON_WIDTH + BUTTON_GAP);
    const int16_t y = _config.top;
    _dsp->drawRoundRect(x, y, BUTTON_WIDTH, BUTTON_HEIGHT, 4, color);

    const int16_t centerX = x + BUTTON_WIDTH / 2;
    const int16_t centerY = y + BUTTON_HEIGHT / 2;
    if (index == 0) {
        _drawPrevious(centerX, centerY, color);
    } else if (index == 1) {
        _drawPlayPause(centerX, centerY, color);
    } else {
        _drawNext(centerX, centerY, color);
    }
}

void TransportWidget::_draw() {
    if (!_active || _locked || !_dsp) { return; }
    _clear();
    const uint16_t color = config.theme.pmode;
    for (uint8_t i = 0; i < 3; ++i) { _drawButton(i, color); }
    _dirty = false;
}

void TransportWidget::_clear() {
    if (!_dsp) { return; }
    _dsp->fillRect(_config.left, _config.top, WIDTH, HEIGHT,
                   config.theme.background);
}

void TransportWidget::setPlaying(bool playing) {
    if (_playing == playing && !_dirty) { return; }
    _playing = playing;
    _dirty = true;
    if (_active && !_locked) { _draw(); }
}

void TransportWidget::invalidate() {
    _dirty = true;
    if (_active && !_locked) { _draw(); }
}

#endif
