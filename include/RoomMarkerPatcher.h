#pragma once

#include <string>
#include <vector>

namespace MPL::RoomMarkerPatcher
{
    void ConfigureCell(
        RE::TESObjectCELL* a_cell,
        bool a_enabled,
        const std::vector<std::string>& a_excludedPlugins);
    void ProcessReference(RE::TESObjectREFR* a_reference);
}  // namespace MPL::RoomMarkerPatcher
