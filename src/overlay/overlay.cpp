#include "overlay/overlay.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_freetype.h"
#include "overlay/esp.h"
#include "util/paths.h"
#include "util/screenshot.h"

#include <algorithm>

#include <d3d9.h>
#include <dwmapi.h>

#include <cstdio>
#include <new>

namespace overlay
{
namespace
{
const wchar_t* kClassName = L"OAROverlay";

// Makes the overlay's ImGui context current for a scope and restores the previous one (the menu's).
struct ContextScope
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* ctx) { ImGui::SetCurrentContext(ctx); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT; // never take mouse input
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool FileExists(const char* path)
{
    const DWORD attr = ::GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
}

static D3DPRESENT_PARAMETERS& Params(void* p) { return *static_cast<D3DPRESENT_PARAMETERS*>(p); }

bool Overlay::create(HINSTANCE instance)
{
    instance_ = instance;
    presentParams_ = new (std::nothrow) D3DPRESENT_PARAMETERS();
    if (!presentParams_) return false;

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, nullptr, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, kClassName, nullptr };
    class_ = ::RegisterClassExW(&wc);
    if (!class_) return false;

    const int w = ::GetSystemMetrics(SM_CXSCREEN), h = ::GetSystemMetrics(SM_CYSCREEN);
    hwnd_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"OAR overlay",
                              WS_POPUP, 0, 0, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd_) return false;

    // Per-pixel alpha comes from DWM glass: the whole client area is "frame", the back buffer carries alpha.
    ::SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 255, LWA_ALPHA);
    const MARGINS margins = { -1, -1, -1, -1 };
    ::DwmExtendFrameIntoClientArea(hwnd_, &margins);

    if (!createDevice()) return false;

    ContextScope scope(nullptr);
    context_ = ImGui::CreateContext(); // becomes current
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)w, (float)h);
    loadFonts();
    ImGui_ImplDX9_Init(reinterpret_cast<IDirect3DDevice9*>(device_));
    backendUp_ = true;
    placed_ = { 0, 0, w, h };
    return true;
}

void Overlay::destroy()
{
    if (context_)
    {
        ContextScope scope(context_);
        if (backendUp_) ImGui_ImplDX9_Shutdown();
        backendUp_ = false;
        ImGui::DestroyContext(context_);
        context_ = nullptr;
    }
    releaseDevice();
    if (hwnd_) ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (class_) ::UnregisterClassW(kClassName, instance_);
    class_ = 0;
    delete static_cast<D3DPRESENT_PARAMETERS*>(presentParams_);
    presentParams_ = nullptr;
}

bool Overlay::createDevice()
{
    if (FAILED(::Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_))) return false;
    RECT rc;
    ::GetClientRect(hwnd_, &rc);

    D3DPRESENT_PARAMETERS& pp = Params(presentParams_);
    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.hDeviceWindow = hwnd_;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;            // no flip model: per-pixel alpha needs the DWM redirection surface
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth = (UINT)(rc.right - rc.left);
    pp.BackBufferHeight = (UINT)(rc.bottom - rc.top);
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; // the host loop paces frames
    const DWORD flags = D3DCREATE_FPU_PRESERVE;
    HRESULT hr = d3d_->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_, flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &device_);
    if (FAILED(hr))
        hr = d3d_->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_, flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &device_);
    if (FAILED(hr)) return false;
    device_->SetMaximumFrameLatency(1);
    return true;
}

void Overlay::releaseDevice()
{
    if (device_) { device_->Release(); device_ = nullptr; }
    if (d3d_)    { d3d_->Release();    d3d_ = nullptr; }
}

// After a removed/hung device (driver reset): rebuild it. A failed attempt keeps `deviceLost_` set and is
// retried every half second, so a driver that is still coming back doesn't kill the ESP for the session.
bool Overlay::recoverDevice(double now)
{
    if (now - lastRecovery_ < 0.5) return false;
    lastRecovery_ = now;
    ContextScope scope(context_);
    if (backendUp_)
    {
        ImGui_ImplDX9_Shutdown();
        backendUp_ = false;
    }
    releaseDevice();
    if (!createDevice()) return false;
    ImGui_ImplDX9_Init(reinterpret_cast<IDirect3DDevice9*>(device_));
    backendUp_ = true;
    deviceLost_ = false;
    return true;
}

bool Overlay::resizeBackBuffer(UINT w, UINT h)
{
    D3DPRESENT_PARAMETERS& pp = Params(presentParams_);
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT hr = device_->ResetEx(&pp, nullptr);
    ImGui_ImplDX9_CreateDeviceObjects();
    if (hr == D3DERR_DEVICEREMOVED || hr == D3DERR_DEVICEHUNG) deviceLost_ = true;
    return SUCCEEDED(hr);
}

void Overlay::loadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    // 1-bit glyphs (no anti-aliasing, hinted to the pixel grid): every glyph pixel is fully on or off, so the
    // letters stay vivid and the one pixel outline stays crisp. Soft glyph edges would blend into the outline.
    io.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_Monochrome | ImGuiFreeTypeLoaderFlags_MonoHinting;
    ImFontConfig cfg;
    // Tahoma is hand-hinted for small unsmoothed text, which is what 1-bit rendering needs; Segoe UI is the
    // fallback (clean, but cramped at 13 px without anti-aliasing).
    for (const char* path : { "C:\\Windows\\Fonts\\tahoma.ttf", "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\verdana.ttf" })
        if (!font_ && FileExists(path))
            font_ = io.Fonts->AddFontFromFileTTF(path, 13.0f);
    if (!font_) font_ = io.Fonts->AddFontDefault();
}

void Overlay::frame(const scene::Snapshot& snap, const Settings& cfg, const RECT& area, bool visible)
{
    const int w = area.right - area.left, h = area.bottom - area.top;
    if (!context_ || (!device_ && !deviceLost_) || !visible || w < 2 || h < 2)
    {
        if (shown_)
        {
            ::ShowWindow(hwnd_, SW_HIDE);
            shown_ = false;
        }
        return;
    }
    const double t0 = util::Now();
    if (deviceLost_ && !recoverDevice(t0))
        return;

    // Keep the window glued to the game's client area.
    const bool moved = area.left != placed_.left || area.top != placed_.top || w != placed_.right - placed_.left || h != placed_.bottom - placed_.top;
    if (moved || !shown_)
    {
        // Inserting after a topmost window makes this one topmost too, right beneath it.
        ::SetWindowPos(hwnd_, anchor_ ? anchor_ : HWND_TOPMOST, area.left, area.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown_ = true;
        if (w != placed_.right - placed_.left || h != placed_.bottom - placed_.top)
        {
            ContextScope scope(context_);
            resizeBackBuffer((UINT)w, (UINT)h);
        }
        placed_ = { area.left, area.top, area.left + w, area.top + h };
    }

    draw(snap, cfg, w, h, t0, nullptr);
}

// Renders the ESP into the back buffer at the exact size of the game's client area, so one ImGui unit is
// one screen pixel. With `capturePath` the finished frame is also written to a BMP (alpha kept) before it is presented.
bool Overlay::draw(const scene::Snapshot& snap, const Settings& cfg, int w, int h, double now, const std::wstring* capturePath)
{
    ContextScope scope(context_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = lastFrame_ > 0.0 ? (float)std::min(now - lastFrame_, 0.1) : 1.0f / 60.0f;
    lastFrame_ = now;

    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    DrawEsp(ImGui::GetBackgroundDrawList(), font_, io.DisplaySize, snap, cfg);
    ImGui::EndFrame();

    device_->SetRenderState(D3DRS_ZENABLE, FALSE);
    device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    if (device_->BeginScene() >= 0)
    {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device_->EndScene();
    }
    const bool saved = capturePath ? util::SaveBackBuffer(device_, *capturePath, false) : true;
    const HRESULT hr = device_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (hr == D3DERR_DEVICEREMOVED || hr == D3DERR_DEVICEHUNG) deviceLost_ = true;
    frameMs_ = frameMs_ * 0.9f + (float)((util::Now() - now) * 1000.0) * 0.1f;
    return saved;
}

// Renders one frame without showing the window (development aid: `oar_external --esp-shot`).
bool Overlay::capture(const scene::Snapshot& snap, const Settings& cfg, const std::wstring& path)
{
    if (!context_ || !device_) return false;
    const D3DPRESENT_PARAMETERS& pp = Params(presentParams_);
    return draw(snap, cfg, (int)pp.BackBufferWidth, (int)pp.BackBufferHeight, util::Now(), &path);
}
}
