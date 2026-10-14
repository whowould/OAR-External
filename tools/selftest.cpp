// Self-test for the parts that can be checked without the game: pattern matching, settings round-trip,
// the config store and the projection math.   oar_selftest.exe  ->  exit code 0 when everything passes.
#include "features/ghost.h"
#include "game/process.h"
#include "menu/config_store.h"
#include "overlay/pixel.h"
#include "overlay/projection.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_failed++; } \
    } while (0)
static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

static void TestPattern()
{
    const uint8_t data[] = { 0x00, 0x48, 0x8B, 0x1D, 0x11, 0x22, 0x33, 0x44, 0x48, 0x85, 0xDB, 0x48, 0x8B, 0x1D, 0xAA, 0xBB, 0xCC, 0xDD, 0x48, 0x85 };
    std::vector<size_t> hits;
    game::Pattern("48 8B 1D ?? ?? ?? ?? 48 85").findAll(data, sizeof(data), hits, 8);
    CHECK(hits.size() == 2 && hits[0] == 1 && hits[1] == 11);      // the second match ends exactly at the buffer end

    hits.clear();
    game::Pattern("48 8B 1D ?? ?? ?? ?? 48 85 DB").findAll(data, sizeof(data), hits, 8);
    CHECK(hits.size() == 1 && hits[0] == 1);

    hits.clear();
    game::Pattern("?? ?? 8b").findAll(data, sizeof(data), hits, 1); // leading wildcards, lowercase hex, result limit
    CHECK(hits.size() == 1 && hits[0] == 0);

    hits.clear();
    game::Pattern("DE AD").findAll(data, sizeof(data), hits, 8);
    CHECK(hits.empty());
}

static void TestSettings()
{
    Settings a;
    a.esp = false;
    a.guardColor[1] = 0.123f;
    a.checkinBelow = 7;
    a.checkinKey = ImGuiKey_F11;
    a.accent = 3;

    Settings b;
    CHECK(settings::FromText(settings::ToText(a), b));
    CHECK(b.esp == false && Near(b.guardColor[1], 0.123f, 1e-4f) && b.checkinBelow == 7 && b.checkinKey == ImGuiKey_F11 && b.accent == 3);
    CHECK(settings::ToText(a) == settings::ToText(b));

    Settings c;
    CHECK(!settings::FromText("nothing=here\ngarbage", c));        // no recognised key: rejected
    CHECK(settings::FromText("esp=0\nunknown.key=5\r\n", c) && !c.esp && c.guards); // unknown keys ignored, the rest stays default

    // Hand-edited or imported values are forced into range; NaN falls back to the default.
    Settings d, def;
    CHECK(settings::FromText("style.opacity=nan\nstyle.textScale=1e9\ncheckin.below=-5\nesp.key=99999\nguards.color=2,-1,nan,0.5\n", d));
    CHECK(d.opacity == def.opacity && d.textScale == 3.0f && d.checkinBelow == 0.0f && d.espKey == def.espKey);
    CHECK(d.guardColor[0] == 1.0f && d.guardColor[1] == 0.0f && d.guardColor[3] == 0.5f);
}

static void TestStore()
{
    CHECK(store::Sanitize("  a/b\\c:d*e?  ") == "abcde");
    CHECK(store::Sanitize("...").empty() && store::Sanitize("").empty());

    const std::string name = "selftest-config-" + std::to_string(::GetCurrentProcessId());
    Settings s;
    s.maxDistance = 42;
    CHECK(store::Save(name, s));
    CHECK(store::Exists(name));
    bool listed = false;
    for (const store::Entry& e : store::List()) listed |= e.name == name;
    CHECK(listed);
    Settings loaded;
    CHECK(store::Load(name, loaded) && loaded.maxDistance == 42);
    CHECK(store::Remove(name) && !store::Exists(name));
    CHECK(!store::Load(name, loaded));
}

static void TestPixelSnapping()
{
    using namespace overlay::pixel;
    // Every corner goes through std::round (halves round away from zero), in either corner order.
    Rect r = SnapRect(10.4f, 20.5f, 30.6f, 40.4f);
    CHECK(r.x0 == 10 && r.y0 == 21 && r.x1 == 31 && r.y1 == 40);
    r = SnapRect(30.6f, 40.4f, 10.4f, 20.5f);
    CHECK(r.x0 == 10 && r.y0 == 21 && r.x1 == 31 && r.y1 == 40);
    r = SnapRect(5.2f, 5.2f, 5.4f, 5.3f);                           // collapses to a point: still one pixel
    CHECK(r.width() == 1 && r.height() == 1);
    r = SnapRect(-3.6f, -0.4f, 2.5f, 1.4f);                         // partly off-screen (negative) coordinates
    CHECK(r.x0 == -4 && r.y0 == 0 && r.x1 == 3 && r.y1 == 1);

    CHECK(Thickness(0.2f) == 1 && Thickness(1.5f) == 2 && Thickness(3.4f) == 3);
    CHECK(FontPx(13.0f) == 13 && FontPx(11.96f) == 12 && FontPx(1.0f) == 6);

    // A 1 px line goes through the middle of a pixel, a 2 px line along a pixel edge.
    CHECK(Near(SnapLine(10.7f, 1), 10.5f) && Near(SnapLine(10.2f, 1), 10.5f) && Near(SnapLine(10.5f, 3), 10.5f));
    CHECK(Near(SnapLine(10.7f, 2), 11.0f) && Near(SnapLine(10.2f, 4), 10.0f));
    CHECK(Near(PixelCentre(-0.2f), -0.5f) && Near(PixelCentre(0.1f), 0.5f) && Near(PixelCentre(7.9f), 7.5f));
}

// Ghost against fake NPCs built in this process's own memory, laid out like the game's objects.
namespace
{
struct FakeNpc // 0x100 bytes: pointers at fixed offsets, like a Blueprint actor
{
    alignas(16) uint8_t bytes[0x100] = {};
    uintptr_t addr() const { return reinterpret_cast<uintptr_t>(bytes); }
    uintptr_t& ptr(uint32_t off) { return *reinterpret_cast<uintptr_t*>(bytes + off); }
    uint8_t& byte(uint32_t off) { return bytes[off]; }
};
ue::Field F(uint32_t off, uint8_t mask = 0) { return ue::Field{ off, mask, true }; }
}

static void TestGhost()
{
    game::Process proc;
    CHECK(proc.attachSelf());

    FakeNpc me, other;                       // the local pawn and some other player's pawn
    FakeNpc sensorA, sensorB, eyesA, eyesB;  // sensor actor -> PawnSensing component (bits at +0xC4: enable 01, see 04, hear 08)
    FakeNpc guard, guardEars, guardSpotComp;
    FakeNpc cop, copEyes;
    FakeNpc camA, camB, camSpotComp;         // camB is already disabled by the game
    eyesA.byte(0xC4) = eyesB.byte(0xC4) = guardEars.byte(0xC4) = copEyes.byte(0xC4) = 0x0D;
    sensorA.ptr(0x28) = eyesA.addr();
    sensorB.ptr(0x28) = eyesB.addr();
    guard.ptr(0x10) = guardEars.addr();      // PawnSensing
    guard.ptr(0x18) = guardSpotComp.addr();  // SpotPlayerComponent
    guard.ptr(0x20) = me.addr();             // TargetPlayer
    guard.ptr(0x28) = me.addr();             // EscortingPlayer
    guard.byte(0x30) = 1;                    // Sensing?
    guardSpotComp.ptr(0x40) = me.addr();     // SpottedPlayer
    cop.ptr(0x10) = copEyes.addr();
    cop.ptr(0x20) = other.addr();            // police chasing someone else
    cop.byte(0x30) = 1;
    camA.byte(0x10) = 0;                     // Ignored?
    camB.byte(0x10) = 1;
    camA.ptr(0x18) = camSpotComp.addr();
    camSpotComp.ptr(0x40) = other.addr();    // spotting a different player
    me.byte(0x50) = 1;                       // Sus?

    features::GhostLayout layout;
    layout.seePawns = F(0xC4, 0x04);
    layout.hearNoises = F(0xC4, 0x08);
    layout.spottedPlayer = F(0x40);
    layout.sensorEyes = F(0x28);
    layout.guardEars = F(0x10); layout.guardSpot = F(0x18); layout.guardTarget = F(0x20); layout.guardEscorting = F(0x28); layout.guardAware = F(0x30, 0xFF);
    layout.policeEyes = F(0x10); layout.policeTarget = F(0x20); layout.policeAware = F(0x30, 0xFF);
    layout.cameraIgnored = F(0x10, 0xFF); layout.cameraSpot = F(0x18);
    layout.pawnSus = F(0x50, 0xFF);

    features::GhostWorld world;
    world.localPawn = me.addr();
    world.sensors = { sensorA.addr(), sensorB.addr() };
    world.guards = { guard.addr() };
    world.police = { cop.addr() };
    world.cameras = { camA.addr(), camB.addr() };

    std::vector<uintptr_t> dead;             // objects the scanner no longer considers alive
    const features::Ghost::AliveFn alive = [&](uintptr_t a) { return std::find(dead.begin(), dead.end(), a) == dead.end(); };

    features::Ghost ghost;
    Settings cfg;
    cfg.ghost = true;

    // Enabled: eyes and ears closed (the "enable updates" bit is untouched), awareness of me dropped, cameras off.
    CHECK(ghost.tick(proc, layout, cfg, world, alive) == 4); // 1 guard + 1 cop + 2 cameras
    CHECK(eyesA.byte(0xC4) == 0x01 && eyesB.byte(0xC4) == 0x01 && guardEars.byte(0xC4) == 0x01 && copEyes.byte(0xC4) == 0x01);
    CHECK(guard.ptr(0x20) == 0 && guard.ptr(0x28) == 0 && guard.byte(0x30) == 0 && guardSpotComp.ptr(0x40) == 0);
    CHECK(cop.ptr(0x20) == other.addr() && cop.byte(0x30) == 1); // chasing someone else: left alone
    CHECK(camA.byte(0x10) == 1 && camB.byte(0x10) == 1);
    CHECK(camSpotComp.ptr(0x40) == other.addr());                // spotting someone else: left alone
    CHECK(me.byte(0x50) == 0);                                   // no longer suspicious
    CHECK(ghost.engaged());

    // Idempotent: a second tick changes nothing.
    CHECK(ghost.tick(proc, layout, cfg, world, alive) == 4);
    CHECK(eyesA.byte(0xC4) == 0x01);

    // One part off: police get their eyes back, the rest stays blind.
    cfg.ghostPolice = false;
    ghost.tick(proc, layout, cfg, world, alive);
    CHECK(copEyes.byte(0xC4) == 0x0D && eyesA.byte(0xC4) == 0x01);
    cfg.ghostPolice = true;
    ghost.tick(proc, layout, cfg, world, alive);
    CHECK(copEyes.byte(0xC4) == 0x01);

    // A dead object is never written to, in either direction.
    dead = { eyesB.addr(), sensorB.addr() };                     // sensorB went away while Ghost was on
    eyesB.byte(0xC4) = 0xAA;                                     // memory reused for something else
    cfg.ghost = false;
    CHECK(ghost.tick(proc, layout, cfg, world, alive) == 0);
    CHECK(eyesB.byte(0xC4) == 0xAA);                             // untouched
    CHECK(eyesA.byte(0xC4) == 0x0D && guardEars.byte(0xC4) == 0x0D && copEyes.byte(0xC4) == 0x0D); // everything else given back
    CHECK(camA.byte(0x10) == 0 && camB.byte(0x10) == 1);        // the camera the game had disabled stays disabled
    CHECK(!ghost.engaged());

    // Off and idle: no writes at all.
    eyesA.byte(0xC4) = 0x55;
    ghost.tick(proc, layout, cfg, world, alive);
    CHECK(eyesA.byte(0xC4) == 0x55);

    // forget(): saved state is dropped without touching memory.
    cfg.ghost = true;
    dead.clear();
    eyesA.byte(0xC4) = 0x0D;
    ghost.tick(proc, layout, cfg, world, alive);
    CHECK(eyesA.byte(0xC4) == 0x01);
    ghost.forget();
    cfg.ghost = false;
    ghost.tick(proc, layout, cfg, world, alive);
    CHECK(eyesA.byte(0xC4) == 0x01);                             // nothing was remembered, so nothing was restored

    // An unresolved layout does nothing instead of writing through offset 0.
    features::Ghost blank;
    cfg.ghost = true;
    eyesA.byte(0xC4) = 0x0D;
    blank.tick(proc, features::GhostLayout{}, cfg, world, alive);
    CHECK(eyesA.byte(0xC4) == 0x0D && !blank.engaged());
}

static scene::View MakeView(float yaw, float fov, uint8_t axis)
{
    scene::View v;
    v.location = { 0, 0, 0 };
    v.rotation = { 0, yaw, 0 };
    v.fov = fov;
    v.axisConstraint = axis;
    v.valid = true;
    return v;
}

static void TestProjection()
{
    ImVec2 p;
    // 90 degree horizontal FOV on 1920x1080: a point at 45 degrees right lands on the right edge.
    overlay::Projection x(MakeView(0, 90, 1), 1920, 1080);
    CHECK(x.project({ 100, 0, 0 }, p) && Near(p.x, 960) && Near(p.y, 540));
    CHECK(x.project({ 100, 100, 0 }, p) && Near(p.x, 1920) && Near(p.y, 540));
    CHECK(x.project({ 100, 0, 100 }, p) && Near(p.x, 960) && Near(p.y, 540 - 960));     // same scale on both axes
    CHECK(x.project({ 200, -100, 0 }, p) && Near(p.x, 960 - 480));
    CHECK(!x.project({ -100, 0, 0 }, p));                                                // behind the camera

    // Vertical FOV: the 90 degrees now span the height.
    overlay::Projection y(MakeView(0, 90, 0), 1920, 1080);
    CHECK(y.project({ 100, 0, 100 }, p) && Near(p.y, 0) && Near(p.x, 960));
    CHECK(y.project({ 100, 100, 0 }, p) && Near(p.x, 960 + 540));

    // Unreal is left-handed: yaw 90 faces +Y and the camera's right-hand side is -X.
    overlay::Projection turned(MakeView(90, 90, 1), 1920, 1080);
    CHECK(turned.project({ 0, 100, 0 }, p) && Near(p.x, 960));
    CHECK(turned.project({ -100, 100, 0 }, p) && Near(p.x, 1920));
    CHECK(turned.project({ 100, 100, 0 }, p) && Near(p.x, 0));

    // Letterboxed (constrained aspect) views keep the image centred in a smaller rectangle.
    scene::View boxed = MakeView(0, 90, 1);
    boxed.constrainAspect = true;
    boxed.aspect = 1.0f;
    overlay::Projection square(boxed, 1920, 1080);
    CHECK(square.project({ 100, 100, 0 }, p) && Near(p.x, 960 + 540));

    CHECK(!overlay::Projection(scene::View{}, 1920, 1080).valid());
}

int main()
{
    TestPattern();
    TestSettings();
    TestStore();
    TestProjection();
    TestPixelSnapping();
    TestGhost();
    printf(g_failed ? "%d check(s) FAILED\n" : "all checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
