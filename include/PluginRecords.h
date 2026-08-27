#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

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

    std::optional<FormID> FindBaseForm(std::span<const std::byte> a_data);
    std::optional<std::string> FindEditorID(
        std::span<const std::byte> a_data);
    void MergePlacement(
        std::unordered_map<FormID, Placement>& a_placements,
        FormID a_reference,
        Placement a_placement);
}  // namespace MPL::PluginRecords
