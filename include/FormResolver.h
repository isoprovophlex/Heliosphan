#pragma once

#include <RE/Skyrim.h>
#include <string_view>

namespace MPL::FormResolver
{
    RE::FormID Resolve(std::string_view a_selector);
}  // namespace MPL::FormResolver
