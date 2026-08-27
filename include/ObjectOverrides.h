#pragma once

#include <Heliosphan.h>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace RE
{
    class TESObjectCELL;
    class TESObjectREFR;
}

namespace MPL::ObjectOverrides
{
    void Initialize();
    bool HasOverrides();
    bool HasOverrideFor(const RE::TESObjectREFR* a_reference);
    RE::FormID ProjectBase(std::string_view a_profile, RE::FormID a_base);
    bool ApplyToReference(
        RE::TESObjectREFR* a_reference,
        const std::vector<Heliosphan::WindowSyncProfile>& a_profiles);
    std::size_t ApplyToCell(
        RE::TESObjectCELL* a_cell,
        const std::vector<Heliosphan::WindowSyncProfile>& a_profiles);

    namespace Patches
    {
        struct Vector3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        struct ObjectTransform
        {
            std::optional<std::string> base;
            std::optional<Vector3> position;
            std::optional<Vector3> rotation;
            std::optional<float> scale;
        };

        struct ObjectPlacement
        {
            std::string cell;
            std::string base;
            std::optional<Vector3> position;
            std::optional<Vector3> rotation;
            std::optional<float> scale;
        };

        using ObjectTransformMap =
            std::map<std::string, ObjectTransform>;
        using ObjectPlacementMap =
            std::map<std::string, ObjectPlacement>;

        struct Group
        {
            std::vector<std::string> pluginInclusions;
            std::vector<std::string> pluginExclusions;
            std::map<std::string, std::string> overrides;
            ObjectTransformMap transforms;
            ObjectPlacementMap placements;
        };

        void ClearGroups();
        void AddGroup(
            std::string a_profile,
            std::string a_id,
            Group a_group,
            bool a_detailedLogging);
        std::size_t GetConfiguredProjectionCount();
        void Initialize();
        bool HasProjections();
        RE::FormID ProjectBase(RE::FormID a_reference, RE::FormID a_base);
        std::size_t GetProjectedPlacementCount();
        bool GetProjectedPlacement(
            std::size_t a_index,
            RE::FormID* a_cell,
            RE::FormID* a_base,
            std::string_view* a_profile = nullptr);
        void SetDetailedLogging(std::string_view a_profile, bool a_enabled);
        void BeginGameLoad();
        void CompleteGameLoad(RE::TESObjectCELL* a_cell);
        void ApplyTransformsToReference(RE::TESObjectREFR* a_reference);
        void EnsurePlacements(RE::TESObjectCELL* a_cell = nullptr);
    }  // namespace Patches
}  // namespace MPL::ObjectOverrides
