#include <RegionWeatherPatcher.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MPL::RegionWeatherPatcher
{
    namespace
    {
        struct MappedWeather
        {
            RE::TESWeather* weather = nullptr;
            RE::TESGlobal* global = nullptr;
            std::uint32_t chance = 0;
            std::string sourceEditorID;
            std::string targetEditorID;
        };

        struct Summary
        {
            std::size_t targetRegions = 0;
            std::size_t patchedRegions = 0;
            std::size_t missingSourceRegions = 0;
            std::size_t missingWeatherData = 0;
            std::size_t sourceEntries = 0;
            std::size_t copiedEntries = 0;
            std::size_t omittedEntries = 0;
        };

        bool EqualsIgnoreCase(
            const std::string_view a_left,
            const std::string_view a_right)
        {
            return std::ranges::equal(
                a_left,
                a_right,
                [](const char a, const char b)
                {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
        }

        bool StartsWithIgnoreCase(
            const std::string_view a_text,
            const std::string_view a_prefix)
        {
            return a_text.size() >= a_prefix.size() &&
                   EqualsIgnoreCase(
                       a_text.substr(0, a_prefix.size()),
                       a_prefix);
        }

        std::string EditorID(
            API::MMSF::Interface* a_mmsf,
            const RE::TESForm* a_form)
        {
            return a_mmsf && a_form ?
                       a_mmsf->LookupEDIDForFormID(a_form->formID) :
                       std::string{};
        }

        template <class T>
        T* LookupForm(API::MMSF::Interface* a_mmsf, const std::string_view a_editorID)
        {
            if (!a_mmsf || a_editorID.empty())
            {
                return nullptr;
            }
            if (auto* cached =
                    a_mmsf->LookupCachedForm(std::string(a_editorID)))
            {
                return cached->As<T>();
            }
            const auto formID =
                a_mmsf->LookupFormIDForEDID(std::string(a_editorID));
            return formID ? RE::TESForm::LookupByID<T>(formID) : nullptr;
        }

        RE::TESRegionDataWeather* WeatherData(RE::TESRegion* a_region)
        {
            if (!a_region || !a_region->dataList)
            {
                return nullptr;
            }
            for (auto* data : a_region->dataList->regionDataList)
            {
                if (data &&
                    data->GetType() == RE::TESRegionData::Type::kWeather)
                {
                    return static_cast<RE::TESRegionDataWeather*>(data);
                }
            }
            return nullptr;
        }

        void ClearWeatherTypes(RE::TESRegionDataWeather* a_data)
        {
            for (auto* weatherType : a_data->weatherTypes)
            {
                RE::free(weatherType);
            }
            a_data->weatherTypes.clear();
        }

        RE::WeatherType* MakeWeatherType(const MappedWeather& a_mapped)
        {
            auto* result = RE::calloc<RE::WeatherType>(1);
            if (result)
            {
                result->weather = a_mapped.weather;
                result->chance = a_mapped.chance;
                result->global = a_mapped.global;
            }
            return result;
        }

        const RE::TESFile* LoadedPlugin(
            RE::TESDataHandler* a_dataHandler,
            const std::string_view a_name)
        {
            if (!a_dataHandler || a_name.empty())
            {
                return nullptr;
            }
            if (const auto* plugin =
                    a_dataHandler->LookupLoadedModByName(a_name))
            {
                return plugin;
            }
            return a_dataHandler->LookupLoadedLightModByName(a_name);
        }

        bool IsTargetRegion(
            const RE::TESRegion* a_region,
            const std::unordered_set<const RE::TESFile*>& a_plugins)
        {
            const auto* origin = a_region ? a_region->GetFile(0) : nullptr;
            return origin && a_plugins.contains(origin);
        }

        template <class... Args>
        void LogDetailedWarning(
            const bool a_enabled,
            const std::string_view a_profile,
            std::format_string<Args...> a_format,
            Args&&... a_args)
        {
            if (a_enabled)
            {
                logger::warn(
                    "[Region Weather] [{}] {}",
                    a_profile,
                    std::format(
                        a_format,
                        std::forward<Args>(a_args)...));
            }
        }
    }  // namespace

    void Apply(
        const Settings& a_settings,
        const std::string_view a_profile,
        const std::string_view a_weatherPrefix,
        const std::string_view a_regionPrefix,
        API::MMSF::Interface* a_mmsf,
        const bool a_detailedLogging)
    {
        if (!a_settings.enabled)
        {
            return;
        }

        Summary summary;
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler || !a_mmsf)
        {
            logger::error(
                "[Region Weather] [{}] startup failed | TESDataHandler/MMSF unavailable",
                a_profile);
            return;
        }
        if (a_weatherPrefix.empty() || a_regionPrefix.empty())
        {
            logger::warn(
                "[Region Weather] [{}] startup skipped | weatherPrefix/regionPrefix missing",
                a_profile);
            return;
        }

        std::unordered_set<const RE::TESFile*> targetPlugins;
        for (const auto& pluginName : a_settings.plugins)
        {
            const auto* plugin = LoadedPlugin(dataHandler, pluginName);
            if (plugin)
            {
                targetPlugins.insert(plugin);
            }
        }
        if (targetPlugins.empty())
        {
            logger::info(
                "[Region Weather] [{}] status=skipped | reason=no-target-plugin",
                a_profile);
            return;
        }

        for (auto* targetRegion :
            dataHandler->GetFormArray<RE::TESRegion>())
        {
            if (!IsTargetRegion(targetRegion, targetPlugins))
            {
                continue;
            }
            const auto targetEditorID = EditorID(a_mmsf, targetRegion);
            if (!StartsWithIgnoreCase(targetEditorID, a_regionPrefix))
            {
                continue;
            }
            ++summary.targetRegions;

            const std::string sourceEditorID{
                targetEditorID.substr(a_regionPrefix.size())
            };
            auto* sourceRegion =
                LookupForm<RE::TESRegion>(a_mmsf, sourceEditorID);
            if (!sourceRegion || sourceRegion == targetRegion)
            {
                ++summary.missingSourceRegions;
                LogDetailedWarning(
                    a_detailedLogging,
                    a_profile,
                    "'{}' skipped | source='{}' missing",
                    targetEditorID,
                    sourceEditorID);
                continue;
            }

            auto* sourceData = WeatherData(sourceRegion);
            auto* targetData = WeatherData(targetRegion);
            if (!sourceData || !targetData)
            {
                ++summary.missingWeatherData;
                LogDetailedWarning(
                    a_detailedLogging,
                    a_profile,
                    "'{}' skipped | RDWT={} missing",
                    targetEditorID,
                    !sourceData ? "the winning source region" :
                                  "the target region");
                continue;
            }

            std::vector<MappedWeather> mapped;
            for (auto* sourceType : sourceData->weatherTypes)
            {
                if (!sourceType || !sourceType->weather)
                {
                    continue;
                }
                ++summary.sourceEntries;
                auto sourceWeatherEditorID =
                    EditorID(a_mmsf, sourceType->weather);
                if (sourceWeatherEditorID.empty())
                {
                    ++summary.omittedEntries;
                    LogDetailedWarning(
                        a_detailedLogging,
                        a_profile,
                        "'{}' omitted {:08X} | source EditorID missing",
                        targetEditorID,
                        sourceType->weather->formID);
                    continue;
                }

                const auto targetWeatherEditorID =
                    std::string(a_weatherPrefix) + sourceWeatherEditorID;
                auto* targetWeather =
                    LookupForm<RE::TESWeather>(
                        a_mmsf,
                        targetWeatherEditorID);
                if (!targetWeather)
                {
                    ++summary.omittedEntries;
                    if (a_detailedLogging)
                    {
                        logger::warn(
                            "[Region Weather] [{}] '{}' omitted '{}' paired weather missing",
                            a_profile,
                            targetEditorID,
                            sourceWeatherEditorID);
                    }
                    continue;
                }

                mapped.push_back({
                    .weather = targetWeather,
                    .global = sourceType->global,
                    .chance = sourceType->chance,
                    .sourceEditorID = std::move(sourceWeatherEditorID),
                    .targetEditorID = targetWeatherEditorID,
                });
            }

            if (mapped.empty())
            {
                LogDetailedWarning(
                    a_detailedLogging,
                    a_profile,
                    "'{}' unchanged | matching=0",
                    targetEditorID);
                continue;
            }

            std::vector<RE::WeatherType*> replacement;
            replacement.reserve(mapped.size());
            bool allocationFailed = false;
            for (const auto& entry : mapped)
            {
                auto* weatherType = MakeWeatherType(entry);
                if (!weatherType)
                {
                    allocationFailed = true;
                    break;
                }
                replacement.push_back(weatherType);
            }
            if (allocationFailed)
            {
                for (auto* weatherType : replacement)
                {
                    RE::free(weatherType);
                }
                logger::error(
                    "[Region Weather] [{}] '{}' unchanged | allocation failed",
                    a_profile,
                    targetEditorID);
                continue;
            }

            ClearWeatherTypes(targetData);
            for (auto iterator = replacement.rbegin();
                iterator != replacement.rend();
                ++iterator)
            {
                targetData->weatherTypes.push_front(*iterator);
            }
            ++summary.patchedRegions;
            summary.copiedEntries += mapped.size();

        }

        logger::info(
            "[Region Weather] [{}] targets={} | patched={} | sourceMissing={} | RDWTMissing={} | entries={} copied/{} omitted",
            a_profile,
            summary.targetRegions,
            summary.patchedRegions,
            summary.missingSourceRegions,
            summary.missingWeatherData,
            summary.sourceEntries,
            summary.copiedEntries,
            summary.omittedEntries);
    }
}  // namespace MPL::RegionWeatherPatcher
