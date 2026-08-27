#pragma once

#include <cstdint>

namespace RE
{
    class TESObjectCELL;
    class TESObjectREFR;
    class TESRegion;
    class TESWeather;
}  // namespace RE

namespace SKSE
{
    class SerializationInterface;
}

namespace MPL::WindowSync
{
    void RegisterObjectOverrideProjection();
    void Initialize();
    void ProcessReference(RE::TESObjectREFR* a_reference);
    RE::TESRegion* CaptureSourceRegion();
    void PrepareCellChange(
        RE::TESObjectCELL* a_destination,
        RE::TESWeather* a_sourceWeather,
        RE::TESRegion* a_sourceRegion);
    void FinishCellChange(const RE::TESObjectCELL* a_destination);

    void Save(SKSE::SerializationInterface*);
    void Load(SKSE::SerializationInterface*, std::uint32_t a_version, std::uint32_t a_length);
    void Reset();
}  // namespace MPL::WindowSync
