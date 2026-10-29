#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::ModelPatcher
{
    struct Rule
    {
        std::string mesh;
        std::vector<std::string> geometries;
        bool externalEmittance = true;
    };

    inline std::string NormalizeMesh(std::string_view a_path)
    {
        std::string path(a_path);
        std::ranges::replace(path, '/', '\\');
        std::ranges::transform(path, path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (path.starts_with("meshes\\")) path.erase(0, 7);
        return path;
    }

    inline bool ValidRule(const Rule& a_rule)
    {
        const auto path = NormalizeMesh(a_rule.mesh);
        return a_rule.externalEmittance && path.ends_with(".nif") &&
            !path.starts_with('\\') && path.find(':') == std::string::npos &&
            path.find("..") == std::string::npos && !a_rule.geometries.empty() &&
            std::ranges::none_of(a_rule.geometries, [](const auto& name) { return name.empty(); });
    }

    inline bool MatchesGeometry(const Rule& a_rule, std::string_view a_name)
    {
        return std::ranges::any_of(a_rule.geometries, [&](const auto& name) { return name == a_name; });
    }
}
