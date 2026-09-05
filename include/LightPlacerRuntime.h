#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

namespace MPL::LightPlacer::Detail
{
    inline const std::string* LightName(const nlohmann::json& a_entry)
    {
        if (!a_entry.is_object())
        {
            return nullptr;
        }
        const auto data = a_entry.find("data");
        if (data == a_entry.end() || !data->is_object())
        {
            return nullptr;
        }
        const auto light = data->find("light");
        return light != data->end() && light->is_string() ?
                   &light->get_ref<const std::string&>() : nullptr;
    }

    class FileRestores
    {
    public:
        template <class Writer>
        bool Replace(
            const std::string& a_path,
            const std::string& a_original,
            const std::string_view a_replacement,
            Writer&& a_write)
        {
            originals.try_emplace(a_path, a_original);
            return a_write(a_path, a_replacement);
        }

        template <class Writer>
        bool Restore(Writer&& a_write)
        {
            for (auto entry = originals.begin(); entry != originals.end();)
            {
                bool restored = false;
                try
                {
                    restored = a_write(entry->first, entry->second);
                }
                catch (...)
                {
                    // Keep the original available for the next restore attempt.
                }
                if (restored)
                {
                    entry = originals.erase(entry);
                }
                else
                {
                    ++entry;
                }
            }
            return originals.empty();
        }

        bool Empty() const { return originals.empty(); }
        std::size_t Size() const { return originals.size(); }

    private:
        std::unordered_map<std::string, std::string> originals;
    };
}  // namespace MPL::LightPlacer::Detail
