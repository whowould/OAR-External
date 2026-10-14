#pragma once
#include <string>

struct IDirect3DDevice9;

namespace util
{
// Saves the device's current back buffer as a top-down 32-bit BMP (BGRA). `opaque` forces alpha to 255
// (menu window); otherwise the alpha channel is kept (overlay). Call between EndScene() and Present().
bool SaveBackBuffer(IDirect3DDevice9* device, const std::wstring& path, bool opaque);
}
