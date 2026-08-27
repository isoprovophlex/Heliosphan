#include <Heliosphan.h>
#include <LifecycleTiming.h>

#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

namespace MPL::LifecycleTiming
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using Duration = Clock::duration;

        struct Timer
        {
            Clock::time_point started;
            bool active = false;
        };

        struct StartupTimer : Timer
        {
            std::array<Duration,
                static_cast<std::size_t>(StartupPhase::Count)>
                phases{};
            std::optional<StartupPhase> activePhase;
            Clock::time_point phaseStarted;
            Duration engineWait{};
            std::optional<Clock::time_point> engineWaitStarted;
            Duration queueWait{};
            std::optional<Clock::time_point> queueWaitStarted;
        };

        StartupTimer startup;
        Timer gameLoad;
        std::mutex timingLock;

        double Milliseconds(const Duration a_duration)
        {
            return std::chrono::duration<double, std::milli>(
                       a_duration)
                .count();
        }

        void Begin(Timer& a_timer)
        {
            a_timer = { Clock::now(), true };
        }
    }  // namespace

    void BeginStartup()
    {
        std::scoped_lock lock(timingLock);
        startup = {};
        startup.started = Clock::now();
        startup.active = true;
        startup.engineWaitStarted = startup.started;
    }

    void ResumeStartupAfterEngineWait()
    {
        std::scoped_lock lock(timingLock);
        if (!startup.active || !startup.engineWaitStarted)
        {
            return;
        }
        startup.engineWait += Clock::now() - *startup.engineWaitStarted;
        startup.engineWaitStarted.reset();
    }

    void BeginStartupEngineWait()
    {
        std::scoped_lock lock(timingLock);
        if (startup.active && !startup.engineWaitStarted)
        {
            startup.engineWaitStarted = Clock::now();
        }
    }

    void BeginStartupPhase(const StartupPhase a_phase)
    {
        std::scoped_lock lock(timingLock);
        if (!startup.active || a_phase == StartupPhase::Count)
        {
            return;
        }
        if (a_phase == StartupPhase::DeferredReplay &&
            startup.queueWaitStarted)
        {
            startup.queueWait += Clock::now() -
                                 *startup.queueWaitStarted;
            startup.queueWaitStarted.reset();
        }
        startup.activePhase = a_phase;
        startup.phaseStarted = Clock::now();
    }

    void FinishStartupPhase(const StartupPhase a_phase)
    {
        std::scoped_lock lock(timingLock);
        if (!startup.active || startup.activePhase != a_phase ||
            a_phase == StartupPhase::Count)
        {
            return;
        }
        startup.phases[static_cast<std::size_t>(a_phase)] +=
            Clock::now() - startup.phaseStarted;
        startup.activePhase.reset();
    }

    void BeginDeferredStartupWait()
    {
        std::scoped_lock lock(timingLock);
        if (startup.active)
        {
            startup.queueWaitStarted = Clock::now();
        }
    }

    void FinishStartup()
    {
        std::scoped_lock lock(timingLock);
        if (!std::exchange(startup.active, false))
        {
            return;
        }
        const auto now = Clock::now();
        if (startup.engineWaitStarted)
        {
            startup.engineWait += now - *startup.engineWaitStarted;
            startup.engineWaitStarted.reset();
        }
        if (startup.queueWaitStarted)
        {
            startup.queueWait += now - *startup.queueWaitStarted;
            startup.queueWaitStarted.reset();
        }
        if (startup.activePhase)
        {
            startup.phases[static_cast<std::size_t>(
                *startup.activePhase)] += now - startup.phaseStarted;
            startup.activePhase.reset();
        }
        if (!Heliosphan::IsSpeedLoggingEnabled())
        {
            return;
        }
        logger::info(
            "[Startup Timing] component=Heliosphan stage=finish total={:.3f} ms, engine wait={:.3f} ms, queue wait={:.3f} ms, configuration={:.3f} ms, region patching={:.3f} ms, plugin indexing={:.3f} ms, cell classification={:.3f} ms, external emittance={:.3f} ms, Light Placer={:.3f} ms, deferred replay={:.3f} ms",
            Milliseconds(now - startup.started),
            Milliseconds(startup.engineWait),
            Milliseconds(startup.queueWait),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::Configuration)]),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::RegionPatching)]),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::PluginIndex)]),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::CellClassification)]),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::ExternalEmittance)]),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::LightPlacer)]),
            Milliseconds(startup.phases[static_cast<std::size_t>(
                StartupPhase::DeferredReplay)]));
    }

    void BeginGameLoad()
    {
        std::scoped_lock lock(timingLock);
        Begin(gameLoad);
        if (Heliosphan::IsSpeedLoggingEnabled())
        {
            logger::info(
                "[Game Load Timing] component=Heliosphan stage=start");
        }
    }

    void FinishGameLoad()
    {
        std::scoped_lock lock(timingLock);
        if (!std::exchange(gameLoad.active, false) ||
            !Heliosphan::IsSpeedLoggingEnabled())
        {
            return;
        }
        logger::info(
            "[Game Load Timing] component=Heliosphan stage=finish total={:.3f} ms",
            Milliseconds(Clock::now() - gameLoad.started));
    }
}  // namespace MPL::LifecycleTiming
