#include <PluginIndex.h>
#include <PluginRecords.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <zlib.h>

namespace MPL::PluginIndex
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

        constexpr auto kGroup = Signature('G', 'R', 'U', 'P');
        constexpr auto kReference = Signature('R', 'E', 'F', 'R');
        constexpr auto kStatic = Signature('S', 'T', 'A', 'T');
        constexpr auto kName = Signature('N', 'A', 'M', 'E');
        constexpr auto kExtendedSize = Signature('X', 'X', 'X', 'X');
        constexpr std::uint32_t kDeleted = 0x00000020;
        constexpr std::uint32_t kCompressed = 0x00040000;
        constexpr std::int32_t kCellChildren = 6;
        constexpr std::size_t kMaxStoredRecordSize =
            64ULL * 1024ULL * 1024ULL;
        constexpr std::size_t kMaxDecompressedRecordSize =
            256ULL * 1024ULL * 1024ULL;
        constexpr std::size_t kMaxPlacementsPerPlugin =
            2'000'000;
        constexpr std::size_t kMaxTotalPlacements =
            4'000'000;

#pragma pack(push, 1)
        struct RecordHeader
        {
            std::uint32_t signature;
            std::uint32_t dataSize;
            std::uint32_t flags;
            std::uint32_t formID;
            std::uint32_t revision;
            std::uint16_t version;
            std::uint16_t unknown;
        };

        struct GroupHeader
        {
            std::uint32_t signature;
            std::uint32_t size;
            std::uint32_t label;
            std::int32_t type;
            std::uint32_t stamp;
            std::uint32_t unknown;
        };
#pragma pack(pop)

        static_assert(sizeof(RecordHeader) == 24);
        static_assert(sizeof(GroupHeader) == 24);

        template <class T>
        bool Read(std::ifstream& a_stream, T& a_value)
        {
            return static_cast<bool>(
                a_stream.read(
                    reinterpret_cast<char*>(std::addressof(a_value)),
                    sizeof(T)));
        }

        struct RecordBuffers
        {
            std::vector<std::byte> stored;
            std::vector<std::byte> decompressed;
        };

        std::optional<std::span<const std::byte>> ReadRecordData(
            std::ifstream& a_stream,
            const RecordHeader& a_header,
            std::size_t& a_compressedCount,
            RecordBuffers& a_buffers)
        {
            if (a_header.dataSize > kMaxStoredRecordSize)
            {
                logger::warn(
                "[Window Sync] index record rejected | size={}B | limit={}B",
                    a_header.dataSize,
                    kMaxStoredRecordSize);
                return std::nullopt;
            }
            a_buffers.stored.resize(a_header.dataSize);
            if (!a_buffers.stored.empty() &&
                !a_stream.read(
                    reinterpret_cast<char*>(a_buffers.stored.data()),
                    static_cast<std::streamsize>(a_buffers.stored.size())))
            {
                return std::nullopt;
            }
            if ((a_header.flags & kCompressed) == 0)
            {
                return std::span<const std::byte>(a_buffers.stored);
            }

            ++a_compressedCount;
            if (a_buffers.stored.size() < sizeof(std::uint32_t))
            {
                return std::nullopt;
            }
            std::uint32_t decompressedSize = 0;
            std::memcpy(
                std::addressof(decompressedSize),
                a_buffers.stored.data(),
                sizeof(decompressedSize));
            if (decompressedSize == 0 ||
                decompressedSize >
                    kMaxDecompressedRecordSize)
            {
                logger::warn(
                "[Window Sync] index decompressed record rejected | size={}B | allocation range exceeded",
                    decompressedSize);
                return std::nullopt;
            }
            a_buffers.decompressed.resize(decompressedSize);
            uLongf actualSize = decompressedSize;
            const auto status = uncompress(
                reinterpret_cast<Bytef*>(a_buffers.decompressed.data()),
                std::addressof(actualSize),
                reinterpret_cast<const Bytef*>(
                    a_buffers.stored.data() + sizeof(decompressedSize)),
                static_cast<uLong>(
                    a_buffers.stored.size() - sizeof(decompressedSize)));
            if (status != Z_OK || actualSize != decompressedSize)
            {
                return std::nullopt;
            }
            return std::span<const std::byte>(a_buffers.decompressed);
        }

        struct BaseFormRead
        {
            bool succeeded = false;
            std::optional<RE::FormID> base;
            std::size_t bytesRead = 0;
        };

        BaseFormRead ReadUncompressedBaseForm(
            std::ifstream& a_stream,
            const RecordHeader& a_header)
        {
            if (a_header.dataSize > kMaxStoredRecordSize)
            {
                logger::warn(
                "[Window Sync] index record rejected | size={}B | limit={}B",
                    a_header.dataSize,
                    kMaxStoredRecordSize);
                return {};
            }

            std::size_t remaining = a_header.dataSize;
            std::size_t bytesRead = 0;
            std::optional<std::uint32_t> extendedSize;
            while (remaining >=
                   sizeof(std::uint32_t) + sizeof(std::uint16_t))
            {
                std::uint32_t signature = 0;
                std::uint16_t smallSize = 0;
                if (!Read(a_stream, signature) || !Read(a_stream, smallSize))
                {
                    return {};
                }
                constexpr auto headerSize =
                    sizeof(std::uint32_t) + sizeof(std::uint16_t);
                bytesRead += headerSize;
                remaining -= headerSize;

                const std::size_t size = extendedSize ?
                                             *extendedSize :
                                             smallSize;
                extendedSize.reset();
                if (size > remaining)
                {
                    return BaseFormRead{
                        .succeeded = true,
                        .bytesRead = bytesRead,
                    };
                }
                if (signature == kExtendedSize)
                {
                    if (size != sizeof(std::uint32_t))
                    {
                        return BaseFormRead{
                            .succeeded = true,
                            .bytesRead = bytesRead,
                        };
                    }
                    std::uint32_t value = 0;
                    if (!Read(a_stream, value))
                    {
                        return {};
                    }
                    extendedSize = value;
                    bytesRead += sizeof(value);
                    remaining -= sizeof(value);
                    continue;
                }
                if (signature == kName)
                {
                    if (size < sizeof(RE::FormID))
                    {
                        return BaseFormRead{
                            .succeeded = true,
                            .bytesRead = bytesRead,
                        };
                    }
                    RE::FormID base = 0;
                    if (!Read(a_stream, base))
                    {
                        return {};
                    }
                    bytesRead += sizeof(base);
                    return BaseFormRead{
                        .succeeded = true,
                        .base = base,
                        .bytesRead = bytesRead,
                    };
                }
                a_stream.seekg(
                    static_cast<std::streamoff>(size),
                    std::ios::cur);
                if (!a_stream)
                {
                    return {};
                }
                remaining -= size;
            }
            return BaseFormRead{
                .succeeded = true,
                .bytesRead = bytesRead,
            };
        }

        class Parser
        {
        public:
            Parser(
                const RE::TESFile& a_file,
                Result& a_result,
                const BuildOptions& a_options,
                const std::unordered_map<RE::FormID, Placement>*
                    a_winningPlacements = nullptr) :
                file(a_file),
                result(a_result),
                options(a_options),
                winningPlacements(a_winningPlacements)
            {}

            bool Parse(const std::filesystem::path& a_path)
            {
                stream.open(a_path, std::ios::binary);
                if (!stream)
                {
                    return Fail("the plugin file could not be opened");
                }
                stream.seekg(0, std::ios::end);
                const auto length = stream.tellg();
                if (length < 0)
                {
                    return Fail("the plugin file length could not be read");
                }
                stream.seekg(0, std::ios::beg);
                return ParseRange(
                    static_cast<std::uint64_t>(length),
                    std::nullopt);
            }

            const std::string& FailureReason() const
            {
                return failureReason;
            }

        private:
            bool Fail(const std::string_view a_reason)
            {
                if (failureReason.empty())
                {
                    failureReason = a_reason;
                }
                return false;
            }

            bool ParseRange(
                const std::uint64_t a_end,
                const std::optional<RE::FormID> a_cell)
            {
                while (true)
                {
                    const auto position = stream.tellg();
                    if (position < 0)
                    {
                        return Fail("the parser lost its stream position");
                    }
                    const auto start =
                        static_cast<std::uint64_t>(position);
                    if (start == a_end)
                    {
                        return true;
                    }
                    if (start > a_end || a_end - start < sizeof(std::uint32_t))
                    {
                        return Fail("a record extends outside its containing group");
                    }

                    std::uint32_t signature = 0;
                    if (!Read(stream, signature))
                    {
                        return Fail("a record signature could not be read");
                    }
                    stream.seekg(
                        static_cast<std::streamoff>(start),
                        std::ios::beg);

                    if (signature == kGroup)
                    {
                        if (a_end - start < sizeof(GroupHeader))
                        {
                            return Fail("a group header is truncated");
                        }
                        GroupHeader group{};
                        if (!Read(stream, group) ||
                            group.size < sizeof(GroupHeader) ||
                            group.size > a_end - start)
                        {
                            return Fail("a group header has an invalid size");
                        }
                        const auto groupEnd = start + group.size;
                        auto cell = a_cell;
                        if (group.type == kCellChildren)
                        {
                            const auto cellID =
                                file.GetRuntimeFormID(group.label);
                            const auto* runtimeCell = cellID ?
                                                          RE::TESForm::LookupByID<
                                                              RE::TESObjectCELL>(
                                                              cellID) :
                                                          nullptr;
                            const bool exterior =
                                options.skipExteriorCells && runtimeCell &&
                                !runtimeCell->IsInteriorCell();
                            const bool excluded =
                                cellID && options.excludedCells.contains(cellID);
                            if (exterior || excluded)
                            {
                                if (exterior)
                                {
                                    ++result.exteriorCellGroupsSkipped;
                                }
                                else
                                {
                                    ++result.excludedCellGroupsSkipped;
                                }
                                result.cellGroupBytesSkipped +=
                                    group.size - sizeof(GroupHeader);
                                stream.seekg(
                                    static_cast<std::streamoff>(groupEnd),
                                    std::ios::beg);
                                continue;
                            }
                            cell = group.label;
                        }
                        if (!ParseRange(groupEnd, cell))
                        {
                            return false;
                        }
                        stream.seekg(
                            static_cast<std::streamoff>(groupEnd),
                            std::ios::beg);
                        continue;
                    }

                    if (a_end - start < sizeof(RecordHeader))
                    {
                        return Fail("a record header is truncated");
                    }
                    RecordHeader header{};
                    if (!Read(stream, header) ||
                        header.dataSize > a_end - start - sizeof(header))
                    {
                        return Fail("a record header has an invalid data size");
                    }
                    const auto recordEnd =
                        start + sizeof(header) + header.dataSize;
                    if (options.indexReferences &&
                        header.signature == kReference && a_cell)
                    {
                        ++result.referencesRead;
                        std::optional<RE::FormID> base;
                        if ((header.flags & kCompressed) != 0)
                        {
                            const auto data = ReadRecordData(
                                stream,
                                header,
                                result.compressedReferences,
                                buffers);
                            if (!data)
                            {
                                return Fail(
                                    "a reference record could not be read or decompressed");
                            }
                            base = PluginRecords::FindBaseForm(*data);
                        }
                        else
                        {
                            const auto read =
                                ReadUncompressedBaseForm(stream, header);
                            if (!read.succeeded)
                            {
                                return Fail(
                                    "an uncompressed reference base could not be read");
                            }
                            base = read.base;
                            ++result.minimallyScannedReferences;
                            result.referencePayloadBytesSkipped +=
                                header.dataSize - std::min<std::size_t>(
                                                      header.dataSize,
                                                      read.bytesRead);
                        }
                        const auto reference =
                            file.GetRuntimeFormID(header.formID);
                        const auto cell =
                            file.GetRuntimeFormID(*a_cell);
                        if (reference && cell)
                        {
                            const auto runtimeBase =
                                base && *base ?
                                    file.GetRuntimeFormID(*base) :
                                    0;
                            const bool selected =
                                !options.retainPlacement ||
                                options.retainPlacement(
                                    reference,
                                    runtimeBase,
                                    cell);
                            const bool replacesSelected =
                                winningPlacements &&
                                winningPlacements->contains(reference);
                            if (!selected && !replacesSelected)
                            {
                                ++result.placementRecordsDiscarded;
                                stream.seekg(
                                    static_cast<std::streamoff>(recordEnd),
                                    std::ios::beg);
                                continue;
                            }
                            if (!result.placements.contains(reference) &&
                                result.placements.size() >=
                                    kMaxPlacementsPerPlugin)
                            {
                                logger::warn(
                "[Window Sync] index stopped | placements={} | plugin='{}'",
                                    kMaxPlacementsPerPlugin,
                                    file.GetFilename());
                                return Fail(
                                    "the per-plugin placement limit was reached");
                            }
                            PluginRecords::MergePlacement(
                                result.placements,
                                reference,
                                Placement{
                                    .base = runtimeBase,
                                    .cell = cell,
                                    .deleted =
                                        (header.flags & kDeleted) != 0,
                                });
                        }
                    }
                    else if (options.indexStaticEditorIDs &&
                              header.signature == kStatic)
                    {
                        std::size_t compressed = 0;
                        const auto data = ReadRecordData(
                            stream,
                            header,
                            compressed,
                            buffers);
                        if (!data)
                        {
                            return Fail(
                                "a static record could not be read or decompressed");
                        }
                        const auto formID =
                            file.GetRuntimeFormID(header.formID);
                        if (formID)
                        {
                            if (const auto editorID =
                                    PluginRecords::FindEditorID(*data))
                            {
                                result.editorIDs.insert_or_assign(
                                    formID,
                                    *editorID);
                            }
                        }
                    }
                    stream.seekg(
                        static_cast<std::streamoff>(recordEnd),
                        std::ios::beg);
                }
            }

            const RE::TESFile& file;
            Result& result;
            const BuildOptions& options;
            const std::unordered_map<RE::FormID, Placement>*
                winningPlacements;
            RecordBuffers buffers;
            std::ifstream stream;
            std::string failureReason;
        };

        std::filesystem::path PluginPath(const RE::TESFile& a_file)
        {
            const auto filename = std::string(a_file.GetFilename());
            const std::array candidates{
                std::filesystem::path("Data") / filename,
                std::filesystem::path(filename),
                std::filesystem::path(a_file.path) / filename,
            };
            for (const auto& candidate : candidates)
            {
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error))
                {
                    return candidate;
                }
            }
            return candidates.front();
        }

        bool IsPluginFile(const RE::TESFile& a_file)
        {
            const auto extension =
                std::filesystem::path(a_file.GetFilename())
                    .extension()
                    .string();
            return _stricmp(extension.c_str(), ".esm") == 0 ||
                   _stricmp(extension.c_str(), ".esp") == 0 ||
                   _stricmp(extension.c_str(), ".esl") == 0;
        }

        void AppendLoadedPlugins(
            const RE::TESFile* const* a_files,
            const std::size_t a_count,
            std::unordered_set<const RE::TESFile*>& a_seen,
            std::vector<const RE::TESFile*>& a_plugins)
        {
            if (!a_files)
            {
                return;
            }
            for (std::size_t index = 0; index < a_count; ++index)
            {
                const auto* file = a_files[index];
                if (file && IsPluginFile(*file) &&
                    a_seen.insert(file).second)
                {
                    a_plugins.push_back(file);
                }
            }
        }

        std::vector<const RE::TESFile*> LoadedPlugins()
        {
            std::vector<const RE::TESFile*> plugins;
            auto* dataHandler =
                RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                return plugins;
            }
            std::unordered_set<const RE::TESFile*> seen;
            AppendLoadedPlugins(
                dataHandler->GetLoadedMods(),
                dataHandler->GetLoadedModCount(),
                seen,
                plugins);
            AppendLoadedPlugins(
                dataHandler->GetLoadedLightMods(),
                dataHandler->GetLoadedLightModCount(),
                seen,
                plugins);
            return plugins;
        }
    }  // namespace

    Result Build(const BuildOptions& a_options)
    {
        Result result;
        const auto plugins = LoadedPlugins();
        result.pluginsDiscovered = plugins.size();
        const auto recordFailure = [&](const RE::TESFile& a_plugin)
        {
            const std::string filename{ a_plugin.GetFilename() };
            if (std::ranges::find(result.failedPlugins, filename) ==
                result.failedPlugins.end())
            {
                result.failedPlugins.push_back(filename);
            }
        };
        BuildOptions parseOptions = a_options;
        parseOptions.preparePlacementFilter = {};
        if (a_options.preparePlacementFilter &&
            a_options.indexStaticEditorIDs)
        {
            BuildOptions preflightOptions = a_options;
            preflightOptions.indexReferences = false;
            preflightOptions.preparePlacementFilter = {};
            preflightOptions.retainPlacement = {};
            for (const auto* plugin : plugins)
            {
                if (!plugin)
                {
                    continue;
                }
                const auto path = PluginPath(*plugin);
                try
                {
                    Result parsed;
                    Parser parser(
                        *plugin,
                        parsed,
                        preflightOptions);
                    if (!parser.Parse(path))
                    {
                        logger::warn(
                    "[Window Sync] EditorID preflight parse failed | plugin='{}' | path='{}' | {}",
                            plugin->GetFilename(),
                            path.string(),
                            parser.FailureReason());
                        recordFailure(*plugin);
                        continue;
                    }
                    for (auto& [formID, editorID] : parsed.editorIDs)
                    {
                        result.editorIDs.insert_or_assign(
                            formID,
                            std::move(editorID));
                    }
                }
                catch (const std::exception& error)
                {
                    logger::error(
                    "[Window Sync] EditorID preflight failed | plugin='{}' | path='{}' | {}",
                        plugin->GetFilename(),
                        path.string(),
                        error.what());
                    recordFailure(*plugin);
                }
            }
            if (!result.failedPlugins.empty())
            {
            logger::warn("[Window Sync] index stopped | EditorID preflight incomplete");
                return result;
            }
            logger::info(
                "[Window Sync] EditorID preflight | plugins={} | EditorIDs={}",
                plugins.size(),
                result.editorIDs.size());
            parseOptions.indexStaticEditorIDs = false;
        }
        if (a_options.preparePlacementFilter)
        {
            a_options.preparePlacementFilter(result.editorIDs);
        }
        for (const auto* plugin : plugins)
        {
            if (!plugin)
            {
                continue;
            }
            const auto path = PluginPath(*plugin);
            try
            {
                Result parsed;
                Parser parser(
                    *plugin,
                    parsed,
                    parseOptions,
                    std::addressof(result.placements));
                if (!parser.Parse(path))
                {
                    logger::warn(
                        "[Window Sync] index parse failed | plugin='{}' | path='{}' | {} | runtimeReferences=true",
                        plugin->GetFilename(),
                        path.string(),
                        parser.FailureReason());
                    recordFailure(*plugin);
                    continue;
                }
                bool totalLimitReached = false;
                for (auto& [formID, placement] :
                     parsed.placements)
                {
                    if (!result.placements.contains(formID) &&
                        result.placements.size() >=
                            kMaxTotalPlacements)
                    {
                        totalLimitReached = true;
                        break;
                    }
                    PluginRecords::MergePlacement(
                        result.placements,
                        formID,
                        std::move(placement));
                }
                for (auto& [formID, editorID] : parsed.editorIDs)
                {
                    result.editorIDs.insert_or_assign(
                        formID,
                        std::move(editorID));
                }
                if (totalLimitReached)
                {
                    logger::error(
                        "[Window Sync] index placement limit={} | plugin='{}' | path='{}' | runtimeReferences=true",
                        kMaxTotalPlacements,
                        plugin->GetFilename(),
                        path.string());
                    recordFailure(*plugin);
                    break;
                }
                result.referencesRead +=
                    parsed.referencesRead;
                result.compressedReferences +=
                    parsed.compressedReferences;
                result.minimallyScannedReferences +=
                    parsed.minimallyScannedReferences;
                result.placementRecordsDiscarded +=
                    parsed.placementRecordsDiscarded;
                result.referencePayloadBytesSkipped +=
                    parsed.referencePayloadBytesSkipped;
                result.exteriorCellGroupsSkipped +=
                    parsed.exteriorCellGroupsSkipped;
                result.excludedCellGroupsSkipped +=
                    parsed.excludedCellGroupsSkipped;
                result.cellGroupBytesSkipped +=
                    parsed.cellGroupBytesSkipped;
                ++result.pluginsParsed;
            }
            catch (const std::bad_alloc&)
            {
                logger::error(
                    "[Window Sync] index allocation exhausted | plugin='{}' | path='{}' | runtimeReferences=true",
                    plugin->GetFilename(),
                    path.string());
                recordFailure(*plugin);
            }
            catch (const std::length_error& error)
            {
                logger::error(
                    "[Window Sync] index allocation rejected | plugin='{}' | path='{}' | {}",
                    plugin->GetFilename(),
                    path.string(),
                    error.what());
                recordFailure(*plugin);
            }
            catch (const std::exception& error)
            {
                logger::error(
                    "[Window Sync] index failed | plugin='{}' | path='{}' | {}",
                    plugin->GetFilename(),
                    path.string(),
                    error.what());
                recordFailure(*plugin);
            }
        }
        result.complete =
            result.pluginsParsed == result.pluginsDiscovered &&
            result.failedPlugins.empty();
        if (result.complete && a_options.retainPlacement)
        {
            std::erase_if(
                result.placements,
                [&](const auto& a_entry)
                {
                    const auto& placement = a_entry.second;
                    return placement.deleted ||
                           !a_options.retainPlacement(
                               a_entry.first,
                               placement.base,
                               placement.cell);
                });
        }
        if (!result.failedPlugins.empty())
        {
            std::string failures;
            for (const auto& plugin : result.failedPlugins)
            {
                if (!failures.empty())
                {
                    failures.append(", ");
                }
                failures.append(plugin);
            }
            logger::warn(
                "[Window Sync] index partial | failedPlugins={}",
                failures);
        }
        logger::info(
            "[Window Sync] index | plugins={}/{} | references={} | retained={} | discarded={} | minimal={} | compressed={} | payloadBypassed={}B | exteriorGroups={} | excludedGroups={} | cellDataBypassed={}B",
            result.pluginsParsed,
            result.pluginsDiscovered,
            result.referencesRead,
            result.placements.size(),
            result.placementRecordsDiscarded,
            result.minimallyScannedReferences,
            result.compressedReferences,
            result.referencePayloadBytesSkipped,
            result.exteriorCellGroupsSkipped,
            result.excludedCellGroupsSkipped,
            result.cellGroupBytesSkipped);
        return result;
    }
}  // namespace MPL::PluginIndex
