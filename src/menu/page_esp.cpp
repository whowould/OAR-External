#include "menu/internal.h"

#include "imgui_internal.h"

#include <cmath>

using namespace theme;
using namespace ui;

namespace menu
{
void EspGuards()
{
    Settings& c = S.cfg;
    if (!BeginColumns("##esp_guards"))
        return;
    if (BeginGroup("##g_guards", icon::Shield, "Guards", &c.guards))
    {
        Checkbox("Box", &c.guardBox);
        Checkbox("Health bar", &c.guardHealth);
        Checkbox("Name", &c.guardName);
        Checkbox("State (patrolling, alert...)", &c.guardState);
        Checkbox("Distance", &c.guardDistance);
        Checkbox("Head marker", &c.guardHead);
        Checkbox("Snaplines", &c.guardSnap);
        Checkbox("Show dead guards", &c.guardDead);
    }
    EndGroup();
    NextColumn();
    if (BeginGroup("##g_guardcolors", icon::Palette, "Colors"))
    {
        ColorRow("Guard", c.guardColor);
        ColorRow("Alerted / investigating", c.guardAlertColor);
        ColorRow("Dead", c.guardDeadColor);
    }
    EndGroup();
    if (BeginGroup("##g_guardinfo", icon::Info, "About"))
        Note("Only guards are drawn. Police, SWAT and every other NPC are ignored.");
    EndGroup();
    EndColumns();
}

void EspCameras()
{
    Settings& c = S.cfg;
    if (!BeginColumns("##esp_cameras"))
        return;
    if (BeginGroup("##g_cameras", icon::Monitor, "Security cameras", &c.cameras))
    {
        Checkbox("Box", &c.camBox);
        Checkbox("Label (CAM #n)", &c.camLabel);
        Checkbox("State (EMP, destroyed, sees you)", &c.camState);
        Checkbox("Distance", &c.camDistance);
        Checkbox("View direction", &c.camCone);
        ImGui::BeginDisabled(!c.camCone);
        Slider("Direction length", &c.camConeLength, 2, 30, "%.0f m");
        ImGui::EndDisabled();
        Checkbox("Hide destroyed cameras", &c.camHideDestroyed);
    }
    EndGroup();
    NextColumn();
    if (BeginGroup("##g_camcolors", icon::Palette, "Colors"))
    {
        ColorRow("Working", c.camColor);
        ColorRow("EMP'd", c.camEmpColor);
        ColorRow("Destroyed", c.camDeadColor);
        ColorRow("Sees you", c.camSeenColor);
    }
    EndGroup();
    EndColumns();
}

void EspStyle()
{
    Settings& c = S.cfg;
    if (!BeginColumns("##esp_style"))
        return;
    if (BeginGroup("##g_overlay", icon::Layers, "Overlay"))
    {
        CheckboxKey("ESP", &c.esp, &c.espKey);
        Slider("Max distance (0 = unlimited)", &c.maxDistance, 0, 200, "%.0f m");
        // The overlay draws whole pixels only (crisp lines, one font raster per size), so these are integers.
        int thickness = ImMax(1, (int)roundf(c.boxThickness));
        if (SliderInt("Line thickness", &thickness, 1, 4, "%d px"))
            c.boxThickness = (float)thickness;
        constexpr float kBaseText = 13.0f; // text size is stored as a scale of this
        int textPx = ImMax(8, (int)roundf(kBaseText * c.textScale));
        if (SliderInt("Text size", &textPx, 8, 28, "%d px"))
            c.textScale = (float)textPx / kBaseText;
        float percent = c.opacity * 100.0f; // stored as 0..1, shown as a percentage
        if (Slider("Opacity", &percent, 20.0f, 100.0f, "%.0f%%"))
            c.opacity = percent / 100.0f;
        Checkbox("Text outline", &c.outline);
    }
    EndGroup();
    NextColumn();
    if (BeginGroup("##g_dev", icon::Code, "Developer"))
    {
        Checkbox("Mark every actor in the level", &c.debugActors);
        Note("Draws a dot and class name for each actor near you. Handy for finding what something is called, and for checking the projection.");
    }
    EndGroup();
    EndColumns();
}
}
