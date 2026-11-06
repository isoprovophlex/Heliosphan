#include <VanillaIndex.h>
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace MPL::VanillaIndex
{
    namespace
    {
        constexpr std::uint32_t kMagic = 0x58444956;
        constexpr std::uint32_t kVersion = 1;
        constexpr std::uint32_t kMaxEntries = 2'000'000;
        constexpr std::uint32_t kMaxBytes = 64 * 1024 * 1024;

        void Append(std::vector<std::byte>& a_bytes, const std::uint32_t a_value)
        {
            const auto offset = a_bytes.size();
            a_bytes.resize(offset + sizeof(a_value));
            std::memcpy(a_bytes.data() + offset, &a_value, sizeof(a_value));
        }

        std::uint32_t Read(std::span<const std::byte> a_data, std::size_t& a_offset)
        {
            if (a_offset > a_data.size() || a_data.size() - a_offset < sizeof(std::uint32_t))
                throw std::runtime_error("truncated vanilla index");
            std::uint32_t value;
            std::memcpy(&value, a_data.data() + a_offset, sizeof(value));
            a_offset += sizeof(value);
            return value;
        }

        class Hash
        {
        public:
            Hash()
            {
                if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                    throw std::runtime_error("SHA256 provider unavailable");
                DWORD length = 0, written = 0;
                if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&length), sizeof(length), &written, 0) < 0)
                {
                    BCryptCloseAlgorithmProvider(algorithm, 0);
                    algorithm = nullptr;
                    throw std::runtime_error("SHA256 object size unavailable");
                }
                try
                {
                    object.resize(length);
                    if (BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) < 0)
                        throw std::runtime_error("SHA256 initialization failed");
                }
                catch (...) { BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr; throw; }
            }
            ~Hash()
            {
                if (hash) BCryptDestroyHash(hash);
                if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            }
            Hash(const Hash&) = delete;
            Hash& operator=(const Hash&) = delete;
            void Add(const std::span<const std::byte> a_data)
            {
                if (a_data.size() > std::numeric_limits<ULONG>::max() || BCryptHashData(hash,
                        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(a_data.data())),
                        static_cast<ULONG>(a_data.size()), 0) < 0)
                    throw std::runtime_error("SHA256 input failed");
            }
            std::string Finish()
            {
                std::array<UCHAR, 32> digest{};
                if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
                    throw std::runtime_error("SHA256 finalization failed");
                constexpr std::string_view hex = "0123456789ABCDEF";
                std::string text;
                text.reserve(digest.size() * 2);
                for (const auto value : digest) { text.push_back(hex[value >> 4]); text.push_back(hex[value & 15]); }
                return text;
            }
        private:
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            std::vector<UCHAR> object;
        };
    }

    std::vector<std::byte> Encode(const Dataset& a_data)
    {
        if (a_data.placements.size() > kMaxEntries || a_data.editorIDs.size() > kMaxEntries)
            throw std::runtime_error("vanilla index entry limit exceeded");
        std::vector<std::byte> bytes;
        Append(bytes, kMagic); Append(bytes, kVersion);
        Append(bytes, static_cast<std::uint32_t>(a_data.placements.size()));
        Append(bytes, static_cast<std::uint32_t>(a_data.editorIDs.size()));
        for (const auto& placement : a_data.placements)
        {
            if (!placement.reference || !placement.cell) throw std::runtime_error("invalid vanilla placement");
            Append(bytes, placement.reference); Append(bytes, placement.base); Append(bytes, placement.cell);
            Append(bytes, placement.deleted ? 1 : 0);
        }
        for (const auto& [form, name] : a_data.editorIDs)
        {
            if (!form || name.empty() || name.size() > 65535) throw std::runtime_error("invalid vanilla EditorID");
            Append(bytes, form); Append(bytes, static_cast<std::uint32_t>(name.size()));
            const auto* first = reinterpret_cast<const std::byte*>(name.data());
            bytes.insert(bytes.end(), first, first + name.size());
        }
        if (bytes.size() > kMaxBytes) throw std::runtime_error("vanilla index size limit exceeded");
        return bytes;
    }

    Dataset Decode(const std::span<const std::byte> a_data)
    {
        if (a_data.size() > kMaxBytes) throw std::runtime_error("vanilla index size limit exceeded");
        std::size_t offset = 0;
        if (Read(a_data, offset) != kMagic || Read(a_data, offset) != kVersion)
            throw std::runtime_error("unsupported vanilla index format");
        const auto placements = Read(a_data, offset), editorIDs = Read(a_data, offset);
        if (placements > kMaxEntries || editorIDs > kMaxEntries ||
            static_cast<std::uint64_t>(placements) * 16 + static_cast<std::uint64_t>(editorIDs) * 9 > a_data.size() - offset)
            throw std::runtime_error("invalid vanilla index counts");
        Dataset result;
        result.placements.reserve(placements); result.editorIDs.reserve(editorIDs);
        for (std::uint32_t index = 0; index < placements; ++index)
        {
            const auto reference = Read(a_data, offset), base = Read(a_data, offset), cell = Read(a_data, offset), flags = Read(a_data, offset);
            if (!reference || !cell || flags > 1) throw std::runtime_error("invalid vanilla placement");
            result.placements.push_back({reference, base, cell, flags != 0});
        }
        for (std::uint32_t index = 0; index < editorIDs; ++index)
        {
            const auto form = Read(a_data, offset), length = Read(a_data, offset);
            if (!form || !length || length > 65535 || length > a_data.size() - offset)
                throw std::runtime_error("invalid vanilla EditorID");
            result.editorIDs.emplace_back(form, std::string(reinterpret_cast<const char*>(a_data.data() + offset), length));
            offset += length;
        }
        if (offset != a_data.size()) throw std::runtime_error("unexpected vanilla index trailing data");
        return result;
    }

    std::vector<std::byte> Compress(const std::span<const std::byte> a_data)
    {
        if (a_data.empty() || a_data.size() > kMaxBytes) throw std::runtime_error("invalid vanilla index size");
        uLongf length = compressBound(static_cast<uLong>(a_data.size()));
        std::vector<std::byte> output(length);
        if (compress2(reinterpret_cast<Bytef*>(output.data()), &length,
                reinterpret_cast<const Bytef*>(a_data.data()), static_cast<uLong>(a_data.size()), Z_BEST_COMPRESSION) != Z_OK)
            throw std::runtime_error("vanilla index compression failed");
        output.resize(length);
        return output;
    }

    std::vector<std::byte> Expand(const std::span<const std::byte> a_data, const std::uint32_t a_expectedBytes)
    {
        if (a_data.empty() || a_data.size() > kMaxBytes || a_expectedBytes < 16 || a_expectedBytes > kMaxBytes)
            throw std::runtime_error("invalid compressed vanilla index size");
        uLongf length = a_expectedBytes;
        std::vector<std::byte> output(length);
        if (uncompress(reinterpret_cast<Bytef*>(output.data()), &length,
                reinterpret_cast<const Bytef*>(a_data.data()), static_cast<uLong>(a_data.size())) != Z_OK || length != a_expectedBytes)
            throw std::runtime_error("vanilla index decompression failed");
        return output;
    }

    std::string SHA256(const std::span<const std::byte> a_data)
    {
        Hash hash; hash.Add(a_data); return hash.Finish();
    }

    std::string SHA256(std::istream& a_stream, const std::uint64_t a_bytes)
    {
        struct Rewind
        {
            std::istream& stream;
            ~Rewind() { stream.clear(); stream.seekg(0, std::ios::beg); }
        } rewind{a_stream};
        a_stream.clear(); a_stream.seekg(0, std::ios::beg);
        Hash hash;
        std::vector<std::byte> buffer(1024 * 1024);
        std::uint64_t remaining = a_bytes;
        while (remaining)
        {
            const auto length = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
            if (!a_stream.read(reinterpret_cast<char*>(buffer.data()), length)) throw std::runtime_error("vanilla fingerprint read failed");
            hash.Add(std::span(buffer).first(static_cast<std::size_t>(length)));
            remaining -= length;
        }
        return hash.Finish();
    }
}
