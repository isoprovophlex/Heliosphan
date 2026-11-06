#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::ManifestSettings
{
    struct Manifest
    {
        std::string id;
        std::filesystem::path source;
        std::optional<bool> speedLogs;
        std::optional<bool> notifications;
    };

    std::vector<Manifest> Load(const std::filesystem::path& a_root);
    const Manifest* Find(const std::vector<Manifest>& a_manifests, std::string_view a_id);
    bool WriteBoolean(const std::filesystem::path& a_source, std::string_view a_id,
        std::string_view a_key, bool a_enabled);
}
