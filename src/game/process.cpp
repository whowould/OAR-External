#include "game/process.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace game
{
//-----------------------------------------------------------------------------
// Pattern
//-----------------------------------------------------------------------------

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)std::tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
}

Pattern::Pattern(std::string_view text)
{
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == ' ') { i++; continue; }
        if (text[i] == '?')
        {
            bytes_.push_back(-1);
            while (i < text.size() && text[i] == '?') i++;
        }
        else
        {
            bytes_.push_back((int16_t)(HexValue(text[i]) * 16 + (i + 1 < text.size() ? HexValue(text[i + 1]) : 0)));
            i += 2;
        }
    }
    while (anchor_ < bytes_.size() && bytes_[anchor_] < 0) anchor_++;
    if (anchor_ == bytes_.size()) anchor_ = 0;
}

void Pattern::findAll(const uint8_t* data, size_t len, std::vector<size_t>& out, size_t limit) const
{
    const size_t n = bytes_.size();
    if (n == 0 || len < n) return;
    const uint8_t first = (uint8_t)bytes_[anchor_];
    const size_t last = len - n; // last valid match start
    for (size_t pos = 0; pos <= last && out.size() < limit;)
    {
        const void* hit = memchr(data + pos + anchor_, first, last - pos + 1);
        if (!hit) break;
        const size_t start = (size_t)((const uint8_t*)hit - data) - anchor_;
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++)
            ok = bytes_[i] < 0 || data[start + i] == (uint8_t)bytes_[i];
        if (ok) out.push_back(start);
        pos = start + 1;
    }
}

//-----------------------------------------------------------------------------
// Process
//-----------------------------------------------------------------------------

static DWORD FindPid(const wchar_t* exeName)
{
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = { sizeof(e) };
    DWORD pid = 0;
    for (BOOL ok = ::Process32FirstW(snap, &e); ok && !pid; ok = ::Process32NextW(snap, &e))
        if (::_wcsicmp(e.szExeFile, exeName) == 0)
            pid = e.th32ProcessID;
    ::CloseHandle(snap);
    return pid;
}

// Blueprint classes (guards, cameras, phones...) are cooked into the .pak files, not the exe, so their
// property layouts can change in a content-only patch. This stamps the paks (name, size, mtime) so the
// offset cache is tied to the content as well as the executable. 0 if the paks cannot be found.
static uint32_t ContentStamp(HANDLE process)
{
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!::QueryFullProcessImageNameW(process, 0, path, &len)) return 0;

    // <game>\OAR\Binaries\Win64\<exe>  ->  <game>\OAR\Content\Paks
    std::wstring dir(path, len);
    for (int up = 0; up < 3; up++)
    {
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return 0;
        dir.resize(slash);
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = ::FindFirstFileW((dir + L"\\Content\\Paks\\*.pak").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;

    uint32_t hash = 2166136261u; // FNV-1a
    const auto mix = [&hash](const void* data, size_t n) {
        for (size_t i = 0; i < n; i++) hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
    };
    do
    {
        mix(fd.cFileName, wcslen(fd.cFileName) * sizeof(wchar_t));
        mix(&fd.nFileSizeLow, sizeof(fd.nFileSizeLow));
        mix(&fd.nFileSizeHigh, sizeof(fd.nFileSizeHigh));
        mix(&fd.ftLastWriteTime, sizeof(fd.ftLastWriteTime));
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
    return hash;
}

AttachResult Process::attach(const wchar_t* exeName)
{
    detach();
    const DWORD pid = FindPid(exeName);
    if (!pid) return AttachResult::NotRunning;

    HANDLE h = ::OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return ::GetLastError() == ERROR_ACCESS_DENIED ? AttachResult::AccessDenied : AttachResult::Failed;

    // The first module of a process is its main executable.
    uintptr_t base = 0;
    size_t size = 0;
    if (HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid); snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W m = { sizeof(m) };
        if (::Module32FirstW(snap, &m))
        {
            base = (uintptr_t)m.modBaseAddr;
            size = m.modBaseSize;
        }
        ::CloseHandle(snap);
    }
    if (!base)
    {
        ::CloseHandle(h);
        return AttachResult::Failed;
    }

    handle_ = h;
    pid_ = pid;
    base_ = base;
    imageSize_ = size;

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (read(base, dos) && read(base + dos.e_lfanew, nt))
    {
        char id[40];
        snprintf(id, sizeof(id), "%08x-%08x-%08x", (unsigned)nt.FileHeader.TimeDateStamp, (unsigned)nt.OptionalHeader.SizeOfImage, (unsigned)ContentStamp(handle_));
        buildId_ = id;
    }
    return AttachResult::Ok;
}

bool Process::attachSelf()
{
    detach();
    HANDLE self = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return false;
    handle_ = self;
    pid_ = ::GetCurrentProcessId();
    base_ = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    return true;
}

void Process::detach()
{
    if (handle_)
        ::CloseHandle(handle_);
    handle_ = nullptr;
    pid_ = 0;
    base_ = 0;
    imageSize_ = 0;
    buildId_.clear();
}

bool Process::alive() const
{
    DWORD code = 0;
    return handle_ && ::GetExitCodeProcess(handle_, &code) && code == STILL_ACTIVE;
}

bool Process::read(uintptr_t addr, void* out, size_t size) const
{
    if (!handle_ || !addr) return false;
    reads_.fetch_add(1, std::memory_order_relaxed);
    SIZE_T got = 0;
    return ::ReadProcessMemory(handle_, (LPCVOID)addr, out, size, &got) && got == size;
}

bool Process::write(uintptr_t addr, const void* data, size_t size) const
{
    if (!handle_ || !addr) return false;
    SIZE_T put = 0;
    return ::WriteProcessMemory(handle_, (LPVOID)addr, data, size, &put) && put == size;
}

std::vector<uintptr_t> Process::scan(const Pattern& pattern, size_t limit) const
{
    std::vector<uintptr_t> hits;
    if (!handle_ || pattern.size() == 0) return hits;

    constexpr size_t kChunk = 1 << 20, kPage = 0x1000;
    const size_t overlap = pattern.size() - 1;
    std::vector<uint8_t> buf(kChunk + overlap);
    std::vector<size_t> local;
    size_t carry = 0; // bytes kept from the previous chunk so matches spanning a boundary are found

    for (size_t off = 0; off < imageSize_ && hits.size() < limit; off += kChunk)
    {
        const size_t want = std::min(kChunk, imageSize_ - off);
        uint8_t* dst = buf.data() + carry;
        if (!read(base_ + off, dst, want))
        {
            // Some pages of a loaded image can be unreadable: fall back to page granularity.
            for (size_t p = 0; p < want; p += kPage)
            {
                const size_t n = std::min(kPage, want - p);
                if (!read(base_ + off + p, dst + p, n))
                    memset(dst + p, 0, n);
            }
        }
        const size_t total = carry + want;
        local.clear();
        pattern.findAll(buf.data(), total, local, limit - hits.size());
        for (size_t m : local)
            hits.push_back(base_ + off - carry + m);
        const size_t keep = std::min(overlap, total); // a match needs `size` bytes, so the tail can't hold one twice
        memmove(buf.data(), buf.data() + total - keep, keep);
        carry = keep;
    }
    return hits;
}

HWND Process::mainWindow() const
{
    struct Ctx { DWORD pid; HWND best; } ctx = { pid_, nullptr };
    ::EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            ::GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid != c->pid || !::IsWindowVisible(hwnd) || ::GetWindow(hwnd, GW_OWNER)) return TRUE;
            RECT rc;
            if (!::GetClientRect(hwnd, &rc) || rc.right < 320 || rc.bottom < 200) return TRUE;
            wchar_t cls[64] = {};
            ::GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"UnrealWindow") == 0) { c->best = hwnd; return FALSE; }
            if (!c->best) c->best = hwnd;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}
}
