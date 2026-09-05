#pragma once

#include <PluginIndex.h>
#include <filesystem>

#ifdef ENABLE_COMMONLIBSSE_TESTING
namespace MPL::PluginIndex::Testing
{
    bool Parse(
        const std::filesystem::path& a_path,
        const BuildOptions& a_options,
        Result& a_result,
        const std::unordered_map<RE::FormID, Placement>& a_winningPlacements,
        const std::unordered_set<RE::FormID>& a_exteriorCells = {});
}
#endif
