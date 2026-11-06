#pragma once

namespace RE
{
    class TESWeather;
}

namespace MPL::WeatherSoundFix
{
    bool Initialize();
    bool Enabled();
    void BeginWeatherChange(RE::TESWeather* a_weather) noexcept;
    void Poll() noexcept;
}
