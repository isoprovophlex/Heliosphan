#include <CellClassifier.h>
#include <FormResolver.h>
#include <HeliosphanLogic.h>
#include <ObjectOverrides.h>
#include <Heliosphan.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace MPL::CellClassifier
{
    namespace
    {
        struct Rule
        {
            std::string owner;
            Settings settings;
            bool detailedLogging = false;
            std::unordered_set<RE::FormID> forms;
            std::unordered_set<RE::FormID> references;
            std::unordered_set<RE::FormID> matchedReferences;
            std::unordered_set<RE::FormID> includedCells;
            std::unordered_set<RE::FormID> excludedCells;
            std::vector<RE::BGSKeyword*> excludedLocationTypes;
            bool active = true;
            bool cellFiltersResolved = false;
        };

        struct State
        {
            struct BaseProjection
            {
                RE::FormID original = 0;
                RE::FormID projected = 0;
            };

            std::mutex lock;
            std::vector<Rule> rules;
            std::unordered_map<RE::FormID, std::vector<std::string>> cells;
            std::vector<RE::FormID> cellOrder;
            std::unordered_map<RE::FormID, BaseProjection> projectedBases;
            bool placementFilterPrepared = false;
            std::atomic_bool preparationStarted{ false };
            std::atomic_bool ready{ false };
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        bool ContainsCaseInsensitive(
            const std::string_view a_value,
            const std::string_view a_fragment)
        {
            if (a_fragment.empty())
            {
                return false;
            }
            return std::search(
                       a_value.begin(),
                       a_value.end(),
                       a_fragment.begin(),
                       a_fragment.end(),
                       [](const char a_left, const char a_right)
                       {
                           return std::tolower(
                                      static_cast<unsigned char>(a_left)) ==
                                  std::tolower(
                                      static_cast<unsigned char>(a_right));
                       }) != a_value.end();
        }

        bool EqualsCaseInsensitive(
            const std::string_view a_left,
            const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                   _strnicmp(
                       a_left.data(),
                       a_right.data(),
                       a_left.size()) == 0;
        }

        bool IsStatic(const RE::FormID a_formID)
        {
            const auto* form = a_formID ?
                                   RE::TESForm::LookupByID(a_formID) :
                                   nullptr;
            return form && form->GetFormType() == RE::FormType::Static;
        }

        bool HasFormContains(const Rule& a_rule)
        {
            return a_rule.settings.formContains &&
                   std::ranges::any_of(
                       *a_rule.settings.formContains,
                       [](const std::string& a_value)
                       {
                           return !a_value.empty();
                    });
        }

        bool HasIndexedForms(const Rule& a_rule)
        {
            return !a_rule.settings.forms.empty() || HasFormContains(a_rule);
        }

        bool PluginLoaded(
            RE::TESDataHandler& a_dataHandler,
            const std::string_view a_plugin)
        {
            return !a_plugin.empty() &&
                   HeliosphanLogic::IsPluginLoaded(
                       a_dataHandler.LookupLoadedModByName(a_plugin) !=
                           nullptr,
                       a_dataHandler.LookupLoadedLightModByName(a_plugin) !=
                           nullptr);
        }

        bool MatchesPluginGate(const Rule& a_rule)
        {
            if (a_rule.settings.plugins.empty())
            {
                return true;
            }
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler && std::ranges::any_of(
                                      a_rule.settings.plugins,
                                      [&](const std::string& a_plugin)
                                      {
                                          return PluginLoaded(
                                              *dataHandler,
                                              a_plugin);
                                      });
        }

        bool HasIndexedSelectors(const Rule& a_rule)
        {
            return HasIndexedForms(a_rule) ||
                   !a_rule.settings.references.empty();
        }

        bool MatchesBase(const Rule& a_rule, const RE::FormID a_base)
        {
            return IsStatic(a_base) && a_rule.forms.contains(a_base);
        }

        RE::FormID ProjectForProfile(
            const std::string_view a_profile,
            const RE::FormID a_reference,
            const RE::FormID a_base)
        {
            return ObjectOverrides::Patches::ProjectBase(
                a_reference,
                ObjectOverrides::ProjectBase(a_profile, a_base));
        }

        bool MatchesPlacement(
            const Rule& a_rule,
            const PluginIndex::Placement& a_placement)
        {
            if (MatchesBase(a_rule, a_placement.base))
            {
                return true;
            }
            return std::ranges::any_of(
                a_rule.settings.profiles,
                [&](const std::string& a_profile)
                {
                    return !a_profile.empty() &&
                           MatchesBase(
                               a_rule,
                               ProjectForProfile(
                                   a_profile,
                                   a_placement.reference,
                                   a_placement.base));
                });
        }

        const RE::BGSKeyword* GetExcludedLocationType(
            const Rule& a_rule,
            const RE::TESObjectCELL* a_cell)
        {
            if (!a_cell)
            {
                return nullptr;
            }
            const auto* location = a_cell->GetLocation();
            const auto found = location ?
                                   std::ranges::find_if(
                                       a_rule.excludedLocationTypes,
                                       [&](const RE::BGSKeyword* a_keyword)
                                       {
                                           return a_keyword &&
                                                  location->HasKeyword(
                                                      a_keyword);
                                       }) :
                                   a_rule.excludedLocationTypes.end();
            return found != a_rule.excludedLocationTypes.end() ?
                       *found :
                       nullptr;
        }

        const char* TextOrFallback(
            const char* a_value,
            const char* a_fallback)
        {
            return a_value && *a_value ? a_value : a_fallback;
        }

        void LogExcludedCell(
            const Rule& a_rule,
            const RE::TESObjectCELL* a_cell,
            const RE::BGSKeyword* a_locationType)
        {
            if (!a_rule.detailedLogging || !a_cell)
            {
                return;
            }
            if (!a_locationType)
            {
                logger::info(
                    "[Window Sync] [{}] Excluded cell '{}' [{:08X}] because it is listed in excludedCells",
                    a_rule.owner,
                    TextOrFallback(
                        a_cell->GetFormEditorID(),
                        "<no EditorID>"),
                    a_cell->GetFormID());
                return;
            }
            const auto* location = a_cell->GetLocation();
            logger::info(
                "[Window Sync] [{}] Excluded cell '{}' [{:08X}] because location '{}' [{:08X}] has excluded location type '{}' [{:08X}]",
                a_rule.owner,
                TextOrFallback(
                    a_cell->GetFormEditorID(),
                    "<no EditorID>"),
                a_cell->GetFormID(),
                TextOrFallback(
                    location ? location->GetFullName() : nullptr,
                    "<unnamed location>"),
                location ? location->GetFormID() : 0,
                TextOrFallback(
                    a_locationType->GetFormEditorID(),
                    "<no EditorID>"),
                a_locationType->GetFormID());
        }

        void AddUnique(
            std::vector<std::string>& a_destination,
            const std::string_view a_profile)
        {
            if (!a_profile.empty() &&
                std::ranges::none_of(
                    a_destination,
                    [&](const std::string& a_existing)
                    {
                        return EqualsCaseInsensitive(
                            a_existing,
                            a_profile);
                    }))
            {
                a_destination.emplace_back(a_profile);
            }
        }

        RE::FormID EffectiveBase(
            const std::vector<std::string>& a_profiles,
            const RE::FormID a_reference,
            const RE::FormID a_base)
        {
            auto overridden = a_base;
            for (auto profile = a_profiles.rbegin();
                 profile != a_profiles.rend();
                 ++profile)
            {
                const auto projected =
                    ObjectOverrides::ProjectBase(*profile, a_base);
                if (projected != a_base)
                {
                    overridden = projected;
                    break;
                }
            }
            return ObjectOverrides::Patches::ProjectBase(
                a_reference,
                overridden);
        }

        void ResolveCellFilters(Rule& a_rule)
        {
            if (a_rule.cellFiltersResolved)
            {
                return;
            }
            for (const auto& selector : a_rule.settings.includedCells)
            {
                if (const auto formID = FormResolver::Resolve(selector))
                {
                    if (const auto* cell =
                            RE::TESForm::LookupByID<RE::TESObjectCELL>(
                                formID))
                    {
                        a_rule.includedCells.insert(cell->GetFormID());
                    }
                }
            }
            for (const auto& selector : a_rule.settings.excludedCells)
            {
                if (const auto formID = FormResolver::Resolve(selector))
                {
                    if (const auto* cell =
                            RE::TESForm::LookupByID<RE::TESObjectCELL>(
                                formID))
                    {
                        a_rule.excludedCells.insert(cell->GetFormID());
                    }
                }
            }
            for (const auto& selector :
                 a_rule.settings.excludedLocationTypes)
            {
                if (const auto formID = FormResolver::Resolve(selector))
                {
                    if (auto* keyword =
                            RE::TESForm::LookupByID<RE::BGSKeyword>(formID))
                    {
                        a_rule.excludedLocationTypes.push_back(keyword);
                    }
                }
            }
            a_rule.cellFiltersResolved = true;
        }

        void ResolveRule(
            Rule& a_rule,
            const std::unordered_map<RE::FormID, std::string>& a_editorIDs)
        {
            ResolveCellFilters(a_rule);
            a_rule.active = MatchesPluginGate(a_rule);
            if (!a_rule.active)
            {
                if (a_rule.detailedLogging)
                {
                    logger::info(
                        "[Window Sync] [{}] cellContains ignored because no configured plugin is loaded",
                        a_rule.owner);
                }
                return;
            }
            std::size_t missingForms = 0;
            std::size_t invalidForms = 0;
            std::size_t missingReferences = 0;
            for (const auto& selector : a_rule.settings.forms)
            {
                const auto formID = FormResolver::Resolve(selector);
                if (!formID)
                {
                    ++missingForms;
                }
                else if (!IsStatic(formID))
                {
                    ++invalidForms;
                }
                else
                {
                    a_rule.forms.insert(formID);
                }
            }
            for (const auto& selector : a_rule.settings.references)
            {
                if (const auto reference = FormResolver::Resolve(selector))
                {
                    a_rule.references.insert(reference);
                }
                else
                {
                    ++missingReferences;
                }
            }
            if (HasFormContains(a_rule))
            {
                for (const auto& [formID, editorID] : a_editorIDs)
                {
                    if (!formID || editorID.empty())
                    {
                        continue;
                    }
                    if (std::ranges::any_of(
                            *a_rule.settings.formContains,
                            [&](const std::string& a_fragment)
                            {
                                return ContainsCaseInsensitive(
                                    editorID,
                                    a_fragment);
                            }))
                    {
                        a_rule.forms.insert(formID);
                    }
                }
            }
            if (a_rule.settings.profiles.empty())
            {
                a_rule.settings.profiles.push_back(a_rule.owner);
            }
            if (a_rule.detailedLogging)
            {
                logger::info(
                    "[Window Sync] [{}] Resolved cellContains: STAT forms={}, references={}, object placements={}, missing forms={}, invalid forms={}, missing references={}, configured included cells={}, configured excluded cells={}, configured excluded location types={}",
                    a_rule.owner,
                    a_rule.forms.size(),
                    a_rule.references.size(),
                    a_rule.settings.objectPlacements,
                    missingForms,
                    invalidForms,
                    missingReferences,
                    a_rule.includedCells.size(),
                    a_rule.excludedCells.size(),
                    a_rule.excludedLocationTypes.size());
            }
        }
    }  // namespace

    void Clear()
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.rules.clear();
        state.cells.clear();
        state.cellOrder.clear();
        state.projectedBases.clear();
        state.placementFilterPrepared = false;
        state.preparationStarted.store(false, std::memory_order_release);
        state.ready.store(false, std::memory_order_release);
    }

    void AddRule(
        std::string a_owner,
        Settings a_settings,
        const bool a_detailedLogging)
    {
        if (a_owner.empty() ||
            (a_settings.forms.empty() &&
                (!a_settings.formContains ||
                    a_settings.formContains->empty()) &&
                a_settings.references.empty() &&
                !a_settings.objectPlacements))
        {
            return;
        }
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.rules.push_back(Rule{
            .owner = std::move(a_owner),
            .settings = std::move(a_settings),
            .detailedLogging = a_detailedLogging,
        });
    }

    bool RequiresStaticEditorIDs()
    {
        return std::ranges::any_of(
            GetState().rules,
            [](const Rule& a_rule)
            {
                return MatchesPluginGate(a_rule) &&
                       HasFormContains(a_rule);
            });
    }

    bool RequiresPluginIndex()
    {
        return std::ranges::any_of(
            GetState().rules,
            [](const Rule& a_rule)
            {
                return MatchesPluginGate(a_rule) &&
                       HasIndexedSelectors(a_rule);
            });
    }

    void PreparePlacementFilter(
        const PluginIndex::BuildOptions::EditorIDMap& a_editorIDs)
    {
        auto& state = GetState();
        if (state.placementFilterPrepared)
        {
            return;
        }
        Heliosphan::PrepareWindowSyncProfilePriorities();
        ObjectOverrides::Initialize();
        ObjectOverrides::Patches::Initialize();
        for (auto& rule : state.rules)
        {
            ResolveRule(rule, a_editorIDs);
        }
        state.placementFilterPrepared = true;
    }

    bool NeedsPlacement(
        const RE::FormID a_reference,
        const RE::FormID a_base)
    {
        const auto& state = GetState();
        if (!state.placementFilterPrepared || !a_reference || !a_base)
        {
            return false;
        }
        const PluginIndex::Placement placement{
            .reference = a_reference,
            .base = a_base,
        };
        return std::ranges::any_of(
            state.rules,
            [&](const Rule& a_rule)
            {
                return a_rule.active &&
                       (a_rule.references.contains(a_reference) ||
                           (!a_rule.forms.empty() &&
                               MatchesPlacement(a_rule, placement)));
            });
    }

    bool CouldProjectToAny(
        const RE::FormID a_reference,
        const RE::FormID a_base,
        const std::unordered_set<RE::FormID>& a_forms)
    {
        const auto& state = GetState();
        if (!state.placementFilterPrepared || !a_reference || !a_base ||
            a_forms.empty())
        {
            return false;
        }
        const auto matches = [&](const std::string_view a_profile)
        {
            return a_forms.contains(
                ProjectForProfile(
                    a_profile,
                    a_reference,
                    a_base));
        };
        if (a_forms.contains(a_base) || matches(std::string_view{}))
        {
            return true;
        }
        return std::ranges::any_of(
            state.rules,
            [&](const Rule& a_rule)
            {
                return a_rule.active &&
                       std::ranges::any_of(
                           a_rule.settings.profiles,
                           matches);
            });
    }

    std::unordered_set<RE::FormID> GetExcludedCells()
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        std::unordered_set<RE::FormID> cells;
        if (state.rules.empty())
        {
            return cells;
        }
        std::size_t activeRules = 0;
        for (auto& rule : state.rules)
        {
            if (!MatchesPluginGate(rule))
            {
                continue;
            }
            ResolveCellFilters(rule);
            ++activeRules;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler || activeRules == 0)
        {
            return cells;
        }
        for (const auto* cell : dataHandler->interiorCells)
        {
            if (!cell || !cell->IsInteriorCell())
            {
                continue;
            }
            const Rule* excludingRule = nullptr;
            const RE::BGSKeyword* excludedLocationType = nullptr;
            for (const auto& rule : state.rules)
            {
                if (!MatchesPluginGate(rule) ||
                    rule.includedCells.contains(cell->GetFormID()))
                {
                    continue;
                }
                if (const auto* locationType =
                        GetExcludedLocationType(rule, cell))
                {
                    excludingRule = std::addressof(rule);
                    excludedLocationType = locationType;
                    break;
                }
                if (!excludingRule &&
                    rule.excludedCells.contains(cell->GetFormID()))
                {
                    excludingRule = std::addressof(rule);
                }
            }
            if (excludingRule)
            {
                cells.insert(cell->GetFormID());
                LogExcludedCell(
                    *excludingRule,
                    cell,
                    excludedLocationType);
            }
        }
        logger::info(
            "[Window Sync] Pre-parse cell exclusions resolved: active rules={}, excluded interior cells={}",
            activeRules,
            cells.size());
        return cells;
    }

    bool Prepare(const PluginIndex::Result& a_index)
    {
        auto& state = GetState();
        if (state.preparationStarted.exchange(
                true,
                std::memory_order_acq_rel))
        {
            return state.ready.load(std::memory_order_acquire);
        }
        if (!a_index.complete)
        {
            logger::warn(
                "[Window Sync] Heliosphan plugin index is partial; cellContains classification and profile filtering are disabled for this session");
            return false;
        }

        PreparePlacementFilter(a_index.editorIDs);
        std::vector<PluginIndex::Placement> indexedPlacements;
        indexedPlacements.reserve(a_index.placements.size());
        for (const auto& [formID, placement] : a_index.placements)
        {
            auto copy = placement;
            copy.reference = formID;
            indexedPlacements.push_back(copy);
        }
        state.projectedBases.clear();

        std::unordered_map<
            RE::FormID,
            std::vector<PluginIndex::Placement>>
            placementsByCell;
        std::unordered_map<RE::FormID, std::vector<std::string>>
            objectPlacementProfilesByCell;
        for (const auto& placement : indexedPlacements)
        {
            const bool explicitlyReferenced =
                std::ranges::any_of(
                    state.rules,
                    [&](const Rule& a_rule)
                    {
                        return a_rule.active &&
                               a_rule.references.contains(
                                   placement.reference);
                    });
            if (!placement.deleted &&
                (IsStatic(placement.base) || explicitlyReferenced))
            {
                placementsByCell[placement.cell].push_back(placement);
            }
        }
        const auto projectedPlacementCount =
            ObjectOverrides::Patches::GetProjectedPlacementCount();
        for (std::size_t placementIndex = 0;
             placementIndex < projectedPlacementCount;
             ++placementIndex)
        {
            RE::FormID cell = 0;
            RE::FormID base = 0;
            std::string_view profile;
            if (ObjectOverrides::Patches::GetProjectedPlacement(
                    placementIndex,
                    std::addressof(cell),
                    std::addressof(base),
                    std::addressof(profile)))
            {
                if (cell && !profile.empty())
                {
                    AddUnique(objectPlacementProfilesByCell[cell], profile);
                    placementsByCell.try_emplace(cell);
                }
                if (cell && IsStatic(base))
                {
                    placementsByCell[cell].push_back(
                        PluginIndex::Placement{
                            .base = base,
                            .cell = cell,
                        });
                }
            }
        }

        for (const auto& [cellID, placements] : placementsByCell)
        {
            auto* cell =
                RE::TESForm::LookupByID<RE::TESObjectCELL>(cellID);
            if (!cell || !cell->IsInteriorCell())
            {
                continue;
            }
            const Rule* excludingRule = nullptr;
            const RE::BGSKeyword* excludedLocationType = nullptr;
            for (const auto& rule : state.rules)
            {
                const bool explicitlyIncluded =
                    rule.includedCells.contains(cellID);
                const bool explicitlyExcluded =
                    rule.excludedCells.contains(cellID);
                const auto* locationType =
                    rule.active && !explicitlyIncluded &&
                            !explicitlyExcluded ?
                        GetExcludedLocationType(rule, cell) :
                        nullptr;
                const auto exclusion =
                    HeliosphanLogic::EvaluateCellExclusion(
                        rule.active,
                        explicitlyIncluded,
                        explicitlyExcluded,
                        locationType != nullptr);
                if (exclusion ==
                    HeliosphanLogic::CellExclusion::none)
                {
                    continue;
                }
                excludingRule = std::addressof(rule);
                if (exclusion ==
                    HeliosphanLogic::CellExclusion::explicitCell)
                {
                    break;
                }
                excludedLocationType = locationType;
                break;
            }
            if (excludingRule)
            {
                LogExcludedCell(
                    *excludingRule,
                    cell,
                    excludedLocationType);
                continue;
            }
            auto& profiles = state.cells[cellID];
            for (auto& rule : state.rules)
            {
                if (!rule.active)
                {
                    continue;
                }
                const auto placementProfiles =
                    objectPlacementProfilesByCell.find(cellID);
                const bool matchesObjectPlacement =
                    rule.settings.objectPlacements &&
                    placementProfiles != objectPlacementProfilesByCell.end() &&
                    std::ranges::any_of(
                        placementProfiles->second,
                        [&](const std::string& a_profile)
                        {
                            return EqualsCaseInsensitive(
                                a_profile,
                                rule.owner);
                        });
                const bool matchesIndexedForm =
                    !rule.forms.empty() && std::ranges::any_of(
                        placements,
                        [&](const PluginIndex::Placement& a_placement)
                        {
                            return MatchesPlacement(rule, a_placement);
                        });
                const bool matchesExplicitReference =
                    !rule.references.empty() && std::ranges::any_of(
                        placements,
                        [&](const PluginIndex::Placement& a_placement)
                        {
                            return rule.references.contains(
                                a_placement.reference);
                        });
                if (!matchesObjectPlacement && !matchesIndexedForm &&
                    !matchesExplicitReference)
                {
                    continue;
                }
                for (const auto& profile : rule.settings.profiles)
                {
                    AddUnique(profiles, profile);
                }
                if (matchesExplicitReference)
                {
                    for (const auto& placement : placements)
                    {
                        if (rule.references.contains(
                                placement.reference))
                        {
                            rule.matchedReferences.insert(
                                placement.reference);
                        }
                    }
                }
            }
            if (profiles.empty())
            {
                state.cells.erase(cellID);
            }
        }

        state.cellOrder.reserve(state.cells.size());
        for (auto& [cellID, profiles] : state.cells)
        {
            Heliosphan::SortWindowSyncProfileIDs(profiles);
            state.cellOrder.push_back(cellID);
        }
        std::ranges::sort(state.cellOrder);

        for (const auto& placement : indexedPlacements)
        {
            if (placement.deleted || !placement.base)
            {
                continue;
            }
            const auto& profiles = GetProfiles(placement.cell);
            const auto effectiveBase = EffectiveBase(
                profiles,
                placement.reference,
                placement.base);
            if (effectiveBase && effectiveBase != placement.base)
            {
                state.projectedBases.insert_or_assign(
                    placement.reference,
                    State::BaseProjection{
                        .original = placement.base,
                        .projected = effectiveBase,
                    });
            }
        }

        state.ready.store(true, std::memory_order_release);
        logger::info(
            "[Window Sync] Cell classification completed: rules={}, profiled cells={}, projected placements={}, changed projected bases={}, plugins={}/{}, index={}",
            state.rules.size(),
            state.cells.size(),
            projectedPlacementCount,
            state.projectedBases.size(),
            a_index.pluginsParsed,
            a_index.pluginsDiscovered,
            a_index.complete ? "complete" : "partial");
        return true;
    }

    bool IsReady()
    {
        return GetState().ready.load(std::memory_order_acquire);
    }

    bool HasProfiles()
    {
        return GetState().ready.load(std::memory_order_acquire) &&
               !GetState().cells.empty();
    }

    const std::vector<std::string>& GetProfiles(const RE::FormID a_cell)
    {
        static const std::vector<std::string> empty;
        const auto& cells = GetState().cells;
        const auto found = cells.find(a_cell);
        return found != cells.end() ? found->second : empty;
    }

    std::vector<RE::FormID> GetResolvedForms(
        const std::string_view a_owner)
    {
        std::vector<RE::FormID> forms;
        for (const auto& rule : GetState().rules)
        {
            if (EqualsCaseInsensitive(rule.owner, a_owner))
            {
                forms.insert(
                    forms.end(),
                    rule.forms.begin(),
                    rule.forms.end());
            }
        }
        std::ranges::sort(forms);
        const auto unique = std::ranges::unique(forms);
        forms.erase(unique.begin(), unique.end());
        return forms;
    }

    std::vector<RE::FormID> GetResolvedReferences(
        const std::string_view a_owner)
    {
        std::vector<RE::FormID> references;
        for (const auto& rule : GetState().rules)
        {
            if (!rule.active ||
                !EqualsCaseInsensitive(rule.owner, a_owner))
            {
                continue;
            }
            references.insert(
                references.end(),
                rule.matchedReferences.begin(),
                rule.matchedReferences.end());
        }
        std::ranges::sort(references);
        references.erase(
            std::unique(references.begin(), references.end()),
            references.end());
        return references;
    }

    std::size_t GetProfiledCellCount()
    {
        return GetState().cellOrder.size();
    }

    RE::FormID GetProfiledCell(const std::size_t a_index)
    {
        const auto& state = GetState();
        if (a_index >= state.cellOrder.size())
        {
            return 0;
        }
        return state.cellOrder[a_index];
    }

    std::uint32_t ProjectBase(
        const std::uint32_t a_reference,
        const std::uint32_t a_base)
    {
        const auto& state = GetState();
        const auto found = state.projectedBases.find(a_reference);
        return found != state.projectedBases.end() && found->second.projected ?
                   found->second.projected :
                   a_base;
    }

    std::uint32_t OriginalBase(
        const std::uint32_t a_reference,
        const std::uint32_t a_base)
    {
        const auto& state = GetState();
        const auto found = state.projectedBases.find(a_reference);
        return found != state.projectedBases.end() && found->second.original ?
                   found->second.original :
                   a_base;
    }
}  // namespace MPL::CellClassifier
