#include <RoomMarkerPatcher.h>
#include <Heliosphan.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MPL::RoomMarkerPatcher
{
    namespace
    {
        constexpr RE::FormID kRoomMarkerBaseFormID = 0x1F;

        struct State
        {
            std::mutex lock;
            std::unordered_set<RE::FormID> cellsToClean;
            std::unordered_map<
                RE::FormID,
                std::unordered_set<std::string>>
                excludedPlugins;
            std::atomic_bool active{ false };
        };

        struct CleanResult
        {
            bool lightingTemplate{ false };
            bool imageSpace{ false };

            [[nodiscard]] bool Changed() const
            {
                return lightingTemplate || imageSpace;
            }
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        template <class... Args>
        void LogDetailed(
            std::format_string<Args...> a_format,
            Args&&... a_args)
        {
            if (Heliosphan::IsDetailedLoggingEnabled())
            {
                logger::info(
                    "[Room Marker] {}",
                    std::format(
                        a_format,
                        std::forward<Args>(a_args)...));
            }
        }

        bool IsRoomMarker(RE::TESObjectREFR* a_reference)
        {
            const auto* base = a_reference ? a_reference->GetBaseObject() : nullptr;
            return base && base->GetFormID() == kRoomMarkerBaseFormID;
        }

        bool IsCellEnabled(const RE::TESObjectCELL* a_cell)
        {
            if (!a_cell)
            {
                return false;
            }

            auto& state = GetState();
            std::scoped_lock lock(state.lock);
            return state.cellsToClean.contains(a_cell->GetFormID());
        }

        std::string NormalizePluginName(const std::string_view a_name)
        {
            std::string result(a_name);
            std::ranges::transform(
                result,
                result.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return result;
        }

        const RE::TESFile* ExcludedPlugin(
            const RE::TESObjectREFR* a_reference,
            const RE::TESObjectCELL* a_cell)
        {
            if (!a_reference || !a_cell)
            {
                return nullptr;
            }

            auto& state = GetState();
            std::scoped_lock lock(state.lock);
            const auto found =
                state.excludedPlugins.find(a_cell->GetFormID());
            if (found == state.excludedPlugins.end())
            {
                return nullptr;
            }

            const auto* sources = a_reference->sourceFiles.array;
            if (!sources)
            {
                return nullptr;
            }
            for (const auto* source : *sources)
            {
                if (source &&
                    found->second.contains(
                        NormalizePluginName(source->GetFilename())))
                {
                    return source;
                }
            }
            return nullptr;
        }

        CleanResult CleanReference(RE::TESObjectREFR* a_reference)
        {
            CleanResult result;
            if (!IsRoomMarker(a_reference))
            {
                return result;
            }

            auto* cell = a_reference->GetParentCell();
            if (!IsCellEnabled(cell))
            {
                return result;
            }
            if (const auto* plugin = ExcludedPlugin(a_reference, cell))
            {
                return result;
            }

            auto* roomData = a_reference->extraList.GetByType<RE::ExtraRoomRefData>();
            if (!roomData || !roomData->data)
            {
                return result;
            }

            result.lightingTemplate = roomData->data->lightingTemplate != nullptr;
            result.imageSpace = roomData->data->imageSpace != nullptr;
            roomData->data->lightingTemplate = nullptr;
            roomData->data->imageSpace = nullptr;

            return result;
        }

        struct CellCleanResult
        {
            std::size_t markers = 0;
            std::size_t cleaned = 0;
            std::size_t lightingTemplates = 0;
            std::size_t imageSpaces = 0;
        };

        CellCleanResult CleanCellReferences(RE::TESObjectCELL* a_cell)
        {
            CellCleanResult total;
            if (!a_cell)
            {
                return total;
            }

            a_cell->ForEachReference([&](RE::TESObjectREFR* a_reference)
                {
                    if (!IsRoomMarker(a_reference))
                    {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }

                    ++total.markers;
                    const auto result = CleanReference(a_reference);
                    total.cleaned += result.Changed();
                    total.lightingTemplates += result.lightingTemplate;
                    total.imageSpaces += result.imageSpace;
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            return total;
        }
    }  // namespace

    void ConfigureCell(
        RE::TESObjectCELL* a_cell,
        const bool a_enabled,
        const std::vector<std::string>& a_excludedPlugins)
    {
        if (!a_cell)
        {
            return;
        }

        std::unordered_set<std::string> exclusions;
        for (const auto& plugin : a_excludedPlugins)
        {
            if (!plugin.empty())
            {
                exclusions.insert(NormalizePluginName(plugin));
            }
        }

        auto& state = GetState();
        bool changed = false;
        {
            std::scoped_lock lock(state.lock);
            if (a_enabled)
            {
                changed = state.cellsToClean.insert(
                    a_cell->GetFormID()).second;
                const auto found =
                    state.excludedPlugins.find(a_cell->GetFormID());
                const bool exclusionsChanged =
                    found == state.excludedPlugins.end() ?
                        !exclusions.empty() :
                        found->second != exclusions;
                changed = changed || exclusionsChanged;
                if (exclusions.empty())
                {
                    state.excludedPlugins.erase(a_cell->GetFormID());
                }
                else
                {
                    state.excludedPlugins.insert_or_assign(
                        a_cell->GetFormID(),
                        std::move(exclusions));
                }
            }
            else
            {
                changed =
                    state.cellsToClean.erase(a_cell->GetFormID()) != 0;
                changed =
                    state.excludedPlugins.erase(a_cell->GetFormID()) != 0 ||
                    changed;
            }
            state.active.store(
                !state.cellsToClean.empty(),
                std::memory_order_release);
        }
        if (!changed)
        {
            return;
        }

        const auto result = a_enabled ? CleanCellReferences(a_cell) : CellCleanResult{};
        if (result.cleaned == 0)
        {
            return;
        }
        LogDetailed(
            "RoomMarker cleaning {} for cell {:08X}; checked {} initialized marker(s), cleaned {}, removed {} lighting template(s) and {} image space(s)",
            a_enabled ? "enabled" : "disabled",
            a_cell->GetFormID(),
            result.markers,
            result.cleaned,
            result.lightingTemplates,
            result.imageSpaces);
    }

    void ProcessReference(RE::TESObjectREFR* a_reference)
    {
        if (!GetState().active.load(std::memory_order_acquire))
        {
            return;
        }
        CleanReference(a_reference);
    }
}  // namespace MPL::RoomMarkerPatcher
