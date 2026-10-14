#pragma once
// Pixel snapping for the ESP. The overlay window is exactly the game's client area at 1:1 scale, so one
// ImGui unit is one screen pixel; the ESP stays crisp as long as everything lands on whole pixels:
//   * rectangle corners are rounded to integers and drawn as filled rectangles (no anti-aliased strokes);
//   * a line of odd width is centred on a pixel centre (so it covers exactly one row/column), even widths sit on
//     a pixel edge;
//   * font sizes are whole pixels (glyphs are rasterised per size).
#include <algorithm>
#include <cmath>

namespace overlay::pixel
{
inline int Round(float v) { return static_cast<int>(std::round(v)); }

// Half-open pixel rectangle [x0, x1) x [y0, y1).
struct Rect
{
    int x0, y0, x1, y1;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

// Every corner rounded with std::round; never thinner than one pixel.
inline Rect SnapRect(float ax, float ay, float bx, float by)
{
    Rect r{ Round(std::min(ax, bx)), Round(std::min(ay, by)), Round(std::max(ax, bx)), Round(std::max(ay, by)) };
    r.x1 = std::max(r.x1, r.x0 + 1);
    r.y1 = std::max(r.y1, r.y0 + 1);
    return r;
}

inline int Thickness(float t) { return std::max(1, Round(t)); }
inline int FontPx(float size) { return std::max(6, Round(size)); }

// Nearest pixel centre (n + 0.5).
inline float PixelCentre(float v) { return std::round(v - 0.5f) + 0.5f; }

// Where a line endpoint must be so a `thickness` px wide line is pixel-aligned.
inline float SnapLine(float v, int thickness) { return thickness % 2 != 0 ? PixelCentre(v) : std::round(v); }
}
