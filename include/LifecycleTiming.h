#pragma once

namespace MPL::LifecycleTiming
{
    void BeginStartup();
    void LogStartupStart();
    void FinishStartup();

    void BeginGameLoad();
    void FinishGameLoad();
}  // namespace MPL::LifecycleTiming
