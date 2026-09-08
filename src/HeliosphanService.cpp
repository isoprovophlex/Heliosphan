#include <AutoCSTonemapping.h>
#include <ExternalEmittance.h>
#include <HeliosphanService.h>
#include <HeliosphanServiceAPI.h>
#include <LightPlacer.h>
#include <MMSFServices.h>
#include <WeatherRuntime.h>

namespace MPL::HeliosphanService
{
    namespace
    {
        class Service final : public API::Heliosphan::IHeliosphanService
        {
        public:
            std::uint8_t GetVersion() override { return API::Heliosphan::kVersion; }
            std::string GetName() override { return API::Heliosphan::kServiceName; }
            // This service coordinates runtime calls and owns no persisted data.
            void Initialize() override {}
            rfl::Generic::Object Save() override { return {}; }
            void Load(rfl::Generic::Object) override {}

            API::Heliosphan::SetWeatherResult SetWeatherInstant(RE::TESWeather* a_weather, bool a_reset) override
            {
                return WeatherRuntime::SetWeatherInstant(a_weather, a_reset);
            }

            bool RegisterLightPlacerTransformer(const API::Heliosphan::LightPlacerTransformer* a_transformer) override
            {
                return LightPlacer::RegisterTransformer(a_transformer);
            }

            bool RequestLightPlacerReload() override
            {
                return LightPlacer::RequestReload();
            }

            bool RegisterReferenceClient(const API::Heliosphan::ReferenceCallbacks* a_callbacks) override
            {
                return ExternalEmittance::RegisterReferenceClient(a_callbacks);
            }

            bool GetAutoCSTonemapping(const char* a_profile) override
            {
                return a_profile && AutoCSTonemapping::GetProfileEnabled(a_profile);
            }

            bool SetAutoCSTonemapping(const char* a_profile, bool a_enabled) override
            {
                return a_profile && AutoCSTonemapping::SetProfileEnabled(a_profile, a_enabled);
            }

            bool IsAutoCSTonemappingApplied(const char* a_profile) override
            {
                return a_profile && AutoCSTonemapping::IsProfileApplied(a_profile);
            }

            bool SetAutoCSTonemappingSuppressed(const char* a_profile, bool a_suppressed) override
            {
                return a_profile && AutoCSTonemapping::SetProfileSuppressed(a_profile, a_suppressed);
            }

            bool SetCSTonemappingForcedTargets(RE::TESImageSpace* const* a_targets, std::size_t a_count) override
            {
                return AutoCSTonemapping::SetForcedTargets(a_targets, a_count);
            }
        };
    }

    bool Register(API::MMSF::Interface* a_registry)
    {
        static Service service;
        const auto result = MMSFServices::Query(a_registry, API::Heliosphan::kServiceName, API::Heliosphan::kVersion);
        const char* failure = result.failure;
        if (result.service)
        {
            failure = result.service == &service ? nullptr : "service-name-occupied";
        }
        else if (failure && std::string_view(failure) == "service-unavailable")
        {
            a_registry->RegisterService(&service);
            failure = a_registry->QueryService(API::Heliosphan::kServiceName) == &service ? nullptr : "registration-rejected";
        }
        if (failure)
        {
            logger::error("[MMSF Service] service=HELIOSPHAN | version={} | registered=false | reason={}",
                API::Heliosphan::kVersion, failure);
            return false;
        }
        logger::info("[MMSF Service] service=HELIOSPHAN | version={} | registered=true", API::Heliosphan::kVersion);
        return true;
    }
}
