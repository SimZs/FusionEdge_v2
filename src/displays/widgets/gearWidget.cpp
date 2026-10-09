#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY

#    include "../../core/config.h"
#    include "gearWidget.h"

GearWidget::GearWidget(yoDisplay* display, WidgetConfig cfg) : _dsp(display) {
    Widget::init(cfg, config.theme.status_inactive, config.theme.background);
    _width = WIDTH;
    _backMove.width = WIDTH;
}

void GearWidget::_draw() {
    if (!_active || _locked || !_dsp) { return; }

    _clear();
    const uint16_t color = _selected ? config.theme.wifi_border
                                     : config.theme.status_inactive;
    const int16_t left = _config.left;
    const int16_t top = _config.top;

    // Szimmetrikus "tovabbi lehetosegek" ikon, a footer tobbi elemevel
    // azonos lekerekitett dobozban.
    _dsp->drawRoundRect(left, top, WIDTH, HEIGHT, 4, color);
    const int16_t centerX = left + WIDTH / 2;
    _dsp->fillCircle(centerX, top + 6, 2, color);
    _dsp->fillCircle(centerX, top + 12, 2, color);
    _dsp->fillCircle(centerX, top + 18, 2, color);
    _dirty = false;
}

void GearWidget::_clear() {
    if (!_dsp) { return; }
    _dsp->fillRect(_config.left, _config.top, WIDTH, HEIGHT,
                   config.theme.background);
}

void GearWidget::setSelected(bool selected) {
    if (_selected == selected && !_dirty) { return; }
    _selected = selected;
    _dirty = true;
    if (_active && !_locked) { _draw(); }
}

void GearWidget::invalidate() {
    _dirty = true;
    if (_active && !_locked) { _draw(); }
}

#endif
