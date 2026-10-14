#include "util/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace util
{
std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    std::string s((size_t)::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    std::wstring w((size_t)::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

std::string DataDir()
{
    static const std::string dir = [] {
        // OAR_DATA_DIR redirects everything (settings, configs, cache): tests and screenshots must not touch real data.
        wchar_t overridden[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(L"OAR_DATA_DIR", overridden, MAX_PATH) > 0)
        {
            ::CreateDirectoryW(overridden, nullptr);
            return Utf8(overridden);
        }
        PWSTR known = nullptr;
        std::wstring base;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)))
            base = known;
        ::CoTaskMemFree(known);
        if (base.empty()) base = L".";
        base += L"\\OAR External";
        ::CreateDirectoryW(base.c_str(), nullptr);
        return Utf8(base);
    }();
    return dir;
}

std::string DataSubDir(const char* name)
{
    std::string path = DataDir() + "\\" + name;
    ::CreateDirectoryW(Wide(path).c_str(), nullptr);
    return path;
}
}
