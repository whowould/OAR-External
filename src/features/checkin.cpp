#include "features/checkin.h"

namespace features
{
int CheckIn::tick(const game::Process& proc, const ue::Field& activeFlag, std::vector<scene::Phone>& phones, const Settings& settings,
                  const std::function<bool(uintptr_t)>& stillValid)
{
    const bool manual = requested_.exchange(false, std::memory_order_relaxed);
    if (!activeFlag || activeFlag.mask == 0) return 0; // not a resolved bool: never write

    int checkedIn = 0;
    for (scene::Phone& phone : phones)
    {
        if (!phone.active) continue;
        const bool due = manual || (settings.checkinAuto && phone.seconds <= (int)settings.checkinBelow);
        if (due && stillValid(phone.id) && ue::WriteBool(proc, phone.id, activeFlag, false))
        {
            phone.active = false;
            checkedIn++;
        }
    }
    return checkedIn;
}
}
