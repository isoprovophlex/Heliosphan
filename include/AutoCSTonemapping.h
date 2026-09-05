#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
    class TESImageSpace;
}

namespace MPL::AutoCSTonemapping
{
    struct Settings
    {
        // Retained for legacy JSON compatibility; TuningUtil controls suppression.
        bool enabled = true;
        std::vector<std::string> plugins;
    };

    void ClearProfiles();
    void AddProfile(std::string a_id, Settings a_settings);
    bool GetProfileEnabled(std::string_view a_profile);
    bool SetProfileEnabled(std::string_view a_profile, bool a_enabled);
    bool IsProfileApplied(std::string_view a_profile);
    bool SetProfileSuppressed(std::string_view a_profile, bool a_suppressed);
    bool SetForcedTargets(RE::TESImageSpace* const* a_targets, std::size_t a_count);
    void ApplyStartup();
}
