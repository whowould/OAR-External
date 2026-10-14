#include "overlay/esp.h"

#include "imgui_internal.h"
#include "overlay/pixel.h"
#include "overlay/projection.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace overlay
{
namespace
{
constexpr float kTextBase = 13.0f;
using pixel::Rect;
using scene::Camera;
using scene::Guard;
using scene::Phone;
using scene::Terminal;

// Draws the ESP primitives pixel-exactly (see pixel.h): integer rectangles, integer font sizes, centred lines.
struct Painter
{
    ImDrawList*       dl;
    ImFont*           font;
    const Settings&   cfg;
    const Projection& proj;

    ImU32 color(const float c[4], float alpha = 1.0f) const
    {
        const int a = (int)(ImSaturate(c[3] * cfg.opacity * alpha) * 255.0f + 0.5f);
        return IM_COL32((int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f), (int)(c[2] * 255.0f + 0.5f), a);
    }
    // Black outline colour that fades with the colour it belongs to.
    static ImU32 shadow(ImU32 like, float k = 0.85f) { return IM_COL32(0, 0, 0, (int)(((like >> IM_COL32_A_SHIFT) & 0xFF) * k)); }

    // Text height in whole pixels; `scale` shrinks secondary lines.
    float px(float scale = 1.0f) const { return (float)pixel::FontPx(kTextBase * cfg.textScale * scale); }
    int   lineWidth() const { return pixel::Thickness(cfg.boxThickness); }

    void fill(const Rect& r, ImU32 col) const { dl->AddRectFilled(ImVec2((float)r.x0, (float)r.y0), ImVec2((float)r.x1, (float)r.y1), col); }

    // Rectangle outline `t` px thick as four filled strips: exact pixels, no anti-aliasing.
    void frame(const Rect& r, int t, ImU32 col) const
    {
        if (r.width() <= 2 * t || r.height() <= 2 * t)
        {
            fill(r, col);
            return;
        }
        fill({ r.x0, r.y0, r.x1, r.y0 + t }, col);                // top
        fill({ r.x0, r.y1 - t, r.x1, r.y1 }, col);                // bottom
        fill({ r.x0, r.y0 + t, r.x0 + t, r.y1 - t }, col);        // left
        fill({ r.x1 - t, r.y0 + t, r.x1, r.y1 - t }, col);        // right
    }

    // A box: the coloured frame sandwiched between a black pixel outside and a black pixel inside.
    void box(const Rect& r, ImU32 col) const
    {
        const int t = lineWidth();
        if (cfg.outline)
        {
            const ImU32 o = shadow(col);
            frame({ r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1 }, 1, o);
            if (r.width() > 2 * t + 2 && r.height() > 2 * t + 2)
                frame({ r.x0 + t, r.y0 + t, r.x1 - t, r.y1 - t }, 1, o);
        }
        frame(r, t, col);
    }
    void box(ImVec2 a, ImVec2 b, ImU32 col) const { box(pixel::SnapRect(a.x, a.y, b.x, b.y), col); }

    // alignX: 0 = text starts at pos, 0.5 = centred, 1 = text ends at pos.
    void text(ImVec2 pos, ImU32 col, const char* s, float size, float alignX = 0.5f) const
    {
        const float fs = (float)pixel::FontPx(size);
        const float width = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s).x;
        const ImVec2 at((float)pixel::Round(pos.x - width * alignX), (float)pixel::Round(pos.y));
        if (cfg.outline)
        {
            const ImU32 o = shadow(col, 0.9f);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx || dy) dl->AddText(font, fs, ImVec2(at.x + (float)dx, at.y + (float)dy), o, s);
        }
        dl->AddText(font, fs, at, col, s);
    }

    void line(ImVec2 a, ImVec2 b, ImU32 col, int thickness) const
    {
        a = { pixel::SnapLine(a.x, thickness), pixel::SnapLine(a.y, thickness) };
        b = { pixel::SnapLine(b.x, thickness), pixel::SnapLine(b.y, thickness) };
        if (cfg.outline) dl->AddLine(a, b, shadow(col, 0.7f), (float)thickness + 2.0f);
        dl->AddLine(a, b, col, (float)thickness);
    }

    // Round dot of `r` whole pixels around the pixel containing `c`.
    void dot(ImVec2 c, int r, ImU32 col) const
    {
        c = { pixel::PixelCentre(c.x), pixel::PixelCentre(c.y) };
        if (cfg.outline) dl->AddCircleFilled(c, (float)r + 1.5f, shadow(col), 20);
        dl->AddCircleFilled(c, (float)r + 0.5f, col, 20);
    }

    void diamond(ImVec2 c, int r, ImU32 col) const
    {
        const int t = lineWidth();
        c = { pixel::SnapLine(c.x, t), pixel::SnapLine(c.y, t) };
        const float rr = (float)r;
        const ImVec2 pts[4] = { { c.x, c.y - rr }, { c.x + rr, c.y }, { c.x, c.y + rr }, { c.x - rr, c.y } };
        if (cfg.outline) dl->AddPolyline(pts, 4, shadow(col, 0.7f), ImDrawFlags_Closed, (float)t + 2.0f);
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, (float)t);
    }

    // Segment between two world points, clipped to the camera's near plane. Projection rejects depths below 1,
    // so clip to 2: float rounding on coordinates of 1e5 would otherwise land a clipped end just under 1 and
    // make the line flicker in and out.
    bool segment(ue::Vec3 a, ue::Vec3 b, ImVec2& sa, ImVec2& sb) const
    {
        constexpr float kNear = 2.0f;
        const float da = proj.depth(a), db = proj.depth(b);
        if (da < kNear && db < kNear) return false;
        const auto lerp = [](const ue::Vec3& p, const ue::Vec3& q, float t) { return ue::Vec3{ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t }; };
        const float t = (kNear - da) / (db - da); // where the segment crosses the clip plane
        if (da < kNear) a = lerp(a, b, t);
        else if (db < kNear) b = lerp(a, b, t);
        return proj.project(a, sa) && proj.project(b, sb);
    }
};

bool Beyond(const Settings& cfg, const Projection& pr, const ue::Vec3& p, float* meters)
{
    *meters = Projection::Distance(pr.origin(), p) / 100.0f;
    return cfg.maxDistance > 0.0f && *meters > cfg.maxDistance;
}

bool OnScreen(const Rect& r, ImVec2 screen)
{
    return r.x1 >= 0 && r.x0 <= (int)screen.x && r.y1 >= 0 && r.y0 <= (int)screen.y;
}

// Screen rectangle of an axis-aligned cube of half-size `h` around `c`; false if any corner is behind the camera.
bool ProjectCube(const Projection& pr, const ue::Vec3& c, float h, ImVec2& mn, ImVec2& mx)
{
    mn = { FLT_MAX, FLT_MAX };
    mx = { -FLT_MAX, -FLT_MAX };
    for (int i = 0; i < 8; i++)
    {
        const ue::Vec3 corner = { c.x + ((i & 1) ? h : -h), c.y + ((i & 2) ? h : -h), c.z + ((i & 4) ? h : -h) };
        ImVec2 s;
        if (!pr.project(corner, s)) return false;
        mn = { std::min(mn.x, s.x), std::min(mn.y, s.y) };
        mx = { std::max(mx.x, s.x), std::max(mx.y, s.y) };
    }
    return true;
}

// Horizontal centre of a rectangle in whole pixels (text is centred on it).
float CentreX(const Rect& r) { return (float)pixel::Round((float)(r.x0 + r.x1) * 0.5f); }

//-----------------------------------------------------------------------------
// Guards
//-----------------------------------------------------------------------------

std::string GuardState(const Guard& g)
{
    std::string s = g.dead ? "Dead" : g.tiedUp ? "Tied up" : g.alert ? "ALERT" : g.investigating ? "Investigating" : g.escorting ? "Escorting"
                  : g.patrolling ? "Patrolling" : "Idle";
    if (g.emped && !g.dead) s += " - EMP";
    return s;
}

void DrawGuard(const Painter& p, const Guard& g, ImVec2 screen)
{
    const Settings& cfg = p.cfg;
    if (g.dead && !cfg.guardDead) return;
    float meters;
    if (Beyond(cfg, p.proj, g.pos, &meters)) return;

    ImVec2 top, bottom;
    float depth;
    if (!p.proj.project({ g.pos.x, g.pos.y, g.pos.z + g.halfHeight }, top, &depth) || !p.proj.project({ g.pos.x, g.pos.y, g.pos.z - g.halfHeight }, bottom))
        return;
    const float h = bottom.y - top.y;
    if (h < 3.0f) return;
    const float w = std::max(h * 0.45f, p.proj.pixelsAt(g.radius * 2.0f, depth));
    const float cx = (top.x + bottom.x) * 0.5f;
    const Rect r = pixel::SnapRect(cx - w * 0.5f, top.y, cx + w * 0.5f, bottom.y);
    if (!OnScreen(r, screen)) return;

    const bool hot = g.alert || g.investigating;
    const ImU32 col = p.color(g.dead ? cfg.guardDeadColor : hot ? cfg.guardAlertColor : cfg.guardColor);
    const float big = p.px(), small = p.px(0.92f);
    const float mid = CentreX(r);

    if (cfg.guardBox) p.box(r, col);

    if (cfg.guardHealth && !g.dead && g.maxHealth > 0)
    {
        // A 3 px bar with a black border, two pixels clear of the box outline, filling from the bottom.
        const float frac = ImSaturate((float)g.health / (float)g.maxHealth);
        const Rect inner{ r.x0 - 7, r.y0, r.x0 - 4, r.y1 };
        p.fill({ inner.x0 - 1, inner.y0 - 1, inner.x1 + 1, inner.y1 + 1 }, IM_COL32(0, 0, 0, (int)(215 * cfg.opacity)));
        const int filled = pixel::Round(frac * (float)inner.height());
        const ImU32 fillCol = IM_COL32((int)(255 * (1.0f - frac)), (int)(220 * frac + 35), 60, (int)(255 * cfg.opacity));
        p.fill({ inner.x0, inner.y1 - filled, inner.x1, inner.y1 }, fillCol);
    }

    if (cfg.guardName)
        p.text(ImVec2(mid, (float)r.y0 - 2.0f - big - 2.0f), col, g.dead ? "Guard (dead)" : "Guard", big);

    float y = (float)r.y1 + 3.0f;
    char buf[96];
    if (cfg.guardState)
    {
        p.text(ImVec2(mid, y), col, GuardState(g).c_str(), small);
        y += small + 2.0f;
    }
    if (cfg.guardDistance)
    {
        snprintf(buf, sizeof(buf), "%.0fm", meters);
        p.text(ImVec2(mid, y), p.color(cfg.guardColor, 0.85f), buf, small);
    }

    if (cfg.guardHead && !g.dead)
        p.dot(ImVec2(mid, (float)r.y0 + h * 0.1f), std::max(2, pixel::Round(h * 0.035f)), col);
    if (cfg.guardSnap)
        p.line(ImVec2(screen.x * 0.5f, screen.y), ImVec2(mid, (float)r.y1), p.color(cfg.guardColor, 0.55f), 1);
}

//-----------------------------------------------------------------------------
// Cameras
//-----------------------------------------------------------------------------

void DrawCamera(const Painter& p, const Camera& c, ImVec2 screen)
{
    const Settings& cfg = p.cfg;
    if (c.destroyed && cfg.camHideDestroyed) return;
    float meters;
    if (Beyond(cfg, p.proj, c.pos, &meters)) return;

    const ImU32 col = p.color(c.destroyed ? cfg.camDeadColor : c.emped ? cfg.camEmpColor : c.spotting ? cfg.camSeenColor : cfg.camColor);
    const float big = p.px(), small = p.px(0.92f);

    ImVec2 a, b, centre;
    const bool boxed = ProjectCube(p.proj, c.pos, 22.0f, a, b);
    if (!boxed && !p.proj.project(c.pos, centre)) return;
    const Rect r = boxed ? pixel::SnapRect(a.x, a.y, b.x, b.y) : Rect{ pixel::Round(centre.x), pixel::Round(centre.y), pixel::Round(centre.x) + 1, pixel::Round(centre.y) + 1 };
    if (!OnScreen(r, screen)) return;
    if (boxed && cfg.camBox) p.box(r, col);
    const float mid = CentreX(r);

    if (cfg.camCone && !c.destroyed)
    {
        const float len = cfg.camConeLength * 100.0f;
        const ue::Vec3 tip = { c.pos.x + c.forward.x * len, c.pos.y + c.forward.y * len, c.pos.z + c.forward.z * len };
        ImVec2 sa, sb;
        if (p.segment(c.pos, tip, sa, sb))
        {
            const ImU32 lineCol = p.color(c.spotting ? cfg.camSeenColor : cfg.camColor, 0.85f);
            p.line(sa, sb, lineCol, 1);
            p.dot(sb, 2, lineCol);
        }
    }

    float y = (boxed ? (float)r.y1 + 3.0f : (float)r.y1 + 11.0f);
    char buf[96];
    if (cfg.camLabel)
    {
        snprintf(buf, sizeof(buf), "CAM #%d", c.number);
        p.text(ImVec2(mid, boxed ? (float)r.y0 - 2.0f - big - 2.0f : (float)r.y0 - 10.0f - big), col, buf, big);
    }
    if (cfg.camState)
    {
        const char* state = c.destroyed ? "Destroyed" : c.emped ? "EMP" : c.spotting ? "SEES YOU" : c.possessed ? "In use" : nullptr;
        if (state)
        {
            p.text(ImVec2(mid, y), col, state, small);
            y += small + 2.0f;
        }
    }
    if (cfg.camDistance)
    {
        snprintf(buf, sizeof(buf), "%.0fm", meters);
        p.text(ImVec2(mid, y), p.color(cfg.camColor, 0.85f), buf, small);
    }
}

//-----------------------------------------------------------------------------
// Check-in: phones and terminals
//-----------------------------------------------------------------------------

void DrawPhone(const Painter& p, const Phone& ph, ImVec2 screen)
{
    float meters;
    if (Beyond(p.cfg, p.proj, ph.pos, &meters)) return;
    ImVec2 s;
    if (!p.proj.project(ph.pos, s)) return;
    const int cx = pixel::Round(s.x), cy = pixel::Round(s.y);
    if (!OnScreen({ cx, cy, cx, cy }, screen)) return;

    const bool urgent = ph.active && ph.seconds <= 10;
    const ImU32 col = p.color(urgent ? p.cfg.guardAlertColor : p.cfg.phoneColor);
    p.diamond(ImVec2((float)cx, (float)cy), 6, col);
    char buf[64];
    if (!ph.active)       snprintf(buf, sizeof(buf), "Phone (checked in)");
    else if (ph.emped)    snprintf(buf, sizeof(buf), "Phone %ds (EMP)", ph.seconds);
    else                  snprintf(buf, sizeof(buf), "Phone %ds", ph.seconds);
    p.text(ImVec2((float)cx, (float)cy - 9.0f - p.px()), col, buf, p.px());
    snprintf(buf, sizeof(buf), "%.0fm", meters);
    p.text(ImVec2((float)cx, (float)cy + 9.0f), p.color(p.cfg.phoneColor, 0.85f), buf, p.px(0.92f));
}

void DrawTerminal(const Painter& p, const Terminal& t, ImVec2 screen)
{
    float meters;
    if (Beyond(p.cfg, p.proj, t.pos, &meters)) return;
    ImVec2 s;
    if (!p.proj.project(t.pos, s)) return;
    const int cx = pixel::Round(s.x), cy = pixel::Round(s.y);
    if (!OnScreen({ cx, cy, cx, cy }, screen)) return;

    // 11 x 11 px marker centred on the pixel under the point.
    const ImU32 col = p.color(p.cfg.terminalColor);
    p.box(Rect{ cx - 5, cy - 5, cx + 6, cy + 6 }, col);
    char buf[48];
    p.text(ImVec2((float)cx, (float)cy - 9.0f - p.px()), col, t.activated ? "Check-in point (in use)" : "Check-in point", p.px());
    snprintf(buf, sizeof(buf), "%.0fm", meters);
    p.text(ImVec2((float)cx, (float)cy + 9.0f), p.color(p.cfg.terminalColor, 0.85f), buf, p.px(0.92f));
}
}

void DrawEsp(ImDrawList* dl, ImFont* font, ImVec2 screen, const scene::Snapshot& snap, const Settings& cfg)
{
    if (!cfg.esp || snap.link != scene::Link::Ready) return;
    const Projection proj(snap.view, screen.x, screen.y);
    if (!proj.valid()) return;
    const Painter p{ dl, font, cfg, proj };

    // No anti-aliasing: every pixel the ESP touches is exactly the colour it was given (diagonal lines and
    // dots become crisp staircases instead of soft fringes). Restored for whoever draws next.
    const ImDrawListFlags savedFlags = dl->Flags;
    dl->Flags &= ~(ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex | ImDrawListFlags_AntiAliasedFill);

    if (cfg.debugActors && snap.debugActors)
        for (const scene::DebugActor& d : *snap.debugActors)
        {
            ImVec2 s;
            const float meters = Projection::Distance(proj.origin(), d.pos) / 100.0f;
            if (meters > 40.0f || !proj.project(d.pos, s)) continue;
            const float gray[4] = { 0.7f, 0.74f, 0.82f, 0.7f };
            p.dot(s, 1, p.color(gray));
            if (meters < 18.0f) p.text(ImVec2((float)pixel::Round(s.x) + 5.0f, (float)pixel::Round(s.y) - 6.0f), p.color(gray, 0.8f), d.label.c_str(), p.px(0.8f), 0.0f);
        }

    if (cfg.terminalEsp)
        for (const Terminal& t : snap.terminals) DrawTerminal(p, t, screen);
    if (cfg.phoneEsp)
        for (const Phone& ph : snap.phones) DrawPhone(p, ph, screen);
    if (cfg.cameras)
        for (const Camera& c : snap.cameras) DrawCamera(p, c, screen);
    if (cfg.guards)
        for (const Guard& g : snap.guards) DrawGuard(p, g, screen);

    dl->Flags = savedFlags;
}
}
