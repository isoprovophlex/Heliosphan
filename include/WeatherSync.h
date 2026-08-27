#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
    class TESObjectCELL;
    class TESWeather;
}  // namespace RE

namespace MPL::API
{
    class ServiceMap;
}

namespace MPL::WeatherSync
{
    struct WindowSyncProfile
    {
        std::string id;
        std::string regionPrefix;
        std::string fallbackRegion;
        std::optional<bool> showSky;
        std::optional<bool> useSkyLighting;
        std::optional<bool> sunlightShadows;
        std::uint32_t loadOrder = 0;
        bool usesPluginLoadOrder = false;
        bool synchronizesWeather = false;
        bool cleanRoomMarkers = false;
        std::vector<std::string> roomMarkerExcludedPlugins;
        bool debugLogging = false;
    };

    struct WindowObjectOverrideProfileView
    {
        std::string_view id;
        const std::map<std::string, std::string>* objectOverrides = nullptr;
        bool debugLogging = false;
    };

    bool IsDetailedLoggingEnabled();
    bool GetProfileDetailedLogging(std::string_view a_profile);
    bool GetProfileNotifications(std::string_view a_profile);
    bool SetProfileDetailedLogging(std::string_view a_profile, bool a_enabled);
    bool SetProfileNotifications(std::string_view a_profile, bool a_enabled);
    void LoadConfiguration();
    void RecordCellPatch(RE::TESObjectCELL* a_cell, std::string_view a_provider, bool a_hasSkylight);
    bool RecordWindowSyncCell(RE::TESObjectCELL*, std::string_view a_profile);
    std::optional<WindowSyncProfile> GetWindowSyncProfile(std::string_view a_profile);
    std::vector<WindowSyncProfile> GetWindowSyncProfiles();
    std::vector<WindowObjectOverrideProfileView> GetWindowObjectOverrideProfiles();
    std::size_t GetWindowObjectOverrideRuleCount();
    bool IsSynchronizedRegion(std::string_view a_editorID);
    std::string BaseRegionEditorID(std::string_view a_editorID);
    API::ServiceMap* GetMMSFAPI();
    RE::TESWeather* CaptureSourceWeather();
    void OnCellChanged(
        const RE::TESObjectCELL* a_cell,
        RE::TESWeather* a_sourceWeather,
        bool a_usedDefaultRegion);
    void OnDataLoaded();
    void OnGameLoaded();
    void Reset();
}  // namespace MPL::WeatherSync
