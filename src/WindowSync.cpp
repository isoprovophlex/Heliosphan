#include <CellClassifier.h>
#include <ExternalEmittance.h>
#include <LightPlacer.h>
#include <ObjectOverrides.h>
#include <PluginIndex.h>
#include <LumaClient.h>
#include <RegionRuntime.h>
#include <RoomMarkerPatcher.h>
#include <Heliosphan.h>
#include <WindowSync.h>
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
            std::optional<Heliosphan::WindowSyncProfile> profile;
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
            bool hasWindowProfiles = false;
            bool collectingCellInventory = false;
            bool initializationRequested = false;
            bool classificationApplied = false;
            bool startupPreparationStarted = false;
            RE::FormID lastKnownRegion = 0;
            std::optional<PendingTransition> pending;
            std::vector<CellInventoryEntry> cellInventory;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        MPL::API::MMSF::Interface* MMSF()
        {
            return Heliosphan::GetMMSFAPI();
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
            const Heliosphan::WindowSyncProfile& a_profile,
            const std::string_view a_message)
        {
            if (a_profile.debugLogging)
            {
                logger::info("[Window Sync] [{}] {}", a_profile.id, a_message);
            }
        }

        std::vector<Heliosphan::WindowSyncProfile> ResolveProfiles(
            const std::vector<std::string>& a_profileIDs)
        {
            std::vector<Heliosphan::WindowSyncProfile> profiles;
            auto profileIDs = a_profileIDs;
            Heliosphan::SortWindowSyncProfileIDs(profileIDs);
            profiles.reserve(profileIDs.size());
            for (const auto& id : profileIDs)
            {
                if (auto profile =
                        Heliosphan::GetWindowSyncProfile(id))
                {
                    profiles.push_back(std::move(*profile));
                }
            }
            return profiles;
        }

        RE::TESRegion* ResolveTargetRegion(
            const Heliosphan::WindowSyncProfile& a_profile,
            RE::TESRegion* a_sourceRegion)
        {
            auto sourceEditorID = RegionEditorID(a_sourceRegion);
            if (sourceEditorID.empty())
            {
                return nullptr;
            }
            const auto baseEditorID = Heliosphan::BaseRegionEditorID(sourceEditorID);
            return LookupRegion(a_profile.regionPrefix + baseEditorID);
        }

        RE::TESRegion* ResolveFallbackTargetRegion(
            const Heliosphan::WindowSyncProfile& a_profile)
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
            if (Heliosphan::IsSynchronizedRegion(RegionEditorID(fallbackRegion)))
            {
                return fallbackRegion;
            }
            return ResolveTargetRegion(a_profile, fallbackRegion);
        }

        void ShowRegionSyncFailure(
            const Heliosphan::WindowSyncProfile& a_profile)
        {
            const auto message = std::format(
                "{}: Region sync failed. Please report.",
                a_profile.id);
            logger::error("[Heliosphan] [Message Box] {}", message);
            RE::DebugMessageBox(message.c_str());
        }

        void ApplyCellFlags(
            RE::TESObjectCELL* a_cell,
            const Heliosphan::WindowSyncProfile& a_profile)
        {
            if (!a_cell)
            {
                return;
            }
            const auto sunlightShadowsFlag =
                static_cast<RE::TESObjectCELL::Flag>(1 << 15);
            std::string appliedFlags;
            const auto recordFlag =
                [&](const std::string_view a_name, const bool a_value)
            {
                if (!appliedFlags.empty())
                {
                    appliedFlags += ", ";
                }
                appliedFlags += std::format("{}={}", a_name, a_value);
            };
            if (a_profile.showSky)
            {
                a_cell->cellFlags.set(
                    *a_profile.showSky,
                    RE::TESObjectCELL::Flag::kShowSky);
                recordFlag("ShowSky", *a_profile.showSky);
            }
            if (a_profile.useSkyLighting)
            {
                a_cell->cellFlags.set(
                    *a_profile.useSkyLighting,
                    RE::TESObjectCELL::Flag::kUseSkyLighting);
                recordFlag("UseSkyLighting", *a_profile.useSkyLighting);
            }
            if (a_profile.sunlightShadows)
            {
                a_cell->cellFlags.set(
                    *a_profile.sunlightShadows,
                    sunlightShadowsFlag);
                recordFlag(
                    "SunlightShadows",
                    *a_profile.sunlightShadows);
            }
            if (a_profile.debugLogging && !appliedFlags.empty())
            {
                logger::info(
                    "[Window Sync] [{}] Applied cell flags to '{}' [{:08X}]: {}",
                    a_profile.id,
                    a_cell->GetFormEditorID() ?
                        a_cell->GetFormEditorID() :
                        "<no EditorID>",
                    a_cell->GetFormID(),
                    appliedFlags);
            }
        }

        void ApplyIndexedCellSettings(
            RE::TESObjectCELL* a_cell,
            const Heliosphan::WindowSyncProfile& a_profile)
        {
            ApplyCellFlags(a_cell, a_profile);
        }

        void ApplyRoomMarkerCleaning(
            RE::TESObjectCELL* a_cell,
            const std::vector<Heliosphan::WindowSyncProfile>& a_profiles)
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
            const std::vector<std::string>& a_profileIDs)
        {
            auto profiles = ResolveProfiles(a_profileIDs);
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
                    Heliosphan::BaseRegionEditorID(RegionEditorID(sourceRegion));
                logger::warn(
                    "[Window Sync] [{}] Could not find the synchronized region '{}' for cell {:08X}",
                    syncProfile->id,
                    targetEditorID,
                    a_transition.destination ? a_transition.destination->GetFormID() : 0);
                targetRegion = ResolveFallbackTargetRegion(*syncProfile);
                if (targetRegion)
                {
                    a_transition.usedDefaultRegion = true;
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
            if (!Heliosphan::RecordWindowSyncCell(
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
                    "Window Sync applied: cell={:08X}, profile={}, region='{}', layers={}, fallback={}",
                    a_transition.destination->GetFormID(),
                    syncProfile->id,
                    RegionEditorID(targetRegion),
                    profiles.size(),
                    a_transition.usedDefaultRegion));
            return true;
        }

        void ResolveTransition(
            PendingTransition& a_transition,
            const std::vector<std::string>& a_profileIDs)
        {
            a_transition.profile.reset();
            if (!a_profileIDs.empty())
            {
                ResolveMatchedDestination(a_transition, a_profileIDs);
            }
            a_transition.resolved = true;
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
                            sky->region = extra->skyRegion;
                        }
                    }
                }
            }
            Heliosphan::OnCellChanged(
                transition.destination,
                transition.sourceWeather,
                transition.usedDefaultRegion);
        }

        void OnCellClassified(
            RE::TESObjectCELL* a_cell,
            const std::vector<std::string>& a_profileIDs)
        {
            auto& state = GetState();
            if (!a_cell)
            {
                return;
            }
            if (state.collectingCellInventory &&
                !a_profileIDs.empty())
            {
                CellInventoryEntry entry{
                    .cell = a_cell->GetFormID(),
                    .profiles = a_profileIDs,
                };
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
            std::vector<Heliosphan::WindowSyncProfile> profiles;
            if (!a_profileIDs.empty())
            {
                profiles = ResolveProfiles(a_profileIDs);
                for (const auto& profile : profiles)
                {
                    ApplyIndexedCellSettings(a_cell, profile);
                }
                ObjectOverrides::ApplyToCell(a_cell, profiles);
                ApplyRoomMarkerCleaning(a_cell, profiles);
            }
            else
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
            ResolveTransition(*state.pending, a_profileIDs);
            DispatchResolvedTransition();
        }

        void LogCellInventory(std::vector<CellInventoryEntry>& a_entries)
        {
            if (!Heliosphan::IsDetailedLoggingEnabled())
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

        void ApplyPreparedClassification()
        {
            auto& state = GetState();
            if (!state.initializationRequested ||
                state.classificationApplied ||
                !CellClassifier::IsReady())
            {
                return;
            }

            state.classificationApplied = true;
            state.hasWindowProfiles = CellClassifier::HasProfiles();
            state.cellInventory.clear();
            state.collectingCellInventory = true;
            for (std::size_t index = 0;
                 index < CellClassifier::GetProfiledCellCount();
                 ++index)
            {
                const auto cellID =
                    CellClassifier::GetProfiledCell(index);
                if (!cellID)
                {
                    continue;
                }
                auto* cell =
                    RE::TESForm::LookupByID<RE::TESObjectCELL>(cellID);
                if (!cell)
                {
                    continue;
                }
                OnCellClassified(
                    cell,
                    CellClassifier::GetProfiles(cellID));
            }
            FinishCellInventory();
            logger::info(
                "[Window Sync] cellContains profiles={}",
                state.hasWindowProfiles ? "available" : "none");
        }

        bool PrepareWindowSyncProfiles()
        {
            auto& state = GetState();
            if (state.startupPreparationStarted)
            {
                return CellClassifier::IsReady();
            }
            state.startupPreparationStarted = true;

            const bool globalExternalEmittance =
                ExternalEmittance::RequiresCompleteIndex();
            const bool globalLightPlacer =
                LightPlacer::RequiresCompleteInteriorIndex();
            const bool pluginIndexRequired =
                CellClassifier::RequiresPluginIndex() ||
                LightPlacer::RequiresPluginIndex() ||
                ExternalEmittance::RequiresPluginIndex();
            PluginIndex::Result index;
            if (pluginIndexRequired)
            {
                PluginIndex::BuildOptions indexOptions{
                    .indexStaticEditorIDs =
                        CellClassifier::RequiresStaticEditorIDs(),
                    .skipExteriorCells = !globalExternalEmittance,
                    .preparePlacementFilter =
                        [](const PluginIndex::BuildOptions::EditorIDMap& a_editorIDs)
                        {
                            CellClassifier::PreparePlacementFilter(a_editorIDs);
                            ExternalEmittance::PreparePlacementFilter();
                            LightPlacer::PreparePlacementFilter();
                        },
                    .retainPlacement =
                        [](const RE::FormID a_reference,
                           const RE::FormID a_base,
                           const RE::FormID)
                        {
                            return CellClassifier::NeedsPlacement(
                                       a_reference,
                                       a_base) ||
                                   ExternalEmittance::NeedsPlacement(
                                       a_reference,
                                       a_base) ||
                                   LightPlacer::NeedsPlacement(
                                       a_reference,
                                       a_base);
                        },
                };
                if (!globalExternalEmittance && !globalLightPlacer)
                {
                    indexOptions.excludedCells =
                        CellClassifier::GetExcludedCells();
                }
                logger::info(
                    "[Window Sync] Plugin index pruning: exterior cells={}, excluded interior cells={}, global Light Placer={}, global external emittance={}",
                    indexOptions.skipExteriorCells ? "skipped" : "retained",
                    indexOptions.excludedCells.size(),
                    globalLightPlacer ? "active" : "inactive",
                    globalExternalEmittance ? "active" : "inactive");
                index = PluginIndex::Build(indexOptions);
            }
            else
            {
                index.complete = true;
                logger::info(
                    "[Window Sync] Plugin index skipped because no active feature requires saved reference data");
            }
            const bool prepared = CellClassifier::Prepare(index);
            if (prepared)
            {
                LightPlacer::Prepare(index);
                ApplyPreparedClassification();
                ExternalEmittance::Prepare(index);
            }
            else
            {
                LightPlacer::QueueStartupPatch({});
            }
            return prepared;
        }

    }  // namespace

    void ProcessReference(RE::TESObjectREFR* a_reference)
    {
        auto* cell = a_reference ? a_reference->GetParentCell() : nullptr;
        if (!cell ||
            !ObjectOverrides::HasOverrideFor(a_reference))
        {
            return;
        }
        const auto& profileIDs =
            CellClassifier::GetProfiles(cell->GetFormID());
        ObjectOverrides::ApplyToReference(
            a_reference,
            ResolveProfiles(profileIDs));
    }

    void Initialize()
    {
        auto& state = GetState();
        ObjectOverrides::Initialize();
        ObjectOverrides::Patches::Initialize();
        state.initializationRequested = true;
        PrepareWindowSyncProfiles();
        ApplyPreparedClassification();
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
        if (region && !Heliosphan::IsSynchronizedRegion(editorID))
        {
            state.lastKnownRegion = region->GetFormID();
            if (Heliosphan::IsDetailedLoggingEnabled())
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
            !state.hasWindowProfiles)
        {
            state.pending->resolved = true;
            return;
        }
        const auto& profiles =
            CellClassifier::GetProfiles(a_destination->GetFormID());
        if (state.pending && !state.pending->resolved)
        {
            ResolveTransition(*state.pending, profiles);
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
