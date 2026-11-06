#pragma once

#include <PluginRecords.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MPL::VanillaIndex
{
    struct Dataset
    {
        std::vector<PluginRecords::Placement> placements;
        std::vector<std::pair<std::uint32_t, std::string>> editorIDs;
    };

    struct Descriptor
    {
        std::string_view plugin;
        std::uint64_t fileBytes;
        std::string_view sha256;
        std::uint32_t resource;
        std::uint32_t unpackedBytes;
    };

    struct Lookup
    {
        std::shared_ptr<const Dataset> data;
        std::string reason;
        std::string fingerprint;
        double validationMs = 0;
        double decodeMs = 0;
        std::uint64_t validatedBytes = 0;
        bool reused = false;
    };

    std::vector<std::byte> Encode(const Dataset& a_data);
    Dataset Decode(std::span<const std::byte> a_data);
    std::vector<std::byte> Compress(std::span<const std::byte> a_data);
    std::vector<std::byte> Expand(std::span<const std::byte> a_data, std::uint32_t a_expectedBytes);
    std::string SHA256(std::span<const std::byte> a_data);
    std::string SHA256(std::istream& a_stream, std::uint64_t a_bytes);
    bool IsVanillaMaster(std::string_view a_name);

    class Catalog
    {
    public:
        using ResourceReader = std::function<std::vector<std::byte>(std::uint32_t)>;
        Catalog();
#ifdef ENABLE_COMMONLIBSSE_TESTING
        Catalog(std::span<const Descriptor> a_descriptors, ResourceReader a_reader);
#endif
        Lookup Acquire(std::string_view a_plugin, std::uint64_t a_bytes, std::istream& a_stream);

    private:
        std::span<const Descriptor> descriptors;
        ResourceReader reader;
        std::unordered_map<std::string, Lookup> loaded;
    };
}
