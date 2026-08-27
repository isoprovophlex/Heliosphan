#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <RE/Skyrim.h>
#include <REX/REX.h>
#include <SKSE/SKSE.h>
#include <format>
#include <rfl/json.hpp>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <windows.h>

namespace logger = SKSE::log;
using namespace std::literals;
