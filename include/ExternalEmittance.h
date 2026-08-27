#pragma once

#include <PluginIndex.h>
#include <HeliosphanAPI.h>
#include <cstddef>
#include <string>
#include <vector>

namespace RE
{
    class TESObjectCELL;
    class TESObjectREFR;
}

namespace MPL::ExternalEmittance
{
    struct Settings
    {
        std::vector<std::string> forms;
        std::string target;
        std::string cellContainsTarget;
    };

    void ClearProfiles();
    void AddProfile(
        std::string a_profile,
        Settings a_settings,
        bool a_filtered,
        bool a_detailedLogging);
    bool RequiresPluginIndex();
    bool RequiresCompleteIndex();
    void ScheduleFinalReferenceInitialization();
    void PreparePlacementFilter();
    bool NeedsPlacement(RE::FormID a_reference, RE::FormID a_base);
    void Prepare(const PluginIndex::Result& a_index);
    void ProcessReference(RE::TESObjectREFR* a_reference);
    void BeginGameLoad();
    void ReplayCell(RE::TESObjectCELL* a_cell);
    std::size_t NotifyCellEmittanceRefreshed(RE::TESObjectCELL* a_cell);
    bool RegisterReferenceClient(
        const HeliosphanAPI::ReferenceCallbacks* a_callbacks);
}  // namespace MPL::ExternalEmittance
