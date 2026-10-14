#pragma once
// Shared between the menu shell (shell.cpp), live stats (stats.cpp) and the page files.
#include "game/scanner.h"
#include "imgui.h"
#include "menu/config_store.h"
#include "menu/shell.h"
#include "settings.h"
#include "theme.h"
#include "widgets.h"

#include <string>
#include <vector>

namespace menu
{
enum Page { Page_Overview, Page_Esp, Page_Heist, Page_Configs, Page_Settings, Page_COUNT };

struct NavItem { unsigned icon; const char* label; const char* subs[3]; int subCount; };
extern const NavItem kNav[Page_COUNT];

struct State
{
    int    page = Page_Overview;
    int    sub[Page_COUNT] = {};
    double changedAt = -10.0;
    bool   resetScroll = false;
    bool   showDemo = false;

    Settings           cfg;                                    // live settings, edited in place by the widgets
    scene::SnapshotPtr snap = std::make_shared<const scene::Snapshot>(); // latest scanner picture, refreshed every frame
    scene::Scanner*    scanner = nullptr;

    // Configs page
    std::vector<store::Entry> configs;
    std::string activeConfig;
    int         selConfig = 0;
    std::string pendingDelete;
    char        newConfig[40] = "";
    bool        configsStale = true;
};
extern State S;

// Navigation / feedback
void SetPage(int page, int sub = -1);

// Two-column page layout (a table with a 12px gutter)
bool BeginColumns(const char* id, float left_weight = 0.5f);
void NextColumn();
void EndColumns();
void Note(const char* text);        // wrapped, dimmed paragraph
void ApplyTheme();                  // settings -> theme globals (after loading a config)

// Live numbers for the Overview page (stats.cpp)
namespace stats
{
constexpr int kHist = 120; // per-frame menu render times
constexpr int kSpark = 40; // 4 Hz samples for the sparklines
struct Live
{
    float  renderMs[kHist] = {};
    int    renderOff = 0;
    float  renderSpark[kSpark] = {}, scanSpark[kSpark] = {}, readSpark[kSpark] = {};
    int    sparkOff = 0;
    float  renderAvg = 0.0f, scanMs = 0.0f, readsPerSec = 0.0f, overlayMs = 0.0f;
    double startTime = 0.0;
};
extern Live L;
void Init();
void Update(float frame_work_ms, float overlay_ms, const scene::Snapshot& snap);
void Range(const float* v, int n, float* lo, float* hi);
}

// Activity log (stats.cpp)
struct Activity { double t; unsigned icon; std::string text; };
const std::vector<Activity>& Activities();
void Log(unsigned icon, const char* fmt, ...);
void Ago(double t, char* out, size_t n);
void AgoFromAge(double age, char* out, size_t n);
void FormatUptime(char* out, size_t n);

// Pages (page_*.cpp)
void OverviewStatus();
void OverviewActivity();
void EspGuards();
void EspCameras();
void EspStyle();
void HeistCheckIn();
void PageConfigs();
void SettingsAppearance();
void SettingsKeybinds();
void SettingsAbout();
}
