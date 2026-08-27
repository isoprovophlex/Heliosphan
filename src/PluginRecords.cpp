#include <PluginRecords.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace MPL::PluginRecords
{
    namespace
    {
        constexpr std::uint32_t Signature(
            const char a_first,
            const char a_second,
            const char a_third,
            const char a_fourth)
        {
            return static_cast<std::uint8_t>(a_first) |
                   static_cast<std::uint32_t>(
                       static_cast<std::uint8_t>(a_second))
                       << 8 |
                   static_cast<std::uint32_t>(
                       static_cast<std::uint8_t>(a_third))
                       << 16 |
                   static_cast<std::uint32_t>(
                       static_cast<std::uint8_t>(a_fourth))
                       << 24;
        }

        constexpr auto kEditorID = Signature('E', 'D', 'I', 'D');
        constexpr auto kName = Signature('N', 'A', 'M', 'E');
        constexpr auto kExtendedSize = Signature('X', 'X', 'X', 'X');

        template <class T>
        std::optional<T> ReadValue(
            const std::span<const std::byte> a_data,
            const std::size_t a_offset)
        {
            if (a_offset > a_data.size() ||
                sizeof(T) > a_data.size() - a_offset)
            {
                return std::nullopt;
            }
            T value{};
            std::memcpy(
                std::addressof(value),
                a_data.data() + a_offset,
                sizeof(T));
            return value;
        }

        std::optional<std::span<const std::byte>> FindSubrecord(
            const std::span<const std::byte> a_data,
            const std::uint32_t a_target)
        {
            std::size_t offset = 0;
            std::optional<std::uint32_t> extendedSize;
            while (offset + sizeof(std::uint32_t) +
                       sizeof(std::uint16_t) <=
                   a_data.size())
            {
                const auto signature =
                    ReadValue<std::uint32_t>(a_data, offset);
                const auto smallSize = ReadValue<std::uint16_t>(
                    a_data,
                    offset + sizeof(std::uint32_t));
                if (!signature || !smallSize)
                {
                    return std::nullopt;
                }
                offset += sizeof(std::uint32_t) +
                          sizeof(std::uint16_t);

                const std::size_t size = extendedSize ?
                                             *extendedSize :
                                             *smallSize;
                extendedSize.reset();
                if (size > a_data.size() - offset)
                {
                    return std::nullopt;
                }
                if (*signature == kExtendedSize)
                {
                    if (size != sizeof(std::uint32_t))
                    {
                        return std::nullopt;
                    }
                    extendedSize =
                        ReadValue<std::uint32_t>(a_data, offset);
                }
                else if (*signature == a_target)
                {
                    return a_data.subspan(offset, size);
                }
                offset += size;
            }
            return std::nullopt;
        }
    }  // namespace

    std::optional<FormID> FindBaseForm(
        const std::span<const std::byte> a_data)
    {
        const auto value = FindSubrecord(a_data, kName);
        return value && value->size() >= sizeof(FormID) ?
                   ReadValue<FormID>(*value, 0) :
                   std::nullopt;
    }

    std::optional<std::string> FindEditorID(
        const std::span<const std::byte> a_data)
    {
        const auto value = FindSubrecord(a_data, kEditorID);
        if (!value || value->empty())
        {
            return std::nullopt;
        }
        const auto end = std::ranges::find(*value, std::byte{ 0 });
        const auto size = static_cast<std::size_t>(
            std::distance(value->begin(), end));
        return size != 0 ?
                   std::optional<std::string>(
                       std::in_place,
                       reinterpret_cast<const char*>(value->data()),
                       size) :
                   std::nullopt;
    }

    void MergePlacement(
        std::unordered_map<FormID, Placement>& a_placements,
        const FormID a_reference,
        Placement a_placement)
    {
        a_placement.reference = a_reference;
        a_placements.insert_or_assign(
            a_reference,
            std::move(a_placement));
    }
}  // namespace MPL::PluginRecords
