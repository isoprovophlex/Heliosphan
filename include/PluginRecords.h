#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MPL::PluginRecords
{
    using FormID = std::uint32_t;

    struct Placement
    {
        FormID reference = 0;
        FormID base = 0;
        FormID cell = 0;
        bool deleted = false;
    };

    template <class Plugin>
    std::optional<std::vector<Plugin>> OrderActivePlugins(
        std::span<const Plugin> a_active,
        std::span<const Plugin> a_loadOrder)
    {
        std::unordered_set<Plugin> remaining(a_active.begin(), a_active.end());
        std::vector<Plugin> ordered;
        ordered.reserve(remaining.size());
        for (const auto plugin : a_loadOrder)
        {
            if (remaining.erase(plugin))
            {
                ordered.push_back(plugin);
            }
        }
        if (!remaining.empty())
        {
            return std::nullopt;
        }
        return ordered;
    }

    std::optional<FormID> FindBaseForm(std::span<const std::byte> a_data);
    std::optional<std::string> FindEditorID(
        std::span<const std::byte> a_data);
    void MergePlacement(
        std::unordered_map<FormID, Placement>& a_placements,
        FormID a_reference,
        Placement a_placement);
}  // namespace MPL::PluginRecords
