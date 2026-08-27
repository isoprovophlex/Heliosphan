#include <WindowObjectOverrides.h>
#include <MMSF_API.h>
#include <WeatherSync.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace MPL::WindowObjectOverrides
{
    namespace
    {
        struct ProfileOverrides
        {
            std::unordered_map<RE::FormID, RE::TESBoundObject*> forms;
        };

        struct State
        {
            std::unordered_map<std::string, ProfileOverrides> profiles;
            std::unordered_set<RE::FormID> sources;
            std::size_t overrideCount = 0;
            bool initialized = false;
        };

        enum class ResolutionFailure
        {
            kNone,
            kNotFound,
            kNotBoundObject,
        };

        struct BoundObjectResolution
        {
            RE::TESBoundObject* object = nullptr;
            RE::FormID formID = 0;
            ResolutionFailure failure = ResolutionFailure::kNotFound;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        std::string_view Trim(std::string_view a_value)
        {
            while (!a_value.empty() &&
                   std::isspace(
                       static_cast<unsigned char>(a_value.front())))
            {
                a_value.remove_prefix(1);
            }
            while (!a_value.empty() &&
                   std::isspace(
                       static_cast<unsigned char>(a_value.back())))
            {
                a_value.remove_suffix(1);
            }
            return a_value;
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

        std::optional<RE::FormID> ParseHex(std::string_view a_value)
        {
            if (a_value.starts_with("0x") || a_value.starts_with("0X"))
            {
                a_value.remove_prefix(2);
            }
            if (a_value.empty())
            {
                return std::nullopt;
            }
            RE::FormID result = 0;
            const auto [end, error] = std::from_chars(
                a_value.data(),
                a_value.data() + a_value.size(),
                result,
                16);
            return error == std::errc{} &&
                           end == a_value.data() + a_value.size() ?
                       std::optional<RE::FormID>{ result } :
                       std::nullopt;
        }

        RE::FormID ResolveFormID(std::string_view a_selector)
        {
            a_selector = Trim(a_selector);
            if (a_selector.empty())
            {
                return 0;
            }

            const auto separator = a_selector.find_first_of("~:");
            if (separator != std::string_view::npos)
            {
                auto* dataHandler = RE::TESDataHandler::GetSingleton();
                const auto local = ParseHex(a_selector.substr(0, separator));
                const auto plugin = Trim(a_selector.substr(separator + 1));
                return dataHandler && local && !plugin.empty() ?
                           dataHandler->LookupFormID(*local, plugin) :
                           0;
            }

            if (auto* mmsf = WeatherSync::GetMMSFAPI())
            {
                const std::string editorID(a_selector);
                if (auto* form = mmsf->LookupCachedForm(editorID))
                {
                    return form->GetFormID();
                }
                if (const auto formID =
                        mmsf->LookupFormIDForEDID(editorID))
                {
                    return formID;
                }
            }
            if (const auto* form = RE::TESForm::LookupByEditorID(a_selector))
            {
                return form->GetFormID();
            }
            if (a_selector.starts_with("0x") ||
                a_selector.starts_with("0X"))
            {
                return ParseHex(a_selector).value_or(0);
            }
            return 0;
        }

        BoundObjectResolution ResolveBoundObject(
            const std::string_view a_selector)
        {
            const auto formID = ResolveFormID(a_selector);
            auto* form = formID ? RE::TESForm::LookupByID(formID) : nullptr;
            if (!form)
            {
                return {};
            }
            auto* object = form->As<RE::TESBoundObject>();
            return {
                .object = object,
                .formID = formID,
                .failure = object ? ResolutionFailure::kNone :
                                    ResolutionFailure::kNotBoundObject,
            };
        }

        void RecordResolutionFailure(
            const WeatherSync::WindowObjectOverrideProfileView& a_profile,
            const std::string_view a_role,
            const std::string_view a_selector,
            const BoundObjectResolution& a_resolution,
            std::size_t& a_missing,
            std::size_t& a_invalidType)
        {
            if (a_resolution.failure == ResolutionFailure::kNotFound)
            {
                ++a_missing;
                if (a_profile.debugLogging)
                {
                    logger::warn(
                        "[Window Sync] [{}] Object override {} '{}' could not be resolved",
                        a_profile.id,
                        a_role,
                        a_selector);
                }
                return;
            }

            ++a_invalidType;
            if (a_profile.debugLogging)
            {
                logger::warn(
                    "[Window Sync] [{}] Object override {} '{}' resolved to non-bound form {:08X}",
                    a_profile.id,
                    a_role,
                    a_selector,
                    a_resolution.formID);
            }
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
        state.profiles.clear();
        state.sources.clear();
        state.overrideCount = 0;

        for (const auto& profile :
            WeatherSync::GetWindowObjectOverrideProfiles())
        {
            if (!profile.objectOverrides || profile.objectOverrides->empty())
            {
                continue;
            }
            auto& overrides = state.profiles[Lowercase(profile.id)];
            std::size_t unresolved = 0;
            std::size_t missingSources = 0;
            std::size_t invalidSourceTypes = 0;
            std::size_t missingTargets = 0;
            std::size_t invalidTargetTypes = 0;
            std::size_t conflicts = 0;
            for (const auto& [sourceSelector, targetSelector] :
                *profile.objectOverrides)
            {
                const auto source = ResolveBoundObject(sourceSelector);
                const auto target = ResolveBoundObject(targetSelector);
                if (!source.object || !target.object)
                {
                    ++unresolved;
                    if (!source.object)
                    {
                        RecordResolutionFailure(
                            profile,
                            "source",
                            sourceSelector,
                            source,
                            missingSources,
                            invalidSourceTypes);
                    }
                    if (!target.object)
                    {
                        RecordResolutionFailure(
                            profile,
                            "target",
                            targetSelector,
                            target,
                            missingTargets,
                            invalidTargetTypes);
                    }
                    continue;
                }
                const auto sourceID = source.object->GetFormID();
                if (const auto existing = overrides.forms.find(sourceID);
                    existing != overrides.forms.end() &&
                    existing->second != target.object)
                {
                    ++conflicts;
                }
                overrides.forms.insert_or_assign(sourceID, target.object);
                state.sources.insert(sourceID);
            }
            state.overrideCount += overrides.forms.size();
            if (unresolved != 0 || conflicts != 0)
            {
                logger::warn(
                    "[Window Sync] [{}] Loaded {} Window Sync object override(s), "
                    "unresolved_rules={}, missing_sources={}, invalid_source_types={}, "
                    "missing_targets={}, invalid_target_types={}, conflicts={}",
                    profile.id,
                    overrides.forms.size(),
                    unresolved,
                    missingSources,
                    invalidSourceTypes,
                    missingTargets,
                    invalidTargetTypes,
                    conflicts);
            }
            else
            {
                logger::info(
                    "[Window Sync] [{}] Loaded {} Window Sync object override(s)",
                    profile.id,
                    overrides.forms.size());
            }
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
        const auto* overrides = FindProfile(a_profile);
        if (!a_base || !overrides)
        {
            return a_base;
        }
        const auto projected = overrides->forms.find(a_base);
        return projected != overrides->forms.end() && projected->second ?
                   projected->second->GetFormID() :
                   a_base;
    }

    bool ApplyToReference(
        RE::TESObjectREFR* a_reference,
        const std::vector<WeatherSync::WindowSyncProfile>& a_profiles)
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

        for (auto profile = a_profiles.rbegin();
             profile != a_profiles.rend();
             ++profile)
        {
            const auto* overrides = FindProfile(profile->id);
            if (!overrides)
            {
                continue;
            }
            const auto found =
                overrides->forms.find(source->GetFormID());
            if (found == overrides->forms.end() || found->second == source)
            {
                continue;
            }
            a_reference->SetObjectReference(found->second);
            if (profile->debugLogging)
            {
                logger::info(
                    "[Window Sync] [{}] Applied object override to reference {:08X}: {:08X} to {:08X}",
                    profile->id,
                    a_reference->GetFormID(),
                    source->GetFormID(),
                    found->second->GetFormID());
            }
            return true;
        }
        return false;
    }

    std::size_t ApplyToCell(
        RE::TESObjectCELL* a_cell,
        const std::vector<WeatherSync::WindowSyncProfile>& a_profiles)
    {
        if (!a_cell || a_profiles.empty())
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
        if (changed != 0 &&
            std::ranges::any_of(
                a_profiles,
                [](const auto& a_profile)
                {
                    return a_profile.debugLogging;
                }))
        {
            logger::info(
                "[Window Sync] Applied {} Window Sync object override(s) in cell {:08X}",
                changed,
                a_cell->GetFormID());
        }
        return changed;
    }
}  // namespace MPL::WindowObjectOverrides
