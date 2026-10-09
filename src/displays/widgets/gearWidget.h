#pragma once

#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY
#    include <LovyanGFX.hpp>
#    include "widgetsconfig.h"
#    include "../display_select.h"
#    include "widget.h"

// Kodbol rajzolt, fajlrendszert nem hasznalo beallitas ikon.
class GearWidget : public Widget {
  public:
    GearWidget(yoDisplay* dsp, WidgetConfig cfg);

    void setSelected(bool selected);
    void invalidate();

    static constexpr uint16_t WIDTH  = 70;
    static constexpr uint16_t HEIGHT = 25;

  protected:
    void _draw() override;
    void _clear() override;
    void _reset() override { _dirty = true; }

  private:
    yoDisplay* _dsp = nullptr;
    bool _selected = false;
    bool _dirty = true;
};
#endif
