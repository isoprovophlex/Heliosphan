#include <AutoCSTonemapping.h>
#include <ExternalEmittance.h>
#include <Heliosphan.h>
#include <HeliosphanAPI.h>
#include <LifecycleTiming.h>
#include <LightPlacer.h>
#include <LumaClient.h>
#include <ObjectOverrides.h>
#include <Papyrus.h>
#include <Plugin.h>
#include <REL/Version.h>
#include <SKSE/API.h>
#include <WeatherRuntime.h>
#include <WindowSync.h>

namespace
{
    bool lumaReady = false;

    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message)
        {
            return;
        }
        switch (a_message->type)
        {
        case SKSE::MessagingInterface::kPostLoad:
            {
                lumaReady = MPL::LumaClient::Load();
                if (!lumaReady)
                {
                    logger::critical(
                        "Heliosphan requires LumaUtil API version {}; "
                        "Heliosphan will remain inactive",
                        MPL::LumaAPI::kVersion);
                    break;
                }
                MPL::LifecycleTiming::ResumeStartupAfterEngineWait();
                MPL::LifecycleTiming::BeginStartupPhase(
                    MPL::LifecycleTiming::StartupPhase::Configuration);
                MPL::Heliosphan::LoadConfiguration();
                MPL::LifecycleTiming::FinishStartupPhase(
                    MPL::LifecycleTiming::StartupPhase::Configuration);
                MPL::LifecycleTiming::BeginStartupEngineWait();
                break;
            }
        case SKSE::MessagingInterface::kDataLoaded:
            {
                if (!lumaReady)
                {
                    break;
                }
                MPL::LifecycleTiming::ResumeStartupAfterEngineWait();
                MPL::Heliosphan::OnDataLoaded();
                MPL::LifecycleTiming::BeginStartupPhase(
                    MPL::LifecycleTiming::StartupPhase::LightPlacer);
                MPL::LightPlacer::InitializeConfigurationFiles();
                MPL::LifecycleTiming::FinishStartupPhase(
                    MPL::LifecycleTiming::StartupPhase::LightPlacer);
                MPL::WindowSync::Initialize();
                MPL::ExternalEmittance::ScheduleFinalReferenceInitialization();
                if (auto* tasks = SKSE::GetTaskInterface())
                {
                    tasks->AddTask(
                        []
                        {
                            MPL::AutoCSTonemapping::ApplyStartup();
                        });
                }
                else
                {
                    logger::warn(
                        "SKSE task interface is unavailable; Auto CS "
                        "Tonemapping will run without deferred startup ordering");
                    MPL::AutoCSTonemapping::ApplyStartup();
                }
                break;
            }
        case SKSE::MessagingInterface::kPreLoadGame:
            {
                MPL::LifecycleTiming::BeginGameLoad();
                MPL::ObjectOverrides::Patches::BeginGameLoad();
                MPL::ExternalEmittance::BeginGameLoad();
                MPL::WindowSync::Reset();
                MPL::Heliosphan::Reset();
                break;
            }
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            if (lumaReady)
            {
                MPL::ObjectOverrides::Patches::BeginGameLoad();
                MPL::ExternalEmittance::BeginGameLoad();
                MPL::Heliosphan::OnGameLoaded();
            }
            break;
        default:
            break;
        }
    }

    void Serialize(SKSE::SerializationInterface* a_serialization)
    {
        MPL::WindowSync::Save(a_serialization);
    }

    void Deserialize(SKSE::SerializationInterface* a_serialization)
    {
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (a_serialization->GetNextRecordInfo(
            type,
            version,
            length))
        {
            if (type == 'WNSY')
            {
                MPL::WindowSync::Load(
                    a_serialization,
                version,
                length);
            }
        }
    }

    void Revert(SKSE::SerializationInterface*)
    {
        MPL::ObjectOverrides::Patches::BeginGameLoad();
        MPL::ExternalEmittance::BeginGameLoad();
        MPL::WindowSync::Reset();
        MPL::Heliosphan::Reset();
    }

    const MPL::HeliosphanAPI::Interface api{
        .version = MPL::HeliosphanAPI::kVersion,
        .SetWeatherInstant = MPL::WeatherRuntime::SetWeatherInstant,
        .RegisterLightPlacerTransformer =
            MPL::LightPlacer::RegisterTransformer,
        .RequestLightPlacerReload = MPL::LightPlacer::RequestReload,
        .RegisterReferenceClient =
            MPL::ExternalEmittance::RegisterReferenceClient,
    };
}  // namespace

extern "C" __declspec(dllexport)
const MPL::HeliosphanAPI::Interface*
Heliosphan_RequestAPI(
    const std::uint32_t a_version)
{
    return a_version == MPL::HeliosphanAPI::kVersion ?
               std::addressof(api) :
               nullptr;
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    MPL::LifecycleTiming::BeginStartup();
    SKSE::Init(a_skse);
    logger::info("Game version : {}", a_skse->RuntimeVersion().string());
    SKSE::GetPapyrusInterface()->Register(MPL::Papyrus::Bind);
    SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID('WSYN');
    serialization->SetSaveCallback(Serialize);
    serialization->SetLoadCallback(Deserialize);
    serialization->SetRevertCallback(Revert);
    return true;
}

SKSEPluginInfo(
        .Version = REL::Version{
            MPL::Plugin::MAJOR,
            MPL::Plugin::MINOR,
            MPL::Plugin::PATCH,
            0 },
    .Name = "Heliosphan"sv, .Author = "isoprovophlex"sv, .SupportEmail = ""sv, .StructCompatibility = SKSE::StructCompatibility::Independent, .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)
