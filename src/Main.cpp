#include <AutoCSTonemapping.h>
#include <ExternalEmittance.h>
#include <Heliosphan.h>
#include <HeliosphanService.h>
#include <LifecycleTiming.h>
#include <LightPlacer.h>
#include <LumaClient.h>
#include <ObjectOverrides.h>
#include <Papyrus.h>
#include <Plugin.h>
#include <REL/Version.h>
#include <SKSE/API.h>
#include <WindowSync.h>

namespace
{
    bool lumaReady = false;
    bool startupClosed = false;

    void ConnectLuma(const std::string_view a_phase)
    {
        if (lumaReady || startupClosed)
        {
            return;
        }
        auto* registry = MPL::API::MMSF::RequestMMSFAPI();
        lumaReady = MPL::LumaClient::Load(a_phase, registry);
        if (!lumaReady)
        {
            return;
        }
        MPL::LifecycleTiming::ResumeStartupAfterEngineWait();
        MPL::LifecycleTiming::BeginStartupPhase(
            MPL::LifecycleTiming::StartupPhase::Configuration);
        MPL::Heliosphan::LoadConfiguration();
        MPL::HeliosphanService::Register(registry);
        MPL::LifecycleTiming::FinishStartupPhase(
            MPL::LifecycleTiming::StartupPhase::Configuration);
        MPL::LifecycleTiming::BeginStartupEngineWait();
    }

    void OnMMSFMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message || !a_message->sender ||
            std::string_view(a_message->sender) != "MMSF" ||
            a_message->type != MPL::API::MMSF::MMSFMessage::kMessage_MMSFServicesReg)
        {
            return;
        }
        if (!a_message->data || a_message->dataLen != sizeof(MPL::API::MMSF::MMSFMessage))
        {
            logger::warn("[MMSF Connection] registration message rejected | invalid payload");
            return;
        }
        ConnectLuma("MMSFServicesReg");
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message)
        {
            return;
        }
        switch (a_message->type)
        {
        case SKSE::MessagingInterface::kPostPostLoad:
            {
                // Subscribe after Luma's PostLoad listener, then cover either registration order.
                if (!SKSE::GetMessagingInterface()->RegisterListener("MMSF", OnMMSFMessage))
                {
                    logger::error("[MMSF Connection] registration listener unavailable");
                }
                ConnectLuma("PostPostLoad");
                break;
            }
        case SKSE::MessagingInterface::kDataLoaded:
            {
                startupClosed = true;
                MPL::LumaClient::LogCallbackSummary("DataLoaded-entry");
                if (!lumaReady)
                {
                    logger::critical(
                        "[Heliosphan] startup disabled | MMSF=2 | service=LUMA | required={} | not registered before record loading",
                        MPL::API::Luma::kVersion);
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
                    logger::warn("[Heliosphan] deferred startup unavailable | Auto CS Tonemapping immediate");
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

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    MPL::LifecycleTiming::BeginStartup();
    SKSE::Init(a_skse);
    logger::info("[Heliosphan] startup | game={}", a_skse->RuntimeVersion().string());
    logger::info(
        "[Heliosphan] API diagnostics=1 | version={} | build={} {} | callbackScope=received-only",
        MPL::Plugin::NAME,
        __DATE__,
        __TIME__);
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
