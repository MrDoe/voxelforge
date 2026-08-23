#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

namespace vf {

inline void initLogging()
{
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);
}

} // namespace vf
