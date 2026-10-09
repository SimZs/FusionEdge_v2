#pragma once

#include "../../core/options.h"
#if DSP_MODEL != DSP_DUMMY
#    include <LovyanGFX.hpp>
#    include "widgetsconfig.h"
#    include "../display_select.h"
#    include "widget.h"

// Harom, kodbol rajzolt PLAYER transportgomb: elozo, play/pause, kovetkezo.
class TransportWidget : public Widget {
  public:
    TransportWidget(yoDisplay* dsp, WidgetConfig cfg);

    void setPlaying(bool playing);
    void invalidate();

    static constexpr uint16_t BUTTON_WIDTH = 100;
    static constexpr uint16_t BUTTON_HEIGHT = 25;
    static constexpr uint16_t BUTTON_GAP = 13;
    static constexpr uint16_t WIDTH = BUTTON_WIDTH * 3 + BUTTON_GAP * 2;
    static constexpr uint16_t HEIGHT = BUTTON_HEIGHT;

  protected:
    void _draw() override;
    void _clear() override;
    void _reset() override { _dirty = true; }

  private:
    void _drawButton(uint8_t index, uint16_t color);
    void _drawPrevious(int16_t centerX, int16_t centerY, uint16_t color);
    void _drawPlayPause(int16_t centerX, int16_t centerY, uint16_t color);
    void _drawNext(int16_t centerX, int16_t centerY, uint16_t color);

    yoDisplay* _dsp = nullptr;
    bool _playing = false;
    bool _dirty = true;
};
#endif
