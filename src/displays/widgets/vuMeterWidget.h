#pragma once

#include "../../core/options.h"

#if DSP_MODEL != DSP_DUMMY && defined(USE_VU_METER_SCREENSAVER)

#include "widget.h"
#include <LovyanGFX.hpp>

class vuMeterWidget : public Widget {
public:
    vuMeterWidget();
    virtual ~vuMeterWidget();

    void init(WidgetConfig conf, uint16_t fgcolor, uint16_t bgcolor) override;
    void init(uint16_t bgcolor);

    void setLevel(float left, float right);
    void setStation(const char* station);
    void setTitle(const char* title1, const char* title2);
    void setTime(const char* time);
    bool ready();

    void loop() override;
    void _draw() override;
    void _clear() override;
    void _reset() override;

private:
    bool _createSprite();
    void _calcGeometry();
    void _drawFrame();
    void _drawClock();
    void _drawMeter(int x, int y, int w, int h, const char* label, float phase);
    void _drawScale(int x, int y, int w, int h);
    void _drawNeedle(int x, int y, int w, int h, float phase);
    void _drawDigital();
    void _drawLabels();
    void _drawLabelLine(const char* text, int16_t cx, int16_t cy, int16_t maxW,
                         uint8_t fontSize, uint16_t color);
    float _phaseToAngle(float phase) const;

    float _levelL = 0.0f, _levelR = 0.0f;
    float _phaseL = 0.0f, _phaseR = 0.0f;
    uint32_t _lastAnim = 0;

    bool _geomReady = false;
    bool _spriteReady = false;
    bool _spriteAllocAttempted = false;

    int _meterXL = 0;
    int _meterXR = 0;
    int _meterY  = 0;
    int _meterW  = 0;
    int _meterH  = 0;
    float _meterVScale = 1.0f;   // meterH / reference 220px -- scales needle
    float _meterHScale = 1.0f;   // meterW / reference 232px -- pivots, "VU"/
                                  // label text rows, scale-arc radius, etc.
                                  // so they land inside the box on any panel
                                  // size instead of the fixed pixel offsets
                                  // tuned for the original 480x320 design.

    LGFX_Sprite _sprite;

    char _stationText[96] = "";
    char _title1Text[128] = "";
    char _title2Text[128] = "";
    char _time[8] = "";
};

#endif
