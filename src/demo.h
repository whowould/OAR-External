#pragma once
// A synthetic scene for `oar.exe --demo`: lets the menu and the ESP be tried (and screenshotted) without a running mission.
#include "game/scene.h"

namespace demo
{
scene::SnapshotPtr Make(double time);
}
