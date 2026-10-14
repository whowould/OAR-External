#pragma once
// Ghost: NPCs ignore the local player, even when they can see them.
//
// What the game's Blueprints do, and what is changed to defeat each part:
//   * Guards and police see and hear through a PawnSensing component (guards' eyes live in a separate
//     SensingActor child actor). `bSeePawns` / `bHearNoises` are cleared, so OnSeePawn / OnHearNoise never fire.
//   * Security cameras run CameraCheck() on every tick unless `Ignored?` (or `EMPed`) is set; `Ignored?` is
//     the game's own "camera is disabled" flag, so it is set.
//   * Awareness that already points at the local player is dropped: SpotPlayerComponent.SpottedPlayer (a pending
//     spot timer then fires for nobody), TargetPlayer / EscortingPlayer on guards and police, and the player's `Sus?`.
//
// Everything changed is remembered and written back when Ghost is switched off (or one of its parts is), so a
// camera that was already disabled by the game stays disabled. Nothing is applied to objects that are gone.
//
// Limits: it works where the NPC logic runs (hosting or solo); the sensors cannot tell players apart, so
// teammates are ignored too; an alarm raised by something else (a body, a destroyed camera) can still send
// guards looking for you, and a move order already in progress finishes its path.
#include "game/process.h"
#include "game/unreal.h"
#include "settings.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace features
{
// Offsets the feature touches, resolved by reflection in the scanner. A field that is not found just
// disables the part of Ghost that needs it.
struct GhostLayout
{
    ue::Field seePawns, hearNoises;                         // PawnSensingComponent
    ue::Field spottedPlayer;                                // SpotPlayerComponent
    ue::Field sensorEyes;                                   // SensingActor_C::PawnSensing (a guard's eyes)
    ue::Field guardEars, guardSpot, guardTarget, guardEscorting, guardAware; // NPC_Guard_C: PawnSensing, SpotPlayerComponent, TargetPlayer, EscortingPlayer, Sensing?
    ue::Field policeEyes, policeTarget, policeAware;        // NPC_Police_base_C: PawnSensing, TargetPlayer, Sensing?
    ue::Field cameraIgnored, cameraSpot;                    // CameraBP_C: Ignored?, SpotPlayerComponent
    ue::Field pawnSus;                                      // PlayerCharacter_C::Sus?
};

// Live objects of this tick (addresses of actors the scanner is tracking).
struct GhostWorld
{
    uintptr_t              localPawn = 0;
    std::vector<uintptr_t> guards, police, cameras, sensors;
};

class Ghost
{
public:
    using AliveFn = std::function<bool(uintptr_t)>; // actor is still the same live object

    // Applies the enabled parts, restores the disabled ones. Returns how many NPCs are being ghosted.
    size_t tick(const game::Process& proc, const GhostLayout& layout, const Settings& cfg, const GhostWorld& world, const AliveFn& alive);
    // Writes every remembered original back (for objects that are still alive).
    void restore(const game::Process& proc, const AliveFn& alive);
    // The objects are gone (world change, detach): forget the saved state without writing.
    void forget();
    bool engaged() const { return !sensing_.empty() || !cameras_.empty(); }

private:
    enum class Group : uint8_t { Guards, Police, Cameras };
    struct SavedSensing { uintptr_t owner; uint8_t bits; Group group; };
    struct SavedCamera  { uint8_t ignored; };

    void blind(const game::Process& proc, const GhostLayout& f, uintptr_t owner, const ue::Field& sensingField, Group group);
    void blindCamera(const game::Process& proc, const GhostLayout& f, uintptr_t camera);
    void dropAwareness(const game::Process& proc, const GhostLayout& f, uintptr_t me, uintptr_t owner, const ue::Field& spot,
                       const ue::Field& target, const ue::Field& escort, const ue::Field& aware);
    void restoreGroup(const game::Process& proc, const AliveFn& alive, Group group);
    void pruneDead(const AliveFn& alive);

    GhostLayout layout_; // as of the last tick: restore() needs the offsets the writes used
    std::unordered_map<uintptr_t, SavedSensing> sensing_; // PawnSensingComponent -> original bits
    std::unordered_map<uintptr_t, SavedCamera>  cameras_; // CameraBP actor -> original `Ignored?`
    double lastPrune_ = 0.0;
};
}
