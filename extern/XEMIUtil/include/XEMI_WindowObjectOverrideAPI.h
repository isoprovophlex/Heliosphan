#pragma once

#include <cstddef>
#include <cstdint>

namespace MPL::XEMIWindowObjectOverrideAPI
{
    inline constexpr std::uint32_t kVersion = 1;

    using Prepare = bool (*)();
    using ProjectBase = std::uint32_t (*)(
        const char*,
        std::size_t,
        std::uint32_t);

    struct Projector
    {
        // The ID is copied during registration.
        const char* id = nullptr;
        Prepare PrepareOverrides = nullptr;
        // The profile pointer is borrowed during this synchronous call.
        ProjectBase ProjectOverrideBase = nullptr;
    };

    struct Interface
    {
        std::uint32_t version = kVersion;
        bool (*RegisterProjector)(const Projector*) = nullptr;
    };

    using RequestInterface = const Interface* (*)(std::uint32_t);
}  // namespace MPL::XEMIWindowObjectOverrideAPI
