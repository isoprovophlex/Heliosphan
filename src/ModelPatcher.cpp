#include <ExternalEmittance.h>
#include <HeliosphanModelServiceAPI.h>
#include <MMSFServices.h>
#include <ModelPatcher.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MPL::ModelPatcher
{
    namespace
    {
        using Flag = RE::BSShaderProperty::EShaderPropertyFlag8;

        struct PropertyPatch
        {
            RE::NiPointer<RE::BSGeometry> geometry;
            RE::NiPointer<RE::BSShaderProperty> original;
            RE::NiPointer<RE::BSLightingShaderProperty> patched;
            RE::NiColor* originalColor = nullptr;
            bool originalOwnEmit = false;

            void Restore()
            {
                auto& slot = geometry->GetGeometryRuntimeData().shaderProperty;
                if (slot.get() == patched.get()) slot = original;
                // Restore clone ownership before its last reference is released.
                patched->emissiveColor = originalColor;
                patched->SetFlags(Flag::kOwnEmit, originalOwnEmit);
                patched->SetFlags(Flag::kExternalEmittance, false);
                patched->DoClearRenderPasses();
            }
        };

        struct ReferencePatch
        {
            RE::NiPointer<RE::NiAVObject> root;
            RE::TESRegion* source = nullptr;
            std::vector<PropertyPatch> properties;

            void Restore()
            {
                for (auto& patch : properties) patch.Restore();
                properties.clear();
            }
        };

        std::unordered_map<std::string, std::vector<Rule>> rules;
        std::unordered_map<RE::FormID, ReferencePatch> references;
        std::recursive_mutex stateLock;
        std::mutex queueLock;
        std::unordered_map<RE::FormID, bool> pending;
        std::unordered_set<RE::FormID> pendingCells;
        std::atomic<std::uint64_t> generation{ 0 };
        std::atomic<bool> enabled{ false };
        bool queued = false;
        bool eventsInstalled = false;

        void RemoveReference(RE::FormID a_id)
        {
            if (auto found = references.find(a_id); found != references.end())
            {
                found->second.Restore();
                references.erase(found);
            }
        }

        void VisitCell(RE::TESObjectCELL* a_cell)
        {
            if (!a_cell) return;
            std::vector<RE::NiPointer<RE::TESObjectREFR>> cellReferences;
            {
                auto& runtime = a_cell->GetRuntimeData();
                const RE::BSSpinLockGuard guard{ runtime.spinLock };
                cellReferences.assign(runtime.references.begin(), runtime.references.end());
            }
            for (const auto& reference : cellReferences) PrepareReference(reference.get());
        }

        void Queue(RE::FormID a_id, bool a_loaded, RE::FormID a_cell = 0)
        {
            if (!enabled.load(std::memory_order_acquire)) return;
            std::uint64_t current;
            {
                const std::scoped_lock lock(queueLock);
                current = generation.load(std::memory_order_relaxed);
                if (a_id) pending.insert_or_assign(a_id, a_loaded);
                if (a_cell) pendingCells.insert(a_cell);
                if (queued) return;
                queued = true;
            }
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks)
            {
                const std::scoped_lock lock(queueLock);
                pending.clear();
                pendingCells.clear();
                queued = false;
                return;
            }
            tasks->AddTask([current]
            {
                const std::scoped_lock lock(stateLock);
                std::unordered_map<RE::FormID, bool> batch;
                std::unordered_set<RE::FormID> cells;
                {
                    const std::scoped_lock queueGuard(queueLock);
                    if (current != generation.load(std::memory_order_acquire)) return;
                    batch.swap(pending);
                    cells.swap(pendingCells);
                    queued = false;
                }
                for (const auto& [id, loaded] : batch)
                {
                    auto* reference = loaded ? RE::TESForm::LookupByID<RE::TESObjectREFR>(id) : nullptr;
                    if (reference) PrepareReference(reference);
                    else RemoveReference(id);
                }
                for (const auto id : cells) VisitCell(RE::TESForm::LookupByID<RE::TESObjectCELL>(id));
            });
        }

        class Events final :
            public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
            public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
            public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
            {
                if (a_event) Queue(a_event->formID, a_event->loaded);
                return RE::BSEventNotifyControl::kContinue;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event,
                RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
            {
                if (a_event && a_event->reference) Queue(a_event->reference->GetFormID(), a_event->attached);
                return RE::BSEventNotifyControl::kContinue;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
            {
                if (a_event && a_event->cell) Queue(0, true, a_event->cell->GetFormID());
                return RE::BSEventNotifyControl::kContinue;
            }
        } events;

        class Service final : public API::HeliosphanModels::IService
        {
        public:
            std::uint8_t GetVersion() override { return API::HeliosphanModels::kVersion; }
            std::string GetName() override { return API::HeliosphanModels::kServiceName; }
            void Initialize() override {}
            rfl::Generic::Object Save() override { return {}; }
            void Load(rfl::Generic::Object) override {}
            void PrepareReference(RE::TESObjectREFR* a_reference) override { ModelPatcher::PrepareReference(a_reference); }
            bool HasModelRules(const char* a_mesh) override { return ModelPatcher::HasModelRules(a_mesh); }
        };
    }

    bool RegisterService(API::MMSF::Interface* a_registry)
    {
        static Service service;
        const auto result = MMSFServices::Query(a_registry, service.GetName(), service.GetVersion());
        if (result.service) return result.service == &service;
        if (!result.failure || std::string_view(result.failure) != "service-unavailable") return false;
        a_registry->RegisterService(&service);
        const bool registered = a_registry->QueryService(service.GetName()) == &service;
        logger::info("[MMSF Service] service=HELIOSPHAN_MODELS | version=1 | registered={}", registered);
        return registered;
    }

    void Reset()
    {
        const std::scoped_lock lock(stateLock);
        {
            const std::scoped_lock queueGuard(queueLock);
            generation.fetch_add(1, std::memory_order_acq_rel);
            pending.clear();
            pendingCells.clear();
            queued = false;
        }
        for (auto& [id, patch] : references) patch.Restore();
        references.clear();
    }

    void ClearProfiles()
    {
        const std::scoped_lock lock(stateLock);
        Reset();
        rules.clear();
        enabled.store(false, std::memory_order_release);
    }

    void AddProfile(std::string_view a_profile, const std::vector<Rule>& a_rules)
    {
        const std::scoped_lock lock(stateLock);
        for (const auto& rule : a_rules)
        {
            if (!ValidRule(rule))
            {
                logger::warn("[Model Patch] {} | mesh={} | skipped=invalid-rule", a_profile, rule.mesh);
                continue;
            }
            rules[NormalizeMesh(rule.mesh)].push_back(rule);
        }
        enabled.store(!rules.empty(), std::memory_order_release);
    }

    bool HasModelRules(const char* a_mesh)
    {
        if (!a_mesh || !enabled.load(std::memory_order_acquire)) return false;
        const std::scoped_lock lock(stateLock);
        return rules.contains(NormalizeMesh(a_mesh));
    }

    void PrepareReference(RE::TESObjectREFR* a_reference)
    {
        if (!a_reference || !enabled.load(std::memory_order_acquire)) return;
        const auto* base = a_reference->GetBaseObject();
        const auto* model = base ? base->As<RE::TESModel>() : nullptr;
        const auto* path = model ? model->GetModel() : nullptr;
        if (!path) return;
        const std::scoped_lock lock(stateLock);
        const auto found = rules.find(NormalizeMesh(path));
        if (found == rules.end()) return;
        ExternalEmittance::ProcessReference(a_reference);
        const auto id = a_reference->GetFormID();
        auto* root = a_reference->GetCurrent3D();
        if (!root) { RemoveReference(id); return; }
        const auto* extra = a_reference->extraList.GetByType<RE::ExtraEmittanceSource>();
        auto* source = extra && extra->source ? extra->source->As<RE::TESRegion>() : nullptr;
        if (!source) { RemoveReference(id); return; }
        if (auto existing = references.find(id); existing != references.end())
        {
            auto& patch = existing->second;
            if (patch.root.get() == root && patch.source == source &&
                std::ranges::all_of(patch.properties, [](const auto& item)
                { return item.geometry->GetGeometryRuntimeData().shaderProperty.get() == item.patched.get(); })) return;
            RemoveReference(id);
        }

        ReferencePatch patch{ .root = RE::NiPointer(root), .source = source };
        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry)
        {
            const auto* name = geometry ? geometry->name.c_str() : nullptr;
            if (!name || !std::ranges::any_of(found->second, [&](const auto& rule)
                { return MatchesGeometry(rule, name); })) return RE::BSVisit::BSVisitControl::kContinue;
            auto& slot = geometry->GetGeometryRuntimeData().shaderProperty;
            auto* original = slot ? netimmerse_cast<RE::BSLightingShaderProperty*>(slot.get()) : nullptr;
            if (!original || original->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance))
                return RE::BSVisit::BSVisitControl::kContinue;
            RE::NiPointer<RE::NiObject> object(original->Clone());
            auto* clone = object ? netimmerse_cast<RE::BSLightingShaderProperty*>(object.get()) : nullptr;
            if (!clone || clone == original) return RE::BSVisit::BSVisitControl::kContinue;
            const auto ownEmit = clone->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit);
            patch.properties.push_back({ RE::NiPointer(geometry), slot, RE::NiPointer(clone), clone->emissiveColor, ownEmit });
            clone->emissiveColor = &source->emittanceColor;
            clone->SetFlags(Flag::kOwnEmit, false);
            clone->SetFlags(Flag::kExternalEmittance, true);
            slot.reset(clone);
            clone->DoClearRenderPasses();
            logger::info("[Model Patch] reference={:08X} | mesh={} | geometry={} | externalEmittance=true | XEMI={:08X}",
                id, path, geometry->name.c_str(), source->GetFormID());
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (!patch.properties.empty())
        {
            if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) source->SetCurrentWeather(sky->currentWeather);
            if (auto* cell = a_reference->GetParentCell())
            {
                auto& runtime = cell->GetRuntimeData();
                const RE::BSSpinLockGuard guard{ runtime.spinLock };
                if (auto* data = runtime.loadedData; data && data->emittanceSourceRefMap.find(source) == data->emittanceSourceRefMap.end())
                    data->emittanceSourceRefMap.insert_or_assign(static_cast<RE::TESForm*>(source), a_reference->GetHandle());
            }
            references.emplace(id, std::move(patch));
        }
    }

    void QueueReference(RE::TESObjectREFR* a_reference)
    {
        if (a_reference) Queue(a_reference->GetFormID(), true);
    }

    void Initialize()
    {
        if (!enabled.load(std::memory_order_acquire)) return;
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton(); source && !eventsInstalled)
        {
            source->AddEventSink<RE::TESObjectLoadedEvent>(&events);
            source->AddEventSink<RE::TESCellAttachDetachEvent>(&events);
            source->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
            eventsInstalled = true;
        }
        if (auto* tes = RE::TES::GetSingleton()) tes->ForEachReference([](RE::TESObjectREFR* reference)
        {
            QueueReference(reference);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        logger::info("[Model Patch] meshes={} | events={}", rules.size(), eventsInstalled);
    }
}
