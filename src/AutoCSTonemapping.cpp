#include <AutoCSTonemapping.h>
#include <HeliosphanLogic.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <ranges>
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
            bool suppressed = false;
            bool applied = false;
        };

        struct State
        {
            std::mutex lock;
            std::vector<Profile> profiles;
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

    }

    void ClearProfiles()
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.profiles.clear();
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
        const auto found = std::ranges::find_if(
            state.profiles,
            [&](const Profile& a_candidate)
            {
                return EqualsIgnoreCase(
                    a_candidate.id,
                    a_profile);
            });
        return found != state.profiles.end() && !found->suppressed;
    }

    bool SetProfileEnabled(
        const std::string_view a_profile,
        const bool a_enabled)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto found = std::ranges::find_if(
            state.profiles,
            [&](const Profile& a_candidate)
            {
                return EqualsIgnoreCase(
                    a_candidate.id,
                    a_profile);
            });
        if (found == state.profiles.end())
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
        const auto found = std::ranges::find_if(
            state.profiles,
            [&](const Profile& a_candidate)
            {
                return EqualsIgnoreCase(
                    a_candidate.id,
                    a_profile);
            });
        return found != state.profiles.end() && found->applied;
    }

    bool SetProfileSuppressed(
        const std::string_view a_profile,
        const bool a_suppressed)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto found = std::ranges::find_if(
            state.profiles,
            [&](const Profile& a_candidate)
            {
                return EqualsIgnoreCase(
                    a_candidate.id,
                    a_profile);
            });
        if (found == state.profiles.end())
        {
            return false;
        }
        found->suppressed = a_suppressed;
        return true;
    }

    void ApplyStartup()
    {
        std::vector<Profile> profiles;
        {
            auto& state = GetState();
            std::scoped_lock lock(state.lock);
            if (state.applied)
            {
                return;
            }
            state.applied = true;
            profiles = state.profiles;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("[Auto CS Tonemapping] init failed | TESDataHandler unavailable");
            return;
        }

        struct ProfileTargets
        {
            std::string id;
            std::unordered_set<RE::TESImageSpace*> imageSpaces;
        };
        std::vector<ProfileTargets> profileTargets;
        std::unordered_set<RE::TESImageSpace*> targets;
        for (const auto& profile : profiles)
        {
            if (!HeliosphanLogic::ShouldApplyAutoCSTonemapping(
                    profile.suppressed))
            {
                continue;
            }
            ProfileTargets matched{ .id = profile.id };
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
                        matched.imageSpaces.insert(imageSpace);
                        targets.insert(imageSpace);
                    }
                }
            }
            if (!matched.imageSpaces.empty())
            {
                profileTargets.push_back(std::move(matched));
            }
        }
        if (targets.empty())
        {
            return;
        }

        const auto filmicCurve =
            RE::GetINISetting(kFilmicCurveSetting);
        const auto filmicDetected =
            filmicCurve && filmicCurve->GetBool();
        const auto whitePointDetected =
            std::ranges::any_of(
                dataHandler->GetFormArray<RE::TESImageSpace>(),
                [](const RE::TESImageSpace* a_imageSpace)
                {
                    return a_imageSpace &&
                           IsWhitePoint(
                               a_imageSpace->data.hdr.white);
                });
        if (!filmicDetected && !whitePointDetected)
        {
            logger::info(
                "[Auto CS Tonemapping] targets={} | active=false",
                targets.size());
            return;
        }

        for (auto* imageSpace : targets)
        {
            imageSpace->data.hdr.white = kWhitePoint;
        }
        {
            auto& state = GetState();
            std::scoped_lock lock(state.lock);
            for (auto& profile : state.profiles)
            {
                profile.applied = !profile.suppressed &&
                                  std::ranges::any_of(
                                      profileTargets,
                                      [&](const ProfileTargets& a_targets)
                                      {
                                          return EqualsIgnoreCase(
                                              profile.id,
                                              a_targets.id);
                                      });
            }
        }
        if (filmicCurve)
        {
            filmicCurve->SetBool(true);
        }
        if (auto* setting =
                RE::GetINISetting(kFilmicWhiteScaleSetting))
        {
            setting->SetFloat(kWhiteScale);
        }
        logger::info(
            "[Auto CS Tonemapping] targets={} | white=0.1 | filmicINI={} | previousWhite={}",
            targets.size(),
            filmicDetected,
            whitePointDetected);
    }
}
