#pragma once

#include <LumaAPI.h>
#include <string>
#include <string_view>

namespace MPL::LumaClient
{
    bool Load();
    bool GetProviderDetailedLogging(std::string_view, bool&);
    bool UpdateProviderDetailedLogging(std::string_view, bool);
}  // namespace MPL::LumaClient
