#pragma once
// What the scanner thread publishes and the overlay / menu consume: an immutable per-tick picture of the game.
#include "game/unreal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scene
{
using ue::Rot;
using ue::Vec3;

enum class Link
{
    Detached,     // game not running
    AccessDenied, // running, but this process is not elevated enough to open it
    Incompatible, // attached, but the engine layout / globals were not recognised
    Waiting,      // attached, engine not ready yet (no world / no player)
    Ready,
};

struct View // APlayerCameraManager::CameraCachePrivate.POV
{
    Vec3    location;
    Rot     rotation;
    float   fov = 90.0f;
    float   aspect = 16.0f / 9.0f;
    bool    constrainAspect = false;
    uint8_t axisConstraint = 0; // EAspectRatioAxisConstraint: 0 = MaintainYFOV, 1 = MaintainXFOV, 2 = MajorAxisFOV
    bool    valid = false;
};

struct Guard
{
    uintptr_t id = 0;
    Vec3      pos;           // capsule centre
    float     halfHeight = 88, radius = 34;
    int       health = 0, maxHealth = 0;
    bool      dead = false, patrolling = false, investigating = false, alert = false, escorting = false,
              emped = false, tiedUp = false;
};

struct Camera
{
    uintptr_t id = 0;
    Vec3      pos, forward;
    int       number = 0;
    bool      destroyed = false, emped = false, possessed = false, ignored = false, spotting = false;
};

struct Phone // GuardPhone: dropped by a dead guard, must be brought to a scan terminal before the timer ends
{
    uintptr_t id = 0;
    Vec3      pos;
    bool      active = false, emped = false;
    int       seconds = 0;
};

struct Terminal // GuardScanPoint: the wall device the phone is checked in at
{
    uintptr_t id = 0;
    Vec3      pos;
    bool      activated = false;
};

struct DebugActor
{
    Vec3        pos;
    std::string label;
};

struct Stats
{
    float    tickMs = 0, discoverMs = 0, tickHz = 0;
    uint64_t reads = 0;
    float    readsPerSec = 0;
    size_t   names = 0, offsetsCached = 0;
    uint64_t cacheHits = 0, cacheMisses = 0;
    size_t   actorsSeen = 0;
    size_t   ghosted = 0; // NPCs Ghost is currently working on
};

struct Event // something worth telling the user (check-in forced, world changed...)
{
    double      time = 0;
    std::string text, highlight;
    int         kind = 0; // 0 info, 1 success, 2 warning
};

struct Snapshot
{
    uint64_t    seq = 0;
    Link        link = Link::Detached;
    std::string detail;        // human readable reason while link != Ready
    uint32_t    pid = 0;
    uintptr_t   base = 0;
    std::string buildId, worldName;
    HWND        gameWindow = nullptr;

    View                  view;
    std::vector<Guard>    guards;
    std::vector<Camera>   cameras;
    std::vector<Phone>    phones;
    std::vector<Terminal> terminals;
    // Refreshed at discovery rate only and shared between snapshots; null unless debug drawing is on.
    std::shared_ptr<const std::vector<DebugActor>> debugActors;
    Stats                 stats;
};
using SnapshotPtr = std::shared_ptr<const Snapshot>;
}
