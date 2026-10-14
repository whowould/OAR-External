#include "features/ghost.h"

#include "util/paths.h"

namespace features
{
namespace
{
// A user-mode heap/object pointer: non-trivial, 8-aligned, below the top of the address space.
bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x00007FFFFFFF0000ull && (p & 7) == 0; }
uintptr_t ReadPtr(const game::Process& proc, uintptr_t addr) { return proc.read<uintptr_t>(addr); }
}

// Clears the sight and hearing bits of the PawnSensing component that `owner` points to (at `sensingField`).
// The original bits are remembered the first time the component is seen.
void Ghost::blind(const game::Process& proc, const GhostLayout& f, uintptr_t owner, const ue::Field& sensingField, Group group)
{
    // Both flags are bits of one byte (checked below); anything else means the layout is not what we expect.
    if (!sensingField || !f.seePawns || !f.hearNoises || f.seePawns.offset != f.hearNoises.offset) return;
    const uintptr_t comp = ReadPtr(proc, owner + sensingField.offset);
    if (!Plausible(comp)) return;

    uint8_t byte = 0;
    if (!proc.read(comp + f.seePawns.offset, byte)) return;
    const uint8_t mask = (uint8_t)(f.seePawns.mask | f.hearNoises.mask);
    sensing_.try_emplace(comp, SavedSensing{ owner, (uint8_t)(byte & mask), group });
    if (byte & mask)
        proc.write<uint8_t>(comp + f.seePawns.offset, (uint8_t)(byte & ~mask));
}

// A camera with `Ignored?` set skips CameraCheck() entirely. If the game had already set it, that stays so.
void Ghost::blindCamera(const game::Process& proc, const GhostLayout& f, uintptr_t camera)
{
    if (!f.cameraIgnored) return;
    uint8_t value = 0;
    if (!proc.read(camera + f.cameraIgnored.offset, value)) return;
    cameras_.try_emplace(camera, SavedCamera{ value });
    if (value == 0)
        ue::WriteBool(proc, camera, f.cameraIgnored, true);
}

// Drops whatever this NPC currently holds about the local player: its chase target, escort and spot timer.
void Ghost::dropAwareness(const game::Process& proc, const GhostLayout& f, uintptr_t me, uintptr_t owner, const ue::Field& spot,
                          const ue::Field& target, const ue::Field& escort, const ue::Field& aware)
{
    if (!me) return;
    if (target && ReadPtr(proc, owner + target.offset) == me)
    {
        proc.write<uintptr_t>(owner + target.offset, 0);
        if (aware) ue::WriteBool(proc, owner, aware, false); // "Sensing?" gates the shoot loop
    }
    if (escort && ReadPtr(proc, owner + escort.offset) == me)
        proc.write<uintptr_t>(owner + escort.offset, 0);
    if (spot && f.spottedPlayer)
    {
        // A running spot timer broadcasts PlayerSpotted(SpottedPlayer) when it ends: with nobody there it raises nothing.
        const uintptr_t comp = ReadPtr(proc, owner + spot.offset);
        if (Plausible(comp) && ReadPtr(proc, comp + f.spottedPlayer.offset) == me)
            proc.write<uintptr_t>(comp + f.spottedPlayer.offset, 0);
    }
}

size_t Ghost::tick(const game::Process& proc, const GhostLayout& layout, const Settings& cfg, const GhostWorld& world, const AliveFn& alive)
{
    layout_ = layout;
    const bool on = cfg.ghost;
    const bool guards = on && cfg.ghostGuards, police = on && cfg.ghostPolice, cameras = on && cfg.ghostCameras;

    // Parts that are off give back what they changed.
    if (!guards) restoreGroup(proc, alive, Group::Guards);
    if (!police) restoreGroup(proc, alive, Group::Police);
    if (!cameras) restoreGroup(proc, alive, Group::Cameras);
    if (!on) return 0;

    const double now = util::Now();
    if (now - lastPrune_ > 0.5)
    {
        lastPrune_ = now;
        pruneDead(alive);
    }

    const uintptr_t me = world.localPawn;
    const ue::Field none;
    size_t affected = 0;

    if (guards)
    {
        for (uintptr_t sensor : world.sensors)
            if (alive(sensor)) blind(proc, layout, sensor, layout.sensorEyes, Group::Guards);
        for (uintptr_t guard : world.guards)
        {
            if (!alive(guard)) continue;
            blind(proc, layout, guard, layout.guardEars, Group::Guards);
            dropAwareness(proc, layout, me, guard, layout.guardSpot, layout.guardTarget, layout.guardEscorting, layout.guardAware);
            affected++;
        }
    }
    if (police)
        for (uintptr_t cop : world.police)
        {
            if (!alive(cop)) continue;
            blind(proc, layout, cop, layout.policeEyes, Group::Police);
            dropAwareness(proc, layout, me, cop, none, layout.policeTarget, none, layout.policeAware);
            affected++;
        }
    if (cameras)
        for (uintptr_t cam : world.cameras)
        {
            if (!alive(cam)) continue;
            blindCamera(proc, layout, cam);
            dropAwareness(proc, layout, me, cam, layout.cameraSpot, none, none, none);
            affected++;
        }

    // "Suspicious" makes guards react to the player; keep it off.
    uint8_t flag = 0;
    if (me && layout.pawnSus && proc.read(me + layout.pawnSus.offset, flag) && (flag & layout.pawnSus.mask))
        ue::WriteBool(proc, me, layout.pawnSus, false);
    return affected;
}

void Ghost::restoreGroup(const game::Process& proc, const AliveFn& alive, Group group)
{
    const uint8_t mask = (uint8_t)(layout_.seePawns.mask | layout_.hearNoises.mask);
    for (auto it = sensing_.begin(); it != sensing_.end();)
    {
        if (it->second.group != group)
        {
            ++it;
            continue;
        }
        uint8_t byte = 0;
        if (alive(it->second.owner) && proc.read(it->first + layout_.seePawns.offset, byte))
            proc.write<uint8_t>(it->first + layout_.seePawns.offset, (uint8_t)((byte & ~mask) | (it->second.bits & mask)));
        it = sensing_.erase(it);
    }
    if (group == Group::Cameras)
    {
        for (const auto& [camera, saved] : cameras_)
            if (alive(camera))
                ue::WriteBool(proc, camera, layout_.cameraIgnored, saved.ignored != 0);
        cameras_.clear();
    }
}

void Ghost::pruneDead(const AliveFn& alive)
{
    for (auto it = sensing_.begin(); it != sensing_.end();)
        it = alive(it->second.owner) ? std::next(it) : sensing_.erase(it);
    for (auto it = cameras_.begin(); it != cameras_.end();)
        it = alive(it->first) ? std::next(it) : cameras_.erase(it);
}

void Ghost::restore(const game::Process& proc, const AliveFn& alive)
{
    restoreGroup(proc, alive, Group::Guards);
    restoreGroup(proc, alive, Group::Police);
    restoreGroup(proc, alive, Group::Cameras);
}

void Ghost::forget()
{
    sensing_.clear();
    cameras_.clear();
}
}
