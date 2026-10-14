#include "menu/internal.h"

#include "imgui_internal.h"

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace menu
{
namespace stats
{
Live L;

void Init() { L.startTime = ImGui::GetTime(); }

// frame_work_ms: CPU time the previous menu frame spent building and submitting the UI (excludes vsync wait).
void Update(float frame_work_ms, float overlay_ms, const scene::Snapshot& snap)
{
    static int frames = 0;
    static double last_sample = -1.0;
    frames++;
    if (frames < 10) // first frames include font baking and device warm-up
        return;
    if (frames < 30)
        for (float& f : L.renderMs) f = frame_work_ms;
    L.renderMs[L.renderOff] = frame_work_ms;
    L.renderOff = (L.renderOff + 1) % kHist;

    double now = ImGui::GetTime();
    if (last_sample >= 0.0 && now - last_sample < 0.25)
        return;
    bool first = last_sample < 0.0;
    last_sample = now;
    float sum = 0.0f;
    for (float f : L.renderMs) sum += f;
    L.renderAvg = sum / kHist;
    L.scanMs = snap.stats.tickMs;
    L.readsPerSec = snap.stats.readsPerSec;
    L.overlayMs = overlay_ms;
    if (first)
        for (int i = 0; i < kSpark; i++) { L.renderSpark[i] = L.renderAvg; L.scanSpark[i] = L.scanMs; L.readSpark[i] = L.readsPerSec; }
    L.renderSpark[L.sparkOff] = L.renderAvg;
    L.scanSpark[L.sparkOff] = L.scanMs;
    L.readSpark[L.sparkOff] = L.readsPerSec;
    L.sparkOff = (L.sparkOff + 1) % kSpark;
}

void Range(const float* v, int n, float* lo, float* hi)
{
    float a = FLT_MAX, b = -FLT_MAX;
    for (int i = 0; i < n; i++) { a = ImMin(a, v[i]); b = ImMax(b, v[i]); }
    float pad = ImMax((b - a) * 0.25f, ImMax(fabsf(b) * 0.05f, 0.05f));
    *lo = a - pad;
    *hi = b + pad;
}
}

//-----------------------------------------------------------------------------
// Activity log
//-----------------------------------------------------------------------------

static std::vector<Activity> s_activity;

const std::vector<Activity>& Activities() { return s_activity; }

void Log(unsigned ic, const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_activity.insert(s_activity.begin(), { ImGui::GetTime(), ic, buf });
    if (s_activity.size() > 12)
        s_activity.pop_back();
}

void AgoFromAge(double age, char* out, size_t n)
{
    if (age < 10.0)         snprintf(out, n, "now");
    else if (age < 60.0)    snprintf(out, n, "%ds", (int)age);
    else if (age < 3600.0)  snprintf(out, n, "%dm", (int)(age / 60.0));
    else if (age < 86400.0) snprintf(out, n, "%dh", (int)(age / 3600.0));
    else                    snprintf(out, n, "%dd", (int)(age / 86400.0));
}

void Ago(double t, char* out, size_t n) { AgoFromAge(ImGui::GetTime() - t, out, n); }

void FormatUptime(char* out, size_t n)
{
    int s = (int)(ImGui::GetTime() - stats::L.startTime);
    snprintf(out, n, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}
}
