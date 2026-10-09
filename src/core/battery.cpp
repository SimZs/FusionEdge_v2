#include "battery.h"

#include <Arduino.h>
#include "options.h"

BatteryMonitor batteryMonitor;

#if BATTERY_ADC_PIN != 255
namespace {
constexpr uint32_t kSampleIntervalMs = 2000;

// Rough 1S Li-ion open-circuit curve. Load and charging can move the voltage
// substantially, so this is an indicator rather than a fuel-gauge reading.
struct ChargePoint {
    uint16_t mv;
    uint8_t percent;
};
constexpr ChargePoint kChargeCurve[] = {
    {3200, 0}, {3400, 5}, {3550, 10}, {3650, 20},
    {3720, 30}, {3770, 40}, {3820, 50}, {3870, 60},
    {3930, 70}, {4000, 85}, {4100, 95}, {4200, 100},
};

uint8_t chargePercent(uint16_t mv) {
    if (mv <= kChargeCurve[0].mv) return 0;
    for (size_t i = 1; i < sizeof(kChargeCurve) / sizeof(kChargeCurve[0]); ++i) {
        if (mv <= kChargeCurve[i].mv) {
            const ChargePoint& lo = kChargeCurve[i - 1];
            const ChargePoint& hi = kChargeCurve[i];
            return lo.percent + static_cast<uint8_t>(
                (static_cast<uint32_t>(mv - lo.mv) * (hi.percent - lo.percent) +
                 (hi.mv - lo.mv) / 2) / (hi.mv - lo.mv));
        }
    }
    return 100;
}
} // namespace
#endif

bool BatteryMonitor::enabled() const {
#if BATTERY_PREVIEW || BATTERY_ADC_PIN != 255
    return true;
#else
    return false;
#endif
}

bool BatteryMonitor::preview() const {
#if BATTERY_PREVIEW
    return true;
#else
    return false;
#endif
}

void BatteryMonitor::begin() {
#if BATTERY_PREVIEW
    static_assert(BATTERY_PREVIEW_PERCENT >= 0 && BATTERY_PREVIEW_PERCENT <= 100,
                  "Battery preview percent must be in the 0-100 range");
    _valid = true;
    _voltageMv = 3900;
    _percent = BATTERY_PREVIEW_PERCENT;
    _published.store(0x01000000UL | (static_cast<uint32_t>(_percent) << 16) |
                         _voltageMv, std::memory_order_relaxed);
    Serial.printf("##[BAT]# DISPLAY PREVIEW ONLY - %u%%, no battery ADC measurement\n",
                  static_cast<unsigned>(_percent));
#elif BATTERY_ADC_PIN != 255
    static_assert(BATTERY_DIVIDER_BOTTOM_OHM > 0, "Battery divider bottom resistor must be nonzero");
    pinMode(BATTERY_ADC_PIN, INPUT);
    // Arduino-ESP32 needs the channel attached before per-pin attenuation.
    (void)analogRead(BATTERY_ADC_PIN);
    analogSetPinAttenuation(BATTERY_ADC_PIN, BATTERY_ADC_ATTENUATION);
    sample();
    const BatteryReading initial = reading();
    Serial.printf("##[BAT]# GPIO%u divider=%lu/%lu ohm cell=%u mV percent=%u valid=%u\n",
                  static_cast<unsigned>(BATTERY_ADC_PIN),
                  static_cast<unsigned long>(BATTERY_DIVIDER_TOP_OHM),
                  static_cast<unsigned long>(BATTERY_DIVIDER_BOTTOM_OHM),
                  static_cast<unsigned>(initial.voltageMv),
                  static_cast<unsigned>(initial.percent),
                  static_cast<unsigned>(initial.valid));
#endif
}

bool BatteryMonitor::loop() {
#if !BATTERY_PREVIEW && BATTERY_ADC_PIN != 255
    const uint32_t now = millis();
    if (now - _lastSampleMs >= kSampleIntervalMs) return sample();
#endif
    return false;
}

bool BatteryMonitor::sample() {
#if BATTERY_ADC_PIN != 255
    _lastSampleMs = millis();
    uint32_t totalMv = 0;
    uint32_t lowest = UINT32_MAX;
    uint32_t highest = 0;
    constexpr uint8_t samples = 12;
    for (uint8_t i = 0; i < samples; ++i) {
        const uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
        totalMv += mv;
        if (mv < lowest) lowest = mv;
        if (mv > highest) highest = mv;
    }
    const uint32_t adcMv = (totalMv - lowest - highest + (samples - 2) / 2) / (samples - 2);
    // A zero reading indicates no usable input; do not advertise an empty cell.
    if (adcMv < 50) {
        const bool changed = _valid;
        _valid = false;
        _published.store(0, std::memory_order_relaxed);
        return changed;
    }
    const uint64_t dividerNumerator =
        static_cast<uint64_t>(adcMv) *
        (BATTERY_DIVIDER_TOP_OHM + BATTERY_DIVIDER_BOTTOM_OHM);
    int32_t cellMv = static_cast<int32_t>(
        (dividerNumerator + BATTERY_DIVIDER_BOTTOM_OHM / 2) /
        BATTERY_DIVIDER_BOTTOM_OHM) + BATTERY_VOLTAGE_OFFSET_MV;
    // An impossible 1S voltage usually means the divider, pin or attenuation
    // does not match the actual board. Show "--" instead of a false 100%.
    if (cellMv < 2500 || cellMv > 4500) {
        const bool changed = _valid;
        _valid = false;
        _published.store(0, std::memory_order_relaxed);
        return changed;
    }
    const uint16_t measuredMv = static_cast<uint16_t>(cellMv);
    const uint16_t filteredMv = _valid
        ? static_cast<uint16_t>((static_cast<uint32_t>(_voltageMv) * 3 + measuredMv + 2) / 4)
        : measuredMv;
    const uint8_t measuredPercent = chargePercent(filteredMv);
    const bool changed = !_valid || measuredPercent != _percent ||
                         abs(static_cast<int>(filteredMv) - static_cast<int>(_voltageMv)) >= 10;
    _voltageMv = filteredMv;
    _percent = measuredPercent;
    _valid = true;
    _published.store(0x01000000UL | (static_cast<uint32_t>(_percent) << 16) |
                         _voltageMv, std::memory_order_relaxed);
    return changed;
#else
    return false;
#endif
}
