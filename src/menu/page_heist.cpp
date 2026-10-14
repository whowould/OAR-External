#include "menu/internal.h"

#include "overlay/projection.h"

#include <algorithm>
#include <climits>
#include <cstdio>

using namespace theme;
using namespace ui;

namespace menu
{
void HeistCheckIn()
{
    Settings& c = S.cfg;
    const scene::Snapshot& snap = *S.snap;

    int active = 0, lowest = INT_MAX;
    for (const scene::Phone& p : snap.phones)
        if (p.active)
        {
            active++;
            lowest = std::min(lowest, p.seconds);
        }

    if (!BeginColumns("##heist", 0.5f))
        return;

    if (BeginGroup("##g_force", icon::Shield, "Force guard check-in"))
    {
        char v[96];
        if (snap.link != scene::Link::Ready) snprintf(v, sizeof(v), "game not ready");
        else if (active == 0)                snprintf(v, sizeof(v), "no phone counting down");
        else                                 snprintf(v, sizeof(v), "%d counting, lowest %d s", active, lowest);
        KeyValue("Status", v);
        ImGui::Dummy(ImVec2(0, dp(2)));

        ImGui::BeginDisabled(snap.link != scene::Link::Ready || S.scanner == nullptr);
        if (Button("Check in now", icon::Check, Btn_Primary, ImVec2(-1, 0)))
        {
            S.scanner->requestCheckIn();
            Log(icon::Check, "Check-in requested");
        }
        ImGui::EndDisabled();
        Keybind("Hotkey", &c.checkinKey);
        Divider();
        CheckboxKey("Auto check-in", &c.checkinAuto, &c.checkinAutoKey);
        ImGui::BeginDisabled(!c.checkinAuto);
        Slider("When the timer reaches", &c.checkinBelow, 0, 15, "%.0f s");
        ImGui::EndDisabled();
    }
    EndGroup();

    NextColumn();
    if (BeginGroup("##g_phones", icon::Clock, "Guard phones"))
    {
        if (snap.phones.empty())
            Note("No phones in the level.");
        int shown = 0;
        for (const scene::Phone& p : snap.phones)
        {
            if (++shown > 6) break;
            char key[32], val[64];
            snprintf(key, sizeof(key), "Phone %d", shown);
            const float m = snap.view.valid ? overlay::Projection::Distance(snap.view.location, p.pos) / 100.0f : 0.0f;
            if (!p.active)      snprintf(val, sizeof(val), "checked in  -  %.0f m", m);
            else if (p.emped)   snprintf(val, sizeof(val), "%d s (EMP)  -  %.0f m", p.seconds, m);
            else                snprintf(val, sizeof(val), "%d s  -  %.0f m", p.seconds, m);
            KeyValue(key, val);
        }
    }
    EndGroup();

    if (BeginGroup("##g_markers", icon::Eye, "ESP markers"))
    {
        CheckboxColor("Dropped phones", &c.phoneEsp, c.phoneColor);
        CheckboxColor("Check-in points", &c.terminalEsp, c.terminalColor);
    }
    EndGroup();

    if (BeginGroup("##g_how", icon::Info, "How it works"))
        Note("Clears the phone's Active flag, the same change a check-in point makes: the countdown stops and no alarm is raised. "
             "Works while you host or play solo. The phone keeps beeping until it is picked up.");
    EndGroup();
    EndColumns();
}
}
