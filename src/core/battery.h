#pragma once

#include <atomic>
#include <stdint.h>

struct BatteryReading {
    bool valid;
    uint16_t voltageMv;
    uint8_t percent;
};

// Samples the battery outside the display task. All accessors return a small
// value snapshot, so drawing the Settings page never starts an ADC conversion.
class BatteryMonitor {
  public:
    void begin();
    bool loop();
    bool enabled() const;
    bool preview() const;
    BatteryReading reading() const {
        const uint32_t value = _published.load(std::memory_order_relaxed);
        return { (value & 0x01000000UL) != 0,
                 static_cast<uint16_t>(value & 0xffffU),
                 static_cast<uint8_t>((value >> 16) & 0xffU) };
    }

  private:
    bool sample();
    bool _valid = false;
    uint16_t _voltageMv = 0;
    uint8_t _percent = 0;
    uint32_t _lastSampleMs = 0;
    std::atomic<uint32_t> _published{0};
};

extern BatteryMonitor batteryMonitor;
