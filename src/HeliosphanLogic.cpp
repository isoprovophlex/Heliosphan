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

    bool ShouldExcludeLocationType(
        const bool a_hasExcludedLocationType,
        const bool a_hasMultiLocationException)
    {
        return a_hasExcludedLocationType &&
               !a_hasMultiLocationException;
    }

    bool ShouldActivateProfileForPlugins(
        const bool a_hasPluginFilter,
        const bool a_hasLoadedPlugin)
    {
        return !a_hasPluginFilter || a_hasLoadedPlugin;
    }

    bool MatchesCellOriginPlugin(
        const bool a_hasPluginFilter,
        const bool a_hasOriginPlugin,
        const bool a_originPluginMatches)
    {
        return !a_hasPluginFilter ||
               (a_hasOriginPlugin && a_originPluginMatches);
    }

    const std::string* FindRegionOverride(
        const std::map<std::string, std::string>& a_overrides,
        const std::string_view a_sourceRegion)
    {
        if (a_sourceRegion.empty())
        {
            return nullptr;
        }
        const auto match = std::ranges::find_if(
            a_overrides,
            [&](const auto& a_override)
            {
                return a_override.first.size() == a_sourceRegion.size() &&
                       _strnicmp(
                           a_override.first.data(),
                           a_sourceRegion.data(),
                           a_sourceRegion.size()) == 0;
            });
        return match != a_overrides.end() ?
                   std::addressof(match->second) :
                   nullptr;
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

    bool ShouldAcceptLoadedCellEvent(
        const std::uint32_t a_eventCell,
        const std::uint32_t a_playerCell,
        const std::uint32_t a_pendingCell,
        const std::uint64_t a_eventGeneration,
        const std::uint64_t a_currentGeneration,
        const bool a_gameLoadPending,
        const bool a_transitionPending)
    {
        if (!a_eventCell ||
            a_eventGeneration != a_currentGeneration)
        {
            return false;
        }
        return (a_gameLoadPending && a_eventCell == a_playerCell) ||
               (a_transitionPending && a_eventCell == a_pendingCell);
    }

    bool IsReadinessBudgetActive(
        const bool a_gameLoadPending,
        const bool a_hasPlayerCell)
    {
        return !a_gameLoadPending || a_hasPlayerCell;
    }

    bool ShouldUseLightPlacerReferenceFallback(
        const bool a_filteredRule,
        const bool a_hasResolvedSource,
        const bool a_hasDirectSourceMatch,
        const bool a_hasWhitelistedReferenceMatch)
    {
        return a_filteredRule && a_hasResolvedSource &&
               !a_hasDirectSourceMatch &&
               a_hasWhitelistedReferenceMatch;
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
