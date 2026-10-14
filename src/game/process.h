#pragma once
// Out-of-process access to the game: attach, typed reads/writes, byte-pattern scanning of the main image.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace game
{
// IDA-style byte pattern: hex bytes separated by spaces, "?" or "??" for wildcards ("48 8B ?? 05").
class Pattern
{
public:
    explicit Pattern(std::string_view text);
    size_t size() const { return bytes_.size(); }
    // Offsets of every match inside [data, data + len).
    void findAll(const uint8_t* data, size_t len, std::vector<size_t>& out, size_t limit) const;

private:
    std::vector<int16_t> bytes_; // -1 = wildcard
    size_t anchor_ = 0;          // first non-wildcard byte, used to skip quickly with memchr
};

enum class AttachResult { Ok, NotRunning, AccessDenied, Failed };

class Process
{
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() { detach(); }

    AttachResult attach(const wchar_t* exeName);
    // Attaches to the calling process itself (lets tests exercise code that reads and writes "game" memory).
    bool attachSelf();
    void detach();
    bool attached() const { return handle_ != nullptr; }
    bool alive() const;

    uint32_t  pid() const { return pid_; }
    uintptr_t base() const { return base_; }
    size_t    imageSize() const { return imageSize_; }
    // PE timestamp + size + content (.pak) stamp: identifies a build and keys the offset cache.
    std::string buildId() const { return buildId_; }
    HWND mainWindow() const;

    bool read(uintptr_t addr, void* out, size_t size) const;
    bool write(uintptr_t addr, const void* data, size_t size) const;
    template <class T> bool read(uintptr_t addr, T& out) const { return read(addr, &out, sizeof(T)); }
    template <class T> T read(uintptr_t addr) const
    {
        T v{};
        read(addr, &v, sizeof(T));
        return v;
    }
    template <class T> bool write(uintptr_t addr, const T& v) const { return write(addr, &v, sizeof(T)); }

    // Addresses of every pattern match in the main module image.
    std::vector<uintptr_t> scan(const Pattern& pattern, size_t limit = 32) const;

    uint64_t readCount() const { return reads_.load(std::memory_order_relaxed); }

private:
    HANDLE      handle_ = nullptr;
    uint32_t    pid_ = 0;
    uintptr_t   base_ = 0;
    size_t      imageSize_ = 0;
    std::string buildId_;
    mutable std::atomic<uint64_t> reads_{ 0 };
};
}
