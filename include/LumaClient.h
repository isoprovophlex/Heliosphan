#pragma once

#include <LumaServiceAPI.h>
#include <string_view>

namespace MPL::LumaClient
{
    bool Load(std::string_view a_phase, API::MMSF::Interface* a_registry);
    void LogCallbackSummary(std::string_view a_phase);
    bool GetProviderDetailedLogging(std::string_view, bool&);
    bool UpdateProviderDetailedLogging(std::string_view, bool);
}  // namespace MPL::LumaClient
