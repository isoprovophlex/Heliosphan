#include <PluginIndex.h>
#include <PluginRecords.h>
#include <PluginIndexTesting.h>
#include <ParserDiagnostics.h>
#include <VanillaIndex.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
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
        constexpr auto kCell = Signature('C', 'E', 'L', 'L');
        constexpr auto kWorldspace = Signature('W', 'R', 'L', 'D');
        constexpr auto kStatic = Signature('S', 'T', 'A', 'T');
        constexpr auto kName = Signature('N', 'A', 'M', 'E');
        constexpr auto kExtendedSize = Signature('X', 'X', 'X', 'X');
        constexpr std::uint32_t kDeleted = 0x00000020;
        constexpr std::uint32_t kCompressed = 0x00040000;
        constexpr std::int32_t kTopLevel = 0;
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

        int MappedReadException(const EXCEPTION_POINTERS* a_exception, const char* a_source,
            const std::size_t a_count, DWORD& a_error)
        {
            const auto& record = *a_exception->ExceptionRecord;
            if (record.ExceptionCode != EXCEPTION_IN_PAGE_ERROR || record.NumberParameters < 3 ||
                record.ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH;
            const auto address = record.ExceptionInformation[1];
            const auto begin = reinterpret_cast<std::uintptr_t>(a_source);
            if (address < begin || address - begin >= a_count) return EXCEPTION_CONTINUE_SEARCH;
            a_error = static_cast<DWORD>(record.ExceptionInformation[2]);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        __declspec(noinline) bool CopyMappedBytes(char* a_destination, const char* a_source,
            const std::size_t a_count, DWORD& a_error
#ifdef ENABLE_COMMONLIBSSE_TESTING
            , const bool a_failRead
#endif
            )
        {
            __try
            {
#ifdef ENABLE_COMMONLIBSSE_TESTING
                if (a_failRead)
                {
                    std::memcpy(a_destination, a_source, a_count / 2);
                    const ULONG_PTR information[]{0, reinterpret_cast<ULONG_PTR>(a_source), ERROR_READ_FAULT};
                    RaiseException(EXCEPTION_IN_PAGE_ERROR, 0, 3, information);
                }
#endif
                std::memcpy(a_destination, a_source, a_count);
                return true;
            }
            __except (MappedReadException(GetExceptionInformation(), a_source, a_count, a_error))
            {
                return false;
            }
        }

        class MappedFile
        {
        public:
            MappedFile() = default;
            ~MappedFile() { Close(); }
            MappedFile(const MappedFile&) = delete;
            MappedFile& operator=(const MappedFile&) = delete;

            bool Open(const std::filesystem::path& a_path, const std::uint64_t a_length, DWORD& a_error)
            {
                file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) { a_error = GetLastError(); return false; }
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(file, &size)) { a_error = GetLastError(); return false; }
                if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != a_length)
                { a_error = ERROR_FILE_INVALID; return false; }
                mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (!mapping) { a_error = GetLastError(); return false; }
                bytes = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
                if (!bytes) { a_error = GetLastError(); return false; }
                return true;
            }

            void Close()
            {
                if (bytes) { UnmapViewOfFile(bytes); bytes = nullptr; }
                if (mapping) { CloseHandle(mapping); mapping = nullptr; }
                if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
            }

            const char* Data() const { return bytes; }

        private:
            HANDLE file = INVALID_HANDLE_VALUE;
            HANDLE mapping = nullptr;
            const char* bytes = nullptr;
        };

        class Reader
        {
        public:
            Reader(Diagnostics::InputStream& a_stream, const std::uint64_t a_length,
                const std::filesystem::path& a_path
#ifdef ENABLE_COMMONLIBSSE_TESTING
                , Testing::ReaderControl* a_control
#endif
                ) :
                stream(a_stream), length(a_length), path(a_path)
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                metrics = a_stream.metrics;
#endif
#ifdef ENABLE_COMMONLIBSSE_TESTING
                control = a_control;
                if (control && control->streamOnly) return;
#endif
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::Duration timer(metrics ? &metrics->mappingNs : nullptr);
#endif
                if (!length)
                {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    if (metrics) metrics->mappingReason = "empty-file";
#endif
                    return;
                }
                DWORD error = ERROR_SUCCESS;
#ifdef ENABLE_COMMONLIBSSE_TESTING
                const bool injectedFailure = control && control->failMapping;
                if (injectedFailure) error = ERROR_NOT_ENOUGH_MEMORY;
                const bool opened = !injectedFailure && mapped.Open(a_path, length, error);
#else
                const bool opened = mapped.Open(a_path, length, error);
#endif
                if (!opened) mapped.Close();
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                if (metrics)
                {
                    metrics->readerSource = opened ? "mapped" : "stream";
                    metrics->mappingReason = opened ? "read-only" : "mapping-unavailable";
                    metrics->mappingError = error;
                }
#endif
#ifdef ENABLE_COMMONLIBSSE_TESTING
                if (control) control->mapped = opened;
#endif
            }

            ~Reader()
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::Duration timer(metrics ? &metrics->unmappingNs : nullptr);
#endif
                mapped.Close();
            }

            Reader(const Reader&) = delete;
            Reader& operator=(const Reader&) = delete;

            std::uint64_t Position() const { return position; }

            bool Read(char* a_data, const std::streamsize a_count)
            {
                if (a_count < 0 || static_cast<std::uint64_t>(a_count) > length - position) return false;
                if (!a_count) return true;
                if (mapped.Data())
                {
                    DWORD error = ERROR_SUCCESS;
                    bool copied;
                    {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                        Diagnostics::SampleScope timer(metrics ? &metrics->mappedReads : nullptr);
#endif
                        copied = CopyMappedBytes(a_data, mapped.Data() + position, static_cast<std::size_t>(a_count), error
#ifdef ENABLE_COMMONLIBSSE_TESTING
                            , control && control->failReadAt && position + a_count > *control->failReadAt
#endif
                            );
                    }
                    if (copied)
                    {
                        position += static_cast<std::uint64_t>(a_count);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                        if (metrics) metrics->mappedBytes += static_cast<std::uint64_t>(a_count);
#endif
                        return true;
                    }
                    mapped.Close();
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    if (metrics)
                    {
                        metrics->readerSource = "mapped-then-stream";
                        metrics->mappingReason = "mapped-read-failed";
                        metrics->mappingError = error;
                    }
#endif
                    logger::warn("[Window Sync] index mapped read failed | plugin={} | offset={} | status={:08X} | retry=stream",
                        path.filename().string(), position, error);
                    stream.clear();
                    stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
#ifdef ENABLE_COMMONLIBSSE_TESTING
                    if (control)
                    {
                        control->recovered = true;
                        if (control->failRecovery) stream.setstate(std::ios::badbit);
                    }
#endif
                    if (!stream) return false;
                }
                stream.read(a_data, a_count);
                position += static_cast<std::uint64_t>(stream.gcount());
                return static_cast<bool>(stream);
            }

            bool SeekTo(const std::uint64_t a_target)
            {
                if (a_target < position || a_target > length) return false;
                if (mapped.Data()) { position = a_target; return true; }
                if (!stream) return false;
                if (a_target != position)
                {
                    stream.seekg(static_cast<std::streamoff>(a_target), std::ios::beg);
                    if (!stream) return false;
                    position = a_target;
                }
                return true;
            }

            bool Skip(const std::uint64_t a_count)
            {
                return a_count <= length - position && SeekTo(position + a_count);
            }

#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            Diagnostics::Metrics* metrics = nullptr;
#endif

        private:
            Diagnostics::InputStream& stream;
            const std::uint64_t length;
            const std::filesystem::path& path;
            std::uint64_t position = 0;
            MappedFile mapped;
#ifdef ENABLE_COMMONLIBSSE_TESTING
            Testing::ReaderControl* control = nullptr;
#endif
        };

        template <class T>
        bool Read(Reader& a_stream, T& a_value)
        {
            return a_stream.Read(reinterpret_cast<char*>(std::addressof(a_value)), sizeof(T));
        }

        struct RecordBuffers
        {
            std::vector<std::byte> stored;
            std::vector<std::byte> decompressed;
        };

        std::optional<std::span<const std::byte>> ReadRecordData(
            Reader& a_stream,
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
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            if (a_stream.metrics) a_stream.metrics->maxStoredCapacity = std::max<std::uint64_t>(
                a_stream.metrics->maxStoredCapacity, a_buffers.stored.capacity());
#endif
            if (!a_buffers.stored.empty() &&
                !a_stream.Read(
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
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            if (a_stream.metrics) a_stream.metrics->maxInflatedCapacity = std::max<std::uint64_t>(
                a_stream.metrics->maxInflatedCapacity, a_buffers.decompressed.capacity());
            Diagnostics::SampleScope inflateTimer(a_stream.metrics ? &a_stream.metrics->inflates : nullptr);
#endif
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
            Reader& a_stream,
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
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                if (a_stream.metrics) ++a_stream.metrics->subrecordHeaders;
#endif
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
                if (!a_stream.Skip(size))
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

        struct PluginSource
        {
            const RE::TESFile& file;

            RE::FormID GetRuntimeFormID(const RE::FormID a_formID) const
            {
                return file.GetRuntimeFormID(a_formID);
            }

            std::string_view GetFilename() const
            {
                return file.GetFilename();
            }

            bool IsExteriorCell(const RE::FormID a_cellID) const
            {
                const auto* cell = a_cellID ?
                    RE::TESForm::LookupByID<RE::TESObjectCELL>(a_cellID) :
                    nullptr;
                return cell && !cell->IsInteriorCell();
            }
        };

        template <class Source>
        class Parser
        {
        public:
            Parser(
                Source a_file,
                Result& a_result,
                const BuildOptions& a_options,
                const std::unordered_map<RE::FormID, Placement>*
                    a_winningPlacements = nullptr,
                VanillaIndex::Catalog* a_vanilla = nullptr) :
                file(std::move(a_file)),
                result(a_result),
                options(a_options),
                winningPlacements(a_winningPlacements),
                vanilla(a_vanilla)
            {}

            bool Parse(const std::filesystem::path& a_path
#ifdef ENABLE_COMMONLIBSSE_TESTING
                , Testing::ReaderControl* a_control = nullptr
#endif
                )
            {
                stream.open(a_path, std::ios::binary);
                if (!stream)
                {
                    return Fail("the plugin file could not be opened");
                }
                std::streampos length;
                {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    Diagnostics::Duration sizeTimer(diagnostics ? &diagnostics->metadataNs : nullptr);
#endif
                    stream.seekg(0, std::ios::end);
                    length = stream.tellg();
                    if (length < 0) return Fail("the plugin file length could not be read");
                    stream.seekg(0, std::ios::beg);
                }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                if (diagnostics) diagnostics->fileBytes = static_cast<std::uint64_t>(length);
                Diagnostics::Duration scanTimer(diagnostics ? &diagnostics->scanNs : nullptr);
#endif
                if (vanilla && VanillaIndex::IsVanillaMaster(file.GetFilename()))
                {
                    const auto lookup = vanilla->Acquire(file.GetFilename(), static_cast<std::uint64_t>(length), stream);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    if (diagnostics)
                    {
                        diagnostics->indexSource = lookup.data ? "embedded" : "scan";
                        diagnostics->validationMs = lookup.validationMs; diagnostics->decodeMs = lookup.decodeMs;
                        diagnostics->validatedBytes = lookup.validatedBytes; diagnostics->indexReused = lookup.reused;
                        diagnostics->indexReason = lookup.reason; diagnostics->fingerprint = lookup.fingerprint;
                    }
#endif
                    if (options.detailedLogging)
                    {
                        logger::info("[Vanilla Index] plugin={} | source={} | validation={:.3f}ms | decode={:.3f}ms | reused={} | reason={} | sha256={}",
                            file.GetFilename(), lookup.data ? "embedded" : "scan", lookup.validationMs, lookup.decodeMs,
                            lookup.reused, lookup.reason, lookup.fingerprint.empty() ? "<not-hashed>" : lookup.fingerprint);
                    }
                    if (lookup.data) return ApplyPrepared(*lookup.data);
                }
                Reader reader(stream, static_cast<std::uint64_t>(length), a_path
#ifdef ENABLE_COMMONLIBSSE_TESTING
                    , a_control
#endif
                    );
                return ParseRange(
                    reader,
                    static_cast<std::uint64_t>(length),
                    std::nullopt);
            }

            const std::string& FailureReason() const
            {
                return failureReason;
            }

            bool ApplyPrepared(const VanillaIndex::Dataset& a_data)
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::Duration applyTimer(diagnostics ? &diagnostics->preparedApplyNs : nullptr);
#endif
                result.embeddedPlugins = 1;
                if (options.indexStaticEditorIDs)
                {
                    for (const auto& [local, name] : a_data.editorIDs)
                    {
                        const auto form = RuntimeFormID(local);
                        if (form) result.editorIDs.insert_or_assign(form, name);
                    }
                }
                if (!options.indexReferences) return true;
                result.embeddedPlacements = a_data.placements.size();
                struct Cell { RE::FormID runtime = 0; bool skip = false; };
                std::unordered_map<RE::FormID, Cell> cells;
                for (const auto& placement : a_data.placements)
                {
                    const auto [entry, added] = cells.try_emplace(placement.cell);
                    auto& cell = entry->second;
                    if (added)
                    {
                        cell.runtime = RuntimeFormID(placement.cell);
                        cell.skip = (options.skipExteriorCells && IsExteriorCell(cell.runtime)) ||
                            (cell.runtime && options.excludedCells.contains(cell.runtime));
                    }
                    if (cell.skip)
                    {
                        if (result.placements.empty() && (!winningPlacements || winningPlacements->empty())) continue;
                        ++result.referencesRead;
                        const auto reference = RuntimeFormID(placement.reference);
                        if (reference && HasRetainedPlacement(reference) &&
                            !StorePlacement(reference, Placement{.cell = cell.runtime, .deleted = true})) return false;
                        continue;
                    }
                    ++result.referencesRead;
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    Diagnostics::SampleScope referenceTimer(diagnostics ? &diagnostics->references : nullptr);
#endif
                    const auto reference = RuntimeFormID(placement.reference);
                    const auto base = placement.base ? RuntimeFormID(placement.base) : 0;
                    if (reference && cell.runtime && !SelectPlacement(reference, base, cell.runtime, placement.deleted)) return false;
                }
                return true;
            }

#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            void SetDiagnostics(Diagnostics::Metrics* a_metrics)
            {
                diagnostics = a_metrics;
                stream.metrics = a_metrics;
            }
#endif

        private:
            bool HasRetainedPlacement(const RE::FormID a_reference) const
            {
                return result.placements.contains(a_reference) ||
                    (winningPlacements && winningPlacements->contains(a_reference));
            }

            bool SelectPlacement(const RE::FormID a_reference, const RE::FormID a_base,
                const RE::FormID a_cell, const bool a_deleted)
            {
                bool selected = true;
                if (options.retainPlacement)
                {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    Diagnostics::SampleScope filterTimer(diagnostics ? &diagnostics->filters : nullptr);
#endif
                    selected = options.retainPlacement(a_reference, a_base, a_cell);
                }
                if (!selected && !HasRetainedPlacement(a_reference))
                {
                    ++result.placementRecordsDiscarded;
                    return true;
                }
                return StorePlacement(a_reference, Placement{.base = a_base, .cell = a_cell, .deleted = a_deleted});
            }

            RE::FormID RuntimeFormID(const RE::FormID a_formID) const
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::SampleScope timer(diagnostics ? &diagnostics->formIDs : nullptr);
#endif
                return file.GetRuntimeFormID(a_formID);
            }

            bool IsExteriorCell(const RE::FormID a_cellID) const
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::SampleScope timer(diagnostics ? &diagnostics->exteriorLookups : nullptr);
#endif
                return file.IsExteriorCell(a_cellID);
            }

            bool Fail(const std::string_view a_reason)
            {
                if (failureReason.empty())
                {
                    failureReason = a_reason;
                }
                return false;
            }

            bool ParseRange(
                Reader& a_reader,
                const std::uint64_t a_end,
                const std::optional<RE::FormID> a_cell,
                const bool a_skipPlacements = false)
            {
                while (true)
                {
                    const auto start = a_reader.Position();
                    if (start == a_end)
                    {
                        return true;
                    }
                    if (start > a_end || a_end - start < sizeof(RecordHeader))
                    {
                        return Fail("a record extends outside its containing group");
                    }

                    RecordHeader header{};
                    if (!Read(a_reader, header))
                    {
                        return Fail("a record or group header could not be read");
                    }

                    if (header.signature == kGroup)
                    {
                        const auto group = std::bit_cast<GroupHeader>(header);
                        if (group.size < sizeof(GroupHeader) ||
                            group.size > a_end - start)
                        {
                            return Fail("a group header has an invalid size");
                        }
                        const auto groupEnd = start + group.size;
                        const bool skipTopLevel = group.type == kTopLevel &&
                            !(options.indexReferences && (group.label == kCell || group.label == kWorldspace)) &&
                            !(options.indexStaticEditorIDs && group.label == kStatic);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                        if (diagnostics) ++diagnostics->groupHeaders;
                        if (group.type == kTopLevel) Diagnostics::ObserveGroup(diagnostics, group.label,
                            group.size - sizeof(GroupHeader), skipTopLevel);
#endif
                        if (skipTopLevel)
                        {
                            ++result.topLevelGroupsSkipped;
                            result.topLevelGroupBytesSkipped += group.size - sizeof(GroupHeader);
                            if (!a_reader.SeekTo(groupEnd)) return Fail("a skipped group could not be reached");
                            continue;
                        }
                        auto cell = a_cell;
                        bool skipPlacements = a_skipPlacements;
                        if (group.type == kCellChildren)
                        {
                            const auto cellID =
                                RuntimeFormID(group.label);
                            const bool exterior =
                                options.skipExteriorCells &&
                                IsExteriorCell(cellID);
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
                                const bool hasRetainedPlacements =
                                    !result.placements.empty() ||
                                    (winningPlacements && !winningPlacements->empty());
                                if (!options.indexReferences || !hasRetainedPlacements)
                                {
                                    result.cellGroupBytesSkipped +=
                                        group.size - sizeof(GroupHeader);
                                    if (!a_reader.SeekTo(groupEnd)) return Fail("a skipped cell group could not be reached");
                                    continue;
                                }
                                skipPlacements = true;
                            }
                            cell = group.label;
                        }
                        if (!ParseRange(a_reader, groupEnd, cell, skipPlacements))
                        {
                            return false;
                        }
                        continue;
                    }

                    if (header.dataSize > a_end - start - sizeof(header))
                    {
                        return Fail("a record header has an invalid data size");
                    }
                    const auto recordEnd =
                        start + sizeof(header) + header.dataSize;
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    if (diagnostics) ++diagnostics->recordHeaders;
#endif
                    if (a_skipPlacements)
                    {
                        result.cellGroupBytesSkipped += header.dataSize;
                        if (options.indexReferences &&
                            header.signature == kReference && a_cell)
                        {
                            ++result.referencesRead;
                            const auto reference = RuntimeFormID(header.formID);
                            if (reference && HasRetainedPlacement(reference))
                            {
                                // Suppress the older placement without decoding the skipped payload.
                                if (!StorePlacement(reference, Placement{
                                        .cell = RuntimeFormID(*a_cell),
                                        .deleted = true,
                                    }))
                                {
                                    return false;
                                }
                            }
                        }
                        if (!a_reader.SeekTo(recordEnd)) return Fail("a skipped record could not be reached");
                        continue;
                    }
                    if (options.indexReferences &&
                        header.signature == kReference && a_cell)
                    {
                        ++result.referencesRead;
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                        Diagnostics::SampleScope referenceTimer(diagnostics ? &diagnostics->references : nullptr);
#endif
                        std::optional<RE::FormID> base;
                        if ((header.flags & kCompressed) != 0)
                        {
                            const auto data = ReadRecordData(
                                a_reader,
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
                                ReadUncompressedBaseForm(a_reader, header);
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
                            RuntimeFormID(header.formID);
                        const auto cell =
                            RuntimeFormID(*a_cell);
                        if (reference && cell)
                        {
                            const auto runtimeBase =
                                base && *base ?
                                    RuntimeFormID(*base) :
                                    0;
                            if (!SelectPlacement(reference, runtimeBase, cell, (header.flags & kDeleted) != 0))
                            {
                                return false;
                            }
                        }
                    }
                    else if (options.indexStaticEditorIDs &&
                              header.signature == kStatic)
                    {
                        std::size_t compressed = 0;
                        const auto data = ReadRecordData(
                            a_reader,
                            header,
                            compressed,
                            buffers);
                        if (!data)
                        {
                            return Fail(
                                "a static record could not be read or decompressed");
                        }
                        const auto formID =
                            RuntimeFormID(header.formID);
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
                    if (!a_reader.SeekTo(recordEnd)) return Fail("the next record could not be reached");
                }
            }

            bool StorePlacement(const RE::FormID a_reference, const Placement a_placement)
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::SampleScope storeTimer(diagnostics ? &diagnostics->stores : nullptr);
#endif
                if (!result.placements.contains(a_reference) &&
                    result.placements.size() >= kMaxPlacementsPerPlugin)
                {
                    logger::warn(
                        "[Window Sync] index stopped | placements={} | plugin='{}'",
                        kMaxPlacementsPerPlugin,
                        file.GetFilename());
                    return Fail("the per-plugin placement limit was reached");
                }
                PluginRecords::MergePlacement(result.placements, a_reference, a_placement);
                return true;
            }

            Source file;
            Result& result;
            const BuildOptions& options;
            const std::unordered_map<RE::FormID, Placement>*
                winningPlacements;
            VanillaIndex::Catalog* vanilla;
            RecordBuffers buffers;
            Diagnostics::InputStream stream;
            std::string failureReason;
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            Diagnostics::Metrics* diagnostics = nullptr;
#endif
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

        std::optional<std::vector<const RE::TESFile*>> LoadedPlugins()
        {
            std::vector<const RE::TESFile*> plugins;
            auto* dataHandler =
                RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                return std::nullopt;
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
            std::vector<const RE::TESFile*> loadOrder;
            for (const auto* file : dataHandler->files)
            {
                loadOrder.push_back(file);
            }
            return PluginRecords::OrderActivePlugins<const RE::TESFile*>(plugins, loadOrder);
        }
    }  // namespace

#ifdef ENABLE_COMMONLIBSSE_TESTING
    bool Testing::Parse(
        const std::filesystem::path& a_path,
        const BuildOptions& a_options,
        Result& a_result,
        const std::unordered_map<RE::FormID, Placement>& a_winningPlacements,
        const std::unordered_set<RE::FormID>& a_exteriorCells
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
        , Diagnostics::Metrics* a_diagnostics
#endif
        , ReaderControl* a_readerControl
        , VanillaIndex::Catalog* a_catalog
        )
    {
        struct FixtureSource
        {
            const std::unordered_set<RE::FormID>& exteriorCells;
            std::string filename;

            RE::FormID GetRuntimeFormID(const RE::FormID a_formID) const
            {
                return a_formID;
            }

            std::string_view GetFilename() const
            {
                return filename;
            }

            bool IsExteriorCell(const RE::FormID a_cellID) const
            {
                return exteriorCells.contains(a_cellID);
            }
        };
        Parser parser(FixtureSource{ a_exteriorCells, a_path.filename().string() }, a_result, a_options, &a_winningPlacements, a_catalog);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
        parser.SetDiagnostics(a_diagnostics);
#endif
        return parser.Parse(a_path, a_readerControl);
    }

    bool Testing::ApplyDataset(const VanillaIndex::Dataset& a_data,
        const BuildOptions& a_options, Result& a_result,
        const std::unordered_map<RE::FormID, Placement>& a_winningPlacements,
        const std::unordered_set<RE::FormID>& a_exteriorCells, const RE::FormID a_mappingOffset)
    {
        struct FixtureSource
        {
            const std::unordered_set<RE::FormID>& exteriorCells;
            RE::FormID offset;
            RE::FormID GetRuntimeFormID(const RE::FormID a_formID) const { return a_formID ? a_formID + offset : 0; }
            const char* GetFilename() const { return "fixture.esp"; }
            bool IsExteriorCell(const RE::FormID a_cellID) const { return exteriorCells.contains(a_cellID); }
        };
        Parser parser(FixtureSource{a_exteriorCells, a_mappingOffset}, a_result, a_options, &a_winningPlacements);
        return parser.ApplyPrepared(a_data);
    }
#endif

    Result Build(const BuildOptions& a_options)
    {
        Result result;
        VanillaIndex::Catalog vanilla;
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
        Diagnostics::Session diagnostics;
        if (a_options.detailedLogging)
        {
            logger::info("[Parser Profile] diagnostic build | enabled=true | timing=sampled | initialSamples={} | stride={} | reports=SKSE-log-directory",
                Diagnostics::kInitialSamples, Diagnostics::kSampleStride);
        }
#endif
        const auto loadedPlugins = [&]
        {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            Diagnostics::Duration timer(&diagnostics.enumerateNs);
#endif
            return LoadedPlugins();
        }();
        if (!loadedPlugins)
        {
            logger::error("[Window Sync] index stopped | active plugin load order unavailable");
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            diagnostics.Finish(a_options, result);
#endif
            return result;
        }
        const auto& plugins = *loadedPlugins;
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
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            std::size_t profiledPluginOrder = 0;
#endif
            for (const auto* plugin : plugins)
            {
                if (!plugin)
                {
                    continue;
                }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                auto& row = diagnostics.rows.emplace_back();
                row.plugin = Diagnostics::UTF8Path(plugin->GetFilename()); row.phase = "editor_id_preflight"; row.ordinal = ++profiledPluginOrder;
                Diagnostics::PluginScope pluginTimer(row);
#endif
                const auto path = [&]
                {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    Diagnostics::Duration timer(&row.metrics.pathNs);
#endif
                    return PluginPath(*plugin);
                }();
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                row.path = Diagnostics::UTF8Path(path);
#endif
                try
                {
                    Result parsed;
                    Parser parser(
                        PluginSource{ *plugin },
                        parsed,
                        preflightOptions, nullptr, &vanilla);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    parser.SetDiagnostics(&row.metrics);
#endif
                    const bool succeeded = parser.Parse(path);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    row.Capture(parsed, succeeded, parser.FailureReason());
#endif
                    if (!succeeded)
                    {
                        logger::warn(
                    "[Window Sync] EditorID preflight parse failed | plugin='{}' | path='{}' | {}",
                            plugin->GetFilename(),
                            path.string(),
                            parser.FailureReason());
                        recordFailure(*plugin);
                        continue;
                    }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    Diagnostics::Duration mergeTimer(&row.metrics.mergeNs);
#endif
                    result.topLevelGroupsSkipped += parsed.topLevelGroupsSkipped;
                    result.topLevelGroupBytesSkipped += parsed.topLevelGroupBytesSkipped;
                    for (auto& [formID, editorID] : parsed.editorIDs)
                    {
                        result.editorIDs.insert_or_assign(
                            formID,
                            std::move(editorID));
                    }
                }
                catch (const std::exception& error)
                {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                    row.failure = error.what();
#endif
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
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                diagnostics.Finish(a_options, result);
#endif
                return result;
            }
            if (a_options.detailedLogging)
            {
                logger::info(
                    "[Window Sync] EditorID preflight | plugins={} | EditorIDs={}",
                    plugins.size(),
                    result.editorIDs.size());
            }
            parseOptions.indexStaticEditorIDs = false;
        }
        if (a_options.preparePlacementFilter)
        {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            Diagnostics::Duration timer(&diagnostics.prepareFilterNs);
#endif
            a_options.preparePlacementFilter(result.editorIDs);
        }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
        std::size_t profiledPluginOrder = 0;
#endif
        for (const auto* plugin : plugins)
        {
            if (!plugin)
            {
                continue;
            }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            auto& row = diagnostics.rows.emplace_back();
            row.plugin = Diagnostics::UTF8Path(plugin->GetFilename()); row.phase = "placements"; row.ordinal = ++profiledPluginOrder;
            Diagnostics::PluginScope pluginTimer(row);
#endif
            const auto path = [&]
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::Duration timer(&row.metrics.pathNs);
#endif
                return PluginPath(*plugin);
            }();
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            row.path = Diagnostics::UTF8Path(path);
#endif
            try
            {
                Result parsed;
                Parser parser(
                    PluginSource{ *plugin },
                    parsed,
                    parseOptions,
                    std::addressof(result.placements), &vanilla);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                parser.SetDiagnostics(&row.metrics);
#endif
                const bool succeeded = parser.Parse(path);
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                row.Capture(parsed, succeeded, parser.FailureReason());
#endif
                if (!succeeded)
                {
                    logger::warn(
                        "[Window Sync] index parse failed | plugin='{}' | path='{}' | {} | runtimeReferences=true",
                        plugin->GetFilename(),
                        path.string(),
                        parser.FailureReason());
                    recordFailure(*plugin);
                    continue;
                }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                Diagnostics::Duration mergeTimer(&row.metrics.mergeNs);
#endif
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
                result.topLevelGroupsSkipped += parsed.topLevelGroupsSkipped;
                result.topLevelGroupBytesSkipped += parsed.topLevelGroupBytesSkipped;
                result.embeddedPlugins += parsed.embeddedPlugins;
                result.embeddedPlacements += parsed.embeddedPlacements;
                ++result.pluginsParsed;
            }
            catch (const std::bad_alloc&)
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                row.failure = "allocation exhausted";
#endif
                logger::error(
                    "[Window Sync] index allocation exhausted | plugin='{}' | path='{}' | runtimeReferences=true",
                    plugin->GetFilename(),
                    path.string());
                recordFailure(*plugin);
            }
            catch (const std::length_error& error)
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                row.failure = error.what();
#endif
                logger::error(
                    "[Window Sync] index allocation rejected | plugin='{}' | path='{}' | {}",
                    plugin->GetFilename(),
                    path.string(),
                    error.what());
                recordFailure(*plugin);
            }
            catch (const std::exception& error)
            {
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
                row.failure = error.what();
#endif
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
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
            Diagnostics::Duration timer(&diagnostics.pruneNs);
#endif
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
        if (a_options.detailedLogging)
        {
            logger::info(
                "[Window Sync] index | plugins={}/{} | references={} | retained={} | discarded={} | minimal={} | compressed={} | payloadBypassed={}B | exteriorGroups={} | excludedGroups={} | cellDataBypassed={}B | topGroupsSkipped={} | topGroupDataBypassed={}B | embeddedPlugins={} | embeddedPlacements={}",
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
                result.cellGroupBytesSkipped,
                result.topLevelGroupsSkipped,
                result.topLevelGroupBytesSkipped,
                result.embeddedPlugins,
                result.embeddedPlacements);
        }
#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
        diagnostics.Finish(a_options, result);
#endif
        return result;
    }
}  // namespace MPL::PluginIndex
