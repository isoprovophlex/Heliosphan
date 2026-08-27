#include <AutoCSTonemapping.h>
#include <LumaClient.h>
#include <Papyrus.h>
#include <Plugin.h>
#include <REL/Version.h>
#include <SKSE/API.h>
#include <WeatherRuntime.h>
#include <WeatherSync.h>
#include <WeatherSyncAPI.h>
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
            lumaReady = MPL::LumaClient::Load();
            if (!lumaReady)
            {
                logger::critical(
                    "WeatherSync requires LumaUtil API version {}; "
                    "WeatherSync will remain inactive",
                    MPL::LumaAPI::kVersion);
                break;
            }
            MPL::WeatherSync::LoadConfiguration();
            MPL::WindowSync::RegisterObjectOverrideProjection();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (!lumaReady)
            {
                break;
            }
            MPL::WeatherSync::OnDataLoaded();
            MPL::WindowSync::Initialize();
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
        case SKSE::MessagingInterface::kPreLoadGame:
            MPL::WindowSync::Reset();
            MPL::WeatherSync::Reset();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            if (lumaReady)
            {
                MPL::WeatherSync::OnGameLoaded();
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
        MPL::WindowSync::Reset();
        MPL::WeatherSync::Reset();
    }

    const MPL::WeatherSyncAPI::Interface api{
        .version = MPL::WeatherSyncAPI::kVersion,
        .SetWeatherInstant = MPL::WeatherRuntime::SetWeatherInstant,
    };
}  // namespace

extern "C" __declspec(dllexport)
const MPL::WeatherSyncAPI::Interface*
WeatherSync_RequestAPI(
    const std::uint32_t a_version)
{
    return a_version == MPL::WeatherSyncAPI::kVersion ?
               std::addressof(api) :
               nullptr;
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
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
    .Name = "WeatherSync"sv, .Author = "isoprovophlex"sv, .SupportEmail = ""sv, .StructCompatibility = SKSE::StructCompatibility::Independent, .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)
