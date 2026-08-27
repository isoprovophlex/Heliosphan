#pragma once

namespace MPL::LifecycleTiming
{
    enum class StartupPhase
    {
        Configuration,
        RegionPatching,
        PluginIndex,
        CellClassification,
        ExternalEmittance,
        LightPlacer,
        DeferredReplay,
        Count,
    };

    void BeginStartup();
    void ResumeStartupAfterEngineWait();
    void BeginStartupEngineWait();
    void BeginStartupPhase(StartupPhase);
    void FinishStartupPhase(StartupPhase);
    void BeginDeferredStartupWait();
    void FinishStartup();

    void BeginGameLoad();
    void FinishGameLoad();
}  // namespace MPL::LifecycleTiming
