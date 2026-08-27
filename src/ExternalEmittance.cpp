#include <CellClassifier.h>
#include <ExternalEmittance.h>
#include <FormResolver.h>
#include <LifecycleTiming.h>
#include <algorithm>
#include <exception>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MPL::ExternalEmittance
{
    namespace
    {
        struct ConfiguredProfile
        {
            std::string profile;
            Settings settings;
            bool filtered = false;
            bool detailedLogging = false;
        };

        struct ResolvedProfile
        {
            std::string profile;
            std::unordered_set<RE::FormID> forms;
            std::unordered_set<RE::FormID> references;
            RE::FormID target = 0;
            bool filtered = false;
            bool matchOriginalBase = false;
            bool detailedLogging = false;
        };

        struct Plan
        {
            std::size_t profile = 0;
        };

        struct RegisteredClient
        {
            std::string id;
            void (*OnReferenceEmittanceChanged)(RE::TESObjectREFR*) = nullptr;
        };

        struct State
        {
            std::vector<ConfiguredProfile> configuredProfiles;
            std::vector<ResolvedProfile> profiles;
            std::unordered_set<RE::FormID> watchedBases;
            std::unordered_set<RE::FormID> directReferences;
            std::unordered_map<RE::FormID, Plan> startupPlans;
            std::unordered_map<RE::FormID, Plan> runtimePlans;
            std::vector<RegisteredClient> clients;
            std::unordered_set<RE::FormID> emissiveLightReferences;
            bool gameLoadPending = false;
            bool placementFilterPrepared = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        struct ReferenceInitializationHook
        {
            static void thunk(RE::TESObjectREFR* a_reference)
            {
                func(a_reference);
                ProcessReference(a_reference);
            }

            static inline REL::Relocation<decltype(thunk)> func;
            static inline constexpr std::size_t index = 0x13;
        };

        bool referenceInitializationHookInstalled = false;
        bool referenceInitializationScheduled = false;

        bool ApplyPlan(RE::TESObjectREFR* a_reference, const Plan& a_plan);

        void ReplayPlannedReferences()
        {
            auto& state = GetState();
            std::size_t available = 0;
            std::size_t changed = 0;
            for (const auto& [referenceID, plan] : state.startupPlans)
            {
                auto* reference =
                    RE::TESForm::LookupByID<RE::TESObjectREFR>(
                        referenceID);
                if (!reference)
                {
                    continue;
                }
                ++available;
                if (plan.profile < state.profiles.size())
                {
                    const auto* existing = reference->extraList.GetByType<
                        RE::ExtraEmittanceSource>();
                    const auto* target = RE::TESForm::LookupByID(
                        state.profiles[plan.profile].target);
                    if (existing && target && existing->source == target)
                    {
                        continue;
                    }
                }
                changed += ApplyPlan(reference, plan) ? 1 : 0;
            }
            logger::info(
                "[External Emittance] Final startup replay completed: planned={}, available={}, changed={}",
                state.startupPlans.size(),
                available,
                changed);
        }

        void FinalizeReferenceInitialization()
        {
            InstallReferenceInitializationHook();
            ReplayPlannedReferences();
            LifecycleTiming::FinishStartup();
        }

        bool EqualsIgnoreCase(
            const std::string_view a_left,
            const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                   _strnicmp(
                       a_left.data(),
                       a_right.data(),
                       a_left.size()) == 0;
        }

        bool IsSupportedBase(const RE::TESForm* a_form)
        {
            return a_form &&
                   (a_form->Is(RE::FormType::Static) ||
                       a_form->Is(RE::FormType::MovableStatic) ||
                       a_form->Is(RE::FormType::Light));
        }

        void NotifyClient(
            const RegisteredClient& a_client,
            RE::TESObjectREFR* a_reference)
        {
            try
            {
                a_client.OnReferenceEmittanceChanged(a_reference);
            }
            catch (const std::exception& error)
            {
                logger::error(
                    "[External Emittance] Client '{}' raised an exception: {}",
                    a_client.id,
                    error.what());
            }
            catch (...)
            {
                logger::error(
                    "[External Emittance] Client '{}' raised an unknown exception",
                    a_client.id);
            }
        }

        void NotifyClients(
            RE::TESObjectREFR* a_reference,
            const bool a_sourceChanged)
        {
            const auto* base = a_reference ? a_reference->GetBaseObject() : nullptr;
            if (!base || !base->Is(RE::FormType::Light))
            {
                return;
            }

            auto& state = GetState();
            const bool firstNotification =
                state.emissiveLightReferences.insert(
                                                 a_reference->GetFormID())
                    .second;
            if (!firstNotification && !a_sourceChanged)
            {
                return;
            }
            const auto clients = state.clients;
            for (const auto& client : clients)
            {
                NotifyClient(client, a_reference);
            }
        }

        const Plan* FindPlan(const RE::FormID a_reference)
        {
            const auto& state = GetState();
            if (const auto found = state.startupPlans.find(a_reference);
                found != state.startupPlans.end())
            {
                return std::addressof(found->second);
            }
            if (const auto found = state.runtimePlans.find(a_reference);
                found != state.runtimePlans.end())
            {
                return std::addressof(found->second);
            }
            return nullptr;
        }

        bool ApplyPlan(RE::TESObjectREFR* a_reference, const Plan& a_plan)
        {
            const auto& profiles = GetState().profiles;
            if (!a_reference || a_plan.profile >= profiles.size())
            {
                return false;
            }
            const auto& profile = profiles[a_plan.profile];
            auto* target = RE::TESForm::LookupByID(profile.target);
            if (!target)
            {
                return false;
            }

            auto* existing =
                a_reference->extraList.GetByType<RE::ExtraEmittanceSource>();
            if (existing && existing->source == target)
            {
                NotifyClients(a_reference, false);
                return false;
            }
            if (existing)
            {
                existing->source = target;
            }
            else
            {
                auto* extra =
                    RE::BSExtraData::Create<RE::ExtraEmittanceSource>();
                extra->source = target;
                a_reference->extraList.Add(extra);
            }

            NotifyClients(a_reference, true);
            return true;
        }

        std::optional<Plan> MatchPlan(
            const std::vector<std::string>& a_profileIDs,
            const RE::FormID a_reference,
            const RE::FormID a_originalBase,
            const RE::FormID a_effectiveBase)
        {
            const auto& profiles = GetState().profiles;
            const auto matchesBase = [&](const ResolvedProfile& a_profile)
            {
                return a_profile.forms.contains(a_effectiveBase) ||
                       (a_profile.matchOriginalBase &&
                           a_profile.forms.contains(a_originalBase));
            };
            for (std::size_t offset = 0; offset < profiles.size(); ++offset)
            {
                const auto index = profiles.size() - 1 - offset;
                if (profiles[index].filtered &&
                    profiles[index].references.contains(a_reference))
                {
                    return Plan{
                        .profile = index,
                    };
                }
            }
            for (auto id = a_profileIDs.rbegin();
                id != a_profileIDs.rend();
                ++id)
            {
                for (std::size_t index = 0; index < profiles.size(); ++index)
                {
                    if (profiles[index].filtered &&
                        EqualsIgnoreCase(profiles[index].profile, *id) &&
                        matchesBase(profiles[index]))
                    {
                        return Plan{
                            .profile = index,
                        };
                    }
                }
            }
            for (std::size_t offset = 0; offset < profiles.size(); ++offset)
            {
                const auto index = profiles.size() - 1 - offset;
                if (!profiles[index].filtered &&
                    matchesBase(profiles[index]))
                {
                    return Plan{
                        .profile = index,
                    };
                }
            }
            return std::nullopt;
        }

        std::optional<Plan> MatchReference(RE::TESObjectREFR* a_reference)
        {
            auto* cell = a_reference ? a_reference->GetParentCell() : nullptr;
            auto* base = a_reference ? a_reference->GetBaseObject() : nullptr;
            if (!cell || !base)
            {
                return std::nullopt;
            }
            const auto& profileIDs =
                CellClassifier::GetProfiles(cell->GetFormID());
            const auto originalBase = CellClassifier::OriginalBase(
                a_reference->GetFormID(),
                base->GetFormID());
            const auto effectiveBase = CellClassifier::ProjectBase(
                a_reference->GetFormID(),
                base->GetFormID());
            return MatchPlan(
                profileIDs,
                a_reference->GetFormID(),
                originalBase,
                effectiveBase);
        }

    }  // namespace

    void ClearProfiles()
    {
        auto& state = GetState();
        state.configuredProfiles.clear();
        state.profiles.clear();
        state.watchedBases.clear();
        state.directReferences.clear();
        state.startupPlans.clear();
        state.runtimePlans.clear();
        state.emissiveLightReferences.clear();
        state.gameLoadPending = false;
        state.placementFilterPrepared = false;
    }

    void AddProfile(
        std::string a_profile,
        Settings a_settings,
        const bool a_filtered,
        const bool a_detailedLogging)
    {
        if (!a_profile.empty() &&
            (!a_settings.target.empty() ||
                !a_settings.cellContainsTarget.empty()))
        {
            GetState().configuredProfiles.push_back(ConfiguredProfile{
                .profile = std::move(a_profile),
                .settings = std::move(a_settings),
                .filtered = a_filtered,
                .detailedLogging = a_detailedLogging,
            });
        }
    }

    bool RequiresPluginIndex()
    {
        return !GetState().configuredProfiles.empty();
    }

    bool RequiresCompleteIndex()
    {
        return std::ranges::any_of(
            GetState().configuredProfiles,
            [](const ConfiguredProfile& a_profile)
            {
                return !a_profile.filtered;
            });
    }

    void InstallReferenceInitializationHook()
    {
        if (referenceInitializationHookInstalled ||
            GetState().profiles.empty())
        {
            return;
        }
        REL::Relocation<std::uintptr_t> vtable{
            RE::TESObjectREFR::VTABLE[0]
        };
        ReferenceInitializationHook::func = vtable.write_vfunc(
            ReferenceInitializationHook::index,
            ReferenceInitializationHook::thunk);
        referenceInitializationHookInstalled = true;
        logger::info(
            "[External Emittance] Final reference-initialization hook installed");
    }

    void ScheduleFinalReferenceInitialization()
    {
        if (referenceInitializationScheduled)
        {
            return;
        }
        if (GetState().profiles.empty())
        {
            LifecycleTiming::FinishStartup();
            return;
        }
        referenceInitializationScheduled = true;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks)
        {
            logger::warn(
                "[External Emittance] SKSE task interface is unavailable; finalizing reference initialization immediately");
            FinalizeReferenceInitialization();
            return;
        }
        tasks->AddTask(
            []
            {
                if (auto* nextTasks = SKSE::GetTaskInterface())
                {
                    nextTasks->AddTask(FinalizeReferenceInitialization);
                }
                else
                {
                    FinalizeReferenceInitialization();
                }
            });
    }

    void PreparePlacementFilter()
    {
        auto& state = GetState();
        if (state.placementFilterPrepared)
        {
            return;
        }
        for (const auto& configured : state.configuredProfiles)
        {
            if (configured.filtered &&
                !configured.settings.cellContainsTarget.empty())
            {
                const auto forms =
                    CellClassifier::GetResolvedForms(configured.profile);
                state.watchedBases.insert(forms.begin(), forms.end());
            }
            if (configured.settings.target.empty())
            {
                continue;
            }
            for (const auto& selector : configured.settings.forms)
            {
                const auto formID = FormResolver::Resolve(selector);
                const auto* form =
                    formID ? RE::TESForm::LookupByID(formID) : nullptr;
                if (IsSupportedBase(form))
                {
                    state.watchedBases.insert(formID);
                }
            }
        }
        state.placementFilterPrepared = true;
        logger::info(
            "[External Emittance] Placement filter prepared: watched bases={}",
            state.watchedBases.size());
    }

    bool NeedsPlacement(
        const RE::FormID a_reference,
        const RE::FormID a_base)
    {
        const auto& state = GetState();
        return state.placementFilterPrepared &&
               CellClassifier::CouldProjectToAny(
                   a_reference,
                   a_base,
                   state.watchedBases);
    }

    void Prepare(const PluginIndex::Result& a_index)
    {
        auto& state = GetState();
        std::size_t configuredForms = 0;
        std::size_t missingForms = 0;
        std::size_t invalidForms = 0;
        for (const auto& configured : state.configuredProfiles)
        {
            const auto cellContainsForms = configured.filtered ?
                                               CellClassifier::GetResolvedForms(configured.profile) :
                                               std::vector<RE::FormID>{};
            const auto cellContainsReferences = configured.filtered ?
                                                    CellClassifier::GetResolvedReferences(configured.profile) :
                                                    std::vector<RE::FormID>{};
            if (configured.filtered &&
                !configured.settings.cellContainsTarget.empty())
            {
                configuredForms += cellContainsForms.size();
                const auto target = FormResolver::Resolve(
                    configured.settings.cellContainsTarget);
                if (!target || !RE::TESForm::LookupByID(target))
                {
                    logger::warn(
                        "[External Emittance] [{}] cellContains emittance '{}' could not be resolved; matching-form emittance is disabled",
                        configured.profile,
                        configured.settings.cellContainsTarget);
                }
                else if (!cellContainsForms.empty() ||
                         !cellContainsReferences.empty())
                {
                    state.directReferences.insert(
                        cellContainsReferences.begin(),
                        cellContainsReferences.end());
                    state.profiles.push_back(ResolvedProfile{
                        .profile = configured.profile,
                        .forms = std::unordered_set<RE::FormID>(
                            cellContainsForms.begin(),
                            cellContainsForms.end()),
                        .references = std::unordered_set<RE::FormID>(
                            cellContainsReferences.begin(),
                            cellContainsReferences.end()),
                        .target = target,
                        .filtered = true,
                        .matchOriginalBase = true,
                        .detailedLogging = configured.detailedLogging,
                    });
                }
            }

            if (configured.settings.target.empty())
            {
                continue;
            }
            configuredForms += configured.settings.forms.size();
            const auto target = FormResolver::Resolve(configured.settings.target);
            if (!target || !RE::TESForm::LookupByID(target))
            {
                logger::warn(
                    "[External Emittance] [{}] emittancePatching target '{}' could not be resolved; additional-form emittance is disabled",
                    configured.profile,
                    configured.settings.target);
                continue;
            }

            ResolvedProfile patchingProfile{
                .profile = configured.profile,
                .target = target,
                .filtered = configured.filtered,
                .detailedLogging = configured.detailedLogging,
            };
            for (const auto& selector : configured.settings.forms)
            {
                const auto formID = FormResolver::Resolve(selector);
                const auto* form =
                    formID ? RE::TESForm::LookupByID(formID) : nullptr;
                if (!form)
                {
                    ++missingForms;
                }
                else if (!IsSupportedBase(form))
                {
                    ++invalidForms;
                    if (configured.detailedLogging)
                    {
                        logger::warn(
                            "[External Emittance] [{}] Source '{}' [{:08X}] is not a STAT, MSTT, or LIGH form",
                            configured.profile,
                            selector,
                            formID);
                    }
                }
                else
                {
                    patchingProfile.forms.insert(formID);
                }
            }
            if (!patchingProfile.forms.empty())
            {
                state.profiles.push_back(std::move(patchingProfile));
            }
        }

        std::size_t resolvedForms = 0;
        std::size_t resolvedReferences = 0;
        for (const auto& profile : state.profiles)
        {
            resolvedForms += profile.forms.size();
            resolvedReferences += profile.references.size();
        }
        if (state.profiles.empty())
        {
            logger::info(
                "[External Emittance] Startup preparation completed: profiles={}, configured forms={}, resolved forms=0, direct references=0, optional sources unavailable={}, invalid sources={}, reference plans=0, references changed=0",
                state.configuredProfiles.size(),
                configuredForms,
                missingForms,
                invalidForms);
            return;
        }
        for (const auto& [formID, placement] : a_index.placements)
        {
            if (!formID || placement.deleted || !placement.base ||
                !placement.cell)
            {
                continue;
            }
            const auto& profileIDs =
                CellClassifier::GetProfiles(placement.cell);
            const auto effectiveBase = CellClassifier::ProjectBase(
                formID,
                placement.base);
            if (const auto plan = MatchPlan(
                    profileIDs,
                    formID,
                    placement.base,
                    effectiveBase))
            {
                state.startupPlans.insert_or_assign(formID, *plan);
            }
        }

        std::size_t changed = 0;
        for (const auto& [formID, plan] : state.startupPlans)
        {
            if (auto* reference =
                    RE::TESForm::LookupByID<RE::TESObjectREFR>(formID))
            {
                changed += ApplyPlan(reference, plan) ? 1 : 0;
            }
        }
        logger::info(
            "[External Emittance] Startup preparation completed: profiles={}, configured forms={}, resolved forms={}, direct references={}, optional sources unavailable={}, invalid sources={}, reference plans={}, references changed={}",
            state.configuredProfiles.size(),
            configuredForms,
            resolvedForms,
            resolvedReferences,
            missingForms,
            invalidForms,
            state.startupPlans.size(),
            changed);
    }

    void ProcessReference(RE::TESObjectREFR* a_reference)
    {
        if (!a_reference)
        {
            return;
        }
        auto& state = GetState();
        if (state.profiles.empty())
        {
            return;
        }
        auto* plan = FindPlan(a_reference->GetFormID());
        if (!plan)
        {
            if (auto matched = MatchReference(a_reference))
            {
                plan = std::addressof(
                    state.runtimePlans.insert_or_assign(
                                          a_reference->GetFormID(),
                                          *matched)
                        .first->second);
            }
        }
        if (plan)
        {
            ApplyPlan(a_reference, *plan);
        }
    }

    void BeginGameLoad()
    {
        auto& state = GetState();
        if (state.gameLoadPending)
        {
            return;
        }
        state.gameLoadPending = true;
        state.runtimePlans.clear();
        state.emissiveLightReferences.clear();
    }

    void ReplayCell(RE::TESObjectCELL* a_cell)
    {
        GetState().gameLoadPending = false;
        if (a_cell)
        {
            for (const auto& reference : a_cell->GetRuntimeData().references)
            {
                ProcessReference(reference.get());
            }
        }
    }

    bool RegisterReferenceClient(
        const HeliosphanAPI::ReferenceCallbacks* a_callbacks)
    {
        if (!a_callbacks || !a_callbacks->id || !*a_callbacks->id ||
            !a_callbacks->OnReferenceEmittanceChanged)
        {
            return false;
        }

        auto& state = GetState();
        const RegisteredClient client{
            .id = a_callbacks->id,
            .OnReferenceEmittanceChanged =
                a_callbacks->OnReferenceEmittanceChanged,
        };
        const auto existing = std::ranges::find(
            state.clients,
            client.id,
            &RegisteredClient::id);
        if (existing != state.clients.end())
        {
            return existing->OnReferenceEmittanceChanged ==
                   client.OnReferenceEmittanceChanged;
        }
        state.clients.push_back(client);

        std::size_t replayed = 0;
        for (const auto formID : state.emissiveLightReferences)
        {
            auto* reference =
                RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
            const auto* extra = reference ?
                                    reference->extraList.GetByType<
                                        RE::ExtraEmittanceSource>() :
                                    nullptr;
            if (extra && extra->source)
            {
                NotifyClient(client, reference);
                ++replayed;
            }
        }
        logger::info(
            "[External Emittance] Registered reference client '{}' and replayed {} light reference(s)",
            client.id,
            replayed);
        return true;
    }
}  // namespace MPL::ExternalEmittance
