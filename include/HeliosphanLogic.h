#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace MPL::HeliosphanLogic
{
    enum class CellExclusion
    {
        none,
        explicitCell,
        locationType,
    };

    struct ProfilePriority
    {
        std::string_view id;
        std::uint32_t loadOrder = 0;
        bool usesPluginLoadOrder = false;
    };

    enum class EmittanceBlockStatus
    {
        disabledNoWork,
        enabled,
        disabledMissingTarget,
        disabledMissingCellFilter,
    };

    CellExclusion EvaluateCellExclusion(
        bool a_active,
        bool a_explicitlyIncluded,
        bool a_explicitlyExcluded,
        bool a_excludedLocationType);

    bool ProfilePrecedes(
        const std::optional<ProfilePriority>& a_left,
        const std::optional<ProfilePriority>& a_right,
        std::string_view a_leftFallback,
        std::string_view a_rightFallback);

    bool ShouldApplyTransition(
        std::uint64_t a_scheduledGeneration,
        std::uint64_t a_currentGeneration,
        bool a_hasPendingTransition);

    bool IsPluginLoaded(bool a_fullPluginLoaded, bool a_lightPluginLoaded);

    bool IsWeatherSyncConfigurationValid(
        bool a_enabled,
        std::string_view a_weatherPrefix,
        std::string_view a_regionPrefix);

    EmittanceBlockStatus EvaluateEmittanceBlock(
        bool a_hasForms,
        bool a_hasLightPlacerEntries,
        bool a_hasTarget,
        bool a_requiresCellFilter,
        bool a_windowSyncEnabled,
        bool a_hasCellFilter);
}  // namespace MPL::HeliosphanLogic
