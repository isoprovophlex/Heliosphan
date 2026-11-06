#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace MPL::WeatherSoundFix
{
    // Verified in the 1.6.1170 audio message dispatcher.
    inline constexpr std::uint32_t kSoundFadeInProgress = 1u << 9;

    constexpr bool ShouldCorrectCopiedAttenuation(bool a_target, bool a_playing,
        std::uint32_t a_flags, std::uint16_t a_inherited, std::uint16_t a_current,
        std::uint16_t a_category)
    {
        return a_target && a_playing && (a_flags & kSoundFadeInProgress) == 0 &&
            a_current == a_inherited && a_inherited > a_category;
    }

    struct ResourceKey
    {
        std::uint32_t file = 0;
        std::uint32_t extension = 0;
        std::uint32_t directory = 0;
        bool operator==(const ResourceKey&) const = default;
    };

    struct ResourceHash
    {
        std::size_t operator()(const ResourceKey& a_key) const noexcept
        {
            return std::hash<std::uint32_t>{}(a_key.file) ^
                (std::hash<std::uint32_t>{}(a_key.extension) << 1) ^
                (std::hash<std::uint32_t>{}(a_key.directory) << 2);
        }
    };
}
