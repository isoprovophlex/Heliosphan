#include <LightPlacer.h>

#include <CellClassifier.h>
#include <FormResolver.h>
#include <Heliosphan.h>
#include <HeliosphanLogic.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace MPL::LightPlacer
{
    namespace
    {
        using FormSet = std::unordered_set<RE::FormID>;
        using PlacementList = std::vector<const SourcePlacement*>;
        using Json = nlohmann::json;

        constexpr std::string_view kLogPrefix = "[Window Sync] Light Placer";
        const std::filesystem::path kConfigDirectory = R"(Data\LightPlacer)";
        constexpr std::size_t kMaxTransformerIDLength = 128;
        constexpr std::size_t kMaxTransformedJsonSize =
            64ULL * 1024ULL * 1024ULL;
        struct RegisteredTransformer
        {
            std::string id;
            HeliosphanAPI::LightPlacerTransform transform = nullptr;
            void (*onReloadComplete)(bool) = nullptr;
        };

        struct ConfiguredProfile
        {
            std::string id;
            Settings settings;
            bool filtered = false;
        };

        struct ConfigurationFile
        {
            std::filesystem::path path;
            std::string original;
            Json document;
        };

        std::mutex brokerLock;
        std::vector<ConfiguredProfile> configuredProfiles;
        std::vector<PatchRule> retainedRules;
        std::vector<RegisteredTransformer> transformers;
        FormSet watchedBases;
        FormSet watchedReferences;
        bool placementFilterPrepared = false;
        bool placementFilterComplete = true;
        bool startupRulesReady = false;
        bool deferredReload = false;
        std::once_flag configurationFilesOnce;
        std::shared_ptr<const std::vector<ConfigurationFile>>
            configurationFiles;
        bool configurationFilesComplete = true;
        std::atomic<std::uint64_t> reloadGeneration{ 0 };

        struct PatchStats
        {
            std::size_t filesScanned = 0;
            std::size_t filesPatched = 0;
            std::size_t filesTransformed = 0;
            std::size_t lightEntriesPatched = 0;
            std::size_t lightEntriesSkippedForMalformedFilters = 0;
            std::size_t referencesPartitioned = 0;
            std::size_t whitelistFallbackSources = 0;
            std::size_t transformerFailures = 0;
            std::size_t fileFailures = 0;
        };

        bool IEquals(const std::string_view a_lhs, const std::string_view a_rhs)
        {
            return a_lhs.size() == a_rhs.size() &&
                   _strnicmp(a_lhs.data(), a_rhs.data(), a_lhs.size()) == 0;
        }

        bool IsJsonFile(const std::filesystem::path& a_path)
        {
            return IEquals(a_path.extension().string(), ".json");
        }

        std::string NormalizeIdentifier(const std::string_view a_value)
        {
            std::string result(a_value);
            std::ranges::transform(result, result.begin(), [](const unsigned char a_character) {
                return static_cast<char>(std::tolower(a_character));
            });
            return result;
        }

        std::string DescribeForms(const FormSet& a_forms)
        {
            std::vector<std::string> forms;
            forms.reserve(a_forms.size());
            for (const auto form : a_forms)
            {
                forms.push_back(StableFormKey(form));
            }
            std::ranges::sort(forms);

            std::string result = "[";
            for (std::size_t index = 0; index < forms.size(); ++index)
            {
                if (index > 0)
                {
                    result += ", ";
                }
                result += forms[index];
            }
            result += "]";
            return result;
        }

        bool SourceUsesConfiguredLight(
            const Json& a_source,
            const std::unordered_set<std::string>& a_lights)
        {
            const auto lights = a_source.find("lights");
            if (lights == a_source.end() || !lights->is_array())
            {
                return false;
            }
            return std::ranges::any_of(
                *lights,
                [&](const Json& a_light)
                {
                    const auto data = a_light.is_object() ?
                                          a_light.find("data") :
                                          a_light.end();
                    const auto light =
                        data != a_light.end() && data->is_object() ?
                            data->find("light") :
                            data->end();
                    return light != data->end() && light->is_string() &&
                           a_lights.contains(NormalizeIdentifier(
                               light->get_ref<const std::string&>()));
                });
        }

        std::string ReadFile(const std::filesystem::path& a_path)
        {
            std::ifstream stream(a_path, std::ios::binary);
            return stream ?
                       std::string(
                           std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()) :
                       std::string{};
        }

        bool WriteFile(
            const std::filesystem::path& a_path,
            const std::string_view a_content)
        {
            std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return false;
            }
            stream.write(
                a_content.data(),
                static_cast<std::streamsize>(a_content.size()));
            stream.flush();
            return static_cast<bool>(stream);
        }

        std::vector<ConfigurationFile> ReadConfigurationFiles()
        {
            std::vector<ConfigurationFile> files;
            std::error_code error;
            if (!std::filesystem::exists(kConfigDirectory, error))
            {
                return files;
            }
            for (std::filesystem::recursive_directory_iterator iterator(
                     kConfigDirectory,
                     std::filesystem::directory_options::skip_permission_denied,
                     error),
                 end;
                 iterator != end && !error;
                 iterator.increment(error))
            {
                const auto& entry = *iterator;
                if (!entry.is_regular_file(error) ||
                    !IsJsonFile(entry.path()))
                {
                    continue;
                }
                auto original = ReadFile(entry.path());
                files.push_back(ConfigurationFile{
                    .path = entry.path(),
                    .original = std::move(original),
                    .document = Json{},
                });
                auto& file = files.back();
                file.document = Json::parse(
                    file.original,
                    nullptr,
                    false);
            }
            if (error)
            {
                configurationFilesComplete = false;
                logger::error(
                    "{} configuration scan stopped | {}",
                    kLogPrefix,
                    error.message());
            }
            return files;
        }

        PlacementList PlacementsForSource(
            const Json& a_source,
            const PatchRule& a_rule,
            bool& a_usedWhitelistFallback)
        {
            a_usedWhitelistFallback = false;
            FormSet sourceForms;
            if (const auto formIDs = a_source.find("formIDs");
                formIDs != a_source.end() && formIDs->is_array())
            {
                for (const auto& selector : *formIDs)
                {
                    if (selector.is_string())
                    {
                        const auto formID = FormResolver::Resolve(
                            selector.get_ref<const std::string&>());
                        if (formID)
                        {
                            sourceForms.insert(formID);
                        }
                    }
                }
            }

            std::unordered_set<std::string> sourceModels;
            if (const auto models = a_source.find("models");
                models != a_source.end() && models->is_array())
            {
                for (const auto& model : *models)
                {
                    if (model.is_string())
                    {
                        auto path = NormalizeModelPath(
                            model.get_ref<const std::string&>());
                        if (!path.empty())
                        {
                            sourceModels.insert(std::move(path));
                        }
                    }
                }
            }

            std::unordered_set<std::size_t> matches;
            for (const auto form : sourceForms)
            {
                if (const auto found = a_rule.placementsByBase.find(form);
                    found != a_rule.placementsByBase.end())
                {
                    matches.insert(found->second.begin(), found->second.end());
                }
            }
            for (const auto& model : sourceModels)
            {
                if (const auto found = a_rule.placementsByModel.find(model);
                    found != a_rule.placementsByModel.end())
                {
                    matches.insert(found->second.begin(), found->second.end());
                }
            }

            std::unordered_set<std::size_t> fallbackMatches;
            if (matches.empty() && a_rule.filtered && !sourceForms.empty())
            {
                if (const auto lights = a_source.find("lights");
                    lights != a_source.end() && lights->is_array())
                {
                    for (const auto& lightEntry : *lights)
                    {
                        if (!lightEntry.is_object())
                        {
                            continue;
                        }
                        const auto data = lightEntry.find("data");
                        const auto light =
                            data != lightEntry.end() && data->is_object() ?
                                data->find("light") :
                                data->end();
                        if (light == data->end() || !light->is_string() ||
                            !a_rule.lights.contains(NormalizeIdentifier(
                                light->get_ref<const std::string&>())))
                        {
                            continue;
                        }
                        const auto whiteList = lightEntry.find("whiteList");
                        if (whiteList == lightEntry.end() ||
                            !whiteList->is_array())
                        {
                            continue;
                        }
                        for (const auto& selector : *whiteList)
                        {
                            if (!selector.is_string())
                            {
                                continue;
                            }
                            const auto reference = FormResolver::Resolve(
                                selector.get_ref<const std::string&>());
                            if (const auto found =
                                    a_rule.placementsByReference.find(reference);
                                reference &&
                                found != a_rule.placementsByReference.end())
                            {
                                fallbackMatches.insert(
                                    found->second.begin(),
                                    found->second.end());
                            }
                        }
                    }
                }
            }
            if (HeliosphanLogic::ShouldUseLightPlacerReferenceFallback(
                    a_rule.filtered,
                    !sourceForms.empty(),
                    !matches.empty(),
                    !fallbackMatches.empty()))
            {
                matches = std::move(fallbackMatches);
                a_usedWhitelistFallback = true;
            }

            PlacementList placements;
            placements.reserve(matches.size());
            for (const auto index : matches)
            {
                if (index < a_rule.placements.size())
                {
                    placements.push_back(
                        std::addressof(a_rule.placements[index]));
                }
            }
            return placements;
        }

        bool HasMalformedFilters(const Json& a_light)
        {
            for (const auto key : { "whiteList", "blackList" })
            {
                const auto found = a_light.find(key);
                if (found != a_light.end() && !found->is_array())
                {
                    return true;
                }
            }
            return false;
        }

        FormSet ResolveFilters(
            const Json& a_light,
            const std::string_view a_key)
        {
            FormSet filters;
            if (const auto list = a_light.find(a_key);
                list != a_light.end())
            {
                for (const auto& selector : *list)
                {
                    if (selector.is_string())
                    {
                        const auto form = FormResolver::Resolve(
                            selector.get_ref<const std::string&>());
                        if (form)
                        {
                            filters.insert(form);
                        }
                    }
                }
            }
            return filters;
        }

        PlacementList ApplyExistingFilters(
            const Json& a_light,
            const PlacementList& a_placements)
        {
            const bool hasWhiteList = a_light.contains("whiteList");
            const auto whiteList = ResolveFilters(a_light, "whiteList");
            const auto blackList = ResolveFilters(a_light, "blackList");

            PlacementList placements;
            for (const auto* placement : a_placements)
            {
                if (!placement)
                {
                    continue;
                }
                const auto matches = [&](const FormSet& a_filters) {
                    return std::ranges::any_of(
                        placement->filterIDs,
                        [&](const RE::FormID a_form) {
                            return a_filters.contains(a_form);
                        });
                };
                if (!matches(blackList) &&
                    (!hasWhiteList || matches(whiteList)))
                {
                    placements.push_back(placement);
                }
            }
            return placements;
        }

        FormSet PlacementReferences(const PlacementList& a_placements)
        {
            FormSet references;
            for (const auto* placement : a_placements)
            {
                if (placement)
                {
                    references.insert(placement->reference);
                }
            }
            return references;
        }

        void AppendFormKeys(Json& a_array, const FormSet& a_forms)
        {
            if (!a_array.is_array())
            {
                a_array = Json::array();
            }
            std::unordered_set<std::string> existing;
            for (const auto& value : a_array)
            {
                if (value.is_string())
                {
                    auto key = value.get<std::string>();
                    std::ranges::transform(key, key.begin(), [](const unsigned char a_character) {
                        return static_cast<char>(std::tolower(a_character));
                    });
                    existing.insert(std::move(key));
                }
            }
            std::vector<std::string> additions;
            additions.reserve(a_forms.size());
            for (const auto form : a_forms)
            {
                auto key = StableFormKey(form);
                auto normalized = key;
                std::ranges::transform(
                    normalized,
                    normalized.begin(),
                    [](const unsigned char a_character) {
                        return static_cast<char>(std::tolower(a_character));
                    });
                if (!key.empty() && existing.insert(std::move(normalized)).second)
                {
                    additions.push_back(std::move(key));
                }
            }
            std::ranges::sort(additions);
            for (auto& key : additions)
            {
                a_array.push_back(std::move(key));
            }
        }

        bool SameEmittance(
            const std::string_view a_existing,
            const std::string_view a_target)
        {
            if (IEquals(a_existing, a_target))
            {
                return true;
            }
            const auto existing = FormResolver::Resolve(a_existing);
            const auto target = FormResolver::Resolve(a_target);
            return existing && target && existing == target;
        }

        bool PatchSource(
            Json& a_source,
            const std::vector<PatchRule>& a_rules,
            PatchStats& a_stats)
        {
            auto lights = a_source.find("lights");
            if (lights == a_source.end() || !lights->is_array())
            {
                return false;
            }

            std::vector<PlacementList> sourcePlacements;
            sourcePlacements.reserve(a_rules.size());
            for (const auto& rule : a_rules)
            {
                bool usedWhitelistFallback = false;
                sourcePlacements.push_back(PlacementsForSource(
                    a_source,
                    rule,
                    usedWhitelistFallback));
                a_stats.whitelistFallbackSources +=
                    usedWhitelistFallback ? 1 : 0;
            }
            if (std::ranges::none_of(
                    sourcePlacements,
                    [](const PlacementList& a_placements) {
                        return !a_placements.empty();
                }))
            {
                return false;
            }

            bool changed = false;
            Json output = Json::array();
            for (const auto& inputLight : *lights)
            {
                if (!inputLight.is_object())
                {
                    output.push_back(inputLight);
                    continue;
                }
                const auto data = inputLight.find("data");
                if (data == inputLight.end() || !data->is_object())
                {
                    output.push_back(inputLight);
                    continue;
                }
                const auto light = data->find("light");
                const auto external = data->find("externalEmittance");
                if (light == data->end() || !light->is_string() ||
                    external == data->end() || !external->is_string() ||
                    external->get_ref<const std::string&>().empty())
                {
                    output.push_back(inputLight);
                    continue;
                }
                const auto normalizedLight = NormalizeIdentifier(
                    light->get_ref<const std::string&>());

                bool relevant = false;
                for (std::size_t index = 0; index < a_rules.size(); ++index)
                {
                    relevant |= !sourcePlacements[index].empty() &&
                                a_rules[index].lights.contains(
                                    normalizedLight);
                }
                if (!relevant)
                {
                    output.push_back(inputLight);
                    continue;
                }
                if (HasMalformedFilters(inputLight))
                {
                    ++a_stats.lightEntriesSkippedForMalformedFilters;
                    output.push_back(inputLight);
                    continue;
                }

                Json original = inputLight;
                std::vector<Json> partitions;
                FormSet claimed;
                for (std::size_t offset = 0; offset < a_rules.size(); ++offset)
                {
                    const auto index = a_rules.size() - 1 - offset;
                    const auto& rule = a_rules[index];
                    if (!rule.lights.contains(normalizedLight))
                    {
                        continue;
                    }

                    const auto& candidates = sourcePlacements[index];
                    const auto effective =
                        ApplyExistingFilters(inputLight, candidates);
                    auto references = PlacementReferences(effective);
                    std::erase_if(references, [&](const RE::FormID a_reference) {
                        return claimed.contains(a_reference);
                    });
                    if (references.empty())
                    {
                        continue;
                    }
                    claimed.insert(references.begin(), references.end());
                    if (SameEmittance(
                            external->get_ref<const std::string&>(),
                            rule.externalEmittance))
                    {
                        continue;
                    }

                    Json partition = inputLight;
                    partition["whiteList"] = Json::array();
                    AppendFormKeys(partition["whiteList"], references);
                    partition["data"]["externalEmittance"] =
                        rule.externalEmittance;
                    AppendFormKeys(original["blackList"], references);
                    partitions.push_back(std::move(partition));
                    a_stats.referencesPartitioned += references.size();
                }

                if (partitions.empty())
                {
                    output.push_back(inputLight);
                    continue;
                }
                changed = true;
                ++a_stats.lightEntriesPatched;
                output.push_back(std::move(original));
                for (auto& partition : partitions)
                {
                    output.push_back(std::move(partition));
                }
            }
            if (changed)
            {
                *lights = std::move(output);
            }
            return changed;
        }

        struct TransformOutput
        {
            std::string value;
            bool written = false;
            bool invalid = false;
        };

        bool StoreTransformOutput(
            void* a_context,
            const char* a_data,
            const std::size_t a_size)
        {
            auto* output = static_cast<TransformOutput*>(a_context);
            if (!output)
            {
                return false;
            }
            if (output->written || (!a_data && a_size != 0) ||
                a_size > kMaxTransformedJsonSize)
            {
                output->invalid = true;
                return false;
            }
            try
            {
                output->value.assign(
                    a_data ? a_data : "",
                    a_size);
                output->written = true;
                return true;
            }
            catch (...)
            {
                output->invalid = true;
                return false;
            }
        }

        bool ApplyTransformers(
            const std::filesystem::path& a_path,
            std::string& a_document,
            const std::vector<RegisteredTransformer>& a_transformers,
            PatchStats& a_stats)
        {
            bool changed = false;
            const auto path = a_path.string();
            for (const auto& transformer : a_transformers)
            {
                TransformOutput output;
                bool transformed = false;
                try
                {
                    transformed = transformer.transform(
                        path.c_str(),
                        a_document.data(),
                        a_document.size(),
                        StoreTransformOutput,
                        std::addressof(output));
                }
                catch (const std::exception& error)
                {
                    logger::error(
                    "{} transformer={} failed | file='{}' | {}",
                        kLogPrefix,
                        transformer.id,
                        path,
                        error.what());
                }
                catch (...)
                {
                    logger::error(
                    "{} transformer={} failed | file='{}' | unknown exception",
                        kLogPrefix,
                        transformer.id,
                        path);
                }
                if (!transformed || !output.written || output.invalid)
                {
                    ++a_stats.transformerFailures;
                    logger::error(
                    "{} transformer={} failed | file='{}' | changes ignored",
                        kLogPrefix,
                        transformer.id,
                        path);
                    continue;
                }
                if (output.value == a_document)
                {
                    continue;
                }
                const auto validation =
                    Json::parse(output.value, nullptr, false);
                if (validation.is_discarded() || !validation.is_array())
                {
                    ++a_stats.transformerFailures;
                    logger::error(
                    "{} transformer={} invalid JSON | file='{}' | changes ignored",
                        kLogPrefix,
                        transformer.id,
                        path);
                    continue;
                }
                a_document = std::move(output.value);
                changed = true;
            }
            if (changed)
            {
                ++a_stats.filesTransformed;
            }
            return changed;
        }

        void EditConfigs(
            const std::vector<PatchRule>& a_rules,
            const std::vector<RegisteredTransformer>& a_transformers,
            PatchStats& a_stats,
            std::unordered_map<std::string, std::string>& a_backups)
        {
            InitializeConfigurationFiles();
            const auto files = configurationFiles;
            if (!files || files->empty())
            {
                return;
            }

            for (const auto& file : *files)
            {
                ++a_stats.filesScanned;
                const auto& original = file.original;
                if (original.empty())
                {
                    continue;
                }
                if (ReadFile(file.path) != original)
                {
                    ++a_stats.fileFailures;
                    logger::warn(
                "{} file='{}' skipped | changed after snapshot",
                        kLogPrefix,
                        file.path.string());
                    continue;
                }

                std::string transformed = original;
                bool changed = false;
                if (!a_rules.empty() &&
                    original.find("externalEmittance") != std::string::npos)
                {
                    Json document = file.document;
                    if (document.is_discarded() || !document.is_array())
                    {
                        logger::warn(
                "{} parse failed | file='{}' | partition unchanged",
                            kLogPrefix,
                            file.path.string());
                    }
                    else
                    {
                        for (auto& source : document)
                        {
                            if (source.is_object())
                            {
                                changed |= PatchSource(
                                    source,
                                    a_rules,
                                    a_stats);
                            }
                        }
                        if (changed)
                        {
                            transformed = document.dump(4);
                        }
                    }
                }

                changed |= ApplyTransformers(
                    file.path,
                    transformed,
                    a_transformers,
                    a_stats);
                if (!changed)
                {
                    continue;
                }

                const auto path = file.path.string();
                if (WriteFile(file.path, transformed))
                {
                    a_backups.emplace(path, original);
                    ++a_stats.filesPatched;
                }
                else
                {
                    ++a_stats.fileFailures;
                    logger::error(
                    "{} temporary write failed | file='{}'",
                        kLogPrefix,
                        path);
                }
            }
        }

        bool RestoreConfigs(
            const std::unordered_map<std::string, std::string>& a_backups)
        {
            bool restored = true;
            for (const auto& [path, original] : a_backups)
            {
                const std::filesystem::path originalPath(path);
                if (!WriteFile(originalPath, original))
                {
                    restored = false;
                    logger::error(
                    "{} restore failed | file='{}'",
                        kLogPrefix,
                        path);
                }
            }
            return restored;
        }

        bool RunReloadCommand()
        {
            const char* command = nullptr;
            if (RE::SCRIPT_FUNCTION::LocateConsoleCommand("ReloadLP"))
            {
                command = "ReloadLP";
            }
            else if (RE::SCRIPT_FUNCTION::LocateConsoleCommand("lpreload"))
            {
                command = "lpreload";
            }
            if (!command)
            {
                logger::error(
                "{} reload failed | command unavailable",
                    kLogPrefix);
                return false;
            }
            auto* script = RE::IFormFactory::Create<RE::Script>();
            if (!script)
            {
                return false;
            }
            script->SetCommand(command);
            script->CompileAndRun(nullptr);
            delete script;
            return true;
        }

        void NotifyReloadComplete(
            const std::vector<RegisteredTransformer>& a_transformers,
            const bool a_succeeded)
        {
            for (const auto& transformer : a_transformers)
            {
                if (transformer.onReloadComplete)
                {
                    try
                    {
                        transformer.onReloadComplete(a_succeeded);
                    }
                    catch (const std::exception& error)
                    {
                        logger::error(
                    "{} transformer={} completion failed | {}",
                            kLogPrefix,
                            transformer.id,
                            error.what());
                    }
                    catch (...)
                    {
                        logger::error(
                    "{} transformer={} completion failed | unknown exception",
                            kLogPrefix,
                            transformer.id);
                    }
                }
            }
        }

        void ApplyAndReload(
            const std::vector<PatchRule>& a_rules,
            const std::vector<RegisteredTransformer>& a_transformers)
        {
            InitializeConfigurationFiles();
            if (!configurationFiles || configurationFiles->empty())
            {
                NotifyReloadComplete(a_transformers, true);
                return;
            }
            PatchStats stats;
            std::unordered_map<std::string, std::string> backups;
            bool succeeded = false;
            try
            {
                EditConfigs(
                    a_rules,
                    a_transformers,
                    stats,
                    backups);
                if (backups.empty())
                {
                    succeeded = stats.transformerFailures == 0 &&
                                stats.fileFailures == 0;
                    if (stats.filesScanned != 0)
                    {
                        logger::info(
                            "{} scan | files={} | eligible=0",
                            kLogPrefix,
                            stats.filesScanned);
                    }
                    NotifyReloadComplete(a_transformers, succeeded);
                    return;
                }

                const bool reloaded = RunReloadCommand();
                const bool restored = RestoreConfigs(backups);
                backups.clear();
                succeeded = reloaded && restored &&
                            stats.transformerFailures == 0 &&
                            stats.fileFailures == 0;
                logger::info(
                    "{} patch | files={}/{} | transformed={} | definitions={} | references={} | whitelistFallbacks={} | malformed={} | transformerFailures={} | reload={} | restored={}",
                    kLogPrefix,
                    stats.filesPatched,
                    stats.filesScanned,
                    stats.filesTransformed,
                    stats.lightEntriesPatched,
                    stats.referencesPartitioned,
                    stats.whitelistFallbackSources,
                    stats.lightEntriesSkippedForMalformedFilters,
                    stats.transformerFailures,
                    reloaded,
                    restored);
            }
            catch (const std::exception& error)
            {
                RestoreConfigs(backups);
                logger::error(
                "{} broker failed | restored={} | {}",
                    kLogPrefix,
                    backups.size(),
                    error.what());
            }
            catch (...)
            {
                RestoreConfigs(backups);
                logger::error(
                "{} broker failed | restored={} | unknown exception",
                    kLogPrefix,
                    backups.size());
            }
            NotifyReloadComplete(a_transformers, succeeded);
        }

        void QueueReload()
        {
            const auto generation = ++reloadGeneration;
            if (const auto* tasks = SKSE::GetTaskInterface())
            {
                tasks->AddTask([generation]() {
                    if (generation !=
                        reloadGeneration.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    std::vector<PatchRule> rules;
                    std::vector<RegisteredTransformer> callbacks;
                    {
                        std::scoped_lock lock(brokerLock);
                        rules = retainedRules;
                        callbacks = transformers;
                    }
                    ApplyAndReload(rules, callbacks);
                });
            }
            else
            {
                logger::error(
                "{} queue failed | SKSE task interface unavailable",
                    kLogPrefix);
            }
        }
    }  // namespace

    std::string NormalizeModelPath(std::string_view a_path)
    {
        std::string result(a_path);
        std::ranges::replace(result, '/', '\\');
        std::ranges::transform(
            result,
            result.begin(),
            [](const unsigned char a_character) {
                return static_cast<char>(std::tolower(a_character));
            });
        constexpr std::array prefixes{
            std::string_view{ "data\\meshes\\" },
            std::string_view{ "meshes\\" },
        };
        for (const auto prefix : prefixes)
        {
            if (result.starts_with(prefix))
            {
                result.erase(0, prefix.size());
                break;
            }
        }
        return result;
    }

    std::string StableFormKey(const RE::FormID a_formID)
    {
        if (!a_formID)
        {
            return {};
        }

        auto* dataHandler =
            RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            return std::format("0x{:X}", a_formID);
        }

        const bool isLightForm =
            (a_formID & 0xFF000000) == 0xFE000000;
        const auto fullIndex =
            static_cast<std::uint8_t>(a_formID >> 24);
        if (!isLightForm && fullIndex == 0xFF)
        {
            return std::format("0x{:X}", a_formID);
        }
        const auto* file =
            isLightForm ?
                dataHandler->LookupLoadedLightModByIndex(
                    static_cast<std::uint16_t>(
                        (a_formID & 0x00FFF000) >> 12)) :
                dataHandler->LookupLoadedModByIndex(
                    fullIndex);
        if (!file)
        {
            return std::format("0x{:X}", a_formID);
        }

        const RE::FormID localFormID =
            isLightForm ?
                a_formID & 0x00000FFF :
                a_formID & 0x00FFFFFF;
        return std::format(
            "0x{:X}~{}",
            localFormID,
            file->GetFilename());
    }

    void ClearProfiles()
    {
        std::scoped_lock lock(brokerLock);
        configuredProfiles.clear();
        retainedRules.clear();
        watchedBases.clear();
        watchedReferences.clear();
        placementFilterPrepared = false;
        placementFilterComplete = true;
        startupRulesReady = false;
        deferredReload = false;
        ++reloadGeneration;
    }

    void AddProfile(
        std::string a_id,
        Settings a_settings,
        const bool a_filtered)
    {
        if (a_id.empty() || a_settings.lights.empty() ||
            a_settings.externalEmittance.empty())
        {
            return;
        }
        std::unordered_set<std::string> normalizedLights;
        for (const auto& light : a_settings.lights)
        {
            auto normalized = NormalizeIdentifier(light);
            if (!normalized.empty())
            {
                normalizedLights.insert(std::move(normalized));
            }
        }
        a_settings.lights.assign(
            normalizedLights.begin(),
            normalizedLights.end());
        if (a_settings.lights.empty())
        {
            return;
        }
        std::scoped_lock lock(brokerLock);
        configuredProfiles.push_back(ConfiguredProfile{
            .id = std::move(a_id),
            .settings = std::move(a_settings),
            .filtered = a_filtered,
        });
    }

    bool RequiresPluginIndex()
    {
        std::scoped_lock lock(brokerLock);
        return !configuredProfiles.empty() && configurationFiles &&
               !configurationFiles->empty();
    }

    bool RequiresCompleteInteriorIndex()
    {
        std::scoped_lock lock(brokerLock);
        return configurationFiles && !configurationFiles->empty() &&
               std::ranges::any_of(
            configuredProfiles,
            [](const ConfiguredProfile& a_profile)
            {
                return !a_profile.filtered;
            });
    }

    void InitializeConfigurationFiles()
    {
        std::call_once(
            configurationFilesOnce,
            []
            {
                auto files = ReadConfigurationFiles();
                const auto malformed = std::ranges::count_if(
                    files,
                    [](const ConfigurationFile& a_file)
                    {
                        return a_file.document.is_discarded() ||
                               !a_file.document.is_array();
                    });
                configurationFiles =
                    std::make_shared<const std::vector<ConfigurationFile>>(
                        std::move(files));
                if (!configurationFiles->empty() || malformed != 0)
                {
                    logger::info(
                        "{} snapshot | files={} | malformed={}",
                        kLogPrefix,
                        configurationFiles->size(),
                        malformed);
                }
            });
    }

    void PreparePlacementFilter()
    {
        if (placementFilterPrepared)
        {
            return;
        }
        InitializeConfigurationFiles();
        std::unordered_set<std::string> configuredLights;
        std::unordered_set<std::string> filteredLights;
        {
            std::scoped_lock lock(brokerLock);
            for (const auto& profile : configuredProfiles)
            {
                configuredLights.insert(
                    profile.settings.lights.begin(),
                    profile.settings.lights.end());
                if (profile.filtered)
                {
                    filteredLights.insert(
                        profile.settings.lights.begin(),
                        profile.settings.lights.end());
                }
            }
        }

        FormSet formSources;
        FormSet referenceSources;
        std::unordered_set<std::string> modelSources;
        std::size_t filesScanned = 0;
        std::size_t malformedFiles = 0;
        if (!configuredLights.empty() && configurationFiles)
        {
            for (const auto& file : *configurationFiles)
            {
                ++filesScanned;
                const auto& document = file.document;
                if (document.is_discarded() || !document.is_array())
                {
                    ++malformedFiles;
                    continue;
                }
                for (const auto& source : document)
                {
                    if (!source.is_object() ||
                        !SourceUsesConfiguredLight(source, configuredLights))
                    {
                        continue;
                    }
                    bool hasResolvedFormSource = false;
                    if (const auto formIDs = source.find("formIDs");
                        formIDs != source.end() && formIDs->is_array())
                    {
                        for (const auto& selector : *formIDs)
                        {
                            if (selector.is_string())
                            {
                                const auto formID = FormResolver::Resolve(
                                    selector.get_ref<const std::string&>());
                                const auto* form = formID ?
                                                       RE::TESForm::LookupByID(formID) :
                                                       nullptr;
                                if (form && form->Is(RE::FormType::Static))
                                {
                                    formSources.insert(formID);
                                    hasResolvedFormSource = true;
                                }
                            }
                        }
                    }
                    if (const auto models = source.find("models");
                        models != source.end() && models->is_array())
                    {
                        for (const auto& model : *models)
                        {
                            if (model.is_string())
                            {
                                auto normalized = NormalizeModelPath(
                                    model.get_ref<const std::string&>());
                                if (!normalized.empty())
                                {
                                    modelSources.insert(std::move(normalized));
                                }
                            }
                        }
                    }
                    if (hasResolvedFormSource && !filteredLights.empty())
                    {
                        const auto lights = source.find("lights");
                        if (lights != source.end() && lights->is_array())
                        {
                            for (const auto& lightEntry : *lights)
                            {
                                if (!lightEntry.is_object())
                                {
                                    continue;
                                }
                                const auto data = lightEntry.find("data");
                                const auto light =
                                    data != lightEntry.end() &&
                                            data->is_object() ?
                                        data->find("light") :
                                        data->end();
                                if (light == data->end() ||
                                    !light->is_string() ||
                                    !filteredLights.contains(
                                        NormalizeIdentifier(
                                            light->get_ref<
                                                const std::string&>())))
                                {
                                    continue;
                                }
                                const auto whiteList =
                                    lightEntry.find("whiteList");
                                if (whiteList == lightEntry.end() ||
                                    !whiteList->is_array())
                                {
                                    continue;
                                }
                                for (const auto& selector : *whiteList)
                                {
                                    if (selector.is_string())
                                    {
                                        const auto reference =
                                            FormResolver::Resolve(
                                                selector.get_ref<
                                                    const std::string&>());
                                        if (reference)
                                        {
                                            referenceSources.insert(reference);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        placementFilterComplete = configurationFilesComplete;
        const auto sourceFormCount = formSources.size();
        watchedBases = std::move(formSources);
        watchedReferences = std::move(referenceSources);
        if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
        {
            for (const auto* base :
                 dataHandler->GetFormArray<RE::TESObjectSTAT>())
            {
                const auto* model = base ? base->As<RE::TESModel>() : nullptr;
                if (base && model && modelSources.contains(
                        NormalizeModelPath(model->GetModel())))
                {
                    watchedBases.insert(base->GetFormID());
                }
            }
        }
        placementFilterPrepared = true;
        if (filesScanned != 0 || malformedFiles != 0)
        {
            logger::info(
                "{} filter | files={} | malformed={} | forms={} | models={} | bases={} | references={} | complete={}",
                kLogPrefix,
                filesScanned,
                malformedFiles,
                sourceFormCount,
                modelSources.size(),
                watchedBases.size(),
                watchedReferences.size(),
                placementFilterComplete);
        }
    }

    bool NeedsPlacement(
        const RE::FormID a_reference,
        const RE::FormID a_base)
    {
        if (!placementFilterPrepared)
        {
            return false;
        }
        if (a_reference && watchedReferences.contains(a_reference))
        {
            return true;
        }
        if (!placementFilterComplete)
        {
            const auto* form = a_base ?
                                   RE::TESForm::LookupByID(a_base) :
                                   nullptr;
            return form && form->Is(RE::FormType::Static);
        }
        return CellClassifier::CouldProjectToAny(
            a_reference,
            a_base,
            watchedBases);
    }

    void Prepare(const PluginIndex::Result& a_index)
    {
        std::vector<ConfiguredProfile> profiles;
        {
            std::scoped_lock lock(brokerLock);
            profiles = configuredProfiles;
        }
        std::vector<std::string> profileOrder;
        profileOrder.reserve(profiles.size());
        for (const auto& profile : profiles)
        {
            if (profile.filtered)
            {
                profileOrder.push_back(profile.id);
            }
        }
        Heliosphan::SortWindowSyncProfileIDs(profileOrder);
        const auto profileRank = [&](const std::string_view a_id)
        {
            const auto found = std::ranges::find_if(
                profileOrder,
                [&](const std::string& a_candidate)
                {
                    return IEquals(a_candidate, a_id);
                });
            return static_cast<std::size_t>(
                std::distance(profileOrder.begin(), found));
        };
        std::ranges::stable_sort(
            profiles,
            [&](const ConfiguredProfile& a_left,
                const ConfiguredProfile& a_right)
            {
                if (a_left.filtered != a_right.filtered)
                {
                    return !a_left.filtered;
                }
                if (!a_left.filtered)
                {
                    return false;
                }
                return profileRank(a_left.id) < profileRank(a_right.id);
            });

        std::vector<PatchRule> rules;
        rules.reserve(profiles.size());
        for (const auto& profile : profiles)
        {
            if (!FormResolver::Resolve(profile.settings.externalEmittance))
            {
                logger::error(
                    "{} {} | emittance={} unresolved | profile skipped",
                    kLogPrefix,
                    profile.id,
                    profile.settings.externalEmittance);
                continue;
            }

            PatchRule rule{
                .lights = std::unordered_set<std::string>(
                    profile.settings.lights.begin(),
                    profile.settings.lights.end()),
                .externalEmittance =
                    profile.settings.externalEmittance,
                .filtered = profile.filtered,
            };
            for (const auto& [referenceID, placement] : a_index.placements)
            {
                if (!referenceID || placement.deleted || !placement.base ||
                    !placement.cell)
                {
                    continue;
                }
                const auto& cellProfiles =
                    CellClassifier::GetProfiles(placement.cell);
                if (profile.filtered && std::ranges::none_of(
                        cellProfiles,
                        [&](const std::string& a_active)
                        {
                            return IEquals(a_active, profile.id);
                        }))
                {
                    continue;
                }

                const auto effectiveBase = CellClassifier::ProjectBase(
                    referenceID,
                    placement.base);
                const auto* base = effectiveBase ?
                                       RE::TESForm::LookupByID<
                                           RE::TESObjectSTAT>(effectiveBase) :
                                       nullptr;
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(
                    placement.cell);
                if (!base || !cell || !cell->IsInteriorCell())
                {
                    continue;
                }
                const auto* model = base->As<RE::TESModel>();
                SourcePlacement source{
                    .reference = referenceID,
                    .base = effectiveBase,
                    .cell = placement.cell,
                    .model = model ?
                                 NormalizeModelPath(model->GetModel()) :
                                 std::string{},
                    .filterIDs = {
                        placement.cell,
                        referenceID,
                        placement.base,
                        effectiveBase,
                    },
                };
                for (auto* location = cell->GetLocation();
                     location;
                     location = location->parentLoc)
                {
                    source.filterIDs.push_back(location->GetFormID());
                }
                rule.placements.push_back(std::move(source));
            }
            for (std::size_t index = 0;
                 index < rule.placements.size();
                 ++index)
            {
                const auto& placement = rule.placements[index];
                rule.placementsByReference[placement.reference].push_back(index);
                rule.placementsByBase[placement.base].push_back(index);
                if (!placement.model.empty())
                {
                    rule.placementsByModel[placement.model].push_back(index);
                }
            }
            if (!rule.placements.empty())
            {
                rules.push_back(std::move(rule));
            }
        }
        QueueStartupPatch(std::move(rules));
    }

    void QueueStartupPatch(std::vector<PatchRule> a_rules)
    {
        std::erase_if(a_rules, [](const PatchRule& a_rule) {
            return a_rule.lights.empty() ||
                   a_rule.externalEmittance.empty() ||
                   a_rule.placements.empty();
        });
        bool queue = false;
        std::size_t ruleCount = 0;
        {
            std::scoped_lock lock(brokerLock);
            retainedRules = std::move(a_rules);
            startupRulesReady = true;
            queue = !retainedRules.empty() || deferredReload;
            ruleCount = retainedRules.size();
            deferredReload = false;
        }
        if (!queue)
        {
            return;
        }
        logger::info(
            "{} reload | status=queued | rules={}",
            kLogPrefix,
            ruleCount);
        QueueReload();
    }

    bool RegisterTransformer(
        const HeliosphanAPI::LightPlacerTransformer* a_transformer)
    {
        if (!a_transformer)
        {
            return false;
        }
        const auto idLength =
            a_transformer->id ?
                strnlen_s(
                    a_transformer->id,
                    kMaxTransformerIDLength + 1) :
                0;
        if (idLength == 0 ||
            idLength > kMaxTransformerIDLength ||
            !a_transformer->TransformJson)
        {
            return false;
        }
        RegisteredTransformer value;
        try
        {
            value = {
                .id = a_transformer->id,
                .transform = a_transformer->TransformJson,
                .onReloadComplete =
                    a_transformer->OnReloadComplete,
            };
            std::scoped_lock lock(brokerLock);
            const auto existing = std::ranges::find_if(
                transformers,
                [&](const RegisteredTransformer& a_registered) {
                    return IEquals(
                        a_registered.id,
                        a_transformer->id);
                });
            if (existing != transformers.end())
            {
                if (existing->transform == value.transform &&
                    existing->onReloadComplete ==
                        value.onReloadComplete)
                {
                    return true;
                }
                logger::error(
                    "{} transformer={} rejected | duplicate ID",
                    kLogPrefix,
                    value.id);
                return false;
            }
            transformers.push_back(value);
        }
        catch (const std::exception& error)
        {
            logger::error(
                "{} transformer={} registration failed | {}",
                kLogPrefix,
                a_transformer->id,
                error.what());
            return false;
        }
        catch (...)
        {
            logger::error(
                "{} transformer={} registration failed | unknown exception",
                kLogPrefix,
                a_transformer->id);
            return false;
        }
        logger::info(
            "{} transformer={} | status=registered",
            kLogPrefix,
            value.id);
        return true;
    }

    bool RequestReload()
    {
        try
        {
            if (!SKSE::GetTaskInterface())
            {
                return false;
            }

            {
                std::scoped_lock lock(brokerLock);
                if (!startupRulesReady)
                {
                    deferredReload = true;
                    return true;
                }
            }
            QueueReload();
            return true;
        }
        catch (const std::exception& error)
        {
            logger::error(
                "{} reload request failed | {}",
                kLogPrefix,
                error.what());
        }
        catch (...)
        {
            logger::error(
                "{} reload request failed | unknown exception",
                kLogPrefix);
        }
        return false;
    }
}  // namespace MPL::LightPlacer
