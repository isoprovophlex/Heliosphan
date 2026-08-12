#include <FormResolver.h>
#include <ObjectOverrides.h>
#include <Heliosphan.h>
#include <HeliosphanLogic.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MPL::ObjectOverrides
{
    namespace
    {
        struct ProfileOverrides
        {
            std::unordered_map<RE::FormID, RE::TESBoundObject*> forms;
        };

        struct State
        {
            ProfileOverrides global;
            std::unordered_map<std::string, ProfileOverrides> profiles;
            std::unordered_set<RE::FormID> sources;
            std::mutex applicationLock;
            std::unordered_map<
                RE::FormID,
                std::map<std::string, std::size_t>> applicationsByCell;
            std::size_t overrideCount = 0;
            bool globalDetailedLogging = false;
            bool initialized = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        std::string Lowercase(const std::string_view a_value)
        {
            std::string result(a_value);
            std::ranges::transform(
                result,
                result.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return result;
        }

        void RecordApplication(
            const RE::TESObjectREFR& a_reference,
            const std::string_view a_profile)
        {
            const auto* cell = a_reference.GetParentCell();
            if (!cell)
            {
                return;
            }
            auto& state = GetState();
            std::scoped_lock lock(state.applicationLock);
            ++state.applicationsByCell[cell->GetFormID()][
                std::string(a_profile)];
        }

        std::map<std::string, std::size_t> TakeApplications(
            const RE::FormID a_cell)
        {
            auto& state = GetState();
            std::scoped_lock lock(state.applicationLock);
            const auto found = state.applicationsByCell.find(a_cell);
            if (found == state.applicationsByCell.end())
            {
                return {};
            }
            auto applications = std::move(found->second);
            state.applicationsByCell.erase(found);
            return applications;
        }

        RE::TESForm* ResolveForm(
            const std::string_view a_selector)
        {
            const auto formID = FormResolver::Resolve(a_selector);
            return formID ? RE::TESForm::LookupByID(formID) : nullptr;
        }

        const ProfileOverrides* FindProfile(
            const std::string_view a_profile)
        {
            const auto& profiles = GetState().profiles;
            const auto found = std::ranges::find_if(
                profiles,
                [&](const auto& a_entry)
                {
                    const auto& id = a_entry.first;
                    return id.size() == a_profile.size() &&
                           _strnicmp(
                               id.data(),
                               a_profile.data(),
                               id.size()) == 0;
                });
            return found != profiles.end() ?
                       std::addressof(found->second) :
                       nullptr;
        }
    }  // namespace

    void Initialize()
    {
        auto& state = GetState();
        if (state.initialized)
        {
            return;
        }
        state.global = {};
        state.profiles.clear();
        state.sources.clear();
        {
            std::scoped_lock lock(state.applicationLock);
            state.applicationsByCell.clear();
        }
        state.overrideCount = 0;
        state.globalDetailedLogging = false;

        for (const auto& profile :
            Heliosphan::GetObjectOverrideProfiles())
        {
            if (!profile.overrides || profile.overrides->empty())
            {
                continue;
            }
            auto& overrides = profile.global ?
                                  state.global :
                                  state.profiles[Lowercase(profile.id)];
            const auto logID = std::format(
                "{}:{}",
                profile.id,
                profile.global ? "global" : "windowSync");
            state.globalDetailedLogging |=
                profile.global && profile.debugLogging;
            std::size_t loadedOverrides = 0;
            std::size_t skippedOverrides = 0;
            for (const auto& [sourceSelector, targetSelector] :
                *profile.overrides)
            {
                auto* sourceForm = ResolveForm(sourceSelector);
                if (!sourceForm)
                {
                    ++skippedOverrides;
                    continue;
                }
                auto* source = sourceForm->As<RE::TESBoundObject>();
                if (!source)
                {
                    ++skippedOverrides;
                    continue;
                }
                auto* targetForm = ResolveForm(targetSelector);
                if (!targetForm)
                {
                    ++skippedOverrides;
                    continue;
                }
                auto* target = targetForm->As<RE::TESBoundObject>();
                if (!target)
                {
                    ++skippedOverrides;
                    continue;
                }
                const auto sourceID = source->GetFormID();
                overrides.forms.insert_or_assign(sourceID, target);
                state.sources.insert(sourceID);
                ++loadedOverrides;
            }
            logger::info(
                "[Object Overrides] {} resolve | found={} | skipped={}",
                logID,
                loadedOverrides,
                skippedOverrides);
        }
        state.overrideCount = state.global.forms.size();
        for (const auto& entry : state.profiles)
        {
            state.overrideCount += entry.second.forms.size();
        }
        state.initialized = true;
    }

    bool HasOverrides()
    {
        return GetState().overrideCount != 0;
    }

    bool HasOverrideFor(const RE::TESObjectREFR* a_reference)
    {
        const auto* base =
            a_reference ? a_reference->GetBaseObject() : nullptr;
        return base && GetState().sources.contains(base->GetFormID());
    }

    RE::FormID ProjectBase(
        const std::string_view a_profile,
        const RE::FormID a_base)
    {
        if (!a_base)
        {
            return a_base;
        }
        const auto* filtered = FindProfile(a_profile);
        if (filtered)
        {
            const auto direct = filtered->forms.find(a_base);
            if (direct != filtered->forms.end() && direct->second)
            {
                return direct->second->GetFormID();
            }
        }
        const auto& global = GetState().global.forms;
        const auto globalMatch = global.find(a_base);
        const auto globalBase =
            globalMatch != global.end() && globalMatch->second ?
                globalMatch->second->GetFormID() :
                a_base;
        if (filtered)
        {
            const auto projected = filtered->forms.find(globalBase);
            if (projected != filtered->forms.end() && projected->second)
            {
                return projected->second->GetFormID();
            }
        }
        return globalBase;
    }

    bool ApplyToReference(
        RE::TESObjectREFR* a_reference,
        const std::vector<Heliosphan::WindowSyncProfile>& a_profiles)
    {
        auto* source = a_reference ? a_reference->GetBaseObject() : nullptr;
        if (!source)
        {
            return false;
        }
        if (!GetState().sources.contains(source->GetFormID()))
        {
            return false;
        }

        RE::TESBoundObject* target = nullptr;
        const Heliosphan::WindowSyncProfile* filteredOwner = nullptr;
        for (auto profile = a_profiles.rbegin(); profile != a_profiles.rend();
             ++profile)
        {
            const auto* overrides = FindProfile(profile->id);
            if (!overrides)
            {
                continue;
            }
            const auto found =
                overrides->forms.find(source->GetFormID());
            if (found == overrides->forms.end() || !found->second)
            {
                continue;
            }
            target = found->second;
            filteredOwner = std::addressof(*profile);
            break;
        }
        if (!target)
        {
            const auto& global = GetState().global.forms;
            const auto found = global.find(source->GetFormID());
            if (found != global.end() && found->second)
            {
                target = found->second;
                for (auto profile = a_profiles.rbegin();
                     profile != a_profiles.rend();
                     ++profile)
                {
                    const auto* overrides = FindProfile(profile->id);
                    if (!overrides)
                    {
                        continue;
                    }
                    const auto projected =
                        overrides->forms.find(target->GetFormID());
                    if (projected != overrides->forms.end() &&
                        projected->second)
                    {
                        target = projected->second;
                        filteredOwner = std::addressof(*profile);
                        break;
                    }
                }
            }
        }
        if (!target || target == source)
        {
            return false;
        }

        a_reference->SetObjectReference(target);
        if (a_reference->GetBaseObject() != target)
        {
            return false;
        }
        if ((filteredOwner && filteredOwner->debugLogging) ||
            (!filteredOwner && GetState().globalDetailedLogging))
        {
            RecordApplication(
                *a_reference,
                filteredOwner ? filteredOwner->id : "Global");
        }
        return true;
    }

    std::size_t ApplyToCell(
        RE::TESObjectCELL* a_cell,
        const std::vector<Heliosphan::WindowSyncProfile>& a_profiles)
    {
        if (!a_cell)
        {
            return 0;
        }

        std::size_t changed = 0;
        for (const auto& reference :
            a_cell->GetRuntimeData().references)
        {
            if (reference)
            {
                changed +=
                    ApplyToReference(reference.get(), a_profiles) ? 1 : 0;
            }
        }
        for (const auto& [profile, applied] :
            TakeApplications(a_cell->GetFormID()))
        {
            logger::info(
                "[Object Overrides] {} apply | cell={:08X} | references={}",
                profile,
                a_cell->GetFormID(),
                applied);
        }
        return changed;
    }
}  // namespace MPL::ObjectOverrides

namespace MPL::ObjectOverrides::Patches
{
    namespace
    {
        struct Profile
        {
            std::string profile;
            std::string id;
            std::vector<std::string> pluginInclusions;
            std::vector<std::string> pluginExclusions;
            ObjectTransformMap transforms;
            ObjectPlacementMap placements;
        };

        struct ResolvedPlacement
        {
            std::string key;
            std::string profile;
            std::string id;
            const Profile* owner = nullptr;
            RE::TESObjectCELL* cell = nullptr;
            RE::TESObjectSTAT* base = nullptr;
            RE::NiPoint3 position;
            RE::NiPoint3 rotation;
            std::optional<float> scale;
        };

        struct OwnedPlacement
        {
            RE::ObjectRefHandle handle;
            std::uint64_t generation = 0;
        };

        struct ResolvedTransform
        {
            const Profile* profile = nullptr;
            std::string selector;
            const ObjectTransform* transform = nullptr;
            RE::TESObjectSTAT* base = nullptr;
        };

        struct State
        {
            std::vector<Profile> profiles;
            std::unordered_map<RE::FormID, std::vector<ResolvedTransform>> transforms;
            std::vector<ResolvedPlacement> placements;
            std::unordered_map<RE::FormID, std::vector<std::size_t>>
                placementsByCell;
            std::unordered_map<std::string, OwnedPlacement> created;
            std::unordered_map<RE::FormID, std::string> transformOwners;
            std::size_t projectionCount = 0;
            std::uint64_t loadGeneration = 1;
            bool gameLoadPending = false;
            bool initialized = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        bool IsFinite(const Vector3& a_value)
        {
            return std::isfinite(a_value.x) &&
                   std::isfinite(a_value.y) &&
                   std::isfinite(a_value.z);
        }

        bool IsValidScale(const float a_scale)
        {
            constexpr auto maximumScale =
                static_cast<float>(
                    std::numeric_limits<std::uint16_t>::max()) /
                100.0f;
            return std::isfinite(a_scale) && a_scale >= 0.01f &&
                   a_scale <= maximumScale;
        }

        std::uint16_t ToReferenceScale(const float a_scale)
        {
            return static_cast<std::uint16_t>(
                std::lround(a_scale * 100.0f));
        }

        RE::NiPoint3 ToPoint(const Vector3& a_value)
        {
            return { a_value.x, a_value.y, a_value.z };
        }

        RE::NiPoint3 ToRadians(const Vector3& a_value)
        {
            constexpr auto degreesToRadians =
                std::numbers::pi_v<float> / 180.0f;
            return {
                a_value.x * degreesToRadians,
                a_value.y * degreesToRadians,
                a_value.z * degreesToRadians,
            };
        }

        std::string Lowercase(const std::string_view a_value)
        {
            std::string result(a_value);
            std::ranges::transform(
                result,
                result.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return result;
        }

        RE::TESForm* ResolveForm(const std::string_view a_selector)
        {
            const auto formID = FormResolver::Resolve(a_selector);
            return formID ? RE::TESForm::LookupByID(formID) : nullptr;
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

        bool MatchesPluginFilters(const Profile& a_profile)
        {
            if (a_profile.pluginInclusions.empty() &&
                a_profile.pluginExclusions.empty())
            {
                return true;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
            logger::warn(
                "[Object Overrides] {} ignored | plugin filters unavailable | TESDataHandler unavailable",
                    a_profile.id);
                return false;
            }

            if (std::ranges::any_of(
                    a_profile.pluginExclusions,
                    [&](const auto& a_plugin)
                    {
                        return PluginLoaded(*dataHandler, a_plugin);
                    }))
            {
            logger::info(
                "[Object Overrides] {} ignored | pluginExclusions matched",
                    a_profile.id);
                return false;
            }
            if (!a_profile.pluginInclusions.empty() &&
                !std::ranges::any_of(
                    a_profile.pluginInclusions,
                    [&](const auto& a_plugin)
                    {
                        return PluginLoaded(*dataHandler, a_plugin);
                    }))
            {
            logger::info(
                "[Object Overrides] {} ignored | pluginInclusions unmatched",
                    a_profile.id);
                return false;
            }
            return true;
        }

        RE::TESObjectSTAT* ResolveTransformBase(
            const Profile& a_profile,
            const std::string_view a_reference,
            const std::string_view a_selector)
        {
            auto* form = ResolveForm(a_selector);
            if (!form)
            {
                logger::warn(
                "[Object Overrides] {} transform={} | base={} unresolved",
                    a_profile.id,
                    a_reference,
                    a_selector);
                return nullptr;
            }
            if (form->GetFormType() != RE::FormType::Static)
            {
                logger::warn(
                "[Object Overrides] {} transform={} | base={} [{:08X}] | type=invalid",
                    a_profile.id,
                    a_reference,
                    a_selector,
                    form->GetFormID());
                return nullptr;
            }
            return form->As<RE::TESObjectSTAT>();
        }

        bool ApplyTransform(
            const Profile& a_profile,
            const std::string_view a_selector,
            const ObjectTransform& a_transform,
            RE::TESObjectSTAT* a_replacementBase,
            RE::TESObjectREFR& a_reference)
        {
            const auto* currentBase = a_reference.GetBaseObject();
            if (!currentBase ||
                currentBase->GetFormType() != RE::FormType::Static)
            {
                logger::warn(
                "[Object Overrides] {} transform={} [{:08X}] | STAT base missing",
                    a_profile.id,
                    a_selector,
                    a_reference.GetFormID());
                return false;
            }
            if (a_reference.Is3DLoaded())
            {
                logger::warn(
                "[Object Overrides] {} transform={} [{:08X}] | moved=false | 3D loaded",
                    a_profile.id,
                    a_selector,
                    a_reference.GetFormID());
                return false;
            }

            bool applied = false;
            if (a_replacementBase)
            {
                if (a_reference.GetBaseObject() != a_replacementBase)
                {
                    a_reference.SetObjectReference(a_replacementBase);
                }
                if (a_reference.GetBaseObject() == a_replacementBase)
                {
                    applied = true;
                }
                else
                {
                    logger::warn(
                "[Object Overrides] {} transform={} [{:08X}] | base {:08X}->{:08X} failed | fields continue",
                        a_profile.id,
                        a_selector,
                        a_reference.GetFormID(),
                        currentBase->GetFormID(),
                        a_replacementBase->GetFormID());
                }
            }
            if (a_transform.position)
            {
                a_reference.data.location =
                    ToPoint(*a_transform.position);
                applied = true;
            }
            if (a_transform.rotation)
            {
                a_reference.data.angle =
                    ToRadians(*a_transform.rotation);
                applied = true;
            }
            if (a_transform.scale)
            {
                a_reference.GetReferenceRuntimeData().refScale =
                    ToReferenceScale(*a_transform.scale);
                applied = true;
            }
            return applied;
        }

        bool ApplyResolvedTransforms(
            State& a_state,
            RE::TESObjectREFR& a_reference)
        {
            const auto found =
                a_state.transforms.find(a_reference.GetFormID());
            if (found == a_state.transforms.end())
            {
                return false;
            }

            bool applied = false;
            for (const auto& resolved : found->second)
            {
                if (resolved.profile && resolved.transform)
                {
                    applied |= ApplyTransform(
                        *resolved.profile,
                        resolved.selector,
                        *resolved.transform,
                        resolved.base,
                        a_reference);
                }
            }
            return applied;
        }

        RE::TESObjectCELL* ResolveInteriorCell(
            const Profile& a_profile,
            const std::string_view a_placement,
            const std::string_view a_selector)
        {
            auto* form = ResolveForm(a_selector);
            auto* cell = form ? form->As<RE::TESObjectCELL>() : nullptr;
            if (!cell)
            {
                logger::warn(
                "[Object Overrides] {} placement={} | cell={} unresolved",
                    a_profile.id,
                    a_placement,
                    a_selector);
                return nullptr;
            }
            if (!cell->IsInteriorCell())
            {
                logger::warn(
                "[Object Overrides] {} placement={} | cell={} [{:08X}] | interior=false",
                    a_profile.id,
                    a_placement,
                    a_selector,
                    cell->GetFormID());
                return nullptr;
            }
            return cell;
        }

        RE::TESObjectSTAT* ResolvePlacementBase(
            const Profile& a_profile,
            const std::string_view a_placement,
            const std::string_view a_selector)
        {
            auto* form = ResolveForm(a_selector);
            if (!form)
            {
                logger::warn(
                "[Object Overrides] {} placement={} | base={} unresolved",
                    a_profile.id,
                    a_placement,
                    a_selector);
                return nullptr;
            }
            if (form->GetFormType() != RE::FormType::Static)
            {
                logger::warn(
                "[Object Overrides] {} placement={} | base={} [{:08X}] | type=invalid",
                    a_profile.id,
                    a_placement,
                    a_selector,
                    form->GetFormID());
                return nullptr;
            }
            return form->As<RE::TESObjectSTAT>();
        }

        bool ResolvePlacement(
            const Profile& a_profile,
            const std::string_view a_id,
            const ObjectPlacement& a_placement)
        {
            if (a_id.empty())
            {
                logger::warn(
                "[Object Overrides] {} placement ignored | name empty",
                    a_profile.id);
                return false;
            }
            if (!a_placement.position || !a_placement.rotation)
            {
                logger::warn(
                "[Object Overrides] {} placement={} | position/rotation required",
                    a_profile.id,
                    a_id);
                return false;
            }
            if (!IsFinite(*a_placement.position) ||
                !IsFinite(*a_placement.rotation) ||
                (a_placement.scale &&
                    !IsValidScale(*a_placement.scale)))
            {
                logger::warn(
                "[Object Overrides] {} placement={} | transform/scale invalid",
                    a_profile.id,
                    a_id);
                return false;
            }

            auto* cell = ResolveInteriorCell(
                a_profile,
                a_id,
                a_placement.cell);
            auto* base = ResolvePlacementBase(
                a_profile,
                a_id,
                a_placement.base);
            if (!cell || !base)
            {
                return false;
            }

            auto& state = GetState();
            state.placements.push_back(ResolvedPlacement{
                .key = Lowercase(a_profile.id) + "\x1F" +
                       std::string(a_id),
                .profile = a_profile.profile,
                .id = std::string(a_id),
                .owner = std::addressof(a_profile),
                .cell = cell,
                .base = base,
                .position = ToPoint(*a_placement.position),
                .rotation = ToRadians(*a_placement.rotation),
                .scale = a_placement.scale,
            });
            return true;
        }

        bool IsExistingPlacement(
            const ResolvedPlacement& a_placement,
            const OwnedPlacement& a_owned,
            const std::uint64_t a_generation)
        {
            if (a_owned.generation != a_generation)
            {
                return false;
            }
            const auto reference = a_owned.handle.get();
            return reference && !reference->IsMarkedForDeletion() &&
                   reference->GetParentCell() == a_placement.cell;
        }
    }  // namespace

    void ClearGroups()
    {
        auto& state = GetState();
        if (state.initialized)
        {
        logger::error("[Object Overrides] configuration clear rejected | initialized=true");
            return;
        }
        state.profiles.clear();
        state.transforms.clear();
        state.placements.clear();
        state.created.clear();
        state.transformOwners.clear();
        state.projectionCount = 0;
    }

    void AddGroup(
        std::string a_profile,
        std::string a_id,
        Group a_group)
    {
        auto& state = GetState();
        if (state.initialized ||
            (a_group.transforms.empty() &&
                a_group.placements.empty()))
        {
            return;
        }
        state.profiles.push_back(Profile{
            .profile = std::move(a_profile),
            .id = std::move(a_id),
            .pluginInclusions = std::move(a_group.pluginInclusions),
            .pluginExclusions = std::move(a_group.pluginExclusions),
            .transforms = std::move(a_group.transforms),
            .placements = std::move(a_group.placements),
        });
    }

    std::size_t GetConfiguredProjectionCount()
    {
        const auto& state = GetState();
        std::size_t count = 0;
        for (const auto& profile : state.profiles)
        {
            count += profile.placements.size();
            count += std::ranges::count_if(
                profile.transforms,
                [](const auto& a_entry)
                {
                    return a_entry.second.base.has_value();
                });
        }
        return count;
    }

    void Initialize()
    {
        auto& state = GetState();
        if (state.initialized)
        {
            return;
        }

        std::size_t configuredTransforms = 0;
        std::size_t indexedTransforms = 0;
        std::size_t configuredPlacements = 0;
        std::size_t resolvedPlacements = 0;
        std::size_t activeGroups = 0;
        state.projectionCount = 0;
        for (const auto& profile : state.profiles)
        {
            if (!MatchesPluginFilters(profile))
            {
                continue;
            }
            ++activeGroups;
            configuredTransforms += profile.transforms.size();
            configuredPlacements += profile.placements.size();
            for (const auto& [selector, transform] : profile.transforms)
            {
                if (!transform.base && !transform.position &&
                    !transform.rotation && !transform.scale)
                {
                    logger::warn(
                    "[Object Overrides] {} transform={} | fields empty",
                        profile.id,
                        selector);
                    continue;
                }
                if ((transform.position &&
                        !IsFinite(*transform.position)) ||
                    (transform.rotation &&
                        !IsFinite(*transform.rotation)) ||
                    (transform.scale &&
                        !IsValidScale(*transform.scale)))
                {
                    logger::warn(
                    "[Object Overrides] {} transform={} | transform/scale invalid",
                        profile.id,
                        selector);
                    continue;
                }
                const auto formID = FormResolver::Resolve(selector);
                if (!formID)
                {
                    logger::warn(
                    "[Object Overrides] {} transform selector={} | FormID unresolved",
                        profile.id,
                        selector);
                    continue;
                }
                auto* replacementBase = transform.base ?
                                            ResolveTransformBase(
                                                profile,
                                                selector,
                                                *transform.base) :
                                            nullptr;
                if (transform.base && !replacementBase)
                {
                    continue;
                }
                if (const auto owner = state.transformOwners.find(formID);
                    owner != state.transformOwners.end() &&
                    _stricmp(
                        owner->second.c_str(),
                        profile.id.c_str()) != 0)
                {
                    logger::warn(
                    "[Object Overrides] {} transform={:08X} | replaces profile={}",
                        profile.id,
                        formID,
                        owner->second);
                }
                state.transformOwners.insert_or_assign(formID, profile.id);
                state.transforms[formID].push_back(ResolvedTransform{
                    .profile = std::addressof(profile),
                    .selector = selector,
                    .transform = std::addressof(transform),
                    .base = replacementBase,
                });
                state.projectionCount += replacementBase ? 1 : 0;
                ++indexedTransforms;
            }
            for (const auto& [id, placement] : profile.placements)
            {
                resolvedPlacements +=
                    ResolvePlacement(profile, id, placement) ? 1 : 0;
            }
        }
        for (std::size_t index = 0; index < state.placements.size(); ++index)
        {
            const auto* cell = state.placements[index].cell;
            if (cell)
            {
                state.placementsByCell[cell->GetFormID()].push_back(index);
            }
        }
        state.projectionCount += state.placements.size();
        state.initialized = true;
        std::size_t availableReferences = 0;
        std::size_t replayedReferences = 0;
        for (const auto& transform : state.transforms)
        {
            auto* reference =
                RE::TESForm::LookupByID<RE::TESObjectREFR>(
                    transform.first);
            if (!reference)
            {
                continue;
            }
            ++availableReferences;
            if (ApplyResolvedTransforms(state, *reference))
            {
                ++replayedReferences;
            }
        }
        logger::info(
            "[Object Overrides] init | groups={}/{} | transforms={}/{} | placements={}/{} | replay={}/{}",
            activeGroups,
            state.profiles.size(),
            indexedTransforms,
            configuredTransforms,
            resolvedPlacements,
            configuredPlacements,
            replayedReferences,
            availableReferences);
    }

    bool HasProjections()
    {
        return GetState().projectionCount != 0;
    }

    RE::FormID ProjectBase(
        const RE::FormID a_reference,
        const RE::FormID a_base)
    {
        const auto& state = GetState();
        if (!state.initialized || !a_reference || !a_base)
        {
            return a_base;
        }
        const auto* currentBase = RE::TESForm::LookupByID(a_base);
        if (!currentBase ||
            currentBase->GetFormType() != RE::FormType::Static)
        {
            return a_base;
        }
        const auto found = state.transforms.find(a_reference);
        if (found == state.transforms.end())
        {
            return a_base;
        }
        auto projected = a_base;
        for (const auto& transform : found->second)
        {
            if (transform.base)
            {
                projected = transform.base->GetFormID();
            }
        }
        return projected;
    }

    std::size_t GetProjectedPlacementCount()
    {
        return GetState().placements.size();
    }

    bool GetProjectedPlacement(
        const std::size_t a_index,
        RE::FormID* a_cell,
        RE::FormID* a_base,
        std::string_view* a_profile)
    {
        const auto& placements = GetState().placements;
        if (!a_cell || !a_base || a_index >= placements.size())
        {
            return false;
        }
        const auto& placement = placements[a_index];
        if (!placement.cell || !placement.base)
        {
            return false;
        }
        *a_cell = placement.cell->GetFormID();
        *a_base = placement.base->GetFormID();
        if (a_profile)
        {
            *a_profile = placement.profile;
        }
        return true;
    }

    void BeginGameLoad()
    {
        auto& state = GetState();
        if (state.gameLoadPending)
        {
            return;
        }
        state.gameLoadPending = true;
        if (++state.loadGeneration == 0)
        {
            ++state.loadGeneration;
        }
    }

    void CompleteGameLoad(RE::TESObjectCELL* a_cell)
    {
        GetState().gameLoadPending = false;
        if (a_cell)
        {
            EnsurePlacements(a_cell);
        }
    }

    void ApplyTransformsToReference(RE::TESObjectREFR* a_reference)
    {
        auto& state = GetState();
        if (!a_reference)
        {
            return;
        }
        if (!state.initialized)
        {
            return;
        }
        ApplyResolvedTransforms(state, *a_reference);
    }

    void EnsurePlacements(RE::TESObjectCELL* a_cell)
    {
        auto& state = GetState();
        if (!state.initialized || state.placements.empty())
        {
            return;
        }
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::error("[Object Overrides] placements failed | TESDataHandler unavailable");
            return;
        }

        std::size_t created = 0;
        std::size_t retained = 0;
        std::size_t failed = 0;
        const auto ensurePlacement = [&](const ResolvedPlacement& placement)
        {
            if (const auto existing = state.created.find(placement.key);
                existing != state.created.end())
            {
                if (IsExistingPlacement(
                        placement,
                        existing->second,
                        state.loadGeneration))
                {
                    ++retained;
                    return;
                }
            }

            const auto handle = dataHandler->CreateReferenceAtLocation(
                placement.base,
                placement.position,
                placement.rotation,
                placement.cell,
                nullptr,
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);
            auto reference = handle.get();
            if (!reference)
            {
                ++failed;
                logger::error(
                    "[Object Overrides] {} placement={} | cell={:08X} | create failed",
                    placement.profile,
                    placement.id,
                    placement.cell->GetFormID());
                return;
            }

            reference->SetTemporary();
            if (placement.scale)
            {
                reference->SetScale(*placement.scale);
            }
            state.created.insert_or_assign(
                placement.key,
                OwnedPlacement{
                    .handle = handle,
                    .generation = state.loadGeneration,
                });
            ++created;
        };
        if (a_cell)
        {
            const auto found = state.placementsByCell.find(
                a_cell->GetFormID());
            if (found != state.placementsByCell.end())
            {
                for (const auto index : found->second)
                {
                    if (index < state.placements.size())
                    {
                        ensurePlacement(state.placements[index]);
                    }
                }
            }
        }
        else
        {
            for (const auto& placement : state.placements)
            {
                ensurePlacement(placement);
            }
        }
        if (created != 0 || failed != 0)
        {
            logger::info(
                "[Object Overrides] placements | created={} | retained={} | failed={}",
                created,
                retained,
                failed);
        }
    }
}  // namespace MPL::ObjectOverrides::Patches
