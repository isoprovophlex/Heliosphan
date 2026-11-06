#include <ParserDiagnostics.h>

#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
#include <algorithm>
#include <atomic>
#include <nlohmann/json.hpp>
#include <optional>

namespace MPL::PluginIndex::Diagnostics
{
    namespace
    {
        using Json = nlohmann::ordered_json;
        std::uint64_t Nanoseconds(const Clock::duration a_value)
        { return std::chrono::duration_cast<std::chrono::nanoseconds>(a_value).count(); }
        double Milliseconds(const std::uint64_t a_value) { return static_cast<double>(a_value) / 1'000'000.0; }

        std::optional<std::uint64_t> ThreadCPU()
        {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return std::nullopt;
            const auto value = [](const FILETIME& a_time)
            { return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime; };
            return value(kernel) + value(user);
        }

        std::string SignatureText(const std::uint32_t a_label)
        {
            std::string value(4, ' ');
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                const auto character = static_cast<unsigned char>(a_label >> (index * 8));
                value[index] = character >= 32 && character <= 126 ? static_cast<char>(character) : '?';
            }
            return value;
        }

        Json SampleJSON(const Sample& a_sample)
        {
            return { {"calls", a_sample.calls}, {"initial_exact_ns", a_sample.initialNs},
                {"tail_sample_ns", a_sample.tailNs}, {"tail_samples", a_sample.tailSamples},
                {"estimated_ms", a_sample.EstimatedMs()} };
        }

        Json RowJSON(const Row& a_row)
        {
            const auto& m = a_row.metrics;
            Json groups = Json::array();
            for (const auto& group : m.groups)
                groups.push_back({ {"type", SignatureText(group.label)}, {"count", group.count},
                    {"payload_bytes", group.bytes}, {"skipped", group.skipped} });
            const auto phaseNs = m.pathNs + m.openNs + m.metadataNs + m.scanNs + m.closeNs + m.mergeNs;
            return { {"plugin", a_row.plugin}, {"pass", a_row.phase}, {"order", a_row.ordinal},
                {"path", a_row.path}, {"parsed", a_row.parsed}, {"failure", a_row.failure},
                {"wall_ms", Milliseconds(a_row.wallNs)},
                {"thread_cpu_ms", a_row.cpuValid ? Json(static_cast<double>(a_row.cpu100ns) / 10'000.0) : Json(nullptr)},
                {"file_bytes", m.fileBytes},
                {"reader", { {"source", m.readerSource}, {"reason", m.mappingReason}, {"error", m.mappingError},
                    {"map_ms", Milliseconds(m.mappingNs)}, {"unmap_ms", Milliseconds(m.unmappingNs)},
                    {"copied_bytes", m.mappedBytes}, {"mapped_reads", SampleJSON(m.mappedReads)} }},
                {"index_source", m.indexSource},
                {"embedded_index", { {"reason", m.indexReason}, {"fingerprint", m.fingerprint},
                    {"validation_ms", m.validationMs}, {"decode_ms", m.decodeMs},
                    {"apply_ms", Milliseconds(m.preparedApplyNs)}, {"validated_bytes", m.validatedBytes}, {"reused", m.indexReused} }},
                {"phases_ms", { {"path_resolution", Milliseconds(m.pathNs)}, {"open", Milliseconds(m.openNs)},
                    {"size_query", Milliseconds(m.metadataNs)}, {"scan", Milliseconds(m.scanNs)},
                    {"close", Milliseconds(m.closeNs)}, {"merge", Milliseconds(m.mergeNs)},
                    {"remaining_bookkeeping_and_cleanup", Milliseconds(a_row.wallNs > phaseNs ? a_row.wallNs - phaseNs : 0)} }},
                {"stream", { {"requested_bytes", m.requestedBytes}, {"returned_bytes", m.returnedBytes},
                    {"absolute_seeks", m.absoluteSeeks}, {"relative_seeks", m.relativeSeeks},
                    {"signature_rewinds", m.signatureRewinds}, {"group_headers", m.groupHeaders},
                    {"record_headers", m.recordHeaders}, {"subrecord_headers", m.subrecordHeaders} }},
                {"hot_operations", { {"reads", SampleJSON(m.reads)}, {"seeks", SampleJSON(m.seeks)},
                    {"tells", SampleJSON(m.tells)}, {"runtime_form_ids", SampleJSON(m.formIDs)},
                    {"exterior_cell_lookups", SampleJSON(m.exteriorLookups)},
                    {"reference_processing_inclusive", SampleJSON(m.references)},
                    {"placement_filters", SampleJSON(m.filters)}, {"placement_stores", SampleJSON(m.stores)},
                    {"decompression", SampleJSON(m.inflates)} }},
                {"results", { {"references", a_row.references}, {"placements", a_row.placements},
                    {"editor_ids", a_row.editorIDs}, {"compressed", a_row.compressed}, {"minimal", a_row.minimal},
                    {"discarded", a_row.discarded}, {"exterior_groups_skipped", a_row.exteriorSkipped},
                    {"excluded_groups_skipped", a_row.excludedSkipped}, {"top_groups_skipped", a_row.topSkipped},
                    {"reference_payload_skipped_bytes", a_row.payloadSkipped},
                    {"cell_payload_skipped_bytes", a_row.cellBytesSkipped}, {"top_payload_skipped_bytes", a_row.topBytesSkipped},
                    {"stored_buffer_capacity_bytes", m.maxStoredCapacity},
                    {"inflated_buffer_capacity_bytes", m.maxInflatedCapacity} }},
                {"top_level_groups", std::move(groups)} };
        }
    }

    double Sample::EstimatedMs() const
    {
        const auto tailCalls = calls > kInitialSamples ? calls - kInitialSamples : 0;
        return static_cast<double>(initialNs) / 1'000'000.0 +
            (tailSamples ? static_cast<double>(tailNs) / tailSamples * tailCalls / 1'000'000.0 : 0.0);
    }

    Duration::Duration(std::uint64_t* a_destination) : destination(a_destination)
    { if (destination) started = Clock::now(); }
    Duration::~Duration()
    { if (destination) *destination += Nanoseconds(Clock::now() - started); }

    SampleScope::SampleScope(Sample* a_sample) : sample(a_sample)
    {
        if (!sample) return;
        const auto call = ++sample->calls;
        initial = call <= kInitialSamples;
        measured = initial || (call - kInitialSamples - 1) % kSampleStride == 0;
        if (measured) started = Clock::now();
    }
    SampleScope::~SampleScope()
    {
        if (!measured) return;
        const auto elapsed = Nanoseconds(Clock::now() - started);
        if (initial) sample->initialNs += elapsed;
        else { sample->tailNs += elapsed; ++sample->tailSamples; }
    }

    InputStream::~InputStream() { if (is_open()) close(); }
    void InputStream::open(const std::filesystem::path& a_path, const std::ios::openmode a_mode)
    {
        Duration timer(metrics ? &metrics->openNs : nullptr);
        std::ifstream::open(a_path, a_mode);
    }
    InputStream& InputStream::read(char* a_data, const std::streamsize a_count)
    {
        SampleScope timer(metrics ? &metrics->reads : nullptr);
        std::ifstream::read(a_data, a_count);
        if (metrics)
        {
            if (a_count > 0) metrics->requestedBytes += static_cast<std::uint64_t>(a_count);
            if (gcount() > 0) metrics->returnedBytes += static_cast<std::uint64_t>(gcount());
        }
        return *this;
    }
    InputStream& InputStream::seekg(const std::streamoff a_offset, const std::ios::seekdir a_direction)
    {
        SampleScope timer(metrics ? &metrics->seeks : nullptr);
        if (metrics) { if (a_direction == std::ios::cur) ++metrics->relativeSeeks; else ++metrics->absoluteSeeks; }
        std::ifstream::seekg(a_offset, a_direction);
        return *this;
    }
    std::streampos InputStream::tellg()
    {
        SampleScope timer(metrics ? &metrics->tells : nullptr);
        return std::ifstream::tellg();
    }
    void InputStream::close()
    {
        Duration timer(metrics ? &metrics->closeNs : nullptr);
        std::ifstream::close();
    }

    void Row::Capture(const Result& a_result, const bool a_success, const std::string_view a_failure)
    {
        parsed = a_success; failure = a_failure;
        references = a_result.referencesRead; placements = a_result.placements.size(); editorIDs = a_result.editorIDs.size();
        compressed = a_result.compressedReferences; minimal = a_result.minimallyScannedReferences;
        discarded = a_result.placementRecordsDiscarded; payloadSkipped = a_result.referencePayloadBytesSkipped;
        exteriorSkipped = a_result.exteriorCellGroupsSkipped; excludedSkipped = a_result.excludedCellGroupsSkipped;
        cellBytesSkipped = a_result.cellGroupBytesSkipped; topSkipped = a_result.topLevelGroupsSkipped;
        topBytesSkipped = a_result.topLevelGroupBytesSkipped;
    }
    PluginScope::PluginScope(Row& a_row) : row(a_row), started(Clock::now())
    {
        const auto cpu = ThreadCPU(); cpuValid = cpu.has_value(); cpuStarted = cpu.value_or(0);
    }
    PluginScope::~PluginScope()
    {
        row.wallNs = Nanoseconds(Clock::now() - started);
        const auto cpu = ThreadCPU(); row.cpuValid = cpuValid && cpu.has_value() && *cpu >= cpuStarted;
        if (row.cpuValid) row.cpu100ns = *cpu - cpuStarted;
    }

    void ObserveGroup(Metrics* a_metrics, const std::uint32_t a_label, const std::uint64_t a_bytes, const bool a_skipped)
    {
        if (!a_metrics) return;
        const auto found = std::ranges::find_if(a_metrics->groups,
            [&](const TopGroup& a_group) { return a_group.label == a_label && a_group.skipped == a_skipped; });
        if (found != a_metrics->groups.end()) { ++found->count; found->bytes += a_bytes; }
        else a_metrics->groups.push_back({a_label, 1, a_bytes, a_skipped});
    }

    std::string UTF8Path(const std::filesystem::path& a_path)
    {
        const auto text = a_path.u8string();
        return {text.begin(), text.end()};
    }

    bool Session::WriteReport(const std::filesystem::path& a_path, const BuildOptions& a_options, const Result& a_result) const
    {
        if (!a_options.detailedLogging) return false;
        Json plugins = Json::array();
        for (const auto& row : rows) plugins.push_back(RowJSON(row));
        wchar_t processPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, processPath, MAX_PATH);
        SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
        std::error_code directoryError;
        const auto workingDirectory = std::filesystem::current_path(directoryError);
        Json report{ {"format_version", 1}, {"build", __DATE__ " " __TIME__},
            {"environment", { {"process_image", UTF8Path(processPath)},
                {"working_directory", directoryError ? "<unavailable>" : UTF8Path(workingDirectory)},
                {"logical_processors", system.dwNumberOfProcessors}, {"usvfs_loaded", GetModuleHandleW(L"usvfs_x64.dll") != nullptr},
                {"pid", GetCurrentProcessId()}, {"report_thread", GetCurrentThreadId()} }},
            {"sampling", { {"initial_exact_calls_per_operation_per_plugin", kInitialSamples}, {"tail_stride", kSampleStride},
                {"clock_probe_mean_ns", clockProbeNs} }},
            {"notes", {"Diagnostic instrumentation adds overhead; do not treat this as an uninstrumented benchmark.",
                "Stream bytes/calls are C++ stream operations, not physical disk reads or OS system calls.",
                "Both readers read each header once and track byte position. Mapped skips only move the cursor; stream calls exclude mapped reads, which are reported under reader. Initial stream size queries remain.",
                "Read-only mappings use the same resolved plugin paths, one plugin at a time. Mapping failures fall back to streaming; mapped I/O faults retry the current read through the stream without replaying records.",
                "Mapping/unmapping times are included in scan time. Copied mapped bytes exclude a failed copy attempt; mapped read samples include page faults and are not physical disk read counts.",
                "Hot-operation times are sampled estimates. Reference processing includes stream/filter/store operations; do not add inclusive categories together.",
                "The prime sampling stride reduces alignment with repeating record patterns. Estimates include timer overhead; the clock probe is not subtracted.",
                "Remaining per-plugin time includes bookkeeping, local result destruction and instrumentation overhead.",
                "Whole-plugin thread CPU is coarsely reported by Windows; wall minus CPU includes I/O waits and scheduling delays, not just disk time.",
                "Each plugin/pass has its own row. EditorID preflight, when required, is separate from the placement pass.",
                "Embedded rows include fingerprint validation, decompression and table application in scan time. Hash reads are recorded as validated_bytes, not parser stream operations.",
                "Embedded sources bypass record scanning. Compressed, minimal and group-skip counters describe only the remaining streamed plugins, not changes to the retained results.",
                "The recorded build time excludes report generation and writing."}},
            {"options", { {"index_references", a_options.indexReferences}, {"static_editor_ids", a_options.indexStaticEditorIDs},
                {"skip_exterior_cells", a_options.skipExteriorCells}, {"excluded_cells", a_options.excludedCells.size()} }},
            {"build_phases_ms", { {"before_report", Milliseconds(elapsedNs)}, {"plugin_enumeration", Milliseconds(enumerateNs)},
                {"prepare_placement_filter", Milliseconds(prepareFilterNs)}, {"final_prune", Milliseconds(pruneNs)},
                {"whole_build_thread_cpu", cpuValid ? Json(static_cast<double>(cpu100ns) / 10'000.0) : Json(nullptr)} }},
            {"result", { {"complete", a_result.complete}, {"plugins", a_result.pluginsParsed},
                {"discovered", a_result.pluginsDiscovered}, {"references", a_result.referencesRead},
                {"retained", a_result.placements.size()}, {"embedded_plugins", a_result.embeddedPlugins},
                {"embedded_placements", a_result.embeddedPlacements}, {"failed_plugins", a_result.failedPlugins} }},
            {"plugins", std::move(plugins)} };
        std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << report.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        file.close();
        return !file.fail();
    }

    Session::Session()
    {
        const auto cpu = ThreadCPU(); cpuValid = cpu.has_value(); cpuStarted = cpu.value_or(0);
        constexpr std::uint64_t probes = 128;
        for (std::uint64_t index = 0; index < probes; ++index)
        {
            const auto probe = Clock::now();
            clockProbeNs += Nanoseconds(Clock::now() - probe);
        }
        clockProbeNs /= probes;
    }

    void Session::Finish(const BuildOptions& a_options, const Result& a_result) noexcept
    {
        if (!a_options.detailedLogging) return;
        elapsedNs = Nanoseconds(Clock::now() - started);
        const auto cpu = ThreadCPU(); cpuValid = cpuValid && cpu.has_value() && *cpu >= cpuStarted;
        if (cpuValid) cpu100ns = *cpu - cpuStarted;
        try
        {
            const auto directory = SKSE::log::log_directory();
            if (!directory) { logger::warn("[Parser Profile] report unavailable | log directory missing"); return; }
            SYSTEMTIME utc{}; GetSystemTime(&utc);
            static std::atomic_uint64_t sequence{0};
            const auto path = *directory / std::format("Heliosphan-Parser-{:04}{:02}{:02}-{:02}{:02}{:02}-UTC-{}-{}.json",
                utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, GetCurrentProcessId(), ++sequence);
            const auto reportStarted = Clock::now();
            const bool written = WriteReport(path, a_options, a_result);
            std::uint64_t pathNs = 0, openNs = 0, scanNs = 0, closeNs = 0, mergeNs = 0, reads = 0, seeks = 0, tells = 0;
            std::size_t mapped = 0, mappingFallbacks = 0;
            std::uint64_t mappedReads = 0;
            std::vector<const Row*> slowest;
            for (const auto& row : rows)
            {
                const auto& m = row.metrics;
                pathNs += m.pathNs; openNs += m.openNs; scanNs += m.scanNs; closeNs += m.closeNs; mergeNs += m.mergeNs;
                reads += m.reads.calls; seeks += m.seeks.calls; tells += m.tells.calls;
                mapped += m.readerSource == "mapped" || m.readerSource == "mapped-then-stream";
                mappingFallbacks += m.mappingReason == "mapping-unavailable" || m.mappingReason == "mapped-read-failed";
                mappedReads += m.mappedReads.calls;
                slowest.push_back(&row);
            }
            std::ranges::sort(slowest, [](const Row* a, const Row* b) { return a->wallNs > b->wallNs; });
            logger::info("[Parser Profile] report={} | written={} | passes={} | beforeReport={:.3f}ms | reportWrite={:.3f}ms | threadCPU={:.3f}ms | path={:.3f}ms | open={:.3f}ms | scan={:.3f}ms | close={:.3f}ms | merge={:.3f}ms | reads={} | seeks={} | tells={}",
                path.string(), written, rows.size(), Milliseconds(elapsedNs), Milliseconds(Nanoseconds(Clock::now() - reportStarted)),
                cpuValid ? static_cast<double>(cpu100ns) / 10'000.0 : -1.0,
                Milliseconds(pathNs), Milliseconds(openNs), Milliseconds(scanNs), Milliseconds(closeNs), Milliseconds(mergeNs), reads, seeks, tells);
            logger::info("[Parser Profile] mappedPlugins={} | mappingFallbacks={} | mappedReads={}", mapped, mappingFallbacks, mappedReads);
            for (std::size_t index = 0; index < std::min<std::size_t>(20, slowest.size()); ++index)
            {
                const auto& row = *slowest[index];
                logger::info("[Parser Profile] rank={} | plugin={} | pass={} | total={:.3f}ms | open={:.3f}ms | scan={:.3f}ms | merge={:.3f}ms | references={} | reads={} | seeks={}",
                    index + 1, row.plugin, row.phase, Milliseconds(row.wallNs), Milliseconds(row.metrics.openNs),
                    Milliseconds(row.metrics.scanNs), Milliseconds(row.metrics.mergeNs), row.references, row.metrics.reads.calls, row.metrics.seeks.calls);
            }
        }
        catch (const std::exception& error) { logger::warn("[Parser Profile] report failed | {}", error.what()); }
        catch (...) { logger::warn("[Parser Profile] report failed | unknown error"); }
    }
}
#endif
