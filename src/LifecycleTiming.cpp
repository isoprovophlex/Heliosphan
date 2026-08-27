#include <Heliosphan.h>
#include <LifecycleTiming.h>
#include <chrono>
#include <utility>

namespace MPL::LifecycleTiming
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Timer
        {
            Clock::time_point started;
            bool active = false;
        };

        Timer startup;
        Timer gameLoad;

        void Begin(Timer& a_timer)
        {
            a_timer = { Clock::now(), true };
        }

        void LogStart(const char* a_name)
        {
            if (Heliosphan::IsSpeedLoggingEnabled())
            {
                logger::info(
                    "[{} Timing] component=Heliosphan stage=start",
                    a_name);
            }
        }

        void Finish(Timer& a_timer, const char* a_name)
        {
            if (!std::exchange(a_timer.active, false) ||
                !Heliosphan::IsSpeedLoggingEnabled())
            {
                return;
            }
            logger::info(
                "[{} Timing] component=Heliosphan stage=finish total={:.3f} ms",
                a_name,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - a_timer.started)
                    .count());
        }
    }  // namespace

    void BeginStartup()
    {
        Begin(startup);
    }

    void LogStartupStart()
    {
        LogStart("Startup");
    }

    void FinishStartup()
    {
        Finish(startup, "Startup");
    }

    void BeginGameLoad()
    {
        Begin(gameLoad);
        LogStart("Game Load");
    }

    void FinishGameLoad()
    {
        Finish(gameLoad, "Game Load");
    }
}  // namespace MPL::LifecycleTiming
