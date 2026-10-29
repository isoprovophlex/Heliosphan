#pragma once

#include <MMSF_API.h>

namespace RE
{
    class TESObjectREFR;
}

namespace MPL::API::HeliosphanModels
{
    inline constexpr std::uint8_t kVersion = 1;
    inline constexpr auto kServiceName = "HELIOSPHAN_MODELS";

    class IService : public MMSF::IPluginService
    {
    public:
        // Call on the game task queue before collecting shader properties.
        virtual void PrepareReference(RE::TESObjectREFR*) = 0;
        virtual bool HasModelRules(const char*) = 0;
    };
}
