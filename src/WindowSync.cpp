#include <WindowObjectOverrides.h>
#include <LumaClient.h>
#include <RegionRuntime.h>
#include <RoomMarkerPatcher.h>
#include <WeatherSync.h>
#include <WindowSync.h>
#include <XEMI_API.h>
#include <XEMI_WindowObjectOverrideAPI.h>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace MPL::WindowSync
{
    namespace
    {
        constexpr std::uint32_t kSerializationVersion = 1;

        struct PendingTransition
        {
            RE::TESObjectCELL* destination = nullptr;
            RE::TESWeather* sourceWeather = nullptr;
            RE::TESRegion* sourceRegion = nullptr;
            std::optional<WeatherSync::WindowSyncProfile> profile;
            bool usedDefaultRegion = false;
            bool resolved = false;
            bool finishCalled = false;
        };

        struct CellInventoryEntry
        {
            RE::FormID cell = 0;
            std::vector<std::string> profiles;
        };

        struct State
        {
            HMODULE module = nullptr;
            const XEMIAPI::Interface* api = nullptr;
            bool hasWindowProfiles = false;
            bool collectingCellInventory = false;
            RE::FormID lastKnownRegion = 0;
            std::optional<PendingTransition> pending;
            std::vector<CellInventoryEntry> cellInventory;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        MPL::API::ServiceMap* MMSF()
        {
            return WeatherSync::GetMMSFAPI();
        }

        std::string RegionEditorID(RE::TESRegion* a_region)
        {
            return RegionRuntime::EditorID(MMSF(), a_region);
        }

        RE::TESRegion* LookupRegion(const std::string_view a_editorID)
        {
            auto* mmsf = MMSF();
            if (!mmsf || a_editorID.empty())
            {
                return nullptr;
            }
            if (auto* cached = mmsf->LookupCachedForm(std::string(a_editorID)))
            {
                return cached->As<RE::TESRegion>();
            }
            const auto formID = mmsf->LookupFormIDForEDID(std::string(a_editorID));
            return formID ? RE::TESForm::LookupByID<RE::TESRegion>(formID) : nullptr;
        }

        RE::TESRegion* LastKnownRegion()
        {
            const auto formID = GetState().lastKnownRegion;
            return formID ? RE::TESForm::LookupByID<RE::TESRegion>(formID) : nullptr;
        }

        void LogDetailed(
            const WeatherSync::WindowSyncProfile& a_profile,
            const std::string_view a_message)
        {
            if (a_profile.debugLogging)
            {
                logger::info("[Window Sync] [{}] {}", a_profile.id, a_message);
            }
        }

        std::vector<WeatherSync::WindowSyncProfile> ResolveProfiles(
            const XEMIAPI::CellResult& a_result,
            const RE::FormID a_cell)
        {
            std::vector<WeatherSync::WindowSyncProfile> profiles;
            if (a_result.profileCount != 0 &&
                !a_result.profiles)
            {
                logger::error(
                    "[Window Sync] XEMIUtil returned an invalid cell classification result for {:08X}",
                    a_cell);
                return profiles;
            }
            if (XEMIAPI::HasFlag(
                    a_result,
                    XEMIAPI::CellResultFlag::
                        kProfilesTruncated))
            {
                logger::warn(
                    "[Window Sync] XEMIUtil truncated the cell classification profile list for {:08X}",
                    a_cell);
            }
            const auto count =
                static_cast<std::size_t>(
                    a_result.profileCount);
            profiles.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                if (!a_result.profiles[index])
                {
                    continue;
                }
                const std::string_view id{
                    a_result.profiles[index] };
                if (auto profile =
                        WeatherSync::GetWindowSyncProfile(id))
                {
                    profiles.push_back(std::move(*profile));
                }
                else
                {
                    logger::warn(
                        "[Window Sync] XEMIUtil classified cell {:08X} as profile '{}', "
                        "but no enabled Weather Sync layer defines that ID",
                        a_cell,
                        id);
                }
            }
            std::ranges::sort(
                profiles,
                [](const auto& a_left, const auto& a_right)
                {
                    if (a_left.usesPluginLoadOrder !=
                        a_right.usesPluginLoadOrder)
                    {
                        return !a_left.usesPluginLoadOrder;
                    }
                    if (a_left.loadOrder != a_right.loadOrder)
                    {
                        return a_left.loadOrder < a_right.loadOrder;
                    }
                    return a_left.id < a_right.id;
                });
            return profiles;
        }

        RE::TESRegion* ResolveTargetRegion(
            const WeatherSync::WindowSyncProfile& a_profile,
            RE::TESRegion* a_sourceRegion)
        {
            auto sourceEditorID = RegionEditorID(a_sourceRegion);
            if (sourceEditorID.empty())
            {
                return nullptr;
            }
            const auto baseEditorID = WeatherSync::BaseRegionEditorID(sourceEditorID);
            return LookupRegion(a_profile.regionPrefix + baseEditorID);
        }

        RE::TESRegion* ResolveFallbackTargetRegion(
            const WeatherSync::WindowSyncProfile& a_profile)
        {
            if (a_profile.fallbackRegion.empty())
            {
                return nullptr;
            }
            auto* fallbackRegion = LookupRegion(a_profile.fallbackRegion);
            if (!fallbackRegion)
            {
                return nullptr;
            }
            if (WeatherSync::IsSynchronizedRegion(RegionEditorID(fallbackRegion)))
            {
                return fallbackRegion;
            }
            return ResolveTargetRegion(a_profile, fallbackRegion);
        }

        void ShowRegionSyncFailure(
            const WeatherSync::WindowSyncProfile& a_profile)
        {
            const auto message = std::format(
                "{}: Region sync failed. Please report.",
                a_profile.id);
            RE::DebugMessageBox(message.c_str());
        }

        void ApplyCellFlags(
            RE::TESObjectCELL* a_cell,
            const WeatherSync::WindowSyncProfile& a_profile)
        {
            if (!a_cell)
            {
                return;
            }
            const bool showSkyBefore =
                a_cell->cellFlags.all(RE::TESObjectCELL::Flag::kShowSky);
            const bool skyLightingBefore =
                a_cell->cellFlags.all(RE::TESObjectCELL::Flag::kUseSkyLighting);
            const auto sunlightShadowsFlag =
                static_cast<RE::TESObjectCELL::Flag>(1 << 15);
            const bool sunlightShadowsBefore =
                a_cell->cellFlags.all(sunlightShadowsFlag);
            if (a_profile.showSky)
            {
                a_cell->cellFlags.set(
                    *a_profile.showSky,
                    RE::TESObjectCELL::Flag::kShowSky);
            }
            if (a_profile.useSkyLighting)
            {
                a_cell->cellFlags.set(
                    *a_profile.useSkyLighting,
                    RE::TESObjectCELL::Flag::kUseSkyLighting);
            }
            if (a_profile.sunlightShadows)
            {
                a_cell->cellFlags.set(
                    *a_profile.sunlightShadows,
                    sunlightShadowsFlag);
            }
            if ((a_profile.showSky &&
                    showSkyBefore != *a_profile.showSky) ||
                (a_profile.useSkyLighting &&
                    skyLightingBefore != *a_profile.useSkyLighting) ||
                (a_profile.sunlightShadows &&
                    sunlightShadowsBefore != *a_profile.sunlightShadows))
            {
                LogDetailed(
                    a_profile,
                    std::format(
                        "Pre-applied cell sky flags to indexed interior {:08X}: "
                        "ShowSky {} -> {}, UseSkyLighting {} -> {}, "
                        "SunlightShadows {} -> {}",
                        a_cell->GetFormID(),
                        showSkyBefore,
                        a_profile.showSky.value_or(showSkyBefore),
                        skyLightingBefore,
                        a_profile.useSkyLighting.value_or(
                            skyLightingBefore),
                        sunlightShadowsBefore,
                        a_profile.sunlightShadows.value_or(
                            sunlightShadowsBefore)));
            }
        }

        void ApplyIndexedCellSettings(
            RE::TESObjectCELL* a_cell,
            const WeatherSync::WindowSyncProfile& a_profile)
        {
            ApplyCellFlags(a_cell, a_profile);
        }

        void ApplyRoomMarkerCleaning(
            RE::TESObjectCELL* a_cell,
            const std::vector<WeatherSync::WindowSyncProfile>& a_profiles)
        {
            std::vector<std::string> excludedPlugins;
            bool enabled = false;
            for (const auto& profile : a_profiles)
            {
                if (!profile.cleanRoomMarkers)
                {
                    continue;
                }
                enabled = true;
                excludedPlugins.insert(
                    excludedPlugins.end(),
                    profile.roomMarkerExcludedPlugins.begin(),
                    profile.roomMarkerExcludedPlugins.end());
            }
            RoomMarkerPatcher::ConfigureCell(
                a_cell,
                enabled,
                excludedPlugins);
        }

        void ApplyCellSettings(
            RE::TESObjectCELL* a_cell,
            RE::TESRegion* a_region)
        {
            if (!a_cell || !a_region)
            {
                return;
            }

            RE::ExtraCellSkyRegion* extra = nullptr;
            if (a_cell->extraList.HasType<RE::ExtraCellSkyRegion>())
            {
                extra = a_cell->extraList.GetByType<RE::ExtraCellSkyRegion>();
            }
            else
            {
                extra = RE::BSExtraData::Create<RE::ExtraCellSkyRegion>();
                a_cell->extraList.Add(extra);
            }
            extra->skyRegion = a_region;
        }

        bool ResolveMatchedDestination(
            PendingTransition& a_transition,
            const XEMIAPI::CellResult& a_result)
        {
            auto profiles = ResolveProfiles(
                a_result,
                a_transition.destination ?
                    a_transition.destination->GetFormID() :
                    0);
            if (profiles.empty())
            {
                return false;
            }
            for (const auto& profile : profiles)
            {
                ApplyIndexedCellSettings(
                    a_transition.destination,
                    profile);
            }
            ApplyRoomMarkerCleaning(
                a_transition.destination,
                profiles);

            const auto syncProfile = std::ranges::find_if(
                profiles.rbegin(),
                profiles.rend(),
                [](const auto& a_profile)
                {
                    return a_profile.synchronizesWeather;
                });
            if (syncProfile == profiles.rend())
            {
                return true;
            }

            auto* sourceRegion =
                a_transition.sourceRegion ? a_transition.sourceRegion : LastKnownRegion();
            if (!sourceRegion && !syncProfile->fallbackRegion.empty())
            {
                sourceRegion = LookupRegion(syncProfile->fallbackRegion);
                if (sourceRegion)
                {
                    a_transition.usedDefaultRegion = true;
                    LogDetailed(
                        *syncProfile,
                        std::format(
                            "Using configured fallback region '{}' because no runtime region is available",
                            syncProfile->fallbackRegion));
                }
                else
                {
                    logger::warn(
                        "[Window Sync] [{}] Configured fallback region '{}' could not be resolved",
                        syncProfile->id,
                        syncProfile->fallbackRegion);
                }
            }
            if (!sourceRegion)
            {
                logger::warn(
                    "[Window Sync] [{}] Could not apply cell {:08X} because no source or last-known region is available",
                    syncProfile->id,
                    a_transition.destination ? a_transition.destination->GetFormID() : 0);
                ShowRegionSyncFailure(*syncProfile);
                return false;
            }
            auto* targetRegion = ResolveTargetRegion(*syncProfile, sourceRegion);
            if (!targetRegion)
            {
                const auto targetEditorID =
                    syncProfile->regionPrefix +
                    WeatherSync::BaseRegionEditorID(RegionEditorID(sourceRegion));
                logger::warn(
                    "[Window Sync] [{}] Could not find the synchronized region '{}' for cell {:08X}",
                    syncProfile->id,
                    targetEditorID,
                    a_transition.destination ? a_transition.destination->GetFormID() : 0);
                targetRegion = ResolveFallbackTargetRegion(*syncProfile);
                if (targetRegion)
                {
                    a_transition.usedDefaultRegion = true;
                    LogDetailed(
                        *syncProfile,
                        std::format(
                            "Using configured fallback region '{}' because synchronized region '{}' is unavailable",
                            syncProfile->fallbackRegion,
                            targetEditorID));
                }
                else
                {
                    if (!syncProfile->fallbackRegion.empty())
                    {
                        logger::warn(
                            "[Window Sync] [{}] Configured fallback region '{}' could not be resolved to a synchronized region",
                            syncProfile->id,
                            syncProfile->fallbackRegion);
                    }
                    ShowRegionSyncFailure(*syncProfile);
                    return false;
                }
            }

            ApplyCellSettings(a_transition.destination, targetRegion);
            if (!WeatherSync::RecordWindowSyncCell(
                    a_transition.destination,
                    syncProfile->id))
            {
                ShowRegionSyncFailure(*syncProfile);
                return false;
            }
            a_transition.profile = *syncProfile;
            LogDetailed(
                *syncProfile,
                std::format(
                    "Applied region '{}' and {} Window Sync layer(s) to interior {:08X}",
                    RegionEditorID(targetRegion),
                    profiles.size(),
                    a_transition.destination->GetFormID()));
            return true;
        }

        void ResolveTransition(
            PendingTransition& a_transition,
            const XEMIAPI::CellResult& a_result)
        {
            a_transition.profile.reset();
            if (a_result.status == XEMIAPI::CellStatus::kMatched)
            {
                ResolveMatchedDestination(a_transition, a_result);
            }
            a_transition.resolved = a_result.status != XEMIAPI::CellStatus::kUnknown;
        }

        void DispatchResolvedTransition()
        {
            auto& state = GetState();
            if (!state.pending || !state.pending->resolved || !state.pending->finishCalled)
            {
                return;
            }

            auto transition = std::move(*state.pending);
            state.pending.reset();
            if (transition.profile)
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player && player->GetParentCell() == transition.destination)
                {
                    if (const auto* extra =
                            transition.destination->extraList.GetByType<RE::ExtraCellSkyRegion>();
                        extra && extra->skyRegion)
                    {
                        if (auto* sky = RE::Sky::GetSingleton())
                        {
                            auto* previousRegion = sky->region;
                            LogDetailed(
                                *transition.profile,
                                std::format(
                                    "Sky::region assignment diagnostic before Weather Sync scheduling: "
                                    "engine region='{}', destination region='{}', pointer changed={}, "
                                    "cell attached={}, loaded data={}, active override={:08X}",
                                    previousRegion ?
                                        RegionEditorID(previousRegion) :
                                        "<none>",
                                    RegionEditorID(extra->skyRegion),
                                    previousRegion != extra->skyRegion,
                                    transition.destination->IsAttached(),
                                    transition.destination->GetRuntimeData().loadedData != nullptr,
                                    sky->overrideWeather ?
                                        sky->overrideWeather->GetFormID() :
                                        0));
                            sky->region = extra->skyRegion;
                        }
                    }
                }
            }
            WeatherSync::OnCellChanged(
                transition.destination,
                transition.sourceWeather,
                transition.usedDefaultRegion);
        }

        void OnCellClassified(
            RE::TESObjectCELL* a_cell,
            const XEMIAPI::CellResult* a_result)
        {
            auto& state = GetState();
            if (!a_cell || !a_result)
            {
                return;
            }
            if (state.collectingCellInventory &&
                a_result->status == XEMIAPI::CellStatus::kMatched)
            {
                CellInventoryEntry entry{
                    .cell = a_cell->GetFormID(),
                };
                entry.profiles.reserve(a_result->profileCount);
                for (std::size_t index = 0;
                     index < a_result->profileCount;
                     ++index)
                {
                    if (a_result->profiles && a_result->profiles[index])
                    {
                        entry.profiles.emplace_back(
                            a_result->profiles[index]);
                    }
                }
                const auto existing = std::ranges::find(
                    state.cellInventory,
                    entry.cell,
                    &CellInventoryEntry::cell);
                if (existing == state.cellInventory.end())
                {
                    state.cellInventory.push_back(std::move(entry));
                }
                else
                {
                    existing->profiles = std::move(entry.profiles);
                }
            }
            std::vector<WeatherSync::WindowSyncProfile> profiles;
            if (a_result->status == XEMIAPI::CellStatus::kMatched)
            {
                profiles =
                    ResolveProfiles(*a_result, a_cell->GetFormID());
                for (const auto& profile : profiles)
                {
                    ApplyIndexedCellSettings(a_cell, profile);
                }
                WindowObjectOverrides::ApplyToCell(a_cell, profiles);
                ApplyRoomMarkerCleaning(a_cell, profiles);
            }
            else if (a_result->status == XEMIAPI::CellStatus::kNoMatch)
            {
                RoomMarkerPatcher::ConfigureCell(a_cell, false, {});
            }
            if (!state.pending || state.pending->destination != a_cell)
            {
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (state.pending->finishCalled &&
                (!player || player->GetParentCell() != a_cell))
            {
                state.pending.reset();
                return;
            }
            ResolveTransition(*state.pending, *a_result);
            DispatchResolvedTransition();
        }

        const XEMIAPI::ClientCallbacks callbacks{
            .id = "WeatherSync",
            .OnCellClassified = OnCellClassified,
        };

        void LogCellInventory(std::vector<CellInventoryEntry>& a_entries)
        {
            if (!WeatherSync::IsDetailedLoggingEnabled())
            {
                return;
            }
            std::ranges::sort(
                a_entries,
                {},
                &CellInventoryEntry::cell);
            logger::info(
                "[Window Sync] Window Sync cell inventory begin: {} matched cell(s)",
                a_entries.size());
            for (const auto& entry : a_entries)
            {
                std::string profiles;
                for (const auto& profile : entry.profiles)
                {
                    if (!profiles.empty())
                    {
                        profiles.append(", ");
                    }
                    profiles.append(profile);
                }
                logger::info(
                    "[Window Sync] Window Sync cell {:08X}: profiles=[{}]",
                    entry.cell,
                    profiles);
            }
            logger::info("[Window Sync] Window Sync cell inventory end");
        }

        void FinishCellInventory()
        {
            auto& state = GetState();
            state.collectingCellInventory = false;
            LogCellInventory(state.cellInventory);
        }

        bool PrepareObjectOverrideProjection()
        {
            WindowObjectOverrides::Initialize();
            return WindowObjectOverrides::HasOverrides();
        }

        std::uint32_t ProjectObjectOverrideBase(
            const char* a_profile,
            const std::size_t a_profileLength,
            const std::uint32_t a_base)
        {
            return a_profile && a_profileLength != 0 ?
                       WindowObjectOverrides::ProjectBase(
                           std::string_view(a_profile, a_profileLength),
                           a_base) :
                       a_base;
        }

        const XEMIWindowObjectOverrideAPI::Projector objectOverrideProjector{
            .id = "WeatherSync",
            .PrepareOverrides = PrepareObjectOverrideProjection,
            .ProjectOverrideBase = ProjectObjectOverrideBase,
        };
    }  // namespace

    void RegisterObjectOverrideProjection()
    {
        const auto ruleCount =
            WeatherSync::GetWindowObjectOverrideRuleCount();
        if (ruleCount == 0)
        {
            return;
        }

        auto& state = GetState();
        state.module = GetModuleHandleW(L"XEMIUtil.dll");
        const auto request =
            state.module ?
                reinterpret_cast<
                    XEMIWindowObjectOverrideAPI::RequestInterface>(
                    GetProcAddress(
                        state.module,
                        "XEMIUtil_RequestWindowObjectOverrideAPI")) :
                nullptr;
        const auto* api =
            request ?
                request(XEMIWindowObjectOverrideAPI::kVersion) :
                nullptr;
        if (!api ||
            api->version != XEMIWindowObjectOverrideAPI::kVersion ||
            !api->RegisterProjector)
        {
            logger::warn(
                "[Window Sync] XEMIUtil Window Sync object override projection API is unavailable; override targets cannot contribute to cellContains classification");
            return;
        }

        if (!api->RegisterProjector(
                std::addressof(objectOverrideProjector)))
        {
            logger::error(
                "[Window Sync] XEMIUtil rejected the WeatherSync object override projector");
            return;
        }
        logger::info(
            "[Window Sync] Registered the Window Sync object override projector for {} rule(s) with XEMIUtil",
            ruleCount);
    }

    void ProcessReference(RE::TESObjectREFR* a_reference)
    {
        auto& state = GetState();
        auto* cell = a_reference ? a_reference->GetParentCell() : nullptr;
        if (!cell || !state.api ||
            !WindowObjectOverrides::HasOverrideFor(a_reference))
        {
            return;
        }
        const auto result = state.api->GetCellResult(cell);
        if (result.status != XEMIAPI::CellStatus::kMatched)
        {
            return;
        }
        WindowObjectOverrides::ApplyToReference(
            a_reference,
            ResolveProfiles(result, cell->GetFormID()));
    }

    void Initialize()
    {
        auto& state = GetState();
        WindowObjectOverrides::Initialize();
        state.module = GetModuleHandleW(L"XEMIUtil.dll");
        const auto request = state.module ?
                                 reinterpret_cast<XEMIAPI::RequestInterface>(
                                     GetProcAddress(state.module, "XEMIUtil_RequestAPI")) :
                                 nullptr;
        state.api = request ? request(XEMIAPI::kVersion) : nullptr;
        if (!state.api || state.api->version != XEMIAPI::kVersion ||
            !state.api->RegisterClient || !state.api->HasWindowProfiles ||
            !state.api->GetCellResult)
        {
            state.api = nullptr;
            state.hasWindowProfiles = false;
            logger::info(
                "[Window Sync] XEMIUtil Window Sync API is unavailable; existing cell-patch Weather Sync remains active");
            return;
        }
        state.hasWindowProfiles = state.api->HasWindowProfiles();
        state.cellInventory.clear();
        state.collectingCellInventory = true;
        const bool registered =
            state.api->RegisterClient(std::addressof(callbacks));
        if (!registered)
        {
            state.collectingCellInventory = false;
            state.api = nullptr;
            state.hasWindowProfiles = false;
            logger::error(
                "[Window Sync] XEMIUtil rejected the WeatherSync client registration");
            return;
        }
        if (auto* tasks = SKSE::GetTaskInterface())
        {
            tasks->AddTask(FinishCellInventory);
        }
        else
        {
            logger::warn(
                "[Window Sync] SKSE task interface is unavailable; the Window Sync cell inventory may be incomplete");
            FinishCellInventory();
        }
        logger::info(
            "[Window Sync] XEMIUtil API connected; cellContains profiles={}",
            state.hasWindowProfiles ? "available" : "none");
    }

    RE::TESRegion* CaptureSourceRegion()
    {
        auto& state = GetState();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* sourceCell = player ? player->GetParentCell() : nullptr;
        auto* region = RegionRuntime::GetRegionForm(sourceCell);
        if (!region)
        {
            if (auto* sky = RE::Sky::GetSingleton())
            {
                region = sky->region;
            }
        }
        const auto editorID = RegionEditorID(region);
        if (region && !WeatherSync::IsSynchronizedRegion(editorID))
        {
            state.lastKnownRegion = region->GetFormID();
            if (WeatherSync::IsDetailedLoggingEnabled())
            {
                logger::info(
                    "[Window Sync] Captured last-known region '{}' [{:08X}] from cell {:08X}",
                    editorID.empty() ? "<no EditorID>" : editorID,
                    region->GetFormID(),
                    sourceCell ? sourceCell->GetFormID() : 0);
            }
        }
        return region ? region : LastKnownRegion();
    }

    void PrepareCellChange(
        RE::TESObjectCELL* a_destination,
        RE::TESWeather* a_sourceWeather,
        RE::TESRegion* a_sourceRegion)
    {
        auto& state = GetState();
        if (state.pending && state.pending->destination == a_destination &&
            state.pending->finishCalled && !state.pending->resolved)
        {
            return;
        }
        state.pending = PendingTransition{
            .destination = a_destination,
            .sourceWeather = a_sourceWeather,
            .sourceRegion = a_sourceRegion ? a_sourceRegion : LastKnownRegion(),
        };

        if (!a_destination || !a_destination->IsInteriorCell() ||
            !state.api || !state.hasWindowProfiles)
        {
            state.pending->resolved = true;
            return;
        }
        const auto result = state.api->GetCellResult(a_destination);
        if (state.pending && !state.pending->resolved)
        {
            ResolveTransition(*state.pending, result);
        }
    }

    void FinishCellChange(const RE::TESObjectCELL* a_destination)
    {
        auto& state = GetState();
        if (!state.pending || state.pending->destination != a_destination)
        {
            return;
        }
        state.pending->finishCalled = true;
        DispatchResolvedTransition();
    }

    void Save(SKSE::SerializationInterface* a_serialization)
    {
        if (!a_serialization ||
            !a_serialization->OpenRecord('WNSY', kSerializationVersion))
        {
            logger::error("[Window Sync] Failed to open the serialization record");
            return;
        }
        const auto formID = GetState().lastKnownRegion;
        if (!a_serialization->WriteRecordData(
                std::addressof(formID),
                sizeof(formID)))
        {
            logger::error("[Window Sync] Failed to save the last-known region");
        }
    }

    void Load(
        SKSE::SerializationInterface* a_serialization,
        const std::uint32_t a_version,
        const std::uint32_t a_length)
    {
        if (!a_serialization || a_version != kSerializationVersion ||
            a_length < sizeof(RE::FormID))
        {
            logger::warn(
                "[Window Sync] Ignored unsupported serialization record version {} with {} byte(s)",
                a_version,
                a_length);
            return;
        }

        RE::FormID saved = 0;
        if (a_serialization->ReadRecordData(std::addressof(saved), sizeof(saved)) !=
            sizeof(saved))
        {
            logger::error("[Window Sync] Failed to read the last-known region");
            return;
        }
        RE::FormID resolved = 0;
        if (saved && a_serialization->ResolveFormID(saved, resolved))
        {
            GetState().lastKnownRegion = resolved;
            logger::info(
                "[Window Sync] Restored last-known region {:08X} from the SKSE co-save",
                resolved);
        }
    }

    void Reset()
    {
        auto& state = GetState();
        state.lastKnownRegion = 0;
        state.pending.reset();
    }
}  // namespace MPL::WindowSync
