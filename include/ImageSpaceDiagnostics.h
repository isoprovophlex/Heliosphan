#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace MPL::ImageSpaceDiagnostics
{
    inline void LogData(
        const std::string_view a_phase,
        const RE::FormID a_record,
        const RE::ImageSpaceBaseData& a_data)
    {
        const std::array values{
            a_data.cinematic.saturation, a_data.cinematic.brightness, a_data.cinematic.contrast,
            a_data.tint.amount, a_data.tint.color.red, a_data.tint.color.green, a_data.tint.color.blue,
            a_data.hdr.white, a_data.hdr.sunlightScale, a_data.hdr.skyScale,
            a_data.hdr.eyeAdaptSpeed, a_data.hdr.eyeAdaptStrength,
            a_data.hdr.bloomBlurRadius, a_data.hdr.bloomThreshold,
            a_data.hdr.bloomScale, a_data.hdr.receiveBloomThreshold,
            a_data.depthOfField.strength, a_data.depthOfField.distance, a_data.depthOfField.range
        };
        const auto finite = std::ranges::all_of(values, [](const float a_value) { return std::isfinite(a_value); });
        SKSE::log::info(
            "[Image Space Diagnostic] phase={} | record={:08X} | data={} | finite={} | "
            "saturation={} | brightness={} | contrast={} | tintAmount={} | tintRGB={},{},{} | "
            "white={} | sunlight={} | sky={} | eyeAdaptSpeed={} | eyeAdaptStrength={} | "
            "bloomRadius={} | bloomThreshold={} | bloomScale={} | receiveBloomThreshold={} | dof={},{},{}",
            a_phase, a_record, static_cast<const void*>(&a_data), finite,
            a_data.cinematic.saturation, a_data.cinematic.brightness, a_data.cinematic.contrast,
            a_data.tint.amount, a_data.tint.color.red, a_data.tint.color.green, a_data.tint.color.blue,
            a_data.hdr.white, a_data.hdr.sunlightScale, a_data.hdr.skyScale,
            a_data.hdr.eyeAdaptSpeed, a_data.hdr.eyeAdaptStrength,
            a_data.hdr.bloomBlurRadius, a_data.hdr.bloomThreshold,
            a_data.hdr.bloomScale, a_data.hdr.receiveBloomThreshold,
            a_data.depthOfField.strength, a_data.depthOfField.distance, a_data.depthOfField.range);
    }

    inline void LogRecord(const std::string_view a_phase, const RE::TESImageSpace* a_record)
    {
        if (!a_record) return;
        const auto* file = a_record->GetFile();
        SKSE::log::info(
            "[Image Space Diagnostic] phase={} | record={:08X} | plugin={}",
            a_phase, a_record->GetFormID(), file ? file->GetFilename() : "<none>");
        LogData(a_phase, a_record->GetFormID(), a_record->data);
    }

    inline void Snapshot(const std::string_view a_phase, const RE::FormID a_eventCell)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        auto* sky = RE::Sky::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto* main = RE::Main::GetSingleton();
        auto* blur = RE::UIBlurManager::GetSingleton();
        const auto fader = ui ? ui->GetMenu<RE::FaderMenu>() : RE::GPtr<RE::FaderMenu>{};
        SKSE::log::info(
            "[Visual State] phase={} | eventCell={:08X} | playerCell={:08X} | interior={} | "
            "cellAttached={} | loadedData={} | uiAvailable={} | loadingMenu={} | faderPresent={} | "
            "faderActive={} | paused={} | freezeTime={} | blurCount={} | currentWeather={:08X} | "
            "lastWeather={:08X} | overrideWeather={:08X} | weatherPct={}",
            a_phase, a_eventCell, cell ? cell->GetFormID() : 0,
            cell && cell->IsInteriorCell(), cell && cell->IsAttached(),
            cell && cell->GetRuntimeData().loadedData != nullptr, ui != nullptr,
            ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME), static_cast<bool>(fader),
            fader ? static_cast<int>(fader->GetRuntimeData().isActive) : -1,
            ui ? static_cast<int>(ui->GameIsPaused()) : -1,
            main ? static_cast<int>(main->GetRuntimeData().freezeTime) : -1,
            blur ? static_cast<int>(blur->blurCount) : -1,
            sky && sky->currentWeather ? sky->currentWeather->GetFormID() : 0,
            sky && sky->lastWeather ? sky->lastWeather->GetFormID() : 0,
            sky && sky->overrideWeather ? sky->overrideWeather->GetFormID() : 0,
            sky ? sky->currentWeatherPct : -1.0f);
        if (cell)
        {
            const auto* extra = cell->extraList.GetByType<RE::ExtraCellImageSpace>();
            SKSE::log::info("[Visual State] phase={} | cellImageSpace={:08X}", a_phase,
                extra && extra->imageSpace ? extra->imageSpace->GetFormID() : 0);
            LogRecord("cell-record", extra ? extra->imageSpace : nullptr);
        }
        if (sky && sky->currentWeather)
        {
            for (std::size_t slot = 0; slot < RE::TESWeather::ColorTime::kTotal; ++slot)
            {
                LogRecord(std::string("weather-slot-") + std::to_string(slot),
                    sky->currentWeather->imageSpaces[slot]);
            }
        }
        auto* manager = RE::ImageSpaceManager::GetSingleton();
        SKSE::log::info("[Visual State] phase={} | imageSpaceManager={}", a_phase, manager != nullptr);
        if (manager)
        {
            SKSE::log::info("[Visual State] phase={} | currentBaseAvailable={}",
                a_phase, manager->GetCurrentBaseData() != nullptr);
            if (const auto* current = manager->GetCurrentBaseData())
                LogData("current-base", 0, *current);
            const auto& data = manager->GetImageSpaceData();
            LogData("effective-base", 0, data.baseData);
            const auto& mod = data.modData.data;
            SKSE::log::info(
                "[Visual State] phase={} | baseAmount={} | modAmount={} | modifierFinite={} | "
                "fadeAmount={} | fadeRGB={},{},{} | modifierBlur={}",
                a_phase, data.baseAmount, data.modAmount,
                std::ranges::all_of(mod, [](const float a_value) { return std::isfinite(a_value); }),
                mod[RE::ImageSpaceModData::kFadeAmount], mod[RE::ImageSpaceModData::kFadeR],
                mod[RE::ImageSpaceModData::kFadeG], mod[RE::ImageSpaceModData::kFadeB],
                mod[RE::ImageSpaceModData::kBlurRadius]);
        }
        const auto* curve = RE::GetINISetting("bUseFilmicCurve:Display");
        const auto* white = RE::GetINISetting("fFilmicWhiteScale:Display");
        SKSE::log::info(
            "[Visual State] phase={} | filmicCurve={} | filmicWhiteScaleAvailable={} | filmicWhiteScale={}",
            a_phase, curve ? static_cast<int>(curve->GetBool()) : -1,
            white != nullptr, white ? white->GetFloat() : 0.0f);
        spdlog::default_logger()->flush();
    }

    inline void QueueSnapshot(
        const std::string_view a_phase, const RE::FormID a_eventCell, bool (*a_enabled)())
    {
        if (!a_enabled()) return;
        const auto* tasks = SKSE::GetTaskInterface();
        SKSE::log::info(
            "[Visual State] phase={} | eventCell={:08X} | snapshot={}",
            a_phase, a_eventCell, tasks ? "queued" : "task-interface-unavailable");
        spdlog::default_logger()->flush();
        if (tasks)
        {
            tasks->AddTask([phase = std::string(a_phase), a_eventCell, a_enabled]
            {
                if (a_enabled()) Snapshot(phase, a_eventCell);
            });
        }
    }
}

