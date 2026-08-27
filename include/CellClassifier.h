#pragma once

#include <PluginIndex.h>
#include <RE/Skyrim.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace MPL::CellClassifier
{
    struct ExcludedLocationTypes
    {
        std::vector<std::string> types;
        std::vector<std::string> multiLocationExceptions;
    };

    struct Settings
    {
        std::vector<std::string> forms;
        std::optional<std::vector<std::string>> formContains;
        std::vector<std::string> references;
        std::vector<std::string> plugins;
        bool objectPlacements = false;
        std::vector<std::string> profiles;
        std::vector<std::string> includedCells;
        std::vector<std::string> excludedCells;
        std::variant<
            std::vector<std::string>,
            ExcludedLocationTypes>
            excludedLocationTypes;
        std::string emittance;
    };

    void Clear();
    void AddRule(
        std::string a_owner,
        Settings a_settings,
        bool a_detailedLogging);
    bool RequiresStaticEditorIDs();
    bool RequiresPluginIndex();
    void PreparePlacementFilter(
        const PluginIndex::BuildOptions::EditorIDMap& a_editorIDs);
    bool NeedsPlacement(RE::FormID a_reference, RE::FormID a_base);
    bool CouldProjectToAny(
        RE::FormID a_reference,
        RE::FormID a_base,
        const std::unordered_set<RE::FormID>& a_forms);
    std::unordered_set<RE::FormID> GetExcludedCells();
    bool Prepare(const PluginIndex::Result& a_index);
    bool IsReady();
    bool HasProfiles();
    const std::vector<std::string>& GetProfiles(RE::FormID a_cell);
    std::vector<RE::FormID> GetResolvedForms(std::string_view a_owner);
    std::vector<RE::FormID> GetResolvedReferences(
        std::string_view a_owner);

    std::size_t GetProfiledCellCount();
    RE::FormID GetProfiledCell(std::size_t a_index);
    std::uint32_t ProjectBase(
        std::uint32_t a_reference,
        std::uint32_t a_base);
    std::uint32_t OriginalBase(
        std::uint32_t a_reference,
        std::uint32_t a_base);
}  // namespace MPL::CellClassifier
