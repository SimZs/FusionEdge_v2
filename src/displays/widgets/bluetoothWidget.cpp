#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY

#    include "../../core/config.h"
#    include "bluetoothWidget.h"

BluetoothWidget::BluetoothWidget(yoDisplay* display, WidgetConfig cfg)
    : _dsp(display) {
    Widget::init(cfg, config.theme.status_inactive, config.theme.background);
    _width = WIDTH;
    _backMove.width = WIDTH;
}

void BluetoothWidget::_drawThickLine(int16_t x0, int16_t y0,
                                     int16_t x1, int16_t y1,
                                     uint16_t color) {
    const int16_t left = _config.left;
    const int16_t top  = _config.top;
    _dsp->drawLine(left + x0, top + y0, left + x1, top + y1, color);
    _dsp->drawLine(left + x0 + 1, top + y0, left + x1 + 1, top + y1,
                   color);
}

void BluetoothWidget::_draw() {
    if (!_active || _locked || !_dsp) { return; }

    _clear();
    // Az aktiv Bluetooth a Windows/PC feluletekhez kozelebb allo,
    // sotetebb keket kap; a Wi-Fi temat ettol fuggetlenul hagyjuk.
    constexpr uint16_t bluetoothActive = 0x03DA; // RGB(0, 120, 215)
    const uint16_t color = _state == BT_WIDGET_ACTIVE
                               ? bluetoothActive
                               : config.theme.status_inactive;

    _dsp->drawRoundRect(_config.left - FRAME_PAD_X,
                        _config.top - FRAME_PAD_TOP,
                        FRAME_WIDTH, FRAME_HEIGHT, 4, config.theme.pmode);

    // Bluetooth-runa: kozepso szar, ket jobb oldali haromszog es a ket
    // balrol erkezo atlo. Nem hasznal LittleFS kepfajlt.
    _drawThickLine(15, 2, 15, 23, color);
    _drawThickLine(15, 2, 22, 8, color);
    _drawThickLine(22, 8, 15, 13, color);
    _drawThickLine(15, 13, 22, 18, color);
    _drawThickLine(22, 18, 15, 23, color);
    _drawThickLine(7, 7, 15, 13, color);
    _drawThickLine(7, 19, 15, 13, color);

    if (_state == BT_WIDGET_DISABLED) {
        constexpr uint16_t slashColor = 0xF800; // piros RGB565
        _drawThickLine(5, 3, 25, 22, slashColor);
        _drawThickLine(5, 4, 25, 23, slashColor);
    }
    _dirty = false;
}

void BluetoothWidget::_clear() {
    if (!_dsp) { return; }
    _dsp->fillRect(_config.left - FRAME_PAD_X,
                   _config.top - FRAME_PAD_TOP,
                   FRAME_WIDTH, FRAME_HEIGHT,
                   config.theme.background);
}

void BluetoothWidget::setState(BluetoothWidgetState state) {
    if (_state == state && !_dirty) { return; }
    _state = state;
    _dirty = true;
    if (_active && !_locked) { _draw(); }
}

void BluetoothWidget::invalidate() {
    _dirty = true;
    if (_active && !_locked) { _draw(); }
}

#endif
