#include "menu/internal.h"

#include "imgui_internal.h"

#include <cstdio>

using namespace theme;
using namespace ui;

namespace menu
{
namespace
{
const char* LinkLabel(scene::Link l)
{
    switch (l)
    {
    case scene::Link::Ready:        return "CONNECTED";
    case scene::Link::Waiting:      return "CONNECTING";
    case scene::Link::AccessDenied: return "NEEDS ADMIN";
    case scene::Link::Incompatible: return "UNSUPPORTED";
    default:                        return "NOT RUNNING";
    }
}

ImVec4 LinkColor(scene::Link l)
{
    switch (l)
    {
    case scene::Link::Ready:   return P.success;
    case scene::Link::Waiting: return P.warning;
    case scene::Link::Detached: return P.textMute;
    default:                   return P.danger;
    }
}

const char* AxisName(uint8_t axis)
{
    return axis == 0 ? "vertical FOV" : axis == 1 ? "horizontal FOV" : "major-axis FOV";
}

void ActivityList(int max_items)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    const std::vector<Activity>& log = Activities();
    int n = ImMin((int)log.size(), max_items);
    if (n == 0)
    {
        Note("Nothing yet.");
        return;
    }
    float row = dp(34), w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = win->DC.CursorPos;
    float lx = p.x + dp(11);
    dl->AddLine(ImVec2(lx, p.y + row * 0.5f), ImVec2(lx, p.y + row * (n - 0.5f)), Col(P.border), dp(1.5f));
    char ago[16];
    for (int i = 0; i < n; i++)
    {
        const Activity& a = log[i];
        float cy = p.y + row * (i + 0.5f);
        dl->AddCircleFilled(ImVec2(lx, cy), dp(11), Col(P.group));
        dl->AddCircleFilled(ImVec2(lx, cy), dp(11), Col(i == 0 ? WithAlpha(P.accent, 0.20f) : P.field));
        DrawIcon(dl, a.icon, ImVec2(lx, cy), dp(10), Col(i == 0 ? P.accent : P.textDim));
        Ago(a.t, ago, sizeof(ago));
        ImVec2 as = Measure(F.regular, dp(kTextXS), ago);
        TextFit(dl, F.regular, dp(kTextS), ImVec2(p.x + dp(30), cy - LineH(F.regular, dp(kTextS)) * 0.5f), Col(i == 0 ? P.text : P.textDim), a.text.c_str(), w - dp(30) - as.x - dp(10));
        TextAt(dl, F.regular, dp(kTextXS), ImVec2(p.x + w - as.x, cy - as.y * 0.5f), Col(P.textMute), ago);
    }
    ImGui::ItemSize(ImVec2(w, row * n));
}
}

void OverviewStatus()
{
    using namespace stats;
    const scene::Snapshot& snap = *S.snap;
    if (!BeginColumns("##status", 0.5f))
        return;

    if (BeginGroup("##g_game", icon::Game, "Game"))
    {
        char v[96];
        Badge(LinkLabel(snap.link), LinkColor(snap.link));
        if (snap.link != scene::Link::Ready && !snap.detail.empty())
        {
            ImGui::Dummy(ImVec2(0, dp(2)));
            Note(snap.detail.c_str());
        }
        ImGui::Dummy(ImVec2(0, dp(2)));
        if (snap.pid)
        {
            snprintf(v, sizeof(v), "OAR-Win64-Shipping (pid %u)", snap.pid);
            KeyValue("Process", v);
            KeyValue("Build", snap.buildId.c_str(), true);
        }
        if (!snap.worldName.empty())
            KeyValue("World", snap.worldName.c_str());
        if (snap.view.valid)
        {
            snprintf(v, sizeof(v), "%.0f\xC2\xB0 %s", snap.view.fov, AxisName(snap.view.axisConstraint));
            KeyValue("Camera", v);
        }
    }
    EndGroup();

    if (BeginGroup("##g_scene", icon::Layers, "Scene"))
    {
        char v[64];
        int alive = 0, active = 0, phones = 0;
        for (const auto& g : snap.guards) alive += g.dead ? 0 : 1;
        for (const auto& c : snap.cameras) active += c.destroyed ? 0 : 1;
        for (const auto& p : snap.phones) phones += p.active ? 1 : 0;
        snprintf(v, sizeof(v), "%d alive / %d", alive, (int)snap.guards.size());
        KeyValue("Guards", v);
        snprintf(v, sizeof(v), "%d working / %d", active, (int)snap.cameras.size());
        KeyValue("Cameras", v);
        snprintf(v, sizeof(v), "%d counting / %d", phones, (int)snap.phones.size());
        KeyValue("Guard phones", v);
        snprintf(v, sizeof(v), "%d", (int)snap.terminals.size());
        KeyValue("Check-in points", v);
        if (S.cfg.ghost)
            snprintf(v, sizeof(v), "%zu NPCs ignoring you", snap.stats.ghosted);
        else
            snprintf(v, sizeof(v), "off");
        KeyValue("Ghost", v);
    }
    EndGroup();

    NextColumn();
    if (BeginGroup("##g_perf", icon::Pulse, "Performance"))
    {
        float gap = dp(8), cw = (ImGui::GetContentRegionAvail().x - gap * 2) / 3.0f, chh = dp(62);
        char v[32];
        float lo, hi;
        Range(L.scanSpark, kSpark, &lo, &hi);
        snprintf(v, sizeof(v), "%.2f", L.scanMs);
        StatCell("##c_scan", "Scan ms", v, L.scanSpark, kSpark, L.sparkOff, ImMax(0.0f, lo), hi, cw, chh);
        ImGui::SameLine(0, gap);
        Range(L.readSpark, kSpark, &lo, &hi);
        snprintf(v, sizeof(v), "%.0f", L.readsPerSec);
        StatCell("##c_reads", "Reads/s", v, L.readSpark, kSpark, L.sparkOff, ImMax(0.0f, lo), hi, cw, chh);
        ImGui::SameLine(0, gap);
        Range(L.renderSpark, kSpark, &lo, &hi);
        snprintf(v, sizeof(v), "%.2f", L.renderAvg);
        StatCell("##c_menu", "Menu ms", v, L.renderSpark, kSpark, L.sparkOff, ImMax(0.0f, lo), hi, cw, chh);

        float worst = 0.0f;
        for (float f : L.renderMs) worst = ImMax(worst, f);
        float top = 1.0f;
        for (float s : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f })
            if ((top = s) >= worst * 1.25f)
                break;
        ImGui::Dummy(ImVec2(0, dp(2)));
        LineChart("##frames", L.renderMs, kHist, L.renderOff, 0.0f, top, dp(80), "%.1f");
    }
    EndGroup();

    if (BeginGroup("##g_scanner", icon::Database, "Scanner"))
    {
        const scene::Stats& st = snap.stats;
        char v[64];
        snprintf(v, sizeof(v), "%.0f Hz (%.3f ms)", st.tickHz, st.tickMs);
        KeyValue("Scan loop", v, true);
        snprintf(v, sizeof(v), "%.2f ms, %zu actors", st.discoverMs, st.actorsSeen);
        KeyValue("Discovery", v, true);
        snprintf(v, sizeof(v), "%zu names, %zu offsets", st.names, st.offsetsCached);
        KeyValue("Cached", v, true);
        snprintf(v, sizeof(v), "%llu hit / %llu miss", (unsigned long long)st.cacheHits, (unsigned long long)st.cacheMisses);
        KeyValue("Offset cache", v, true);
        snprintf(v, sizeof(v), "%.2f ms", L.overlayMs);
        KeyValue("Overlay frame", v, true);
    }
    EndGroup();
    EndColumns();
}

void OverviewActivity()
{
    if (!BeginColumns("##activity", 0.58f))
        return;
    if (BeginGroup("##g_log", icon::History, "Recent activity"))
        ActivityList(12);
    EndGroup();
    NextColumn();
    if (BeginGroup("##g_session", icon::Clock, "Session"))
    {
        char buf[48];
        FormatUptime(buf, sizeof(buf));
        KeyValue("Uptime", buf, true);
        KeyValue("Active config", S.activeConfig.empty() ? "Autosaved" : S.activeConfig.c_str());
        KeyValue("Menu renderer", "Direct3D 9, on demand");
        KeyValue("Overlay", "Direct3D 9, DWM glass");
        ImGui::Dummy(ImVec2(0, dp(2)));
        Note("Settings are saved automatically. Named configs live on the Configs page.");
    }
    EndGroup();
    EndColumns();
}
}
