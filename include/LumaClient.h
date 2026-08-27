#pragma once

#include <LumaAPI.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace MPL::LumaClient
{
    bool Load();
    bool GetProviderSettings(std::string_view, bool&, bool&);
    bool UpdateProviderSettings(
        std::string_view,
        std::int8_t,
        std::int8_t);
}  // namespace MPL::LumaClient
