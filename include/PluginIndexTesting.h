#pragma once

#include <PluginIndex.h>
#include <filesystem>
#include <optional>
namespace MPL::VanillaIndex { class Catalog; struct Dataset; }

#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
namespace MPL::PluginIndex::Diagnostics { struct Metrics; }
#endif

#ifdef ENABLE_COMMONLIBSSE_TESTING
namespace MPL::PluginIndex::Testing
{
    struct ReaderControl
    {
        bool streamOnly = false;
        bool failMapping = false;
        std::optional<std::uint64_t> failReadAt;
        bool failRecovery = false;
        bool mapped = false;
        bool recovered = false;
    };

    bool Parse(
        const std::filesystem::path& a_path,
        const BuildOptions& a_options,
        Result& a_result,
        const std::unordered_map<RE::FormID, Placement>& a_winningPlacements,
        const std::unordered_set<RE::FormID>& a_exteriorCells = {}
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
        , Diagnostics::Metrics* a_diagnostics = nullptr
#endif
        , ReaderControl* a_readerControl = nullptr
        , VanillaIndex::Catalog* a_catalog = nullptr
        );
    bool ApplyDataset(const VanillaIndex::Dataset& a_data, const BuildOptions& a_options, Result& a_result,
        const std::unordered_map<RE::FormID, Placement>& a_winningPlacements,
        const std::unordered_set<RE::FormID>& a_exteriorCells = {}, RE::FormID a_mappingOffset = 0);
}
#endif
