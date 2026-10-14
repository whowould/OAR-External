#pragma once
// Persistent key -> integer cache for everything that is expensive to find but stable for one game
// build (global addresses, reflected property offsets). One file per build, so a game update simply
// starts from an empty cache instead of trusting stale values.
#include <cstdint>
#include <string>
#include <unordered_map>

namespace game
{
class OffsetCache
{
public:
    // Loads the cache file of `buildId` (empty if it does not exist yet).
    void open(const std::string& buildId);
    void close();

    bool   get(const std::string& key, uint64_t& value) const;
    void   set(const std::string& key, uint64_t value);
    void   erase(const std::string& key); // drops a value that failed validation
    void   flush(); // writes the file if anything changed since the last flush
    size_t size() const { return entries_.size(); }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    std::string path_;
    std::unordered_map<std::string, uint64_t> entries_;
    bool dirty_ = false;
    mutable uint64_t hits_ = 0, misses_ = 0;
};
}
