#pragma once

#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY
#    include <LovyanGFX.hpp>
#    include "widgetsconfig.h"
#    include "../display_select.h"
#    include "widget.h"

enum BluetoothWidgetState : uint8_t {
    BT_WIDGET_INACTIVE = 0,
    BT_WIDGET_ACTIVE,
    BT_WIDGET_DISABLED,
};

// Kis, fajlmuvelet nelkul rajzolt Bluetooth footer ikon.
class BluetoothWidget : public Widget {
  public:
    BluetoothWidget(yoDisplay* dsp, WidgetConfig cfg);

    void setState(BluetoothWidgetState state);
    void invalidate();

    static constexpr uint16_t WIDTH  = 30;
    static constexpr uint16_t HEIGHT = 25;
    static constexpr uint16_t FRAME_PAD_X = 4;
    static constexpr uint16_t FRAME_PAD_TOP = 9;
    static constexpr uint16_t FRAME_WIDTH = 40;
    static constexpr uint16_t FRAME_HEIGHT = 42;

  protected:
    void _draw() override;
    void _clear() override;
    void _reset() override { _dirty = true; }

  private:
    yoDisplay* _dsp = nullptr;
    BluetoothWidgetState _state = BT_WIDGET_INACTIVE;
    bool _dirty = true;

    void _drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color);
};
#endif
