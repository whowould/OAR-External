#pragma once
// Services the menu needs from the host window (implemented in main.cpp).
// The menu draws its own title bar, so it tells the window which areas are draggable.
#include "imgui.h"

namespace host
{
void  Minimize();
void  Close();
void  SetCaptionHeight(float px);                          // drag region, from the top of the client area
void  AddNoDragRect(const ImVec2& min, const ImVec2& max);  // interactive rect inside the caption (rebuilt every frame)
float FrameWorkMs();                                       // CPU time the previous menu frame took (excludes the vsync wait)
float OverlayMs();                                         // CPU time the previous overlay frame took
}
