// OAR external for One-Armed Robber: the Nebula menu in its own window, a click-through ESP overlay on top of
// the game, and a scanner thread that reads the game's memory.
//
// Threads: this (UI) thread owns both windows and both ImGui contexts; the scanner thread owns the game process
// and hands over immutable snapshots. The menu renders on demand (input, animation, slow heartbeat); while the
// overlay is visible the loop ticks once per display refresh.
//
// Usage:  oar.exe                 normal run (needs administrator, like the game)
//         oar.exe --demo          synthetic scene instead of the game (try the menu and ESP anywhere)
//         oar.exe --shots <dir>   render every page/sub-tab, save <dir>\view_N.bmp, exit
//         oar.exe --dpi <n>       apply a DPI change at startup (e.g. 144 = 150%), same path as moving monitors

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "demo.h"
#include "game/scanner.h"
#include "host.h"
#include "menu/shell.h"
#include "overlay/overlay.h"
#include "theme.h"
#include "util/keys.h"
#include "util/screenshot.h"
#include "widgets.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Direct3D 9Ex with the FLIPEX swap effect for the menu: flip-model presents hand the buffer to DWM directly,
// where plain windowed D3D9 presents cost ~5 ms of CPU per frame in a redirection copy.
static IDirect3D9Ex*         g_d3d = nullptr;
static IDirect3DDevice9Ex*   g_device = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp = {};
static bool                  g_deviceRemoved = false;
static bool                  g_backendUp = false;    // ImGui's DX9 backend is initialised for g_device
static double                g_nextRecreate = 0.0;   // throttles device recreation after a failure
static bool                  g_occluded = false;
static HANDLE                g_timer = nullptr; // high-resolution waitable timer for frame pacing
static UINT                  g_resizeW = 0, g_resizeH = 0;
static HWND                  g_hwnd = nullptr;
static bool                  g_ready = false;
static bool                  g_inSizeMove = false;
static float                 g_frameWorkMs = 0.0f;
static float                 g_nextDelay = 0.0f; // from menu::IdleDelay() after the last frame
static const int             kWindowW = 800, kWindowH = 530; // at 100% scale

static ImGuiContext*  g_menuContext = nullptr;
static scene::Scanner g_scanner;
static overlay::Overlay g_overlay;
static bool           g_demo = false;
static bool           g_overlayActive = false; // overlay window is up: it needs a frame every refresh

struct DragRect { float x0, y0, x1, y1; };
static float                 g_captionHeight = 0.0f;
static std::vector<DragRect> g_noDrag, g_noDragBuild;
static bool                  g_itemHovered = false;

static bool CreateDeviceD3D(HWND hwnd);
static void CleanupDeviceD3D();
static void ResetDevice();
static bool RecreateDevice();
static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

//-----------------------------------------------------------------------------
// host:: hooks used by the menu's custom title bar
//-----------------------------------------------------------------------------

namespace host
{
void  Minimize() { ::ShowWindow(g_hwnd, SW_MINIMIZE); }
void  Close() { ::PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
void  SetCaptionHeight(float px) { g_captionHeight = px; }
void  AddNoDragRect(const ImVec2& a, const ImVec2& b) { g_noDragBuild.push_back({ a.x, a.y, b.x, b.y }); }
float FrameWorkMs() { return g_frameWorkMs; }
float OverlayMs() { return g_overlay.frameMs(); }
}

//-----------------------------------------------------------------------------
// Timing
//-----------------------------------------------------------------------------

static double Seconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// Next display refresh after `after`, from DWM's composition clock (QPC based, like Seconds()).
static double NextVBlank(double after)
{
    static const double freq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (SUCCEEDED(::DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod > 0)
    {
        const double period = (double)ti.qpcRefreshPeriod / freq, vblank = (double)ti.qpcVBlank / freq;
        double next = vblank + period * ceil((after - vblank) / period);
        return next > after + 0.0005 ? next : next + period;
    }
    return after + 1.0 / 60.0;
}

// Sleeps until `t` (seconds, Seconds() clock) or until any input arrives, whichever comes first.
static void WaitUntil(double t)
{
    const double wait = t - Seconds();
    if (wait <= 0.0)
        return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(wait * 1e7); // relative, 100 ns units
    if (g_timer && ::SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
        ::MsgWaitForMultipleObjects(1, &g_timer, FALSE, INFINITE, QS_ALLINPUT);
    else
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)(wait * 1000.0) + 1, QS_ALLINPUT);
}

//-----------------------------------------------------------------------------
// Menu frame
//-----------------------------------------------------------------------------

static void RenderMenuFrame(const std::wstring* capture_path = nullptr)
{
    if (!g_device || g_deviceRemoved) return; // being rebuilt: the main loop retries
    const double t0 = Seconds();
    if (g_resizeW != 0 && g_resizeH != 0)
    {
        g_d3dpp.BackBufferWidth = g_resizeW;
        g_d3dpp.BackBufferHeight = g_resizeH;
        g_resizeW = g_resizeH = 0;
        ResetDevice();
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_noDragBuild.clear();
    menu::Render();
    g_noDrag.swap(g_noDragBuild);
    g_itemHovered = ImGui::IsAnyItemHovered();
    ImGui::EndFrame();

    const ImVec4& bg = theme::P.bg;
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_RGBA((int)(bg.x * 255.0f), (int)(bg.y * 255.0f), (int)(bg.z * 255.0f), 255), 1.0f, 0);
    if (g_device->BeginScene() >= 0)
    {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        g_device->EndScene();
    }
    g_frameWorkMs = (float)((Seconds() - t0) * 1000.0);
    if (capture_path)
        util::SaveBackBuffer(g_device, *capture_path, true);

    g_nextDelay = menu::IdleDelay();
    // Presents immediately: the main loop already waits for the next vblank on a timer, so the
    // driver never blocks (or spins) here. Flip-model windowed presents are composed by DWM: no tearing.
    HRESULT hr = g_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (hr == S_PRESENT_OCCLUDED)
        g_occluded = true;
    else if (hr == D3DERR_DEVICEREMOVED || hr == D3DERR_DEVICEHUNG)
        g_deviceRemoved = true;
}

//-----------------------------------------------------------------------------
// Overlay and hotkeys
//-----------------------------------------------------------------------------

// Draws the ESP over the game's client area while the game (or one of our windows) is in front.
static void RunOverlay(const scene::Snapshot& snap)
{
    const Settings& cfg = menu::Cfg();
    RECT area = {};
    bool visible = false;
    if (g_demo)
    {
        area = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
        visible = cfg.esp;
    }
    else if (cfg.esp && snap.link == scene::Link::Ready && snap.gameWindow && ::IsWindow(snap.gameWindow) && !::IsIconic(snap.gameWindow))
    {
        const HWND front = ::GetForegroundWindow();
        if (front == snap.gameWindow || front == g_hwnd || front == g_overlay.hwnd())
        {
            RECT client;
            POINT origin = { 0, 0 };
            ::GetClientRect(snap.gameWindow, &client);
            ::ClientToScreen(snap.gameWindow, &origin);
            area = { origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
            visible = true;
        }
    }
    g_overlay.frame(snap, cfg, area, visible);
    g_overlayActive = g_overlay.shown();
}

static void PollHotkeys(const scene::Snapshot& snap)
{
    static keys::Edges edges;
    Settings& cfg = menu::Cfg();
    // Sample every key every frame so edges stay correct, but only act when one of our windows or the game is in front.
    const bool toggleEsp = edges.pressed(cfg.espKey), checkIn = edges.pressed(cfg.checkinKey), toggleAuto = edges.pressed(cfg.checkinAutoKey),
               toggleGhost = edges.pressed(cfg.ghostKey);
    const HWND front = ::GetForegroundWindow();
    const bool ours = front == g_hwnd || front == g_overlay.hwnd() || (snap.gameWindow && front == snap.gameWindow) || g_demo;
    if (!ours || ui::IsCapturingKey() || ImGui::GetIO().WantTextInput)
        return;

    if (toggleEsp)
    {
        cfg.esp = !cfg.esp;
        menu::Notice(ui::Toast_Info, ui::icon::Eye, "ESP", cfg.esp ? "enabled" : "disabled");
    }
    if (checkIn)
    {
        g_scanner.requestCheckIn();
        menu::Notice(ui::Toast_Info, ui::icon::Shield, "Check-in", "requested");
    }
    if (toggleGhost)
    {
        cfg.ghost = !cfg.ghost;
        menu::Notice(ui::Toast_Info, ui::icon::Sparkle, "Ghost", cfg.ghost ? "enabled" : "disabled");
    }
    if (toggleAuto)
    {
        cfg.checkinAuto = !cfg.checkinAuto;
        menu::Notice(ui::Toast_Info, ui::icon::Shield, "Auto check-in", cfg.checkinAuto ? "enabled" : "disabled");
    }
}

// Keep the menu above the game (and the overlay) while the game is running.
static void UpdateTopmost(bool want)
{
    static bool current = false;
    if (want == current)
        return;
    current = want;
    ::SetWindowPos(g_hwnd, want ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

//-----------------------------------------------------------------------------
// Entry point
//-----------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int)
{
    std::wstring shots_dir, esp_shot;
    int dpi_override = 0;
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; i++)
        {
            if (wcscmp(argv[i], L"--shots") == 0 && i + 1 < argc)
                shots_dir = argv[++i];
            else if (wcscmp(argv[i], L"--dpi") == 0 && i + 1 < argc)
                dpi_override = _wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--demo") == 0)
                g_demo = true;
            else if (wcscmp(argv[i], L"--esp-shot") == 0 && i + 1 < argc)
                esp_shot = argv[++i];
        }
        ::LocalFree(argv);
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    HMONITOR monitor = ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hinst, ::LoadIcon(nullptr, IDI_APPLICATION),
                       ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"OAROverlayMenu", nullptr };
    ::RegisterClassExW(&wc);

    MONITORINFO mi = { sizeof(mi) };
    ::GetMonitorInfoW(monitor, &mi);
    int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
    int w = (int)(kWindowW * scale), h = (int)(kWindowH * scale);
    int x = mi.rcWork.left + (work_w - w) / 2, y = mi.rcWork.top + (work_h - h) / 2;

    // WS_THICKFRAME/WS_CAPTION are kept only so DWM draws the shadow and rounded corners;
    // the hit test below never reports resize edges, so the menu stays a fixed size.
    g_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"OAR External", WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               x, y, w, h, nullptr, nullptr, hinst, nullptr);

    // Borderless but keep DWM shadow and Win11 rounded corners.
    MARGINS margins = { 1, 1, 1, 1 };
    ::DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int corners = 2; /* DWMWCP_ROUND */
    ::DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    COLORREF border = RGB(0x2A, 0x2F, 0x40);
    ::DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    ::SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    IMGUI_CHECKVERSION();
    g_menuContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    theme::dpi = scale;
    theme::LoadFonts();
    theme::Setup();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(g_device);
    g_backendUp = true;
    menu::Init(g_demo ? nullptr : &g_scanner);
    if (g_demo)
        menu::SetSnapshot(demo::Make(0.0));
    if (!g_overlay.create(hinst)) // creates its own ImGui context and hands the menu's back
        ::MessageBoxW(g_hwnd, L"The overlay window could not be created: ESP is unavailable.", L"OAR External", MB_ICONWARNING);
    g_overlay.setAnchor(g_hwnd);
    ImGui::SetCurrentContext(g_menuContext);

    // --esp-shot <file>: render the demo scene with default settings into a BMP (alpha kept), without showing
    // anything, and exit. Lets the pixel output of the ESP be inspected exactly.
    if (!esp_shot.empty())
    {
        const bool ok = g_overlay.capture(*demo::Make(0.0), Settings(), esp_shot);
        g_overlay.destroy();
        ImGui::SetCurrentContext(g_menuContext);
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_menuContext);
        CleanupDeviceD3D();
        ::DestroyWindow(g_hwnd);
        return ok ? 0 : 2;
    }

    if (!g_demo)
        g_scanner.start();
    g_ready = true;

    ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(g_hwnd);

    // Same code path Windows uses when the window moves to a monitor with another scale.
    if (dpi_override > 0)
    {
        RECT rc;
        ::GetWindowRect(g_hwnd, &rc);
        ::SendMessageW(g_hwnd, WM_DPICHANGED, MAKEWPARAM(dpi_override, dpi_override), (LPARAM)&rc);
    }

    // Frame scheduling. The overlay needs a frame every display refresh while it is up. The menu is on demand:
    // right after input (`eager`), while something animates (IdleDelay() == 0), or at the idle heartbeat.
    // Waits sleep on a high-resolution timer, so nothing spins.
    g_timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer)
        g_timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); // pre-1803 Windows 10
    const bool on_demand = shots_dir.empty();
    const int frames_per_shot = 50;
    int frame = 0;
    int eager = 2;          // menu frames to render right after input, so hover states settle
    double earliest = 0.0, menuNext = 0.0, nextPoll = 0.0;
    bool gameRunning = false;      // the game is attached (or this is a demo): poll hotkeys and the overlay state quickly
    constexpr double kPollPeriod = 1.0 / 60.0;
    bool done = false;
    while (!done)
    {
        // The menu is idle most of the time and may sleep for a second, but hotkeys and the overlay's
        // show/hide (alt-tabbing into the game) must stay responsive while the game is running.
        const auto dueTime = [&] {
            double t = g_overlayActive || eager > 0 ? earliest : std::max(earliest, menuNext);
            return gameRunning ? std::min(t, std::max(nextPoll, 0.0)) : t;
        };
        WaitUntil(dueTime());
        MSG msg;
        bool had_input = false;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            had_input = true;
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (had_input)
            eager = 3;
        if (Seconds() < dueTime())
            continue; // input arrived before the next refresh: it is handled by that frame

        // driver update / TDR: rebuild the menu's device, retrying every half second if the driver isn't back yet
        if (g_deviceRemoved && Seconds() >= g_nextRecreate && !RecreateDevice())
            g_nextRecreate = Seconds() + 0.5;

        // World state for this frame
        const scene::SnapshotPtr snap = g_demo ? demo::Make(Seconds()) : g_scanner.snapshot();
        for (const scene::Event& e : g_scanner.takeEvents())
            menu::HandleEvent(e);
        menu::SetSnapshot(snap);
        gameRunning = g_demo || snap->link != scene::Link::Detached;
        UpdateTopmost(gameRunning);
        PollHotkeys(*snap);
        menu::Sync(); // hotkeys change settings even while the menu window is minimized and not rendering

        // Menu: skipped while minimized, fully covered or while its device is being rebuilt
        bool menuCanRender = !g_deviceRemoved;
        if (menuCanRender && (g_occluded || (on_demand && ::IsIconic(g_hwnd))))
        {
            menuCanRender = false;
            if (g_device->CheckDeviceState(g_hwnd) != S_PRESENT_OCCLUDED && !::IsIconic(g_hwnd))
            {
                g_occluded = false;
                menuCanRender = true;
            }
        }
        const double now = Seconds();
        if (menuCanRender && (eager > 0 || !on_demand || now >= menuNext))
        {
            std::wstring shot;
            if (!on_demand) // screenshot mode: let each view settle, then grab the back buffer
            {
                int view = frame / frames_per_shot;
                if (view >= menu::ViewCount())
                    break;
                if (frame % frames_per_shot == 0)
                    menu::SetView(view);
                if (frame % frames_per_shot == frames_per_shot - 1)
                    shot = shots_dir + L"\\view_" + std::to_wstring(view) + L".bmp";
                frame++;
            }
            RenderMenuFrame(shot.empty() ? nullptr : &shot);
            if (eager > 0)
                eager--;
            menuNext = Seconds() + (on_demand ? g_nextDelay : 0.0);
        }
        else if (!menuCanRender)
            menuNext = now + 0.1; // nothing to draw in the menu: don't wake up at refresh rate for it

        if (on_demand)
            RunOverlay(*snap);
        earliest = NextVBlank(Seconds());
        nextPoll = Seconds() + kPollPeriod;
    }
    if (g_timer)
        ::CloseHandle(g_timer);

    menu::Shutdown();
    g_scanner.stop();
    g_overlay.destroy();
    ImGui::SetCurrentContext(g_menuContext);
    if (g_backendUp) ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_menuContext);
    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

//-----------------------------------------------------------------------------
// D3D9
//-----------------------------------------------------------------------------

static bool CreateDeviceD3D(HWND hwnd)
{
    if (FAILED(::Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d)))
        return false;
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_FLIPEX;                    // flip model (no redirection copy)
    g_d3dpp.BackBufferCount = 2;
    g_d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_d3dpp.BackBufferWidth = (UINT)(rc.right - rc.left);
    g_d3dpp.BackBufferHeight = (UINT)(rc.bottom - rc.top);
    g_d3dpp.EnableAutoDepthStencil = FALSE;                       // 2D UI only
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; // pacing is done by the main loop
    // FPU_PRESERVE: by default D3D9 switches the FPU to single precision, which breaks double timers.
    const DWORD flags = D3DCREATE_FPU_PRESERVE;
    HRESULT hr = g_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, nullptr, &g_device);
    if (FAILED(hr))
        hr = g_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_d3dpp, nullptr, &g_device);
    if (FAILED(hr))
        return false;
    g_device->SetMaximumFrameLatency(1);
    return true;
}

static void CleanupDeviceD3D()
{
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_d3d)    { g_d3d->Release();    g_d3d = nullptr; }
}

// Back buffer size changes go through ResetEx(). Ex devices keep default-pool resources across it,
// but the ImGui backend's buffers are dropped and rebuilt anyway to stay on the documented path.
static void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_device->ResetEx(&g_d3dpp, nullptr);
    if (hr == D3DERR_DEVICEREMOVED || hr == D3DERR_DEVICEHUNG)
        g_deviceRemoved = true;
    IM_ASSERT(hr != D3DERR_INVALIDCALL);
    ImGui_ImplDX9_CreateDeviceObjects();
}

// A removed or hung device can't be reset: release it and build a new one. Returns false (and stays in the
// removed state) if the driver cannot give a device yet; the caller retries.
static bool RecreateDevice()
{
    if (g_backendUp)
    {
        ImGui_ImplDX9_Shutdown();
        g_backendUp = false;
    }
    CleanupDeviceD3D();
    if (!CreateDeviceD3D(g_hwnd))
        return false;
    ImGui_ImplDX9_Init(g_device);
    g_backendUp = true;
    g_deviceRemoved = false;
    return true;
}

//-----------------------------------------------------------------------------
// Window procedure
//-----------------------------------------------------------------------------

static LRESULT HitTest(HWND hwnd, POINT screen)
{
    POINT pt = screen;
    ::ScreenToClient(hwnd, &pt);
    if (pt.y < g_captionHeight && !g_itemHovered)
    {
        for (const DragRect& d : g_noDrag)
            if (pt.x >= d.x0 && pt.x < d.x1 && pt.y >= d.y0 && pt.y < d.y1)
                return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wParam == TRUE)
            return 0; // client area = whole window (no standard frame)
        break;
    case WM_NCHITTEST:
        return HitTest(hwnd, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
    case WM_NCACTIVATE:
        return ::DefWindowProcW(hwnd, msg, wParam, -1); // don't repaint a non-client frame we don't have
    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        return 0;
    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_resizeW = (UINT)LOWORD(lParam);
        g_resizeH = (UINT)HIWORD(lParam);
        if (g_ready && g_inSizeMove)
            RenderMenuFrame(); // keep drawing during the modal move loop
        return 0;
    case WM_DPICHANGED:
    {
        // Moved to a monitor with a different scale: resize to the exact scaled size and
        // rebuild every DPI-dependent value (style sizes, font scale).
        const float scale = HIWORD(wParam) / 96.0f;
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        ::SetWindowPos(hwnd, nullptr, r->left, r->top, (int)(kWindowW * scale), (int)(kWindowH * scale), SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_ready)
        {
            theme::SetDpi(scale);
            menu::OnDpiChanged();
        }
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
