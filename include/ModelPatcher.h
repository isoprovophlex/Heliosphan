#pragma once

#include <MMSF_API.h>
#include <ModelPatchRules.h>

namespace RE
{
    class TESObjectREFR;
}

namespace MPL::ModelPatcher
{
    void ClearProfiles();
    void AddProfile(std::string_view, const std::vector<Rule>&);
    void Initialize();
    void Reset();
    void QueueReference(RE::TESObjectREFR*);
    void PrepareReference(RE::TESObjectREFR*);
    bool HasModelRules(const char*);
    bool RegisterService(API::MMSF::Interface*);
}
