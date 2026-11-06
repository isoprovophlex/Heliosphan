#pragma once

#include <fstream>

#ifdef HELIOSPHAN_PARSER_DIAGNOSTICS
#include <PluginIndex.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace MPL::PluginIndex::Diagnostics
{
    using Clock = std::chrono::steady_clock;
    constexpr std::uint64_t kInitialSamples = 8;
    constexpr std::uint64_t kSampleStride = 257;

    struct Sample
    {
        std::uint64_t calls = 0;
        std::uint64_t initialNs = 0;
        std::uint64_t tailNs = 0;
        std::uint64_t tailSamples = 0;
        double EstimatedMs() const;
    };

    struct TopGroup
    {
        std::uint32_t label = 0;
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
        bool skipped = false;
    };

    struct Metrics
    {
        std::uint64_t fileBytes = 0;
        std::uint64_t pathNs = 0;
        std::uint64_t openNs = 0;
        std::uint64_t metadataNs = 0;
        std::uint64_t scanNs = 0;
        std::uint64_t closeNs = 0;
        std::uint64_t mergeNs = 0;
        std::string indexSource = "scan", indexReason, fingerprint;
        double validationMs = 0, decodeMs = 0;
        std::uint64_t preparedApplyNs = 0, validatedBytes = 0;
        bool indexReused = false;
        std::string readerSource = "stream", mappingReason;
        std::uint64_t mappingNs = 0, unmappingNs = 0, mappedBytes = 0;
        std::uint32_t mappingError = 0;
        Sample mappedReads;
        std::uint64_t requestedBytes = 0;
        std::uint64_t returnedBytes = 0;
        std::uint64_t absoluteSeeks = 0;
        std::uint64_t relativeSeeks = 0;
        std::uint64_t signatureRewinds = 0;
        std::uint64_t groupHeaders = 0;
        std::uint64_t recordHeaders = 0;
        std::uint64_t subrecordHeaders = 0;
        std::uint64_t maxStoredCapacity = 0;
        std::uint64_t maxInflatedCapacity = 0;
        Sample reads, seeks, tells, formIDs, exteriorLookups, references, filters, stores, inflates;
        std::vector<TopGroup> groups;
    };

    class Duration
    {
    public:
        explicit Duration(std::uint64_t* a_destination);
        ~Duration();
        Duration(const Duration&) = delete;
        Duration& operator=(const Duration&) = delete;
    private:
        std::uint64_t* destination;
        Clock::time_point started;
    };

    class SampleScope
    {
    public:
        explicit SampleScope(Sample* a_sample);
        ~SampleScope();
        SampleScope(const SampleScope&) = delete;
        SampleScope& operator=(const SampleScope&) = delete;
    private:
        Sample* sample;
        bool initial = false;
        bool measured = false;
        Clock::time_point started;
    };

    class InputStream : public std::ifstream
    {
    public:
        Metrics* metrics = nullptr;
        ~InputStream();
        void open(const std::filesystem::path& a_path, std::ios::openmode a_mode);
        InputStream& read(char* a_data, std::streamsize a_count);
        InputStream& seekg(std::streamoff a_offset, std::ios::seekdir a_direction);
        std::streampos tellg();
        void close();
    };

    struct Row
    {
        std::string plugin, phase, path, failure;
        std::size_t ordinal = 0;
        bool parsed = false;
        bool cpuValid = false;
        std::uint64_t wallNs = 0;
        std::uint64_t cpu100ns = 0;
        std::size_t references = 0, placements = 0, editorIDs = 0, compressed = 0;
        std::size_t discarded = 0, minimal = 0, exteriorSkipped = 0, excludedSkipped = 0, topSkipped = 0;
        std::uint64_t payloadSkipped = 0, cellBytesSkipped = 0, topBytesSkipped = 0;
        Metrics metrics;
        void Capture(const Result& a_result, bool a_success, std::string_view a_failure);
    };

    class PluginScope
    {
    public:
        explicit PluginScope(Row& a_row);
        ~PluginScope();
        PluginScope(const PluginScope&) = delete;
        PluginScope& operator=(const PluginScope&) = delete;
    private:
        Row& row;
        Clock::time_point started;
        std::uint64_t cpuStarted = 0;
        bool cpuValid = false;
    };

    class Session
    {
    public:
        Session();
        std::vector<Row> rows;
        std::uint64_t enumerateNs = 0, prepareFilterNs = 0, pruneNs = 0;
        void Finish(const BuildOptions& a_options, const Result& a_result) noexcept;
        bool WriteReport(const std::filesystem::path& a_path, const BuildOptions& a_options,
            const Result& a_result) const;
    private:
        Clock::time_point started = Clock::now();
        std::uint64_t elapsedNs = 0;
        std::uint64_t cpuStarted = 0, cpu100ns = 0;
        std::uint64_t clockProbeNs = 0;
        bool cpuValid = false;
    };

    void ObserveGroup(Metrics* a_metrics, std::uint32_t a_label, std::uint64_t a_bytes, bool a_skipped);
    std::string UTF8Path(const std::filesystem::path& a_path);
}
#else
namespace MPL::PluginIndex::Diagnostics
{
    using InputStream = std::ifstream;
}
#endif
