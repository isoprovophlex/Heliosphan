#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <RE/Skyrim.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MPL::PluginIndex
{
    struct Placement
    {
        RE::FormID reference = 0;
        RE::FormID base = 0;
        RE::FormID cell = 0;
        bool deleted = false;
    };

    struct Result
    {
        std::unordered_map<RE::FormID, Placement> placements;
        std::unordered_map<RE::FormID, std::string> editorIDs;
        std::size_t pluginsDiscovered = 0;
        std::size_t pluginsParsed = 0;
        std::size_t referencesRead = 0;
        std::size_t compressedReferences = 0;
        std::size_t minimallyScannedReferences = 0;
        std::size_t placementRecordsDiscarded = 0;
        std::uint64_t referencePayloadBytesSkipped = 0;
        std::size_t exteriorCellGroupsSkipped = 0;
        std::size_t excludedCellGroupsSkipped = 0;
        std::uint64_t cellGroupBytesSkipped = 0;
        std::vector<std::string> failedPlugins;
        bool complete = false;
    };

    struct BuildOptions
    {
        using EditorIDMap =
            std::unordered_map<RE::FormID, std::string>;
        using PlacementFilter =
            std::function<bool(RE::FormID, RE::FormID, RE::FormID)>;

        bool indexReferences = true;
        bool indexStaticEditorIDs = false;
        bool skipExteriorCells = false;
        std::unordered_set<RE::FormID> excludedCells;
        std::function<void(const EditorIDMap&)> preparePlacementFilter;
        PlacementFilter retainPlacement;
    };

    Result Build(const BuildOptions& a_options);
}  // namespace MPL::PluginIndex
