#pragma once
#include <chrono>
#include <string>

namespace util
{
// Seconds on a monotonic clock, usable from any thread (ImGui::GetTime() is per-context).
inline double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::wstring Wide(const std::string& utf8);
std::string  Utf8(const std::wstring& wide);

// %LOCALAPPDATA%\OAR External (created on first use), UTF-8, no trailing separator.
std::string DataDir();
// Subdirectory of DataDir() (created on first use).
std::string DataSubDir(const char* name);
}
