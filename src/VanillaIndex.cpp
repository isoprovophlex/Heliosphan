#include <VanillaIndex.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>

namespace MPL::VanillaIndex
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        std::string Lower(const std::string_view a_name)
        {
            std::string result(a_name);
            std::ranges::transform(result, result.begin(), [](const unsigned char a_value) { return static_cast<char>(std::tolower(a_value)); });
            return result;
        }
        double Milliseconds(const Clock::time_point a_started)
        { return std::chrono::duration<double, std::milli>(Clock::now() - a_started).count(); }

#ifdef HELIOSPHAN_HAS_VANILLA_INDEX_DATA
#include "../assets/VanillaIndex/Manifest.inc"
        std::vector<std::byte> Resource(const std::uint32_t a_id)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&Resource), &module)) throw std::runtime_error("vanilla resource module unavailable");
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(a_id), RT_RCDATA);
            const auto handle = resource ? LoadResource(module, resource) : nullptr;
            const auto* bytes = handle ? static_cast<const std::byte*>(LockResource(handle)) : nullptr;
            const auto length = resource ? SizeofResource(module, resource) : 0;
            if (!bytes || !length) throw std::runtime_error("vanilla index resource unavailable");
            return {bytes, bytes + length};
        }
#endif
    }

    bool IsVanillaMaster(const std::string_view a_name)
    {
        const auto name = Lower(a_name);
        return name == "skyrim.esm" || name == "dawnguard.esm" || name == "dragonborn.esm";
    }

    Catalog::Catalog()
    {
#ifdef HELIOSPHAN_HAS_VANILLA_INDEX_DATA
        descriptors = kDescriptors;
        reader = Resource;
#endif
    }
#ifdef ENABLE_COMMONLIBSSE_TESTING
    Catalog::Catalog(const std::span<const Descriptor> a_descriptors, ResourceReader a_reader)
        : descriptors(a_descriptors), reader(std::move(a_reader)) {}
#endif

    Lookup Catalog::Acquire(const std::string_view a_plugin, const std::uint64_t a_bytes, std::istream& a_stream)
    {
        if (!IsVanillaMaster(a_plugin)) return {{}, "not-a-vanilla-master"};
        const auto name = Lower(a_plugin);
        const auto key = name + ":" + std::to_string(a_bytes);
        if (const auto found = loaded.find(key); found != loaded.end())
        {
            auto lookup = found->second;
            lookup.validationMs = 0; lookup.decodeMs = 0; lookup.validatedBytes = 0; lookup.reused = true;
            return lookup;
        }
        Lookup lookup;
        lookup.reason = "unsupported-file-size";
        const auto candidate = std::ranges::find_if(descriptors, [&](const Descriptor& a_descriptor)
            { return Lower(a_descriptor.plugin) == name && a_descriptor.fileBytes == a_bytes; });
        if (candidate == descriptors.end()) { loaded.emplace(key, lookup); return lookup; }
        auto started = Clock::now();
        try
        {
            lookup.fingerprint = SHA256(a_stream, a_bytes);
            lookup.validationMs = Milliseconds(started); lookup.validatedBytes = a_bytes;
            const auto descriptor = std::ranges::find_if(descriptors, [&](const Descriptor& a_descriptor)
                { return Lower(a_descriptor.plugin) == name && a_descriptor.fileBytes == a_bytes && a_descriptor.sha256 == lookup.fingerprint; });
            if (descriptor == descriptors.end()) lookup.reason = "fingerprint-mismatch";
            else
            {
                started = Clock::now();
                lookup.data = std::make_shared<Dataset>(Decode(Expand(reader(descriptor->resource), descriptor->unpackedBytes)));
                lookup.decodeMs = Milliseconds(started);
                lookup.reason = "recognized";
            }
        }
        catch (const std::exception& error)
        {
            lookup.data.reset(); lookup.reason = error.what();
            if (lookup.fingerprint.empty()) lookup.validationMs = Milliseconds(started);
            else lookup.decodeMs = Milliseconds(started);
        }
        loaded.emplace(key, lookup);
        return lookup;
    }
}
