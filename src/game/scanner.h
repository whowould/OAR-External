#pragma once
// Background thread that owns the game process: attaches, resolves engine globals and reflection,
// tracks the entities we care about and publishes an immutable Snapshot every tick (~125 Hz).
// The UI and overlay threads only ever touch the thread-safe interface below.
#include "game/scene.h"
#include "settings.h"

#include <memory>
#include <vector>

namespace scene
{
class Scanner
{
public:
    Scanner();
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    void start();
    void stop();

    SnapshotPtr        snapshot() const;            // latest picture, never null
    void               setSettings(const Settings& s);
    void               requestCheckIn();            // check in every active phone on the next tick
    std::vector<Event> takeEvents();                // events produced since the last call

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
