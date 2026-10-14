#pragma once
// Draws the ESP for one snapshot into an ImGui draw list.
#include "game/scene.h"
#include "imgui.h"
#include "settings.h"

namespace overlay
{
// `screen` is the size of the game viewport in pixels (the overlay window matches the game's client area).
void DrawEsp(ImDrawList* dl, ImFont* font, ImVec2 screen, const scene::Snapshot& snap, const Settings& cfg);
}
