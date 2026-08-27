#include <AutoCSTonemapping.h>
#include <CellClassifier.h>
#include <ExternalEmittance.h>
#include <LumaClient.h>
#include <LightPlacer.h>
#include <MMSF_API.h>
#include <ObjectOverrides.h>
#include <RegionRuntime.h>
#include <RegionWeatherPatcher.h>
#include <WeatherRuntime.h>
#include <Heliosphan.h>
#include <WindowSync.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <rfl/Skip.hpp>

namespace MPL::Heliosphan
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr auto kConfigurationRoot = "Data/Luma/Heliosphan";
        constexpr auto kReadinessPollInterval = 16ms;
        constexpr std::uint32_t kMaxReadinessPollAttempts = 1000;
        constexpr auto kSkyUpdateWatchdogDelay = 2s;
        constexpr auto kExpirationPoll = 1000ms;
        constexpr auto kCurrentWeatherNotificationDelay = 2s;
        constexpr auto kNotificationDurationAdjustmentDelay = 100ms;
        constexpr float kWeatherTransitionSelectionThreshold = 0.5f;

        struct EmittancePatchingSettings
        {
            std::vector<std::string> Forms;
            std::vector<std::string> lightPlacer;
            std::string emmitance;
        };

        struct WindowSyncSettings
        {
            struct RoomMarkerCleaningSettings
            {
                std::vector<std::string> plugins;
                std::vector<std::string> excludedPlugins;
            };

            bool enabled = false;
            std::optional<bool> showSky;
            std::optional<bool> useSkyLighting;
            std::optional<bool> sunlightShadows;
            std::optional<RoomMarkerCleaningSettings> cleanRoomMarkers;
            EmittancePatchingSettings emittancePatching;
            std::optional<CellClassifier::Settings> cellContains;
            std::vector<ObjectOverrides::Patches::Group> objects;
        };

        struct WeatherSyncSettings
        {
            bool enabled = false;
            std::string weatherPrefix;
            std::string fallbackWeather;
            std::string regionPrefix;
            std::string fallbackRegion;
        };

        struct Settings
        {
            std::string id;
            std::string plugin;
            std::string cellPatchProvider;
            float overrideDurationGameHours = 3.0f;
            bool detailedLogging = false;
            bool notifications = false;
            AutoCSTonemapping::Settings autoCSTonemapping;
            EmittancePatchingSettings emittancePatching;
            WeatherSyncSettings weatherSync;
            WindowSyncSettings windowSync;
            RegionWeatherPatcher::Settings regionWeatherPatcher;
            std::vector<ObjectOverrides::Patches::Group> objects;
            rfl::Skip<std::map<std::string, std::string>>
                compiledGlobalOverrides;
            rfl::Skip<std::map<std::string, std::string>>
                compiledWindowOverrides;
        };

        class Scheduler
        {
        public:
            Scheduler():
             worker([this](std::stop_token a_stop)
                 { Run(a_stop); })
            {
            }

            void Schedule(std::chrono::milliseconds a_delay, std::function<void()> a_task)
            {
                {
                    std::lock_guard lock(mutex);
                    tasks.emplace(std::chrono::steady_clock::now() + a_delay, std::move(a_task));
                }
                wake.notify_all();
            }

        private:
            void Run(std::stop_token a_stop)
            {
                std::unique_lock lock(mutex);
                while (!a_stop.stop_requested())
                {
                    if (tasks.empty())
                    {
                        wake.wait(lock, a_stop, [this]
                            { return !tasks.empty(); });
                        continue;
                    }

                    const auto due = tasks.begin()->first;
                    if (wake.wait_until(lock, a_stop, due, [this, due]
                            { return tasks.empty() || tasks.begin()->first < due; }))
                    {
                        continue;
                    }
                    if (a_stop.stop_requested() || tasks.empty() || tasks.begin()->first > std::chrono::steady_clock::now())
                    {
                        continue;
                    }

                    auto task = std::move(tasks.begin()->second);
                    tasks.erase(tasks.begin());
                    lock.unlock();
                    if (auto* taskInterface = SKSE::GetTaskInterface())
                    {
                        taskInterface->AddTask(std::move(task));
                    }
                    lock.lock();
                }
            }

            std::mutex mutex;
            std::condition_variable_any wake;
            std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> tasks;
            std::jthread worker;
        };

        struct State
        {
            std::vector<Settings> profiles;
            std::vector<bool> roomMarkerCleaningActive;
            std::vector<std::uint32_t> profileLoadOrder;
            std::vector<bool> profileUsesPluginLoadOrder;
            bool profilePrioritiesResolved = false;
            std::unordered_map<RE::FormID, std::size_t> cells;
            std::optional<std::size_t> activeProfile;
            std::optional<std::size_t> pendingProfile;
            RE::TESWeather* pendingSource = nullptr;
            RE::TESObjectCELL* pendingCell = nullptr;
            RE::TESWeather* ownedOverride = nullptr;
            std::optional<float> releaseAtGameDay;
            bool pendingOverrideReleaseNotification = false;
            bool pendingDefaultRegionNotification = false;
            bool pendingDefaultWeatherNotification = false;
            bool gameLoadPending = false;
            std::uint32_t readinessPollAttempts = 0;
            std::uint64_t generation = 0;
            MPL::API::MMSF::Interface* mmsf = nullptr;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        Scheduler& GetScheduler()
        {
            static Scheduler scheduler;
            return scheduler;
        }

        void ClearPendingTransition(State& a_state)
        {
            a_state.pendingProfile.reset();
            a_state.pendingSource = nullptr;
            a_state.pendingCell = nullptr;
            a_state.pendingOverrideReleaseNotification = false;
            a_state.pendingDefaultRegionNotification = false;
            a_state.pendingDefaultWeatherNotification = false;
        }

        std::string ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            std::string text(std::istreambuf_iterator<char>(file), {});
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom)) text.erase(0, bom.size());
            return text;
        }

        bool EqualsIgnoreCase(std::string_view a_left, std::string_view a_right)
        {
            return std::ranges::equal(a_left, a_right, [](const char a, const char b)
                { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
        }

        bool StartsWithIgnoreCase(std::string_view a_text, std::string_view a_prefix)
        {
            return a_text.size() >= a_prefix.size() &&
                   EqualsIgnoreCase(a_text.substr(0, a_prefix.size()), a_prefix);
        }

        bool HasWindowLayerSettings(const WindowSyncSettings& a_settings)
        {
            return a_settings.showSky.has_value() ||
                   a_settings.useSkyLighting.has_value() ||
                   a_settings.sunlightShadows.has_value() ||
                   a_settings.cleanRoomMarkers.has_value() ||
                   a_settings.cellContains.has_value() ||
                   !a_settings.emittancePatching.Forms.empty() ||
                   !a_settings.emittancePatching.lightPlacer.empty() ||
                   !a_settings.emittancePatching.emmitance.empty();
        }

        bool HasObjectOverrides(const Settings& a_settings)
        {
            return !a_settings.compiledWindowOverrides.get().empty();
        }

        bool SynchronizesWeather(const Settings& a_settings)
        {
            return a_settings.weatherSync.enabled &&
                   !a_settings.weatherSync.weatherPrefix.empty() &&
                   !a_settings.weatherSync.regionPrefix.empty();
        }

        bool DefinesWindowSyncProfile(const Settings& a_settings)
        {
            return SynchronizesWeather(a_settings) ||
                   (a_settings.windowSync.enabled &&
                       (HasWindowLayerSettings(a_settings.windowSync) ||
                           HasObjectOverrides(a_settings)));
        }

        void ResolveWindowSyncProfilePriorities(State& a_state)
        {
            if (a_state.profilePrioritiesResolved)
            {
                return;
            }
            a_state.profileLoadOrder.resize(a_state.profiles.size());
            a_state.profileUsesPluginLoadOrder.assign(
                a_state.profiles.size(),
                false);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            for (std::size_t index = 0;
                 index < a_state.profiles.size();
                 ++index)
            {
                const auto& settings = a_state.profiles[index];
                a_state.profileLoadOrder[index] =
                    static_cast<std::uint32_t>(index);
                if (!dataHandler)
                {
                    continue;
                }
                const auto& provider = settings.plugin.empty() ?
                                           settings.id :
                                           settings.plugin;
                std::size_t fileIndex = 0;
                for (const auto* file : dataHandler->files)
                {
                    if (!file)
                    {
                        ++fileIndex;
                        continue;
                    }
                    const std::string_view filename{ file->GetFilename() };
                    const bool matches = provider.contains('.') ?
                                             EqualsIgnoreCase(filename, provider) :
                                             EqualsIgnoreCase(
                                                 std::filesystem::path(filename)
                                                     .stem()
                                                     .string(),
                                                 provider);
                    if (!matches)
                    {
                        ++fileIndex;
                        continue;
                    }
                    const std::string_view summary{ file->summary.c_str() };
                    if (summary.contains("[Luma]"))
                    {
                        a_state.profileLoadOrder[index] =
                            static_cast<std::uint32_t>(fileIndex);
                        a_state.profileUsesPluginLoadOrder[index] = true;
                        logger::info(
                            "[Heliosphan] [{}] Layer priority resolved from [Luma] plugin '{}' at load-order position {}",
                            settings.id,
                            filename,
                            fileIndex);
                    }
                    else
                    {
                        logger::warn(
                            "[Heliosphan] [{}] Provider plugin '{}' is loaded but its description does not contain [Luma]; configuration discovery order will be used for layering",
                            settings.id,
                            filename);
                    }
                    break;
                }
            }
            a_state.profilePrioritiesResolved = true;
        }

        std::string GetEditorID(RE::TESWeather* a_weather)
        {
            auto& state = GetState();
            if (!a_weather || !state.mmsf)
            {
                return {};
            }
            return state.mmsf->LookupEDIDForFormID(a_weather->formID);
        }

        RE::TESWeather* LookupWeather(std::string_view a_editorID)
        {
            auto& state = GetState();
            if (!state.mmsf || a_editorID.empty())
            {
                return nullptr;
            }
            if (auto* cached = state.mmsf->LookupCachedForm(std::string(a_editorID)))
            {
                return cached->As<RE::TESWeather>();
            }
            const auto formID = state.mmsf->LookupFormIDForEDID(std::string(a_editorID));
            return formID ? RE::TESForm::LookupByID<RE::TESWeather>(formID) : nullptr;
        }

        bool DetailedLogsEnabled()
        {
            return std::ranges::any_of(
                GetState().profiles,
                [](const Settings& a_settings)
                { return a_settings.detailedLogging; });
        }

        template <class... Args>
        void LogDetailed(std::format_string<Args...> a_format, Args&&... a_args)
        {
            if (DetailedLogsEnabled())
            {
                logger::info(
                    "[Heliosphan] {}",
                    std::format(a_format, std::forward<Args>(a_args)...));
            }
        }

        std::string DescribeWeather(RE::TESWeather* a_weather)
        {
            if (!a_weather)
            {
                return "<none>";
            }
            const auto editorID = GetEditorID(a_weather);
            return editorID.empty() ?
                       std::format("<no EditorID> [{:08X}]", a_weather->formID) :
                       std::format("{} [{:08X}]", editorID, a_weather->formID);
        }

        std::string DescribeProfile(const std::optional<std::size_t> a_profile)
        {
            const auto& profiles = GetState().profiles;
            return a_profile && *a_profile < profiles.size() ?
                       std::format("{} [{}]", profiles[*a_profile].id, *a_profile) :
                       "<none>";
        }

        void LogProfile(const Settings& a_settings, const std::string& a_message)
        {
            if (a_settings.detailedLogging)
            {
                logger::info("[Heliosphan] [{}] {}", a_settings.id, a_message);
            }
        }

        void DoubleNotificationDuration(const std::string& a_message)
        {
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
            if (!menu)
            {
                return;
            }

            const auto now = RE::GetDurationOfApplicationRunTime();
            for (auto* object : menu->GetRuntimeData().objects)
            {
                auto* notifications = skyrim_cast<RE::HUDNotifications*>(object);
                if (!notifications)
                {
                    continue;
                }
                for (auto& notification : notifications->queue)
                {
                    const std::string_view text{ notification.text.c_str() };
                    if (text != a_message)
                    {
                        continue;
                    }
                    if (notification.time > now)
                    {
                        const auto remaining = notification.time - now;
                        notification.time =
                            remaining <= std::numeric_limits<std::uint32_t>::max() - notification.time ?
                                notification.time + remaining :
                                std::numeric_limits<std::uint32_t>::max();
                    }
                }
            }
        }

        void ShowNotification(const Settings& a_settings, std::string a_message)
        {
            if (!a_settings.notifications)
            {
                return;
            }
            a_message.insert(0, std::format("{}: ", a_settings.id));
            RE::SendHUDMessage::ShowHUDMessage(a_message.c_str(), nullptr, false);
            GetScheduler().Schedule(
                kNotificationDurationAdjustmentDelay,
                [message = std::move(a_message)]
                {
                    DoubleNotificationDuration(message);
                });
        }

        std::string CurrentRegionEditorID()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto region = RegionRuntime::GetRegion(
                GetState().mmsf,
                player ? player->GetParentCell() : nullptr);
            return region.empty() ? "<none>" : region;
        }

        void ScheduleCurrentWeatherNotification(
            const std::size_t a_profile,
            const std::uint64_t a_generation)
        {
            GetScheduler().Schedule(kCurrentWeatherNotificationDelay, [a_profile, a_generation]
                {
                auto& state = GetState();
                if (a_generation != state.generation || a_profile >= state.profiles.size())
                {
                    return;
                }
                auto* sky = RE::Sky::GetSingleton();
                auto editorID = GetEditorID(sky ? sky->currentWeather : nullptr);
                if (editorID.empty())
                {
                    editorID = "<none>";
                }
                ShowNotification(
                    state.profiles[a_profile],
                    std::format("Current Weather: {}", editorID)); });
        }

        std::optional<std::size_t> ProfileForWeather(RE::TESWeather* a_weather)
        {
            const auto editorID = GetEditorID(a_weather);
            if (editorID.empty())
            {
                return std::nullopt;
            }
            const auto& profiles = GetState().profiles;
            for (std::size_t index = 0; index < profiles.size(); ++index)
            {
                const auto& weatherSync = profiles[index].weatherSync;
                if (SynchronizesWeather(profiles[index]) &&
                    StartsWithIgnoreCase(editorID, weatherSync.weatherPrefix))
                {
                    return index;
                }
            }
            return std::nullopt;
        }

        std::string BaseEditorID(RE::TESWeather* a_weather, std::optional<std::size_t> a_sourceProfile)
        {
            auto editorID = GetEditorID(a_weather);
            if (a_sourceProfile)
            {
                const auto& prefix =
                    GetState().profiles[*a_sourceProfile].weatherSync.weatherPrefix;
                if (StartsWithIgnoreCase(editorID, prefix))
                {
                    editorID.erase(0, prefix.size());
                }
            }
            return editorID;
        }

        bool ReleaseOwnedOverride()
        {
            auto& state = GetState();
            auto* sky = RE::Sky::GetSingleton();
            const bool hadReleaseTimer = state.ownedOverride || state.releaseAtGameDay;
            LogDetailed(
                "Override release requested: owned={}, active sky override={}, owns active override={}",
                DescribeWeather(state.ownedOverride),
                DescribeWeather(sky ? sky->overrideWeather : nullptr),
                state.ownedOverride && sky && sky->overrideWeather == state.ownedOverride);
            if (state.ownedOverride && sky && sky->overrideWeather == state.ownedOverride)
            {
                sky->ReleaseWeatherOverride();
            }
            state.ownedOverride = nullptr;
            state.releaseAtGameDay.reset();
            LogDetailed("Owned override tracking cleared");
            return hadReleaseTimer;
        }

        void CheckExpiration(std::uint64_t a_generation)
        {
            auto& state = GetState();
            if (a_generation != state.generation || !state.activeProfile || !state.ownedOverride ||
                !state.releaseAtGameDay)
            {
                return;
            }

            auto* sky = RE::Sky::GetSingleton();
            if (!sky || sky->overrideWeather != state.ownedOverride)
            {
                LogDetailed(
                    "Override expiration cancelled because the active override changed: expected={}, actual={}",
                    DescribeWeather(state.ownedOverride),
                    DescribeWeather(sky ? sky->overrideWeather : nullptr));
                state.ownedOverride = nullptr;
                state.releaseAtGameDay.reset();
                return;
            }

            const auto* calendar = RE::Calendar::GetSingleton();
            if (calendar && calendar->GetCurrentGameTime() >= *state.releaseAtGameDay)
            {
                const auto profile = *state.activeProfile;
                LogDetailed(
                    "Override duration expired at game day {:.6f}; scheduled release was {:.6f}",
                    calendar->GetCurrentGameTime(),
                    *state.releaseAtGameDay);
                ReleaseOwnedOverride();
                LogProfile(state.profiles[profile], "Weather override released after its timer expired");
                ShowNotification(state.profiles[profile], "Weather Override Released");
                return;
            }

            GetScheduler().Schedule(kExpirationPoll, [a_generation]
                { CheckExpiration(a_generation); });
        }

        void ApplyTransition(std::uint64_t a_generation)
        {
            auto& state = GetState();
            LogDetailed(
                "Applying transition generation {}: current generation={}, source={}, active profile={}, destination profile={}",
                a_generation,
                state.generation,
                DescribeWeather(state.pendingSource),
                DescribeProfile(state.activeProfile),
                DescribeProfile(state.pendingProfile));
            if (a_generation != state.generation || !state.pendingSource)
            {
                LogDetailed(
                    "Discarded transition generation {} because it was stale or no longer had a source weather",
                    a_generation);
                return;
            }
            const bool showOverrideReleased = std::exchange(
                state.pendingOverrideReleaseNotification,
                false);
            const bool showDefaultRegionApplied = std::exchange(
                state.pendingDefaultRegionNotification,
                false);
            const bool showDefaultWeatherApplied = std::exchange(
                state.pendingDefaultWeatherNotification,
                false);

            const auto sourceProfile = state.activeProfile ? state.activeProfile : ProfileForWeather(state.pendingSource);
            const auto reportProfile =
                state.pendingProfile ? state.pendingProfile : sourceProfile;
            const auto baseEditorID = BaseEditorID(state.pendingSource, sourceProfile);
            LogDetailed(
                "Resolved source profile={} and base weather EditorID='{}'",
                DescribeProfile(sourceProfile),
                baseEditorID);
            if (baseEditorID.empty())
            {
                logger::warn("[Weather Sync] Could not resolve the source weather EditorID");
                if (reportProfile)
                {
                    const auto& settings = state.profiles[*reportProfile];
                    const auto failureMessage = std::format(
                        "{}: Weather sync has failed. Please report.",
                        settings.id);
                    RE::DebugMessageBox(failureMessage.c_str());
                }
                state.activeProfile.reset();
                ClearPendingTransition(state);
                return;
            }

            const bool entering = state.pendingProfile.has_value();
            auto targetEditorID = baseEditorID;
            if (entering)
            {
                targetEditorID =
                    state.profiles[*state.pendingProfile].weatherSync.weatherPrefix +
                    baseEditorID;
            }
            const auto pairedTargetEditorID = targetEditorID;
            auto* target = LookupWeather(targetEditorID);
            bool usedRegionWeatherFallback = false;
            bool usedConfiguredFallback = false;
            LogDetailed(
                "Resolved target EditorID='{}' to {}; transition mode={}",
                targetEditorID,
                DescribeWeather(target),
                entering ? "entering synchronized interior" : "leaving synchronized interior");
            if (!target)
            {
                if (reportProfile)
                {
                    logger::warn(
                        "[Weather Sync] [{}] Could not find paired weather {}",
                        state.profiles[*reportProfile].id,
                        targetEditorID);
                }
                else
                {
                    logger::warn("[Weather Sync] Could not find paired weather {}", targetEditorID);
                }
            }

            auto previousEditorID =
                showDefaultWeatherApplied ? std::string{} : GetEditorID(state.pendingSource);
            if (previousEditorID.empty())
            {
                previousEditorID = "<none>";
            }

            if (!target && entering)
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* region = RegionRuntime::GetRegionForm(
                    player ? player->GetParentCell() : nullptr);
                auto* selected = region ? region->SelectWeather() : nullptr;
                if (selected && ProfileForWeather(selected) == state.pendingProfile)
                {
                    target = selected;
                    targetEditorID = GetEditorID(target);
                    usedRegionWeatherFallback = true;
                    LogDetailed(
                        "Selected synchronized region weather {} because the exact pair was unavailable",
                        DescribeWeather(target));
                }
                else
                {
                    LogDetailed(
                        "Synchronized region selection did not provide a valid profile weather: region={}, selected={}",
                        region ? std::format("{:08X}", region->formID) : "<none>",
                        DescribeWeather(selected));
                }
            }

            if (!target && reportProfile)
            {
                const auto& settings = state.profiles[*reportProfile];
                auto fallbackEditorID = settings.weatherSync.fallbackWeather;
                if (entering &&
                    !StartsWithIgnoreCase(
                        fallbackEditorID,
                        settings.weatherSync.weatherPrefix))
                {
                    fallbackEditorID.insert(
                        0,
                        settings.weatherSync.weatherPrefix);
                }
                else if (!entering &&
                         StartsWithIgnoreCase(
                             fallbackEditorID,
                             settings.weatherSync.weatherPrefix))
                {
                    fallbackEditorID.erase(
                        0,
                        settings.weatherSync.weatherPrefix.size());
                }

                auto* fallback = LookupWeather(fallbackEditorID);
                const auto fallbackProfile = ProfileForWeather(fallback);
                const bool validFallback =
                    fallback &&
                    (entering ? fallbackProfile == state.pendingProfile : !fallbackProfile);
                if (validFallback)
                {
                    target = fallback;
                    targetEditorID = std::move(fallbackEditorID);
                    usedConfiguredFallback = true;
                    LogDetailed(
                        "Using configured fallback weather {}",
                        DescribeWeather(target));
                }
                else
                {
                    logger::error(
                        "[Weather Sync] [{}] Configured fallback weather '{}' is unavailable or does not belong to the expected profile",
                        settings.id,
                        fallbackEditorID);
                }
            }

            if (!target)
            {
                if (reportProfile)
                {
                    const auto profile = *reportProfile;
                    const auto& settings = state.profiles[profile];
                    LogProfile(
                        settings,
                        "Weather sync failed because no exact, region-selected, or fallback weather was available");
                    if (showOverrideReleased)
                    {
                        ShowNotification(settings, "Weather Override Released");
                    }
                    ShowNotification(
                        settings,
                        std::format("Interior: {}", entering ? "True" : "False"));
                    if (showDefaultRegionApplied)
                    {
                        ShowNotification(
                            settings,
                            "Previous region not found. Default region applied.");
                    }
                    ShowNotification(
                        settings,
                        std::format("Current Region: {}", CurrentRegionEditorID()));
                    ShowNotification(
                        settings,
                        std::format("Previous Weather: {}", previousEditorID));
                    ShowNotification(
                        settings,
                        std::format("Target Weather: {} (Not Found)", targetEditorID));
                    ScheduleCurrentWeatherNotification(profile, a_generation);
                    const auto failureMessage = std::format(
                        "{}: Weather sync has failed. Please report.",
                        settings.id);
                    RE::DebugMessageBox(failureMessage.c_str());
                }
                state.activeProfile.reset();
                ClearPendingTransition(state);
                return;
            }

            const auto result =
                WeatherRuntime::SetWeatherInstant(target, entering);
            LogDetailed(
                "SetWeatherInstant completed through the native cell emittance path: target={}, override={}, status={}, light entries processed={}",
                DescribeWeather(target),
                entering,
                static_cast<std::uint32_t>(result.status),
                result.lightCount);
            state.activeProfile = state.pendingProfile;
            state.pendingSource = nullptr;
            state.pendingCell = nullptr;
            if (!entering)
            {
                state.pendingProfile.reset();
            }
            else
            {
                const auto profile = *state.activeProfile;
                state.ownedOverride = target;
                if (usedConfiguredFallback)
                {
                    ReleaseOwnedOverride();
                    LogDetailed(
                        "Released configured fallback override immediately so the synchronized region can select its weather");
                }
                else if (const auto* calendar = RE::Calendar::GetSingleton();
                    calendar && state.profiles[profile].overrideDurationGameHours > 0.0f)
                {
                    state.releaseAtGameDay =
                        calendar->GetCurrentGameTime() +
                        state.profiles[profile].overrideDurationGameHours / RE::Calendar::GetHoursPerDay();
                    LogDetailed(
                        "Scheduled override release: current game day={:.6f}, release game day={:.6f}, duration hours={:.3f}",
                        calendar->GetCurrentGameTime(),
                        *state.releaseAtGameDay,
                        state.profiles[profile].overrideDurationGameHours);
                    GetScheduler().Schedule(kExpirationPoll, [a_generation]
                        { CheckExpiration(a_generation); });
                }
            }

            if (reportProfile)
            {
                const auto profile = *reportProfile;
                LogProfile(
                    state.profiles[profile],
                    std::format(
                        "{} weather {} ({} native cell emittance light entries processed).",
                        entering ? "Applied" : "Restored",
                        targetEditorID,
                        result.lightCount));
                if (showOverrideReleased)
                {
                    ShowNotification(state.profiles[profile], "Weather Override Released");
                }
                ShowNotification(
                    state.profiles[profile],
                    std::format("Interior: {}", entering ? "True" : "False"));
                if (showDefaultRegionApplied)
                {
                    ShowNotification(
                        state.profiles[profile],
                        "Previous region not found. Default region applied.");
                }
                ShowNotification(
                    state.profiles[profile],
                    std::format("Current Region: {}", CurrentRegionEditorID()));
                ShowNotification(
                    state.profiles[profile],
                    std::format("Previous Weather: {}", previousEditorID));
                ShowNotification(
                    state.profiles[profile],
                    std::format("Target Weather: {}", pairedTargetEditorID));
                if (usedRegionWeatherFallback)
                {
                    ShowNotification(
                        state.profiles[profile],
                        "Target weather not found. Region weather applied.");
                }
                if (showDefaultWeatherApplied || usedConfiguredFallback)
                {
                    ShowNotification(
                        state.profiles[profile],
                        "Previous weather not found. Default weather applied.");
                }
                ScheduleCurrentWeatherNotification(profile, a_generation);
            }
        }

        std::string_view CellReadinessIssue(
            RE::TESObjectCELL* a_cell,
            const bool a_requireRegion)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || player->GetParentCell() != a_cell)
            {
                return "the player parent cell does not match the destination";
            }
            if (!a_cell || !a_cell->IsAttached())
            {
                return "the destination cell is not attached";
            }
            if (!a_cell->GetRuntimeData().loadedData)
            {
                return "the destination cell has no loaded data";
            }
            if (a_requireRegion)
            {
                const auto* cellRegion =
                    a_cell->extraList.GetByType<RE::ExtraCellSkyRegion>();
                auto* sky = RE::Sky::GetSingleton();
                if (cellRegion && cellRegion->skyRegion &&
                    (!sky || sky->region != cellRegion->skyRegion))
                {
                    return "Sky::region has not accepted the destination region";
                }
            }
            return {};
        }

        void ProcessEngineReadyWork(
            const std::uint64_t a_generation,
            const bool a_requireRegion,
            const std::string_view a_trigger)
        {
            auto& state = GetState();
            if (a_generation != state.generation)
            {
                LogDetailed(
                    "Skipped stale {} readiness work for generation {}; current generation is {}",
                    a_trigger,
                    a_generation,
                    state.generation);
                return;
            }

            if (state.gameLoadPending)
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* cell = player ? player->GetParentCell() : nullptr;
                const auto issue =
                    CellReadinessIssue(cell, a_requireRegion);
                if (!issue.empty())
                {
                    LogDetailed(
                        "Deferred game-load initialization after {} because {}",
                        a_trigger,
                        issue);
                    return;
                }

                state.gameLoadPending = false;
                ObjectOverrides::Patches::CompleteGameLoad(cell);
                ExternalEmittance::ReplayCell(cell);
                auto* sourceWeather = CaptureSourceWeather();
                LogDetailed(
                    "Game-load initialization accepted after {}: player cell={}, captured weather={}",
                    a_trigger,
                    cell ? std::format("{:08X}", cell->formID) : "<none>",
                    DescribeWeather(sourceWeather));
                auto* sourceRegion = WindowSync::CaptureSourceRegion();
                WindowSync::PrepareCellChange(
                    cell,
                    sourceWeather,
                    sourceRegion);
                WindowSync::FinishCellChange(cell);
            }

            if (!state.pendingSource)
            {
                return;
            }

            const auto issue =
                CellReadinessIssue(state.pendingCell, a_requireRegion);
            if (!issue.empty())
            {
                LogDetailed(
                    "Deferred transition generation {} after {} because {}",
                    state.generation,
                    a_trigger,
                    issue);
                return;
            }

            LogDetailed(
                "Transition generation {} accepted after {} on readiness attempt {}: destination cell {:08X} is current, attached, and loaded{}",
                state.generation,
                a_trigger,
                state.readinessPollAttempts,
                state.pendingCell->formID,
                a_requireRegion ? ", with the destination region active" : "");
            state.readinessPollAttempts = 0;
            ApplyTransition(state.generation);
        }

        void RunReadinessPoll(const std::uint64_t a_generation)
        {
            auto& state = GetState();
            if (a_generation != state.generation ||
                (!state.gameLoadPending && !state.pendingSource))
            {
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell =
                state.gameLoadPending ?
                    (player ? player->GetParentCell() : nullptr) :
                    state.pendingCell;
            const auto issue = CellReadinessIssue(cell, true);
            ++state.readinessPollAttempts;
            if (issue.empty())
            {
                ProcessEngineReadyWork(
                    a_generation,
                    true,
                    "transition readiness poll");
            }
            else if (state.readinessPollAttempts >=
                     kMaxReadinessPollAttempts)
            {
                logger::error(
                    "[Weather Sync] Cancelled transition generation {} after "
                    "{} readiness attempts; last condition: {}",
                    a_generation,
                    state.readinessPollAttempts,
                    issue);
                ++state.generation;
                state.gameLoadPending = false;
                ObjectOverrides::Patches::CompleteGameLoad(nullptr);
                ExternalEmittance::ReplayCell(nullptr);
                state.readinessPollAttempts = 0;
                ReleaseOwnedOverride();
                state.activeProfile.reset();
                ClearPendingTransition(state);
                return;
            }

            if (a_generation != state.generation ||
                (!state.gameLoadPending && !state.pendingSource))
            {
                return;
            }

            GetScheduler().Schedule(
                kReadinessPollInterval,
                [a_generation]
                { RunReadinessPoll(a_generation); });
        }

        void ScheduleReadinessPoll(const std::uint64_t a_generation)
        {
            GetScheduler().Schedule(
                kReadinessPollInterval,
                [a_generation]
                { RunReadinessPoll(a_generation); });
        }

        void RunReadinessWatchdog(const std::uint64_t a_generation)
        {
            auto& state = GetState();
            if (a_generation != state.generation ||
                (!state.gameLoadPending && !state.pendingSource))
            {
                return;
            }

            logger::warn(
                "[Weather Sync] Transition readiness polling did not complete generation "
                "{} within {} ms; attempting the attached-cell fallback",
                a_generation,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kSkyUpdateWatchdogDelay)
                    .count());
            ProcessEngineReadyWork(
                a_generation,
                false,
                "readiness watchdog");

            if (a_generation == state.generation &&
                (state.gameLoadPending || state.pendingSource))
            {
                GetScheduler().Schedule(
                    kSkyUpdateWatchdogDelay,
                    [a_generation]
                    { RunReadinessWatchdog(a_generation); });
            }
        }

        void ScheduleReadinessWatchdog(const std::uint64_t a_generation)
        {
            GetScheduler().Schedule(
                kSkyUpdateWatchdogDelay,
                [a_generation]
                { RunReadinessWatchdog(a_generation); });
        }

        void ScheduleTransition(
            std::optional<std::size_t> a_destinationProfile,
            RE::TESObjectCELL* a_destinationCell,
            RE::TESWeather* a_sourceWeather,
            bool a_usedDefaultRegion,
            bool a_usedDefaultWeather)
        {
            auto& state = GetState();
            const auto generation = ++state.generation;
            state.pendingProfile = a_destinationProfile;
            state.pendingSource = a_sourceWeather;
            state.pendingCell = a_destinationCell;
            state.pendingOverrideReleaseNotification = false;
            state.pendingDefaultRegionNotification = a_usedDefaultRegion;
            state.pendingDefaultWeatherNotification = a_usedDefaultWeather;
            state.readinessPollAttempts = 0;
            LogDetailed(
                "Transition generation {} started bounded readiness polling: source={}, destination profile={}, destination cell={}",
                generation,
                DescribeWeather(a_sourceWeather),
                DescribeProfile(a_destinationProfile),
                a_destinationCell ?
                    std::format("{:08X}", a_destinationCell->formID) :
                    "<none>");
            if (!a_destinationProfile)
            {
                const auto releaseProfile =
                    state.activeProfile ? state.activeProfile : ProfileForWeather(a_sourceWeather);
                if (ReleaseOwnedOverride() && releaseProfile)
                {
                    LogProfile(
                        state.profiles[*releaseProfile],
                        "Weather override released because the synchronized interior was exited");
                    state.pendingOverrideReleaseNotification = true;
                }
            }
            ScheduleReadinessPoll(generation);
            ScheduleReadinessWatchdog(generation);
        }
    }  // namespace

    bool IsDetailedLoggingEnabled()
    {
        return DetailedLogsEnabled();
    }

    bool GetProfileDetailedLogging(const std::string_view a_profile)
    {
        const auto found = std::ranges::find_if(
            GetState().profiles,
            [&](const Settings& a_settings)
            {
                return EqualsIgnoreCase(a_settings.id, a_profile);
            });
        return found != GetState().profiles.end() &&
               found->detailedLogging;
    }

    bool GetProfileNotifications(const std::string_view a_profile)
    {
        const auto found = std::ranges::find_if(
            GetState().profiles,
            [&](const Settings& a_settings)
            {
                return EqualsIgnoreCase(a_settings.id, a_profile);
            });
        return found != GetState().profiles.end() &&
               found->notifications;
    }

    bool SetProfileDetailedLogging(
        const std::string_view a_profile,
        const bool a_enabled)
    {
        auto& profiles = GetState().profiles;
        const auto found = std::ranges::find_if(
            profiles,
            [&](const Settings& a_settings)
            {
                return EqualsIgnoreCase(a_settings.id, a_profile);
            });
        if (found == profiles.end())
        {
            logger::error(
                "[Heliosphan] Cannot change detailed logging for unknown profile '{}'",
                a_profile);
            return false;
        }
        if (!LumaClient::UpdateProviderSettings(
                found->id,
                a_enabled ? 1 : 0,
                -1))
        {
            return false;
        }
        found->detailedLogging = a_enabled;
        ObjectOverrides::Patches::SetDetailedLogging(
            found->id,
            a_enabled);
        logger::info(
            "[Heliosphan] [{}] Detailed logging {} from Papyrus",
            found->id,
            a_enabled ? "enabled" : "disabled");
        return true;
    }

    bool SetProfileNotifications(
        const std::string_view a_profile,
        const bool a_enabled)
    {
        auto& profiles = GetState().profiles;
        const auto found = std::ranges::find_if(
            profiles,
            [&](const Settings& a_settings)
            {
                return EqualsIgnoreCase(a_settings.id, a_profile);
            });
        if (found == profiles.end())
        {
            logger::error(
                "[Heliosphan] Cannot change notifications for unknown profile '{}'",
                a_profile);
            return false;
        }
        if (!LumaClient::UpdateProviderSettings(
                found->id,
                -1,
                a_enabled ? 1 : 0))
        {
            return false;
        }
        found->notifications = a_enabled;
        logger::info(
            "[Heliosphan] [{}] Notifications {} from Papyrus",
            found->id,
            a_enabled ? "enabled" : "disabled");
        return true;
    }

    void LoadConfiguration()
    {
        auto& state = GetState();
        state.profiles.clear();
        state.roomMarkerCleaningActive.clear();
        state.profileLoadOrder.clear();
        state.profileUsesPluginLoadOrder.clear();
        state.profilePrioritiesResolved = false;
        state.cells.clear();
        AutoCSTonemapping::ClearProfiles();
        LightPlacer::ClearProfiles();
        ExternalEmittance::ClearProfiles();
        ObjectOverrides::Patches::ClearGroups();
        CellClassifier::Clear();

        std::error_code error;
        const std::filesystem::path root{ kConfigurationRoot };
        logger::info("[Heliosphan] Scanning configuration folder {}", root.string());
        if (!std::filesystem::is_directory(root, error))
        {
            logger::info(
                "[Heliosphan] Configuration folder is unavailable; Heliosphan will remain inactive (error={})",
                error ? error.message() : "none");
            return;
        }

        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator iterator(
                 root,
                 std::filesystem::directory_options::skip_permission_denied,
                 error),
            end;
            iterator != end && !error;
            iterator.increment(error))
        {
            if (iterator->is_regular_file(error) && iterator->path().extension() == ".json")
            {
                files.push_back(iterator->path());
            }
        }
        std::ranges::sort(files);
        logger::info("[Heliosphan] Discovered {} JSON configuration file(s)", files.size());
        for (const auto& file : files)
        {
            logger::info("[Heliosphan] Reading configuration {}", file.string());
            const auto parsed = rfl::json::read<Settings, rfl::DefaultIfMissing>(ReadText(file));
            if (!parsed)
            {
                logger::warn("[Heliosphan] Could not read configuration {}: {}", file.string(), parsed.error().what());
                continue;
            }
            auto settings = parsed.value();
            if (settings.id.empty()) settings.id = file.stem().string();
            if (std::ranges::any_of(
                    state.profiles,
                    [&](const Settings& a_existing)
                    {
                        return EqualsIgnoreCase(
                            a_existing.id,
                            settings.id);
                    }))
            {
                logger::warn(
                    "[Heliosphan] Skipping configuration {} because profile ID '{}' "
                    "is already loaded",
                    file.string(),
                    settings.id);
                continue;
            }
            bool detailedLogging = false;
            bool notifications = false;
            if (LumaClient::GetProviderSettings(
                    settings.id,
                    detailedLogging,
                    notifications))
            {
                settings.detailedLogging = detailedLogging;
                settings.notifications = notifications;
            }
            if (settings.weatherSync.enabled &&
                (settings.weatherSync.weatherPrefix.empty() ||
                    settings.weatherSync.regionPrefix.empty()))
            {
                logger::warn(
                    "[Heliosphan] Configuration {} has weatherSync enabled without "
                    "weatherPrefix and regionPrefix; weather synchronization is disabled",
                    file.string());
                settings.weatherSync.enabled = false;
            }
            const bool weatherSynchronization = SynchronizesWeather(settings);
            const auto validateEmittancePatching =
                [&](EmittancePatchingSettings& a_patching,
                    const std::string_view a_scope,
                    const bool a_requiresCellFilter)
                {
                    const bool hasWork = !a_patching.Forms.empty() ||
                                         !a_patching.lightPlacer.empty();
                    if (!hasWork)
                    {
                        a_patching = {};
                        return false;
                    }
                    if (a_patching.emmitance.empty())
                    {
                        logger::warn(
                            "[Heliosphan] Configuration {} has {} emittancePatching without its required 'emmitance' field; that block is disabled",
                            file.string(),
                            a_scope);
                        a_patching = {};
                        return false;
                    }
                    if (a_requiresCellFilter &&
                        (!settings.windowSync.enabled ||
                            !settings.windowSync.cellContains.has_value()))
                    {
                        logger::warn(
                            "[Heliosphan] Configuration {} has windowSync.emittancePatching without enabled windowSync.cellContains; that block is disabled",
                            file.string());
                        a_patching = {};
                        return false;
                    }
                    return true;
                };
            const bool globalEmittancePatching =
                validateEmittancePatching(
                    settings.emittancePatching,
                    "top-level",
                    false);
            const bool windowEmittancePatching =
                validateEmittancePatching(
                    settings.windowSync.emittancePatching,
                    "windowSync",
                    true);
            const bool lightPlacer =
                !settings.emittancePatching.lightPlacer.empty() ||
                !settings.windowSync.emittancePatching.lightPlacer.empty();
            const bool windowLayer =
                settings.windowSync.enabled &&
                HasWindowLayerSettings(settings.windowSync);
            std::size_t globalOverrideCount = 0;
            std::size_t windowOverrideCount = 0;
            std::size_t objectTransformCount = 0;
            std::size_t objectPlacementCount = 0;
            auto& compiledGlobalOverrides =
                settings.compiledGlobalOverrides.get();
            for (const auto& group : settings.objects)
            {
                globalOverrideCount += group.overrides.size();
                objectTransformCount += group.transforms.size();
                objectPlacementCount += group.placements.size();
                for (const auto& [source, target] : group.overrides)
                {
                    compiledGlobalOverrides.insert_or_assign(source, target);
                }
            }
            auto& compiledWindowOverrides =
                settings.compiledWindowOverrides.get();
            for (const auto& group : settings.windowSync.objects)
            {
                if (!group.transforms.empty() || !group.placements.empty() ||
                    !group.pluginInclusions.empty() ||
                    !group.pluginExclusions.empty())
                {
                    logger::warn(
                        "[Heliosphan] Configuration {} has unsupported fields in windowSync.objects; only overrides are accepted there",
                        file.string());
                }
                windowOverrideCount += group.overrides.size();
                for (const auto& [source, target] : group.overrides)
                {
                    compiledWindowOverrides.insert_or_assign(source, target);
                }
            }
            if (!compiledWindowOverrides.empty() &&
                (!settings.windowSync.enabled ||
                    !settings.windowSync.cellContains.has_value()))
            {
                logger::warn(
                    "[Heliosphan] Configuration {} has windowSync.objects overrides without enabled windowSync.cellContains; those overrides are disabled",
                    file.string());
                compiledWindowOverrides.clear();
                windowOverrideCount = 0;
            }
            const bool objectPatching =
                globalOverrideCount != 0 || windowOverrideCount != 0 ||
                objectTransformCount != 0 || objectPlacementCount != 0;
            if (!weatherSynchronization && !windowLayer &&
                !objectPatching &&
                !lightPlacer &&
                !globalEmittancePatching &&
                !windowEmittancePatching &&
                !settings.regionWeatherPatcher.enabled &&
                !settings.autoCSTonemapping.enabled)
            {
                logger::warn(
                    "[Heliosphan] Configuration {} does not declare any active behavior",
                    file.string());
                continue;
            }
            settings.overrideDurationGameHours = std::max(0.0f, settings.overrideDurationGameHours);
            logger::info(
                "[Heliosphan] Loaded profile '{}': provider='{}', weather sync={}, weather prefix='{}', "
                "window sync={}, region prefix='{}', fallback weather='{}', fallback region='{}', "
                "region weather patcher={}, target plugins={}, Auto CS Tonemapping={}, "
                "Auto CS target plugins={}, cellContains emittance={}, global/window emittance forms={}/{}, global/window Light Placer lights={}/{}, global/window object groups={}/{}, global/window overrides={}/{}, transforms={}, placements={}, "
                "override hours={:.3f}, detailed logging={}, notifications={}",
                settings.id,
                settings.cellPatchProvider,
                weatherSynchronization,
                settings.weatherSync.weatherPrefix,
                settings.windowSync.enabled,
                settings.weatherSync.regionPrefix,
                settings.weatherSync.fallbackWeather,
                settings.weatherSync.fallbackRegion,
                settings.regionWeatherPatcher.enabled,
                settings.regionWeatherPatcher.plugins.size(),
                settings.autoCSTonemapping.enabled,
                settings.autoCSTonemapping.plugins.size(),
                settings.windowSync.cellContains.has_value() &&
                    !settings.windowSync.cellContains->emittance.empty(),
                settings.emittancePatching.Forms.size(),
                settings.windowSync.emittancePatching.Forms.size(),
                settings.emittancePatching.lightPlacer.size(),
                settings.windowSync.emittancePatching.lightPlacer.size(),
                settings.objects.size(),
                settings.windowSync.objects.size(),
                globalOverrideCount,
                windowOverrideCount,
                objectTransformCount,
                objectPlacementCount,
                settings.overrideDurationGameHours,
                settings.detailedLogging,
                settings.notifications);
            AutoCSTonemapping::AddProfile(
                settings.id,
                settings.autoCSTonemapping,
                file);
            LightPlacer::Settings globalLightPlacerSettings{
                .lights = std::move(
                    settings.emittancePatching.lightPlacer),
                .externalEmittance =
                    settings.emittancePatching.emmitance,
            };
            LightPlacer::AddProfile(
                settings.id,
                std::move(globalLightPlacerSettings),
                false,
                settings.detailedLogging);
            LightPlacer::Settings windowLightPlacerSettings{
                .lights = std::move(
                    settings.windowSync.emittancePatching.lightPlacer),
                .externalEmittance =
                    settings.windowSync.emittancePatching.emmitance,
            };
            LightPlacer::AddProfile(
                settings.id,
                std::move(windowLightPlacerSettings),
                true,
                settings.detailedLogging);
            const auto globalFormPatchingTarget =
                settings.emittancePatching.Forms.empty() ?
                    std::string{} :
                    settings.emittancePatching.emmitance;
            ExternalEmittance::Settings globalEmittanceSettings{
                .forms = std::move(settings.emittancePatching.Forms),
                .target = globalFormPatchingTarget,
            };
            ExternalEmittance::AddProfile(
                settings.id,
                std::move(globalEmittanceSettings),
                false,
                settings.detailedLogging);
            const auto windowFormPatchingTarget =
                settings.windowSync.emittancePatching.Forms.empty() ?
                    std::string{} :
                    settings.windowSync.emittancePatching.emmitance;
            ExternalEmittance::Settings windowEmittanceSettings{
                .forms = std::move(
                    settings.windowSync.emittancePatching.Forms),
                .target = windowFormPatchingTarget,
                .cellContainsTarget =
                    settings.windowSync.cellContains ?
                        settings.windowSync.cellContains->emittance :
                        std::string{},
            };
            ExternalEmittance::AddProfile(
                settings.id,
                std::move(windowEmittanceSettings),
                true,
                settings.detailedLogging);
            for (std::size_t index = 0;
                 index < settings.objects.size();
                 ++index)
            {
                ObjectOverrides::Patches::AddGroup(
                    settings.id,
                    std::format(
                        "{}:objects[{}]",
                        settings.id,
                        index + 1),
                    std::move(settings.objects[index]),
                    settings.detailedLogging);
            }
            if (settings.windowSync.enabled &&
                settings.windowSync.cellContains)
            {
                CellClassifier::AddRule(
                    settings.id,
                    *settings.windowSync.cellContains,
                    settings.detailedLogging);
            }
            state.profiles.push_back(std::move(settings));
        }
        logger::info("[Heliosphan] Configuration loading finished with {} active profile(s)", state.profiles.size());
    }

    void RecordCellPatch(
        RE::TESObjectCELL* a_cell,
        std::string_view a_provider,
        const bool a_hasSkylight)
    {
        if (!a_cell || !a_hasSkylight)
        {
            return;
        }
        auto& state = GetState();
        for (std::size_t index = 0; index < state.profiles.size(); ++index)
        {
            if (SynchronizesWeather(state.profiles[index]) &&
                EqualsIgnoreCase(
                    state.profiles[index].cellPatchProvider,
                    a_provider))
            {
                state.cells[a_cell->formID] = index;
                if (state.profiles[index].detailedLogging)
                {
                    logger::info(
                        "[Heliosphan] [{}] Registered synchronized interior cell {:08X} from provider '{}'",
                        state.profiles[index].id,
                        a_cell->formID,
                        a_provider);
                }
                return;
            }
        }
    }

    bool RecordWindowSyncCell(RE::TESObjectCELL* a_cell, const std::string_view a_profile)
    {
        if (!a_cell || a_profile.empty())
        {
            return false;
        }
        auto& state = GetState();
        for (std::size_t index = 0; index < state.profiles.size(); ++index)
        {
            const auto& settings = state.profiles[index];
            if (SynchronizesWeather(settings) &&
                EqualsIgnoreCase(settings.id, a_profile))
            {
                state.cells[a_cell->formID] = index;
                LogProfile(
                    settings,
                    std::format(
                        "Registered Window Sync interior cell {:08X}",
                        a_cell->formID));
                return true;
            }
        }
        return false;
    }

    void PrepareWindowSyncProfilePriorities()
    {
        ResolveWindowSyncProfilePriorities(GetState());
    }

    void SortWindowSyncProfileIDs(std::vector<std::string>& a_profiles)
    {
        PrepareWindowSyncProfilePriorities();
        std::ranges::stable_sort(
            a_profiles,
            [](const std::string& a_left, const std::string& a_right)
            {
                const auto left = GetWindowSyncProfile(a_left);
                const auto right = GetWindowSyncProfile(a_right);
                if (left && right)
                {
                    if (left->usesPluginLoadOrder !=
                        right->usesPluginLoadOrder)
                    {
                        return !left->usesPluginLoadOrder;
                    }
                    if (left->loadOrder != right->loadOrder)
                    {
                        return left->loadOrder < right->loadOrder;
                    }
                    return left->id < right->id;
                }
                if (left.has_value() != right.has_value())
                {
                    return left.has_value();
                }
                const auto order = _stricmp(a_left.c_str(), a_right.c_str());
                return order != 0 ? order < 0 : a_left < a_right;
            });
    }

    std::optional<WindowSyncProfile> GetWindowSyncProfile(
        const std::string_view a_profile)
    {
        auto& state = GetState();
        ResolveWindowSyncProfilePriorities(state);
        for (std::size_t index = 0; index < state.profiles.size(); ++index)
        {
            const auto& settings = state.profiles[index];
            const bool synchronizesWeather = SynchronizesWeather(settings);
            if (!DefinesWindowSyncProfile(settings) ||
                !EqualsIgnoreCase(settings.id, a_profile))
            {
                continue;
            }
            return WindowSyncProfile{
                .id = settings.id,
                .regionPrefix = settings.weatherSync.regionPrefix,
                .fallbackRegion = settings.weatherSync.fallbackRegion,
                .showSky = settings.windowSync.showSky,
                .useSkyLighting = settings.windowSync.useSkyLighting,
                .sunlightShadows = settings.windowSync.sunlightShadows,
                .loadOrder =
                    index < state.profileLoadOrder.size() ?
                        state.profileLoadOrder[index] :
                        static_cast<std::uint32_t>(index),
                .usesPluginLoadOrder =
                    index < state.profileUsesPluginLoadOrder.size() &&
                    state.profileUsesPluginLoadOrder[index],
                .synchronizesWeather = synchronizesWeather,
                .cleanRoomMarkers =
                    index < state.roomMarkerCleaningActive.size() &&
                    state.roomMarkerCleaningActive[index],
                .roomMarkerExcludedPlugins =
                    settings.windowSync.cleanRoomMarkers ?
                        settings.windowSync.cleanRoomMarkers->excludedPlugins :
                        std::vector<std::string>{},
                .debugLogging = settings.detailedLogging,
            };
        }
        return std::nullopt;
    }

    std::vector<WindowSyncProfile> GetWindowSyncProfiles()
    {
        std::vector<WindowSyncProfile> profiles;
        for (const auto& settings : GetState().profiles)
        {
            if (auto profile = GetWindowSyncProfile(settings.id))
            {
                profiles.push_back(std::move(*profile));
            }
        }
        return profiles;
    }

    std::vector<ObjectOverrideProfileView>
    GetObjectOverrideProfiles()
    {
        std::vector<ObjectOverrideProfileView> profiles;
        const auto& settingsProfiles = GetState().profiles;
        profiles.reserve(settingsProfiles.size() * 2);
        for (const auto& settings : settingsProfiles)
        {
            const auto& globalOverrides =
                settings.compiledGlobalOverrides.get();
            if (!globalOverrides.empty())
            {
                profiles.push_back(ObjectOverrideProfileView{
                    .id = settings.id,
                    .overrides = std::addressof(globalOverrides),
                    .global = true,
                    .debugLogging = settings.detailedLogging,
                });
            }
            const auto& windowOverrides =
                settings.compiledWindowOverrides.get();
            if (!windowOverrides.empty() &&
                DefinesWindowSyncProfile(settings))
            {
                profiles.push_back(ObjectOverrideProfileView{
                    .id = settings.id,
                    .overrides = std::addressof(windowOverrides),
                    .debugLogging = settings.detailedLogging,
                });
            }
        }
        return profiles;
    }

    std::size_t GetObjectOverrideRuleCount()
    {
        std::size_t count = 0;
        for (const auto& settings : GetState().profiles)
        {
            count += settings.compiledGlobalOverrides.get().size();
            count += settings.compiledWindowOverrides.get().size();
        }
        return count;
    }

    bool IsSynchronizedRegion(const std::string_view a_editorID)
    {
        if (a_editorID.empty())
        {
            return false;
        }
        return std::ranges::any_of(GetState().profiles, [&](const Settings& a_settings)
            { return SynchronizesWeather(a_settings) &&
                     StartsWithIgnoreCase(
                         a_editorID,
                         a_settings.weatherSync.regionPrefix); });
    }

    std::string BaseRegionEditorID(const std::string_view a_editorID)
    {
        for (const auto& settings : GetState().profiles)
        {
            const auto& prefix = settings.weatherSync.regionPrefix;
            if (SynchronizesWeather(settings) &&
                StartsWithIgnoreCase(a_editorID, prefix))
            {
                return std::string(a_editorID.substr(prefix.size()));
            }
        }
        return std::string(a_editorID);
    }

    API::MMSF::Interface* GetMMSFAPI()
    {
        return GetState().mmsf;
    }

    RE::TESWeather* CaptureSourceWeather()
    {
        auto* sky = RE::Sky::GetSingleton();
        if (!sky)
        {
            return nullptr;
        }
        if (sky->lastWeather && sky->currentWeather &&
            sky->currentWeatherPct < kWeatherTransitionSelectionThreshold)
        {
            return sky->lastWeather;
        }
        return sky->currentWeather ? sky->currentWeather : sky->lastWeather;
    }

    void OnCellChanged(
        const RE::TESObjectCELL* a_cell,
        RE::TESWeather* a_sourceWeather,
        const bool a_usedDefaultRegion)
    {
        auto& state = GetState();
        const bool wasGameLoadPending = state.gameLoadPending;
        state.gameLoadPending = false;
        if (wasGameLoadPending)
        {
            ObjectOverrides::Patches::CompleteGameLoad(
                const_cast<RE::TESObjectCELL*>(a_cell));
            ExternalEmittance::ReplayCell(
                const_cast<RE::TESObjectCELL*>(a_cell));
        }
        if (state.profiles.empty() || !state.mmsf)
        {
            LogDetailed(
                "Ignored cell change because active profiles={} and MMSF available={}",
                state.profiles.size(),
                state.mmsf != nullptr);
            return;
        }

        std::optional<std::size_t> destination;
        if (a_cell && a_cell->IsInteriorCell())
        {
            if (const auto found = state.cells.find(a_cell->formID); found != state.cells.end())
            {
                destination = found->second;
            }
        }

        bool usedDefaultWeather = false;
        if (!a_sourceWeather)
        {
            const auto fallbackProfile =
                destination ? destination : state.activeProfile;
            if (fallbackProfile &&
                *fallbackProfile < state.profiles.size())
            {
                const auto& settings = state.profiles[*fallbackProfile];
                if (!settings.weatherSync.fallbackWeather.empty())
                {
                    a_sourceWeather =
                        LookupWeather(settings.weatherSync.fallbackWeather);
                    if (a_sourceWeather)
                    {
                        usedDefaultWeather = true;
                        LogProfile(
                            settings,
                            std::format(
                                "Using configured fallback weather '{}' because no runtime weather is available",
                                settings.weatherSync.fallbackWeather));
                    }
                    else
                    {
                        logger::warn(
                            "[Weather Sync] [{}] Configured fallback weather '{}' could not be resolved",
                            settings.id,
                            settings.weatherSync.fallbackWeather);
                    }
                }
            }
        }

        const auto sourceProfile = state.activeProfile ? state.activeProfile : ProfileForWeather(a_sourceWeather);
        const bool hadPendingTransition = state.pendingSource != nullptr;
        auto* sky = RE::Sky::GetSingleton();
        LogDetailed(
            "Cell change received: destination cell={}, interior={}, registered destination profile={}, "
            "captured source={}, detected source profile={}, active profile={}, pending profile={}, "
            "pending source={}, owned override={}, sky current={}, sky last={}, sky override={}, transition={:.3f}",
            a_cell ? std::format("{:08X}", a_cell->formID) : "<none>",
            a_cell && a_cell->IsInteriorCell(),
            DescribeProfile(destination),
            DescribeWeather(a_sourceWeather),
            DescribeProfile(sourceProfile),
            DescribeProfile(state.activeProfile),
            DescribeProfile(state.pendingProfile),
            DescribeWeather(state.pendingSource),
            DescribeWeather(state.ownedOverride),
            DescribeWeather(sky ? sky->currentWeather : nullptr),
            DescribeWeather(sky ? sky->lastWeather : nullptr),
            DescribeWeather(sky ? sky->overrideWeather : nullptr),
            sky ? sky->currentWeatherPct : 0.0f);
        if (!destination && !sourceProfile && !hadPendingTransition)
        {
            LogDetailed("No synchronized source or destination was involved; no transition was scheduled");
            return;
        }
        if (!a_sourceWeather)
        {
            ++state.generation;
            if (!destination)
            {
                const auto releaseProfile = state.activeProfile;
                if (ReleaseOwnedOverride() && releaseProfile)
                {
                    LogProfile(
                        state.profiles[*releaseProfile],
                        "Weather override released because the synchronized interior was exited");
                    ShowNotification(state.profiles[*releaseProfile], "Weather Override Released");
                }
            }
            state.activeProfile.reset();
            ClearPendingTransition(state);
            logger::warn("[Weather Sync] Could not capture an active weather for the cell transition");
            return;
        }
        const auto weatherProfile = ProfileForWeather(a_sourceWeather);
        if (destination && weatherProfile == destination)
        {
            state.activeProfile = destination;
            ClearPendingTransition(state);
            LogDetailed(
                "Skipped same-profile interior transition for profile {} because weather {} is already synchronized",
                DescribeProfile(destination),
                DescribeWeather(a_sourceWeather));
            return;
        }
        if (!destination && !state.ownedOverride && sourceProfile)
        {
            if (sky && sky->overrideWeather == a_sourceWeather)
            {
                LogDetailed(
                    "Releasing an untracked synchronized override before scheduling the exit transition: {}",
                    DescribeWeather(a_sourceWeather));
                sky->ReleaseWeatherOverride();
            }
        }
        ScheduleTransition(
            destination,
            const_cast<RE::TESObjectCELL*>(a_cell),
            a_sourceWeather,
            a_usedDefaultRegion,
            usedDefaultWeather);
    }

    void OnDataLoaded()
    {
        auto& state = GetState();
        state.roomMarkerCleaningActive.assign(state.profiles.size(), false);
        PrepareWindowSyncProfilePriorities();
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        for (std::size_t index = 0; index < state.profiles.size(); ++index)
        {
            const auto& settings = state.profiles[index];
            if (!settings.windowSync.cleanRoomMarkers)
            {
                continue;
            }
            const auto& plugins =
                settings.windowSync.cleanRoomMarkers->plugins;
            const auto& excludedPlugins =
                settings.windowSync.cleanRoomMarkers->excludedPlugins;
            state.roomMarkerCleaningActive[index] = plugins.empty();
            for (const auto& plugin : plugins)
            {
                const bool loaded =
                    dataHandler && !plugin.empty() &&
                    dataHandler->LookupLoadedModByName(plugin) != nullptr;
                state.roomMarkerCleaningActive[index] =
                    state.roomMarkerCleaningActive[index] || loaded;
                LogProfile(
                    settings,
                    std::format(
                        "RoomMarker cleaning plugin gate '{}' is {}",
                        plugin,
                        loaded ? "loaded" : "not loaded"));
            }
            if (plugins.empty())
            {
                logger::info(
                    "[Room Marker] [{}] Cleaning enabled: no plugin gate "
                    "configured, {} excluded marker source plugin(s)",
                    settings.id,
                    excludedPlugins.size());
            }
            else
            {
                logger::info(
                    "[Room Marker] [{}] Cleaning {}: {} configured plugin "
                    "gate(s), {} excluded marker source plugin(s)",
                    settings.id,
                    state.roomMarkerCleaningActive[index] ?
                        "enabled" :
                        "skipped",
                    plugins.size(),
                    excludedPlugins.size());
            }
            for (const auto& plugin : excludedPlugins)
            {
                LogProfile(
                    settings,
                    std::format(
                        "RoomMarker source plugin '{}' is excluded from cleaning",
                        plugin));
            }
        }
        state.mmsf = MPL::API::MMSF::RequestMMSFAPI();
        logger::info(
            "[Heliosphan] Data-loaded initialization: active profiles={}, MMSF API={}",
            state.profiles.size(),
            state.mmsf ? "available" : "unavailable");
        const bool requiresMMSF = std::ranges::any_of(
            state.profiles,
            [](const Settings& a_settings)
            {
                return SynchronizesWeather(a_settings) ||
                       a_settings.regionWeatherPatcher.enabled;
            });
        if (requiresMMSF && !state.mmsf)
        {
            logger::error(
                "[Weather Sync] Weather and region synchronization disabled because the "
                "MMSF API is unavailable; Object Overrides and non-region Window Sync layers remain active");
        }
        if (state.mmsf)
        {
            for (const auto& settings : state.profiles)
            {
                RegionWeatherPatcher::Apply(
                    settings.regionWeatherPatcher,
                    settings.id,
                    settings.weatherSync.weatherPrefix,
                    settings.weatherSync.regionPrefix,
                    state.mmsf,
                    settings.detailedLogging);
            }
        }
        ObjectOverrides::Patches::Initialize();
    }

    void OnGameLoaded()
    {
        auto& state = GetState();
        state.gameLoadPending = true;
        state.readinessPollAttempts = 0;
        const auto generation = state.generation;
        LogDetailed(
            "Game-load initialization started bounded readiness polling; registered synchronized cells={}",
            state.cells.size());
        ScheduleReadinessPoll(generation);
        ScheduleReadinessWatchdog(generation);
    }

    void Reset()
    {
        auto& state = GetState();
        LogDetailed(
            "Resetting runtime state: generation={}, active profile={}, pending profile={}, pending source={}, "
            "owned override={}, registered cells={}",
            state.generation,
            DescribeProfile(state.activeProfile),
            DescribeProfile(state.pendingProfile),
            DescribeWeather(state.pendingSource),
            DescribeWeather(state.ownedOverride),
            state.cells.size());
        ++state.generation;
        state.activeProfile.reset();
        state.ownedOverride = nullptr;
        state.releaseAtGameDay.reset();
        state.gameLoadPending = false;
        state.readinessPollAttempts = 0;
        ClearPendingTransition(state);
    }
}  // namespace MPL::Heliosphan
