#pragma once
// Every user-configurable value. Plain data, so it can be copied across threads and saved as a config.
// Visit() is the single list of fields: defaults, save, load and export all go through it.
#include "imgui.h"

#include <string>
#include <string_view>
#include <type_traits>

struct Settings
{
    // ESP (all of it)
    bool     esp = true;
    ImGuiKey espKey = ImGuiKey_F6;
    float    maxDistance = 0; // metres, 0 = unlimited

    // Guards (only NPC_Guard; police and other NPCs are never drawn)
    bool  guards = true, guardBox = true, guardHealth = true, guardName = true, guardState = true,
          guardDistance = true, guardHead = true, guardSnap = false, guardDead = false;
    float guardColor[4]      = { 0.97f, 0.32f, 0.36f, 1.0f };
    float guardAlertColor[4] = { 1.00f, 0.62f, 0.20f, 1.0f };
    float guardDeadColor[4]  = { 0.55f, 0.58f, 0.66f, 1.0f };

    // Security cameras
    bool  cameras = true, camBox = true, camLabel = true, camState = true, camDistance = true,
          camCone = true, camHideDestroyed = true;
    float camConeLength = 10; // metres
    float camColor[4]     = { 0.30f, 0.78f, 1.00f, 1.0f };
    float camEmpColor[4]  = { 1.00f, 0.82f, 0.28f, 1.0f };
    float camDeadColor[4] = { 0.55f, 0.58f, 0.66f, 1.0f };
    float camSeenColor[4] = { 1.00f, 0.25f, 0.30f, 1.0f };

    // Guard check-in
    bool     phoneEsp = true, terminalEsp = true;
    bool     checkinAuto = false;
    float    checkinBelow = 10; // auto check-in once this many seconds are left on the phone (the game starts it at 15)
    ImGuiKey checkinKey = ImGuiKey_F7, checkinAutoKey = ImGuiKey_F8;
    float    phoneColor[4]    = { 0.62f, 0.45f, 1.00f, 1.0f };
    float    terminalColor[4] = { 0.30f, 0.90f, 0.55f, 1.0f };

    // Overlay style
    float boxThickness = 1.0f, textScale = 1.0f, opacity = 1.0f; // thickness in whole pixels
    bool  outline = true;

    // Developer
    bool debugActors = false;

    // Appearance (mirrors the theme globals)
    int   accent = 0;
    float rounding = 10;
    bool  animations = true, glow = true, compact = false;
};
static_assert(std::is_trivially_copyable_v<Settings>, "Settings is copied between threads with memcpy semantics");

template <class V> void Visit(Settings& s, V&& v)
{
    v("esp", s.esp);
    v("esp.key", s.espKey);
    v("esp.maxDistance", s.maxDistance);

    v("guards", s.guards);
    v("guards.box", s.guardBox);
    v("guards.health", s.guardHealth);
    v("guards.name", s.guardName);
    v("guards.state", s.guardState);
    v("guards.distance", s.guardDistance);
    v("guards.head", s.guardHead);
    v("guards.snap", s.guardSnap);
    v("guards.dead", s.guardDead);
    v("guards.color", s.guardColor);
    v("guards.alertColor", s.guardAlertColor);
    v("guards.deadColor", s.guardDeadColor);

    v("cameras", s.cameras);
    v("cameras.box", s.camBox);
    v("cameras.label", s.camLabel);
    v("cameras.state", s.camState);
    v("cameras.distance", s.camDistance);
    v("cameras.cone", s.camCone);
    v("cameras.hideDestroyed", s.camHideDestroyed);
    v("cameras.coneLength", s.camConeLength);
    v("cameras.color", s.camColor);
    v("cameras.empColor", s.camEmpColor);
    v("cameras.deadColor", s.camDeadColor);
    v("cameras.seenColor", s.camSeenColor);

    v("checkin.phoneEsp", s.phoneEsp);
    v("checkin.terminalEsp", s.terminalEsp);
    v("checkin.auto", s.checkinAuto);
    v("checkin.below", s.checkinBelow);
    v("checkin.key", s.checkinKey);
    v("checkin.autoKey", s.checkinAutoKey);
    v("checkin.phoneColor", s.phoneColor);
    v("checkin.terminalColor", s.terminalColor);

    v("style.thickness", s.boxThickness);
    v("style.textScale", s.textScale);
    v("style.opacity", s.opacity);
    v("style.outline", s.outline);

    v("dev.debugActors", s.debugActors);

    v("ui.accent", s.accent);
    v("ui.rounding", s.rounding);
    v("ui.animations", s.animations);
    v("ui.glow", s.glow);
    v("ui.compact", s.compact);
}

namespace settings
{
std::string ToText(const Settings& s);
// Starts from defaults and applies every recognised `key=value` line; unknown keys are ignored and every
// value is clamped to its valid range (see Sanitize). Returns false if the text contained no recognised key.
bool FromText(std::string_view text, Settings& out);

// Forces every value into its valid range: NaN/inf and out-of-range numbers (from a hand-edited or imported
// config) fall back to the default or the nearest limit.
void Sanitize(Settings& s);
}
