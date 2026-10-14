#include "menu/internal.h"

#include "util/paths.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>

using namespace theme;
using namespace ui;

namespace menu
{
void SettingsAppearance()
{
    if (!BeginColumns("##set_look"))
        return;
    if (BeginGroup("##g_theme", icon::Palette, "Theme"))
    {
        Caption("ACCENT");
        if (AccentPicker())
            Notice(Toast_Info, icon::Palette, "Accent", kAccents[accent].name);
        ImGui::Dummy(ImVec2(0, dp(2)));
        Slider("Corner radius", &rounding, 0, 16, "%.0f px");
        Checkbox("Animations", &animations);
        Checkbox("Glow effects", &glow);
        Checkbox("Compact sidebar", &S.cfg.compact);
    }
    EndGroup();
    EndColumns();
}

void SettingsKeybinds()
{
    Settings& c = S.cfg;
    if (!BeginColumns("##set_keys"))
        return;
    if (BeginGroup("##g_hotkeys", icon::Keyboard, "Hotkeys"))
    {
        if (Keybind("Toggle ESP", &c.espKey))
            Log(icon::Keyboard, "Toggle ESP bound to %s", KeyLabel(c.espKey));
        if (Keybind("Check in now", &c.checkinKey))
            Log(icon::Keyboard, "Check in now bound to %s", KeyLabel(c.checkinKey));
        if (Keybind("Toggle auto check-in", &c.checkinAutoKey))
            Log(icon::Keyboard, "Auto check-in bound to %s", KeyLabel(c.checkinAutoKey));
        ImGui::Dummy(ImVec2(0, dp(2)));
        Note("Hotkeys work while the game has focus. Right-click a key to clear it.");
    }
    EndGroup();
    NextColumn();
    if (BeginGroup("##g_window", icon::Info, "This window"))
    {
        KeyValue("Search", "Ctrl K", true);
        KeyValue("Switch page", "Ctrl 1-5", true);
        KeyValue("Navigate", "Arrows / Tab", true);
        KeyValue("Close popup", "Esc", true);
    }
    EndGroup();
    EndColumns();
}

void SettingsAbout()
{
    if (!BeginColumns("##set_about"))
        return;
    if (BeginGroup("##g_about", icon::Info, "About"))
    {
        char buf[48], up[16];
        KeyValue("Target", "One-Armed Robber (UE 4.27)");
        KeyValue("Build", __DATE__, true);
        KeyValue("Dear ImGui", IMGUI_VERSION, true);
        snprintf(buf, sizeof(buf), "%.0f%%", dpi * 100.0f);
        KeyValue("Display scale", buf, true);
        FormatUptime(up, sizeof(up));
        KeyValue("Uptime", up, true);
        ImGui::Dummy(ImVec2(0, dp(4)));
        float bw = (ImGui::GetContentRegionAvail().x - dp(8)) * 0.5f;
        if (Button("Data folder", icon::Folder, Btn_Primary, ImVec2(bw, 0)))
        {
            const std::string dir = util::DataDir();
            wchar_t wide[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wide, MAX_PATH);
            ShellExecuteW(nullptr, L"open", wide, nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0, dp(8));
        if (Button(S.showDemo ? "Hide demo" : "ImGui demo", icon::Code, Btn_Ghost, ImVec2(bw, 0)))
            S.showDemo = !S.showDemo;
    }
    EndGroup();
    NextColumn();
    if (BeginGroup("##g_reset", icon::Warning, "Reset"))
    {
        Note("Restores every setting to its default. Saved configs are kept.");
        ImGui::Dummy(ImVec2(0, dp(2)));
        if (Button("Reset all settings", icon::Refresh, Btn_Danger, ImVec2(-1, 0)))
        {
            S.cfg = Settings();
            ApplyTheme();
            S.activeConfig.clear();
            Notice(Toast_Warning, icon::Refresh, "Settings reset", "configs kept");
        }
    }
    EndGroup();
    EndColumns();
}
}
