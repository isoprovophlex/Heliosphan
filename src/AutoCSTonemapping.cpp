#include <AutoCSTonemapping.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace MPL::AutoCSTonemapping
{
    namespace
    {
        constexpr auto kFilmicCurveSetting = "bUseFilmicCurve:Display";
        constexpr auto kFilmicWhiteScaleSetting = "fFilmicWhiteScale:Display";
        constexpr float kWhitePoint = 0.1f;
        constexpr float kWhiteScale = 10.0f;

        struct Profile
        {
            std::string id;
            Settings settings;
            std::unordered_set<RE::TESImageSpace*> targets;
            bool suppressed = false;
            bool applied = false;
        };

        struct State
        {
            std::mutex lock;
            std::vector<Profile> profiles;
            std::unordered_map<RE::TESImageSpace*, float> whiteBaselines;
            std::unordered_set<RE::TESImageSpace*> appliedTargets;
            std::unordered_set<RE::TESImageSpace*> forcedTargets;
            std::optional<bool> filmicCurveBaseline;
            std::optional<float> filmicWhiteScaleBaseline;
            bool autoDetected = false;
            bool forcedTargetsRegistered = false;
            bool applied = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        bool EqualsIgnoreCase(
            const std::string_view a_left,
            const std::string_view a_right)
        {
            return std::ranges::equal(
                a_left,
                a_right,
                [](const char a, const char b)
                {
                    return std::tolower(
                               static_cast<unsigned char>(a)) ==
                           std::tolower(
                               static_cast<unsigned char>(b));
                });
        }

        bool IsWhitePoint(const float a_value)
        {
            return std::abs(a_value - kWhitePoint) <= 0.0001f;
        }

        bool HasSourcePlugin(
            const RE::TESImageSpace* a_imageSpace,
            const RE::TESFile* a_plugin)
        {
            if (!a_imageSpace || !a_plugin)
            {
                return false;
            }
            if (a_imageSpace->sourceFiles.array)
            {
                for (const auto* source :
                     *a_imageSpace->sourceFiles.array)
                {
                    if (source == a_plugin)
                    {
                        return true;
                    }
                }
            }
            return a_imageSpace->GetFile() == a_plugin;
        }

        Profile* FindProfile(State& a_state, const std::string_view a_id)
        {
            const auto found = std::ranges::find_if(a_state.profiles,
                [&](const Profile& a_profile) { return EqualsIgnoreCase(a_profile.id, a_id); });
            return found != a_state.profiles.end() ? std::addressof(*found) : nullptr;
        }

        void CaptureBaselines(State& a_state, RE::TESDataHandler* a_dataHandler)
        {
            if (const auto* setting = RE::GetINISetting(kFilmicCurveSetting);
                setting && !a_state.filmicCurveBaseline)
            {
                a_state.filmicCurveBaseline = setting->GetBool();
            }
            if (const auto* setting = RE::GetINISetting(kFilmicWhiteScaleSetting);
                setting && !a_state.filmicWhiteScaleBaseline)
            {
                a_state.filmicWhiteScaleBaseline = setting->GetFloat();
            }
            for (auto* imageSpace : a_dataHandler->GetFormArray<RE::TESImageSpace>())
            {
                if (imageSpace)
                {
                    a_state.whiteBaselines.try_emplace(imageSpace, imageSpace->data.hdr.white);
                }
            }
        }

        void ApplyProfiles(State& a_state)
        {
            auto targets = a_state.forcedTargets;
            for (auto& profile : a_state.profiles)
            {
                profile.applied = a_state.autoDetected &&
                                  !profile.suppressed &&
                                  !profile.targets.empty();
                if (profile.applied)
                {
                    targets.insert(
                        profile.targets.begin(),
                        profile.targets.end());
                }
            }

            for (auto* imageSpace : a_state.appliedTargets)
            {
                if (targets.contains(imageSpace))
                {
                    continue;
                }
                if (const auto baseline =
                        a_state.whiteBaselines.find(imageSpace);
                    baseline != a_state.whiteBaselines.end())
                {
                    imageSpace->data.hdr.white = baseline->second;
                }
            }
            for (auto* imageSpace : targets)
            {
                if (imageSpace && (!a_state.forcedTargetsRegistered || !a_state.appliedTargets.contains(imageSpace)))
                {
                    imageSpace->data.hdr.white = kWhitePoint;
                }
            }
            a_state.appliedTargets = std::move(targets);

            const bool forceFilmic = !a_state.appliedTargets.empty() ||
                (a_state.forcedTargetsRegistered && std::ranges::any_of(
                    a_state.whiteBaselines, [](const auto& a_entry)
                    { return IsWhitePoint(a_entry.first->data.hdr.white); }));

            if (auto* setting =
                    RE::GetINISetting(kFilmicCurveSetting))
            {
                setting->SetBool(
                    !forceFilmic ?
                        a_state.filmicCurveBaseline.value_or(
                            setting->GetBool()) :
                        true);
            }
            if (auto* setting =
                    RE::GetINISetting(kFilmicWhiteScaleSetting))
            {
                setting->SetFloat(
                    !forceFilmic ?
                        a_state.filmicWhiteScaleBaseline.value_or(
                            setting->GetFloat()) :
                        kWhiteScale);
            }

            logger::info(
                "[Auto CS Tonemapping] targets={} | forced={} | filmic={}",
                a_state.appliedTargets.size(),
                a_state.forcedTargets.size(),
                forceFilmic);
        }

    }

    void ClearProfiles()
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.profiles.clear();
        state.whiteBaselines.clear();
        state.appliedTargets.clear();
        state.forcedTargets.clear();
        state.filmicCurveBaseline.reset();
        state.filmicWhiteScaleBaseline.reset();
        state.autoDetected = false;
        state.forcedTargetsRegistered = false;
        state.applied = false;
    }

    void AddProfile(std::string a_id, Settings a_settings)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.profiles.push_back(Profile{
            .id = std::move(a_id),
            .settings = std::move(a_settings),
        });
    }

    bool GetProfileEnabled(const std::string_view a_profile)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto* found = FindProfile(state, a_profile);
        return found && !found->suppressed;
    }

    bool SetProfileEnabled(
        const std::string_view a_profile,
        const bool a_enabled)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto* found = FindProfile(state, a_profile);
        if (!found)
        {
            logger::error(
                "[Auto CS Tonemapping] setting rejected | profile={} unknown",
                a_profile);
            return false;
        }
        if (!a_enabled)
        {
            logger::warn(
                "[Auto CS Tonemapping] {} disable rejected | control=TuningUtil",
                found->id);
            return false;
        }
        return true;
    }

    bool IsProfileApplied(const std::string_view a_profile)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto* found = FindProfile(state, a_profile);
        return found && found->applied;
    }

    bool SetProfileSuppressed(
        const std::string_view a_profile,
        const bool a_suppressed)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        auto* found = FindProfile(state, a_profile);
        if (!found)
        {
            return false;
        }
        if (state.forcedTargetsRegistered && found->suppressed == a_suppressed)
        {
            return true;
        }
        found->suppressed = a_suppressed;
        if (state.applied)
        {
            ApplyProfiles(state);
        }
        return true;
    }

    bool SetForcedTargets(RE::TESImageSpace* const* a_targets, const std::size_t a_count)
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler || (a_count && !a_targets))
        {
            return false;
        }
        std::unordered_set<RE::TESImageSpace*> targets;
        for (std::size_t index = 0; index < a_count; ++index)
        {
            if (!a_targets[index]) return false;
            targets.insert(a_targets[index]);
        }
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        if (state.forcedTargetsRegistered && state.forcedTargets == targets)
        {
            return true;
        }
        CaptureBaselines(state, dataHandler);
        for (auto* imageSpace : targets)
        {
            state.whiteBaselines.try_emplace(imageSpace, imageSpace->data.hdr.white);
        }
        state.forcedTargetsRegistered = true;
        state.forcedTargets = std::move(targets);
        ApplyProfiles(state);
        return true;
    }

    void ApplyStartup()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("[Auto CS Tonemapping] init failed | TESDataHandler unavailable");
            return;
        }

        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        if (state.applied)
        {
            return;
        }

        CaptureBaselines(state, dataHandler);

        for (auto& profile : state.profiles)
        {
            profile.targets.clear();
            for (const auto& pluginName :
                 profile.settings.plugins)
            {
                const auto* plugin =
                    dataHandler->LookupModByName(pluginName);
                if (!plugin)
                {
                    logger::info(
                        "[Auto CS Tonemapping] {} | status=inactive | plugin={} missing",
                        profile.id,
                        pluginName);
                    continue;
                }
                for (auto* imageSpace :
                     dataHandler
                         ->GetFormArray<RE::TESImageSpace>())
                {
                    if (HasSourcePlugin(imageSpace, plugin))
                    {
                        profile.targets.insert(imageSpace);
                    }
                }
            }
        }

        const auto filmicDetected = state.filmicCurveBaseline.value_or(false);
        const auto whitePointDetected =
            std::ranges::any_of(
                state.whiteBaselines,
                [](const auto& a_entry)
                {
                    return IsWhitePoint(a_entry.second);
                });
        state.autoDetected = filmicDetected || whitePointDetected;
        state.applied = true;
        ApplyProfiles(state);
        logger::info(
            "[Auto CS Tonemapping] detected={} | filmicINI={} | previousWhite={}",
            state.autoDetected,
            filmicDetected,
            whitePointDetected);
    }
}
