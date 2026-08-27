#pragma once

#include <WeatherSync.h>
#include <cstddef>
#include <vector>

namespace RE
{
    class TESObjectCELL;
    class TESObjectREFR;
}

namespace MPL::WindowObjectOverrides
{
    void Initialize();
    bool HasOverrides();
    bool HasOverrideFor(const RE::TESObjectREFR* a_reference);
    RE::FormID ProjectBase(std::string_view a_profile, RE::FormID a_base);
    bool ApplyToReference(
        RE::TESObjectREFR* a_reference,
        const std::vector<WeatherSync::WindowSyncProfile>& a_profiles);
    std::size_t ApplyToCell(
        RE::TESObjectCELL* a_cell,
        const std::vector<WeatherSync::WindowSyncProfile>& a_profiles);
}  // namespace MPL::WindowObjectOverrides
