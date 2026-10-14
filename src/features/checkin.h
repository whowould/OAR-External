#pragma once
// Force guard check-in.
//
// A dead guard drops a GuardPhone. Its Blueprint counts SecondsToAlert down once per second while
// `Active?` is true and raises "Guard did not check in" when it runs out; bringing the phone to a
// GuardScanPoint calls StopPhoneAlert(), which sets `Active? = false`. Clearing that flag from outside
// is the same state change: the countdown loop exits and the alarm branch is skipped.
//
// Not replicated: it takes effect where the phone logic runs, i.e. when hosting or playing solo.
// The phone's beeper and highlight are driven by StopAlertMulti() and keep going until it is picked up.
#include "game/process.h"
#include "game/scene.h"
#include "game/unreal.h"
#include "settings.h"

#include <atomic>
#include <functional>
#include <vector>

namespace features
{
class CheckIn
{
public:
    void request() { requested_.store(true, std::memory_order_relaxed); } // "check in everything now"

    // Checks in every phone that is due (manual request, or auto mode with <= checkinBelow seconds left).
    // `stillValid` is asked right before each write, so a phone that was freed since it was read is never written to.
    // Updates `phones` to reflect the write and returns how many were checked in.
    int tick(const game::Process& proc, const ue::Field& activeFlag, std::vector<scene::Phone>& phones, const Settings& settings,
             const std::function<bool(uintptr_t)>& stillValid);

private:
    std::atomic<bool> requested_{ false };
};
}
