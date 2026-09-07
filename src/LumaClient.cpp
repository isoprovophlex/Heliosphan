#include <LumaAPI.h>
#include <Heliosphan.h>
#include <LumaCallbackDiagnostics.h>
#include <LumaClient.h>
#include <ObjectOverrides.h>
#include <RoomMarkerPatcher.h>
#include <WindowSync.h>
#include <string>

namespace MPL::LumaClient
{
    namespace
    {
        MPL::API::Luma::ILumaPluginService* api = nullptr;
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
        }

        void OnCellChanged(const RE::TESObjectCELL* a_destination)
        {
            const auto sequence = callbackCounters.CellChanged();
            TraceCellCallback("OnCellChanged", a_destination, sequence);
            auto* destination =
                const_cast<RE::TESObjectCELL*>(a_destination);
            WindowSync::FinishCellChange(a_destination);
            Heliosphan::FinishCellTiming(destination);
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

       bool Load(const std::string_view a_phase)
    {
        callbacksRegistered.store(false, std::memory_order_relaxed);
        api = nullptr;

        auto* mmsf = Heliosphan::GetMMSFAPI();
        API::MMSF::IPluginService* service = nullptr;
        const char* failure = nullptr;

        if (!mmsf)
        {
            failure = "MMSF-unavailable";
        }
        else
        {
            const auto features = mmsf->GetVersion();
            if (API::MMSF::GetVersion(features) != 2)
            {
                failure = "MMSF-version-mismatch";
            }
            else if ((features & API::MMSF::MMSFAPIFeatures::kCoreService) ==
                     API::MMSF::MMSFAPIFeatures{})
            {
                failure = "service-registry-unavailable";
            }
            else
            {
                service = mmsf->QueryService("LUMA");
                if (!service)
                {
                    failure = "LUMA-service-unavailable";
                }
                else if (service->GetVersion() != API::Luma::kVersion)
                {
                    failure = "LUMA-version-mismatch";
                }
            }
        }

        if (failure)
        {
            logger::error(
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
        return api->GetProviderSettings(
                   id.c_str(),
                   std::addressof(a_detailedLogging),
                   nullptr);
    }

    bool UpdateProviderDetailedLogging(
        const std::string_view a_id,
        const bool a_detailedLogging)
    {
        const std::string id(a_id);
        return api->UpdateProviderSettings(
                   id.c_str(),
                   a_detailedLogging ?
                       std::int8_t{ 1 } :
                       std::int8_t{ 0 },
                   std::int8_t{ -1 });
    }

}  // namespace MPL::LumaClient
