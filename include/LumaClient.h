#pragma once

#include <LumaAPI.h>
#include <string>
#include <string_view>

namespace MPL::LumaClient
{
    bool Load(std::string_view a_phase);
    void LogCallbackSummary(std::string_view a_phase);
    bool GetProviderDetailedLogging(std::string_view, bool&);
    bool UpdateProviderDetailedLogging(std::string_view, bool);
}  // namespace MPL::LumaClient
