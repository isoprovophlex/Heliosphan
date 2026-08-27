#include <AutoCSTonemapping.h>

#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_set>

namespace MPL::AutoCSTonemapping
{
    namespace
    {
        constexpr auto kFilmicCurveSetting = "bUseFilmicCurve:Display";
        constexpr auto kFilmicWhiteScaleSetting = "fFilmicWhiteScale:Display";
        constexpr auto kConfigurationKey = "autoCSTonemapping";
        constexpr float kWhitePoint = 0.1f;
        constexpr float kWhiteScale = 10.0f;

        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const
            {
                yyjson_doc_free(a_document);
            }
        };

        struct MutableDocumentDeleter
        {
            void operator()(yyjson_mut_doc* a_document) const
            {
                yyjson_mut_doc_free(a_document);
            }
        };

        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;
        using MutableDocument =
            std::unique_ptr<yyjson_mut_doc, MutableDocumentDeleter>;

        struct Profile
        {
            std::string id;
            Settings settings;
            std::filesystem::path sourcePath;
        };

        struct State
        {
            std::mutex lock;
            std::vector<Profile> profiles;
            bool applied = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        bool EqualsIgnoreCase(
            const std::string_view a_left,
            const std::string_view a_right)
        {
            return std::ranges::equal(
                a_left,
                a_right,
                [](const char a, const char b)
                {
                    return std::tolower(
                               static_cast<unsigned char>(a)) ==
                           std::tolower(
                               static_cast<unsigned char>(b));
                });
        }

        bool IsWhitePoint(const float a_value)
        {
            return std::abs(a_value - kWhitePoint) <= 0.0001f;
        }

        bool HasSourcePlugin(
            const RE::TESImageSpace* a_imageSpace,
            const RE::TESFile* a_plugin)
        {
            if (!a_imageSpace || !a_plugin)
            {
                return false;
            }
            if (a_imageSpace->sourceFiles.array)
            {
                for (const auto* source :
                     *a_imageSpace->sourceFiles.array)
                {
                    if (source == a_plugin)
                    {
                        return true;
                    }
                }
            }
            return a_imageSpace->GetFile() == a_plugin;
        }

        std::optional<std::string> ReadText(
            const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            std::string text(
                std::istreambuf_iterator<char>(file),
                {});
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom))
            {
                text.erase(0, bom.size());
            }
            return text;
        }

        bool WriteTextAtomically(
            const std::filesystem::path& a_path,
            const std::string_view a_text)
        {
            auto temporaryPath = a_path;
            temporaryPath += ".tmp";
            {
                std::ofstream file(
                    temporaryPath,
                    std::ios::binary | std::ios::trunc);
                file << a_text << '\n';
                if (!file)
                {
                    file.close();
                    std::error_code removeError;
                    std::filesystem::remove(
                        temporaryPath,
                        removeError);
                    return false;
                }
            }
            if (::MoveFileExW(
                    temporaryPath.c_str(),
                    a_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING |
                        MOVEFILE_WRITE_THROUGH))
            {
                return true;
            }
            std::error_code removeError;
            std::filesystem::remove(temporaryPath, removeError);
            return false;
        }

        bool WriteEnabled(Profile& a_profile, const bool a_enabled)
        {
            const auto text = ReadText(a_profile.sourcePath);
            if (!text)
            {
                return false;
            }

            Document source(yyjson_read(
                text->data(),
                text->size(),
                YYJSON_READ_NOFLAG));
            auto* sourceRoot =
                source ? yyjson_doc_get_root(source.get()) : nullptr;
            MutableDocument document(yyjson_mut_doc_new(nullptr));
            auto* root =
                document.get() && yyjson_is_obj(sourceRoot) ?
                    yyjson_val_mut_copy(document.get(), sourceRoot) :
                    nullptr;
            if (!root)
            {
                return false;
            }
            yyjson_mut_doc_set_root(document.get(), root);

            auto* settings =
                yyjson_mut_obj_get(root, kConfigurationKey);
            if (!yyjson_mut_is_obj(settings))
            {
                yyjson_mut_obj_remove_key(root, kConfigurationKey);
                settings = yyjson_mut_obj(document.get());
                if (!settings ||
                    !yyjson_mut_obj_add_val(
                        document.get(),
                        root,
                        kConfigurationKey,
                        settings))
                {
                    return false;
                }
            }
            auto* key =
                yyjson_mut_strcpy(document.get(), "enabled");
            auto* value =
                yyjson_mut_bool(document.get(), a_enabled);
            if (!key || !value ||
                (!yyjson_mut_obj_replace(settings, key, value) &&
                    !yyjson_mut_obj_add(settings, key, value)))
            {
                return false;
            }

            std::size_t length = 0;
            auto* output = yyjson_mut_write(
                document.get(),
                YYJSON_WRITE_PRETTY,
                &length);
            if (!output)
            {
                return false;
            }
            const std::unique_ptr<char, decltype(&std::free)>
                outputOwner(output, &std::free);
            return WriteTextAtomically(
                a_profile.sourcePath,
                std::string_view(output, length));
        }
    }

    void ClearProfiles()
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.profiles.clear();
        state.applied = false;
    }

    void AddProfile(
        std::string a_id,
        Settings a_settings,
        std::filesystem::path a_sourcePath)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.profiles.push_back(Profile{
            .id = std::move(a_id),
            .settings = std::move(a_settings),
            .sourcePath = std::move(a_sourcePath),
        });
    }

    bool GetProfileEnabled(const std::string_view a_profile)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto found = std::ranges::find_if(
            state.profiles,
            [&](const Profile& a_candidate)
            {
                return EqualsIgnoreCase(
                    a_candidate.id,
                    a_profile);
            });
        return found != state.profiles.end() &&
               found->settings.enabled;
    }

    bool SetProfileEnabled(
        const std::string_view a_profile,
        const bool a_enabled)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        const auto found = std::ranges::find_if(
            state.profiles,
            [&](const Profile& a_candidate)
            {
                return EqualsIgnoreCase(
                    a_candidate.id,
                    a_profile);
            });
        if (found == state.profiles.end())
        {
            logger::error(
                "[Auto CS Tonemapping] Cannot change setting "
                "for unknown profile '{}'",
                a_profile);
            return false;
        }
        if (!WriteEnabled(*found, a_enabled))
        {
            logger::error(
                "[Auto CS Tonemapping] [{}] Could not update setting in {}",
                found->id,
                found->sourcePath.string());
            return false;
        }
        found->settings.enabled = a_enabled;
        logger::info(
            "[Auto CS Tonemapping] [{}] Setting {} from "
            "Papyrus; restart required",
            found->id,
            a_enabled ? "enabled" : "disabled");
        return true;
    }

    void ApplyStartup()
    {
        std::vector<Profile> profiles;
        {
            auto& state = GetState();
            std::scoped_lock lock(state.lock);
            if (state.applied)
            {
                return;
            }
            state.applied = true;
            profiles = state.profiles;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn(
                "[Auto CS Tonemapping] Could not run "
                "because TESDataHandler is unavailable");
            return;
        }

        std::unordered_set<RE::TESImageSpace*> targets;
        for (const auto& profile : profiles)
        {
            if (!profile.settings.enabled)
            {
                continue;
            }
            for (const auto& pluginName :
                 profile.settings.plugins)
            {
                const auto* plugin =
                    dataHandler->LookupModByName(pluginName);
                if (!plugin)
                {
                    logger::info(
                        "[Auto CS Tonemapping] [{}] "
                        "target plugin is not loaded: {}",
                        profile.id,
                        pluginName);
                    continue;
                }
                for (auto* imageSpace :
                     dataHandler
                         ->GetFormArray<RE::TESImageSpace>())
                {
                    if (HasSourcePlugin(imageSpace, plugin))
                    {
                        targets.insert(imageSpace);
                    }
                }
            }
        }
        if (targets.empty())
        {
            return;
        }

        const auto filmicCurve =
            RE::GetINISetting(kFilmicCurveSetting);
        const auto filmicDetected =
            filmicCurve && filmicCurve->GetBool();
        const auto whitePointDetected =
            std::ranges::any_of(
                dataHandler->GetFormArray<RE::TESImageSpace>(),
                [](const RE::TESImageSpace* a_imageSpace)
                {
                    return a_imageSpace &&
                           IsWhitePoint(
                               a_imageSpace->data.hdr.white);
                });
        if (!filmicDetected && !whitePointDetected)
        {
            logger::info(
                "[Auto CS Tonemapping] Found {} "
                "target Image Space(s), but no active CS "
                "Tonemapping configuration was detected",
                targets.size());
            return;
        }

        for (auto* imageSpace : targets)
        {
            imageSpace->data.hdr.white = kWhitePoint;
        }
        if (filmicCurve)
        {
            filmicCurve->SetBool(true);
        }
        if (auto* setting =
                RE::GetINISetting(kFilmicWhiteScaleSetting))
        {
            setting->SetFloat(kWhiteScale);
        }
        logger::info(
            "[Auto CS Tonemapping] Applied white "
            "0.1 to {} Image Space(s); detected by filmic INI={}, "
            "existing white point={}",
            targets.size(),
            filmicDetected,
            whitePointDetected);
    }
}
