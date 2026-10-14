#pragma once
// Global key polling for hotkeys that must work while the game (not our window) has focus.
#include "imgui.h"

#include <unordered_map>

namespace keys
{
int  VirtualKey(ImGuiKey key); // Win32 VK code, 0 when the ImGui key has no direct equivalent
bool Down(ImGuiKey key);

// Rising-edge detector: call pressed() once per frame per key.
class Edges
{
public:
    bool pressed(ImGuiKey key);

private:
    std::unordered_map<int, bool> held_;
};
}
