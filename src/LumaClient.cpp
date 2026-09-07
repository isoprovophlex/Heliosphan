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
        HMODULE module = nullptr;
        const LumaAPI::Interface* api = nullptr;
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

        const LumaAPI::ClientCallbacks callbacks{
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
        module = GetModuleHandleW(L"LumaUtil.dll");
        const auto request =
            module ?
                reinterpret_cast<LumaAPI::RequestInterface>(
                    GetProcAddress(module, "LumaUtil_RequestAPI")) :
                nullptr;
        api = request ? request(LumaAPI::kVersion) : nullptr;
        const char* failure = nullptr;
        if (!module)
        {
            failure = "DLL-unavailable";
        }
        else if (!request)
        {
            failure = "export-unavailable";
        }
        else if (!api)
        {
            failure = "API-request-rejected";
        }
        else if (api->version != LumaAPI::kVersion)
        {
            failure = "version-mismatch";
        }
        else if (!api->RegisterClient || !api->GetProviderSettings ||
                 !api->UpdateProviderSettings)
        {
            failure = "required-function-unavailable";
        }
        if (failure)
        {
            logger::error(
                "[Luma Connection] method=export | phase={} | required={} | reported={} | registration=false | reason={}",
                a_phase,
                LumaAPI::kVersion,
                api ? std::to_string(api->version) : "<unavailable>",
                failure);
            api = nullptr;
            return false;
        }
        const bool registered = api->RegisterClient(std::addressof(callbacks));
        callbacksRegistered.store(registered, std::memory_order_relaxed);
        logger::info(
            "[Luma Connection] method=export | phase={} | required={} | reported={} | registration={} | reason={}",
            a_phase,
            LumaAPI::kVersion,
            api->version,
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
        return api && api->GetProviderSettings &&
               api->GetProviderSettings(
                   id.c_str(),
                   std::addressof(a_detailedLogging),
                   nullptr);
    }

    bool UpdateProviderDetailedLogging(
        const std::string_view a_id,
        const bool a_detailedLogging)
    {
        const std::string id(a_id);
        return api && api->UpdateProviderSettings &&
               api->UpdateProviderSettings(
                   id.c_str(),
                   a_detailedLogging ?
                       std::int8_t{ 1 } :
                       std::int8_t{ 0 },
                   std::int8_t{ -1 });
    }

}  // namespace MPL::LumaClient
