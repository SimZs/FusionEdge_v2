#pragma once

#include <stddef.h>
#include <stdint.h>

void _syncTask(void * pvParameters);
bool _getWeather();

struct ForecastDay {
    bool    valid;
    uint8_t weekday;
    uint8_t day;
    uint8_t month;
    float   minTemp;
    float   maxTemp;
    char    icon[8];
};

class TimeKeeper {
  public:
    volatile bool forceWeather;
    volatile bool forceTimeSync;
    volatile bool busy;
    char *weatherBuf;
    char  weatherIcon[8]; // OpenWeatherMap icon kód pl. "01d"
    float tempC;          // Temperature in the active OpenWeatherMap unit system
  public:
    TimeKeeper();
    bool loop0();
    bool loop1();
    void timeTask();
    void weatherTask();
    void requestForecastRefresh();
    uint8_t forecastSnapshot(ForecastDay* destination, uint8_t capacity) const;
    bool forecastReady() const { return _forecastCount[_forecastActive] > 0; }
    bool forecastLoading() const { return _forecastLoading; }
    void commitForecast(const ForecastDay* days, uint8_t count);
    void setForecastLoading(bool loading) { _forecastLoading = loading; }
    void waitAndReturnPlayer(uint8_t time_s);
    void waitAndDo(uint8_t time_s, void (*callback)());
  private:
    uint32_t _returnPlayerTime, _doAfterTime;
    void (*_aftercallback)();
    void (*_watchdogcallback)();
    void _upRSSI();
    void _upSDPos();
    void _upClock();
    void _upScreensaver();
    void _returnPlayer();
    void _doAfterWait();
    void _doWatchDog();

    ForecastDay _forecast[2][5];
    volatile uint8_t _forecastCount[2];
    volatile uint8_t _forecastActive;
    volatile bool    _forecastLoading;
    
};

extern TimeKeeper timekeeper;
