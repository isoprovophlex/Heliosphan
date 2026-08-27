#pragma once

#include <PluginIndex.h>
#include <RE/Skyrim.h>
#include <HeliosphanAPI.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace MPL::LightPlacer
{
    struct Settings
    {
        std::vector<std::string> lights;
        std::string externalEmittance;
    };

    struct SourcePlacement
    {
        RE::FormID reference = 0;
        RE::FormID base = 0;
        RE::FormID cell = 0;
        std::string model;
        std::vector<RE::FormID> filterIDs;
    };

    struct PatchRule
    {
        std::unordered_set<std::string> lights;
        std::string externalEmittance;
        std::vector<SourcePlacement> placements;
    };

    std::string NormalizeModelPath(std::string_view a_path);
    std::string StableFormKey(RE::FormID a_formID);

    void ClearProfiles();
    void AddProfile(
        std::string a_id,
        Settings a_settings,
        bool a_filtered);
    bool RequiresPluginIndex();
    bool RequiresCompleteInteriorIndex();
    void PreparePlacementFilter();
    bool NeedsPlacement(RE::FormID a_reference, RE::FormID a_base);
    void Prepare(const PluginIndex::Result& a_index);

    // Runs once from Window Sync's startup index. Matching Light Placer entries are
    // partitioned by cell, reloaded, and restored on disk before gameplay begins.
    void QueueStartupPatch(std::vector<PatchRule> a_rules);
    bool RegisterTransformer(
        const HeliosphanAPI::LightPlacerTransformer* a_transformer);
    bool RequestReload();
}  // namespace MPL::LightPlacer
