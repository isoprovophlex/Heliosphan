#include <ManifestSettings.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <rfl/Generic.hpp>
#include <rfl/json.hpp>

namespace MPL::ManifestSettings
{
    namespace
    {
        struct Settings
        {
            std::string id;
            bool MMSFManifest = false;
            std::optional<bool> speedLogs;
            std::optional<bool> notifications;
        };

        bool SameID(const std::string_view a_left, const std::string_view a_right)
        {
            return std::ranges::equal(a_left, a_right, [](const unsigned char a, const unsigned char b)
                { return std::tolower(a) == std::tolower(b); });
        }

        std::string ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            std::string text(std::istreambuf_iterator<char>(file), {});
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom)) text.erase(0, bom.size());
            return text;
        }
    }

    std::vector<Manifest> Load(const std::filesystem::path& a_root)
    {
        std::vector<Manifest> manifests;
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(
                 a_root, std::filesystem::directory_options::skip_permission_denied, error),
             end;
             iterator != end && !error;
             iterator.increment(error))
        {
            const auto filename = iterator->path().filename().string();
            constexpr std::string_view suffix = "settings.json";
            if (!iterator->is_regular_file(error) || filename.size() < suffix.size() ||
                !SameID(std::string_view(filename).substr(filename.size() - suffix.size()), suffix))
                continue;
            const auto parsed = rfl::json::read<Settings, rfl::DefaultIfMissing>(ReadText(iterator->path()));
            if (!parsed || !parsed->MMSFManifest || parsed->id.empty()) continue;
            manifests.push_back({ parsed->id, iterator->path(), parsed->speedLogs, parsed->notifications });
        }
        return manifests;
    }

    const Manifest* Find(const std::vector<Manifest>& a_manifests, const std::string_view a_id)
    {
        const Manifest* found = nullptr;
        for (const auto& manifest : a_manifests)
        {
            if (!SameID(manifest.id, a_id)) continue;
            if (found)
            {
                logger::warn("[Heliosphan Settings] profile={} | status=rejected | reason=duplicate-manifests", a_id);
                return nullptr;
            }
            found = &manifest;
        }
        return found;
    }

    bool WriteBoolean(const std::filesystem::path& a_source, const std::string_view a_id,
        const std::string_view a_key, const bool a_enabled)
    {
        if (a_source.empty() || (a_key != "speedLogs" && a_key != "notifications")) return false;
        const auto manifests = Load(a_source.parent_path());
        const auto* manifest = Find(manifests, a_id);
        if (!manifest || manifest->source != a_source) return false;
        const auto parsed = rfl::json::read<rfl::Generic::Object>(ReadText(a_source));
        if (!parsed) return false;
        auto object = parsed.value();
        object[std::string(a_key)] = a_enabled;
        auto temporary = a_source;
        temporary += L".tmp";
        if (rfl::json::save(temporary.string(), object, rfl::json::pretty) &&
            ::MoveFileExW(temporary.c_str(), a_source.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        std::error_code error;
        std::filesystem::remove(temporary, error);
        logger::warn("[Heliosphan Settings] profile={} | setting={} | status=write-failed", a_id, a_key);
        return false;
    }
}
