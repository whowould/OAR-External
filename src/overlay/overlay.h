#pragma once
// Transparent, click-through, always-on-top window that sits exactly over the game's client area and
// draws the ESP with its own Dear ImGui context (the menu keeps its own, in a separate window).
#include "game/scene.h"
#include "settings.h"

#include <windows.h>

#include <string>

struct IDirect3D9Ex;
struct IDirect3DDevice9Ex;
struct ImGuiContext;
struct ImFont;

namespace overlay
{
class Overlay
{
public:
    Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    ~Overlay() { destroy(); }

    bool create(HINSTANCE instance);
    void destroy();

    // The overlay is kept directly beneath this (topmost) window in z-order, so the menu always stays on top of the ESP.
    void setAnchor(HWND above) { anchor_ = above; }

    // Draws one frame over `area` (screen coordinates), or hides the window when `visible` is false.
    void frame(const scene::Snapshot& snap, const Settings& cfg, const RECT& area, bool visible);

    // Renders one frame into a BMP without showing the window (development aid).
    bool capture(const scene::Snapshot& snap, const Settings& cfg, const std::wstring& path);

    HWND  hwnd() const { return hwnd_; }
    bool  shown() const { return shown_; }
    float frameMs() const { return frameMs_; }

private:
    bool draw(const scene::Snapshot& snap, const Settings& cfg, int w, int h, double now, const std::wstring* capturePath);
    bool createDevice();
    void releaseDevice();
    bool recoverDevice(double now);
    bool resizeBackBuffer(UINT w, UINT h);
    void loadFonts();

    HWND                hwnd_ = nullptr;
    HWND                anchor_ = nullptr;
    ATOM                class_ = 0;
    HINSTANCE           instance_ = nullptr;
    IDirect3D9Ex*       d3d_ = nullptr;
    IDirect3DDevice9Ex* device_ = nullptr;
    ImGuiContext*       context_ = nullptr;
    ImFont*             font_ = nullptr;
    void*               presentParams_ = nullptr; // D3DPRESENT_PARAMETERS, kept out of the header
    RECT                placed_{};
    bool                shown_ = false;
    bool                deviceLost_ = false;
    bool                backendUp_ = false;   // ImGui's DX9 backend is initialised for the current device
    double              lastRecovery_ = 0.0;
    double              lastFrame_ = 0.0;
    float               frameMs_ = 0.0f;
};
}
