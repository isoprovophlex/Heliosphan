#include <WeatherSoundFix.h>
#include <WeatherSoundState.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MPL::WeatherSoundFix
{
    namespace
    {
        constexpr std::size_t kMaxPendingCopies = 4096;

        struct CopyState
        {
            ResourceKey resource;
            std::uint16_t inheritedAttenuation = 0;
        };

        struct State
        {
            std::atomic_bool enabled{ false };
            std::unordered_set<ResourceKey, ResourceHash> resources;
            std::mutex audioLock;
            std::mutex gameLock;
            std::unordered_map<std::uintptr_t, CopyState> copies;
            std::unordered_set<std::uint32_t> targetSounds;
            RE::FormID requestedWeather = 0;
        };

        State& GetState()
        {
            // Installed hooks can outlive normal DLL static destruction.
            static auto* state = new State;
            return *state;
        }

        ResourceKey Key(const RE::BSResource::ID& a_resource)
        {
            ResourceKey key{ .file = a_resource.file, .directory = a_resource.dir };
            std::memcpy(&key.extension, a_resource.ext, sizeof(key.extension));
            return key;
        }

        bool Watched(const RE::BSGameSound* a_sound)
        {
            const auto& state = GetState();
            return a_sound && state.enabled.load(std::memory_order_acquire) &&
                state.resources.contains(Key(a_sound->resourceID));
        }

        void CorrectCopiedAttenuation(RE::BSXAudio2GameSound* a_sound) noexcept
        {
            if (!Watched(a_sound) || !a_sound->sourceVoice || !a_sound->IsPlaying() ||
                (a_sound->flags.underlying() & kSoundFadeInProgress) != 0) return;
            const auto* manager = RE::BSAudioManager::GetSingleton();
            if (!manager || manager->ownerThreadID != GetCurrentThreadId()) return;
            auto& state = GetState();
            try
            {
                CopyState copy;
                {
                    const std::scoped_lock guard(state.audioLock);
                    const auto found = state.copies.find(reinterpret_cast<std::uintptr_t>(a_sound));
                    if (found == state.copies.end() || !state.targetSounds.contains(a_sound->unk4C)) return;
                    copy = found->second;
                    state.copies.erase(found);
                }
                if (copy.resource != Key(a_sound->resourceID)) return;
                const auto category = a_sound->QCategory() ? a_sound->QCategory()->GetCategoryAttenuation() : std::uint16_t{ 0 };
                if (!ShouldCorrectCopiedAttenuation(true, a_sound->IsPlaying(), a_sound->flags.underlying(),
                        copy.inheritedAttenuation, a_sound->systemAttenuation, category)) return;
                a_sound->systemAttenuation = category;
                a_sound->SetVolumeImpl();
            }
            catch (...) {}
        }

        struct CopyHook
        {
            static void thunk(RE::BSXAudio2GameSound* a_sound, RE::BSGameSound* a_source, bool a_update)
            {
                const bool watched = Watched(a_source);
                const auto resource = watched ? Key(a_source->resourceID) : ResourceKey{};
                original(a_sound, a_source, a_update);
                if (!watched || a_sound->systemAttenuation == 0) return;
                auto& state = GetState();
                try
                {
                    const std::scoped_lock guard(state.audioLock);
                    if (state.copies.size() < kMaxPendingCopies)
                        state.copies.insert_or_assign(reinterpret_cast<std::uintptr_t>(a_sound),
                            CopyState{ resource, a_sound->systemAttenuation });
                }
                catch (...) {}
            }
            static inline REL::Relocation<decltype(thunk)> original;
        };

        struct ReleaseHook
        {
            static void thunk(RE::BSXAudio2Audio* a_audio, RE::BSGameSound* a_sound)
            {
                if (Watched(a_sound))
                {
                    auto& state = GetState();
                    try
                    {
                        const std::scoped_lock guard(state.audioLock);
                        state.copies.erase(reinterpret_cast<std::uintptr_t>(a_sound));
                    }
                    catch (...) {}
                }
                original(a_audio, a_sound);
            }
            static inline REL::Relocation<decltype(thunk)> original;
        };

        struct UpdateHook
        {
            static bool thunk(RE::BSXAudio2GameSound* a_sound)
            {
                const bool result = original(a_sound);
                CorrectCopiedAttenuation(a_sound);
                return result;
            }
            static inline REL::Relocation<decltype(thunk)> original;
        };

        struct PlayHook
        {
            static void thunk(RE::BSXAudio2GameSound* a_sound)
            {
                original(a_sound);
                CorrectCopiedAttenuation(a_sound);
            }
            static inline REL::Relocation<decltype(thunk)> original;
        };

        template <class Hook>
        void Install(REL::Relocation<std::uintptr_t>& a_table, std::size_t a_slot)
        {
            Hook::original = *reinterpret_cast<std::uintptr_t*>(a_table.address() + a_slot * sizeof(std::uintptr_t));
            a_table.write_vfunc(a_slot, Hook::thunk);
        }

        // Layout verified against SE, AE, VR and 1.7.99 weather-sound constructors.
        struct WeatherSoundEntry
        {
            RE::BSSoundHandle handle;
            std::uint32_t padding;
            RE::TESWeather* weather;
            std::uint32_t type;
            RE::FormID descriptor;
        };
        static_assert(offsetof(WeatherSoundEntry, weather) == 0x10);
        static_assert(offsetof(WeatherSoundEntry, descriptor) == 0x1C);

        void Disable() noexcept
        {
            if (GetState().enabled.exchange(false, std::memory_order_acq_rel))
            {
                try { logger::error("[Weather Sound] correction disabled | target update failed"); }
                catch (...) {}
            }
        }
    }

    bool Enabled() { return GetState().enabled.load(std::memory_order_acquire); }

    bool Initialize()
    {
        auto& state = GetState();
        if (Enabled()) return false;
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) return false;
        try
        {
            std::unordered_set<RE::FormID> descriptors;
            for (auto* weather : data->GetFormArray<RE::TESWeather>())
            {
                if (!weather) continue;
                for (const auto* entry : weather->sounds)
                {
                    if (!entry || !descriptors.insert(entry->soundFormID).second) continue;
                    auto* form = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(entry->soundFormID);
                    auto* standard = form && form->soundDescriptor ? skyrim_cast<RE::BGSStandardSoundDef*>(form->soundDescriptor) : nullptr;
                    if (standard)
                        for (const auto& file : standard->soundFiles) state.resources.insert(Key(file));
                }
            }
            REL::Relocation<std::uintptr_t> audioTable{ RE::VTABLE_BSXAudio2Audio[0] };
            REL::Relocation<std::uintptr_t> soundTable{ RE::BSXAudio2GameSound::VTABLE[0] };
            Install<ReleaseHook>(audioTable, 0x04);
            Install<CopyHook>(soundTable, 0x0C);
            Install<UpdateHook>(soundTable, 0x0E);
            Install<PlayHook>(soundTable, 0x15);
            state.enabled.store(true, std::memory_order_release);
            logger::info("[Weather Sound] inherited attenuation correction enabled | game={}",
                REL::Module::get().version().string());
            return true;
        }
        catch (const std::exception& error)
        {
            state.enabled.store(false, std::memory_order_release);
            logger::error("[Weather Sound] correction unavailable | {}", error.what());
            return false;
        }
    }

    void BeginWeatherChange(RE::TESWeather* a_weather) noexcept
    {
        if (!Enabled()) return;
        auto& state = GetState();
        try
        {
            const std::scoped_lock gameGuard(state.gameLock);
            auto* player = a_weather ? RE::PlayerCharacter::GetSingleton() : nullptr;
            const auto* cell = player ? player->GetParentCell() : nullptr;
            state.requestedWeather = cell && cell->IsAttached() && cell->IsInteriorCell() ? a_weather->GetFormID() : 0;
            const std::scoped_lock audioGuard(state.audioLock);
            state.targetSounds.clear();
        }
        catch (...) { Disable(); }
    }

    void Poll() noexcept
    {
        if (!Enabled()) return;
        auto& state = GetState();
        try
        {
            const std::scoped_lock gameGuard(state.gameLock);
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto* cell = player ? player->GetParentCell() : nullptr;
            auto* sky = RE::Sky::GetSingleton();
            std::unordered_set<std::uint32_t> targets;
            if (cell && cell->IsAttached() && cell->IsInteriorCell() && state.requestedWeather != 0 &&
                sky && sky->currentWeather && sky->currentWeather->GetFormID() == state.requestedWeather && sky->skySoundList)
            {
                for (const auto* opaque : *sky->skySoundList)
                {
                    const auto* entry = reinterpret_cast<const WeatherSoundEntry*>(opaque);
                    if (entry && entry->weather && entry->weather->GetFormID() == state.requestedWeather &&
                        entry->handle.soundID != RE::BSSoundHandle::kInvalidID)
                        targets.insert(entry->handle.soundID);
                }
            }
            const std::scoped_lock audioGuard(state.audioLock);
            state.targetSounds.swap(targets);
        }
        catch (...) { Disable(); }
    }
}
