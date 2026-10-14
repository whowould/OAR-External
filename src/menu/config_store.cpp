#include "menu/config_store.h"

#include "util/paths.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace store
{
namespace
{
using util::Wide;
using util::Utf8;

std::wstring PathOf(const std::string& name) { return Wide(Directory() + "\\" + name + ".cfg"); }

bool ReadFile(const std::wstring& path, std::string& out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
        out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteFile(const std::wstring& path, const std::string& data)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

double FileTimeToEpoch(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)(u.QuadPart / 10000000ull) - 11644473600.0;
}
}

std::string Directory() { return util::DataSubDir("configs"); }

std::string Sanitize(std::string name)
{
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return c < 32 || strchr("\\/:*?\"<>|", c) != nullptr; }), name.end());
    const size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(" .");
    return a == std::string::npos || b == std::string::npos ? std::string() : name.substr(a, b - a + 1);
}

std::vector<Entry> List()
{
    std::vector<Entry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(Wide(Directory() + "\\*.cfg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        std::wstring n = fd.cFileName;
        n.resize(n.size() - 4); // ".cfg"
        out.push_back({ Utf8(n), FileTimeToEpoch(fd.ftLastWriteTime) });
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.modified > b.modified; });
    return out;
}

bool Save(const std::string& name, const Settings& s)
{
    const std::string clean = Sanitize(name);
    return !clean.empty() && WriteFile(PathOf(clean), settings::ToText(s));
}

bool Load(const std::string& name, Settings& out)
{
    std::string text;
    return ReadFile(PathOf(Sanitize(name)), text) && settings::FromText(text, out);
}

bool Remove(const std::string& name) { return ::DeleteFileW(PathOf(Sanitize(name)).c_str()) != 0; }
bool Exists(const std::string& name) { return ::GetFileAttributesW(PathOf(Sanitize(name)).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool LoadCurrent(Settings& out)
{
    std::string text;
    return ReadFile(Wide(util::DataDir() + "\\settings.cfg"), text) && settings::FromText(text, out);
}

void SaveCurrent(const Settings& s) { WriteFile(Wide(util::DataDir() + "\\settings.cfg"), settings::ToText(s)); }

double AgeSeconds(const Entry& e) { return std::max(0.0, (double)std::time(nullptr) - e.modified); }
}
