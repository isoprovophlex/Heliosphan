#include <HeliosphanLogic.h>
#include <algorithm>
#include <cstring>

namespace MPL::HeliosphanLogic
{
    CellExclusion EvaluateCellExclusion(
        const bool a_active,
        const bool a_explicitlyIncluded,
        const bool a_explicitlyExcluded,
        const bool a_excludedLocationType)
    {
        if (!a_active || a_explicitlyIncluded)
        {
            return CellExclusion::none;
        }
        if (a_explicitlyExcluded)
        {
            return CellExclusion::explicitCell;
        }
        return a_excludedLocationType ?
                   CellExclusion::locationType :
                   CellExclusion::none;
    }

    bool ProfilePrecedes(
        const std::optional<ProfilePriority>& a_left,
        const std::optional<ProfilePriority>& a_right,
        const std::string_view a_leftFallback,
        const std::string_view a_rightFallback)
    {
        if (a_left && a_right)
        {
            if (a_left->usesPluginLoadOrder !=
                a_right->usesPluginLoadOrder)
            {
                return !a_left->usesPluginLoadOrder;
            }
            if (a_left->loadOrder != a_right->loadOrder)
            {
                return a_left->loadOrder < a_right->loadOrder;
            }
            return a_left->id < a_right->id;
        }
        if (a_left.has_value() != a_right.has_value())
        {
            return a_left.has_value();
        }
        const auto order = _strnicmp(
            a_leftFallback.data(),
            a_rightFallback.data(),
            std::min(a_leftFallback.size(), a_rightFallback.size()));
        if (order != 0)
        {
            return order < 0;
        }
        if (a_leftFallback.size() != a_rightFallback.size())
        {
            return a_leftFallback.size() < a_rightFallback.size();
        }
        return a_leftFallback < a_rightFallback;
    }

    bool ShouldApplyTransition(
        const std::uint64_t a_scheduledGeneration,
        const std::uint64_t a_currentGeneration,
        const bool a_hasPendingTransition)
    {
        return a_scheduledGeneration == a_currentGeneration &&
               a_hasPendingTransition;
    }

    bool IsPluginLoaded(
        const bool a_fullPluginLoaded,
        const bool a_lightPluginLoaded)
    {
        return a_fullPluginLoaded || a_lightPluginLoaded;
    }

    bool IsWeatherSyncConfigurationValid(
        const bool a_enabled,
        const std::string_view a_weatherPrefix,
        const std::string_view a_regionPrefix)
    {
        return !a_enabled ||
               (!a_weatherPrefix.empty() && !a_regionPrefix.empty());
    }

    EmittanceBlockStatus EvaluateEmittanceBlock(
        const bool a_hasForms,
        const bool a_hasLightPlacerEntries,
        const bool a_hasTarget,
        const bool a_requiresCellFilter,
        const bool a_windowSyncEnabled,
        const bool a_hasCellFilter)
    {
        if (!a_hasForms && !a_hasLightPlacerEntries)
        {
            return EmittanceBlockStatus::disabledNoWork;
        }
        if (!a_hasTarget)
        {
            return EmittanceBlockStatus::disabledMissingTarget;
        }
        if (a_requiresCellFilter &&
            (!a_windowSyncEnabled || !a_hasCellFilter))
        {
            return EmittanceBlockStatus::disabledMissingCellFilter;
        }
        return EmittanceBlockStatus::enabled;
    }
}  // namespace MPL::HeliosphanLogic
