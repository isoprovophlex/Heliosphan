#include <LumaServiceAPI.h>
#include <Heliosphan.h>
#include <ImageSpaceDiagnostics.h>
#include <LumaCallbackDiagnostics.h>
#include <LumaClient.h>
#include <MMSFServices.h>
#include <MMSFProviderSettings.h>
#include <ObjectOverrides.h>
#include <RoomMarkerPatcher.h>
#include <WindowSync.h>
#include <string>

namespace MPL::LumaClient
{
    namespace
    {
        MPL::API::Luma::ILumaPluginService* api = nullptr;
        API::MMSF::Interface* registry = nullptr;
        Diagnostics::CallbackCounters callbackCounters;
        std::atomic_bool callbacksRegistered{ false };

        void TraceCellCallback(
            const std::string_view a_callback,
            const RE::TESObjectCELL* a_cell,
            const std::uint64_t a_sequence)
        {
            if (Heliosphan::IsDetailedLoggingEnabled())
            {
                logger::info(
                    "[Luma Callback] sequence={} | event={} | cell={:08X} | thread={}",
                    a_sequence,
                    a_callback,
                    a_cell ? a_cell->GetFormID() : 0,
                    GetCurrentThreadId());
                spdlog::default_logger()->flush();
            }
        }

        void OnReferenceInitialized(RE::TESObjectREFR* a_reference)
        {
            callbackCounters.ReferenceInitialized();
            RoomMarkerPatcher::ProcessReference(a_reference);
            WindowSync::ProcessReference(a_reference);
            ObjectOverrides::Patches::ApplyTransformsToReference(a_reference);
        }

        void OnCellChanging(RE::TESObjectCELL* a_destination)
        {
            const auto sequence = callbackCounters.CellChanging();
            TraceCellCallback("OnCellChanging", a_destination, sequence);
            Heliosphan::BeginCellTiming(a_destination);
            if (a_destination)
            {
                ObjectOverrides::Patches::EnsurePlacements(a_destination);
            }
            auto* sourceWeather = Heliosphan::CaptureSourceWeather();
            auto* sourceRegion = WindowSync::CaptureSourceRegion();
            WindowSync::PrepareCellChange(
                a_destination,
                sourceWeather,
                sourceRegion);
            TraceCellCallback("OnCellChanging-return", a_destination, sequence);
        }

        void OnCellChanged(const RE::TESObjectCELL* a_destination)
        {
            const auto sequence = callbackCounters.CellChanged();
            TraceCellCallback("OnCellChanged", a_destination, sequence);
            auto* destination =
                const_cast<RE::TESObjectCELL*>(a_destination);
            WindowSync::FinishCellChange(a_destination);
            Heliosphan::FinishCellTiming(destination);
            TraceCellCallback("OnCellChanged-return", a_destination, sequence);
            ImageSpaceDiagnostics::QueueSnapshot("cell-changed", a_destination ? a_destination->GetFormID() : 0,
                Heliosphan::IsDetailedLoggingEnabled);
        }

        void OnCellPatched(
            RE::TESObjectCELL* a_cell,
            const char* a_provider,
            const bool a_hasSkylight)
        {
            callbackCounters.CellPatched();
            Heliosphan::RecordCellPatch(
                a_cell,
                a_provider ? std::string_view(a_provider) :
                             std::string_view{},
                a_hasSkylight);
        }

        const MPL::API::Luma::ClientCallbacks callbacks{
            .id = "Heliosphan",
            .OnReferenceInitialized = OnReferenceInitialized,
            .OnCellChanging = OnCellChanging,
            .OnCellChanged = OnCellChanged,
            .OnCellPatched = OnCellPatched,
        };
    }  // namespace

    bool Load(const std::string_view a_phase, API::MMSF::Interface* a_registry)
    {
        if (api)
        {
            return true;
        }

        const auto [service, failure] = MMSFServices::Query(a_registry, "LUMA", API::Luma::kVersion);
        if (failure)
        {
            logger::info(
                "[Luma Connection] method=MMSF | service=LUMA | phase={} | required={} | reported={} | registration=false | reason={}",
                a_phase,
                API::Luma::kVersion,
                service ? std::to_string(service->GetVersion()) : "<unavailable>",
                failure);
            return false;
        }

        auto* candidate =
            static_cast<API::Luma::ILumaPluginService*>(service);
        const bool registered =
            candidate->RegisterClient(std::addressof(callbacks));

        if (registered)
        {
            registry = a_registry;
            api = candidate;
        }
        callbacksRegistered.store(registered, std::memory_order_relaxed);

        logger::info(
            "[Luma Connection] method=MMSF | service=LUMA | phase={} | required={} | reported={} | registration={} | reason={}",
            a_phase,
            API::Luma::kVersion,
            service->GetVersion(),
            registered,
            registered ? "accepted" : "registration-rejected");
        return registered;
    }

    void LogCallbackSummary(const std::string_view a_phase)
    {
        const auto counts = callbackCounters.Snapshot();
        logger::info(
            "[Luma Callbacks] phase={} | scope=received-cumulative | registered={} | references={} | changing={} | changed={} | patched={}",
            a_phase,
            callbacksRegistered.load(std::memory_order_relaxed),
            counts.references,
            counts.changing,
            counts.changed,
            counts.patched);
    }

    bool GetProviderDetailedLogging(
        const std::string_view a_id,
        bool& a_detailedLogging)
    {
        const std::string id(a_id);
        return MMSFProviderSettings::ReadDetailedLogging(registry, id.c_str(), a_detailedLogging);
    }

    bool UpdateProviderDetailedLogging(
        const std::string_view a_id,
        const bool a_detailedLogging)
    {
        const std::string id(a_id);
        return MMSFProviderSettings::WriteDetailedLogging(registry, id.c_str(), a_detailedLogging);
    }

}  // namespace MPL::LumaClient
