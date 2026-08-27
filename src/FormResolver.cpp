#include <FormResolver.h>
#include <MMSF_API.h>
#include <Heliosphan.h>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>

namespace MPL::FormResolver
{
    namespace
    {
        std::string_view Trim(std::string_view a_value)
        {
            while (!a_value.empty() &&
                   std::isspace(
                       static_cast<unsigned char>(a_value.front())))
            {
                a_value.remove_prefix(1);
            }
            while (!a_value.empty() &&
                   std::isspace(
                       static_cast<unsigned char>(a_value.back())))
            {
                a_value.remove_suffix(1);
            }
            return a_value;
        }

        std::optional<RE::FormID> ParseHex(std::string_view a_value)
        {
            if (a_value.starts_with("0x") || a_value.starts_with("0X"))
            {
                a_value.remove_prefix(2);
            }
            if (a_value.empty())
            {
                return std::nullopt;
            }
            RE::FormID result = 0;
            const auto [end, error] = std::from_chars(
                a_value.data(),
                a_value.data() + a_value.size(),
                result,
                16);
            return error == std::errc{} &&
                           end == a_value.data() + a_value.size() ?
                       std::optional<RE::FormID>{ result } :
                       std::nullopt;
        }
    }  // namespace

    RE::FormID Resolve(std::string_view a_selector)
    {
        a_selector = Trim(a_selector);
        if (a_selector.empty())
        {
            return 0;
        }

        const auto separator = a_selector.find_first_of("~:");
        if (separator != std::string_view::npos)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            const auto local = ParseHex(a_selector.substr(0, separator));
            const auto plugin = Trim(a_selector.substr(separator + 1));
            return dataHandler && local && !plugin.empty() ?
                       dataHandler->LookupFormID(*local, plugin) :
                       0;
        }

        if (auto* mmsf = Heliosphan::GetMMSFAPI())
        {
            const std::string editorID(a_selector);
            if (auto* form = mmsf->LookupCachedForm(editorID))
            {
                return form->GetFormID();
            }
            if (const auto formID = mmsf->LookupFormIDForEDID(editorID))
            {
                return formID;
            }
        }
        if (const auto* form = RE::TESForm::LookupByEditorID(a_selector))
        {
            return form->GetFormID();
        }
        if (a_selector.starts_with("0x") ||
            a_selector.starts_with("0X"))
        {
            return ParseHex(a_selector).value_or(0);
        }
        return 0;
    }
}  // namespace MPL::FormResolver
