#include "game/cache.h"

#include "util/paths.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game
{
void OffsetCache::open(const std::string& buildId)
{
    close();
    if (buildId.empty()) return;
    path_ = util::DataSubDir("cache") + "\\offsets-" + buildId + ".txt";
    if (FILE* f = _wfopen(util::Wide(path_).c_str(), L"r"))
    {
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            char* eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            *eq = 0;
            char* end = nullptr;
            const uint64_t value = strtoull(eq + 1, &end, 16);
            if (end == eq + 1) continue; // no digits: ignore the line rather than cache a bogus zero
            entries_[line] = value;
        }
        fclose(f);
    }
}

void OffsetCache::close()
{
    flush();
    entries_.clear();
    path_.clear();
    dirty_ = false;
}

bool OffsetCache::get(const std::string& key, uint64_t& value) const
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        misses_++;
        return false;
    }
    hits_++;
    value = it->second;
    return true;
}

void OffsetCache::set(const std::string& key, uint64_t value)
{
    auto [it, inserted] = entries_.try_emplace(key, value);
    if (!inserted && it->second == value) return;
    it->second = value;
    dirty_ = true;
}

void OffsetCache::erase(const std::string& key)
{
    if (entries_.erase(key)) dirty_ = true;
}

// Written to a temporary file first and renamed over the old one, so a crash can't leave a truncated cache.
void OffsetCache::flush()
{
    if (!dirty_ || path_.empty()) return;
    const std::wstring target = util::Wide(path_), temp = target + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"w");
    if (!f) return;
    bool ok = true;
    for (const auto& [k, v] : entries_)
        ok &= fprintf(f, "%s=%llx\n", k.c_str(), (unsigned long long)v) > 0;
    ok &= fclose(f) == 0;
    if (ok && ::MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        dirty_ = false;
    else
        ::DeleteFileW(temp.c_str());
}
}
