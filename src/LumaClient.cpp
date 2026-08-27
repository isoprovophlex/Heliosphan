#include <Heliosphan.h>
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

        void OnReferenceInitialized(RE::TESObjectREFR* a_reference)
        {
            RoomMarkerPatcher::ProcessReference(a_reference);
            WindowSync::ProcessReference(a_reference);
            ObjectOverrides::Patches::ApplyTransformsToReference(a_reference);
        }

        void OnCellChanging(RE::TESObjectCELL* a_destination)
        {
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

    bool Load()
    {
        module = GetModuleHandleW(L"LumaUtil.dll");
        const auto request =
            module ?
                reinterpret_cast<LumaAPI::RequestInterface>(
                    GetProcAddress(module, "LumaUtil_RequestAPI")) :
                nullptr;
        api = request ? request(LumaAPI::kVersion) : nullptr;
        if (!api || api->version != LumaAPI::kVersion ||
            !api->RegisterClient ||
            !api->GetProviderSettings ||
            !api->UpdateProviderSettings)
        {
            api = nullptr;
            return false;
        }
        return api->RegisterClient(std::addressof(callbacks));
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
