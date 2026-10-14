#include "game/scanner.h"

#include "features/checkin.h"
#include "features/ghost.h"
#include "game/cache.h"
#include "game/process.h"
#include "game/unreal.h"
#include "util/paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace scene
{
namespace
{
constexpr const wchar_t* kGameExe = L"OAR-Win64-Shipping.exe";
constexpr double kTickPeriod = 1.0 / 250.0;   // camera view (changes with every mouse movement)
constexpr double kEntityPeriod = 1.0 / 100.0; // guards, cameras, phones, terminals
constexpr double kGhostPeriod = 1.0 / 33.0;   // Ghost writes
constexpr double kDiscoverPeriod = 0.5;       // actor list walk
constexpr uint32_t kDestroyedFlags = 0x00008000u | 0x00010000u; // RF_BeginDestroyed | RF_FinishDestroyed
constexpr size_t kDebugActorLimit = 800;

enum class Kind : uint8_t { None, Guard, Camera, Phone, Terminal, Police, Sensor }; // Police and Sensor are only tracked for Ghost

// Blueprint classes we track. Matching walks the super chain, so subclasses are picked up too.
struct ClassKind { const char* name; Kind kind; };
constexpr ClassKind kClassKinds[] = {
    { "NPC_Guard_C", Kind::Guard },
    { "CameraBP_C", Kind::Camera },
    { "GuardPhone_C", Kind::Phone },
    { "GuardScanPoint_C", Kind::Terminal },
    { "NPC_Police_base_C", Kind::Police },
    { "SensingActor_C", Kind::Sensor }, // a guard's eyes: a child actor holding its PawnSensing
};

bool Finite(const ue::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Part of an object image that starts at byte `lo` of the object; members are addressed by their absolute offset.
struct Slice
{
    const uint8_t* data;
    uint32_t       lo;
    template <class T> T get(uint32_t offset) const
    {
        T v{};
        memcpy(&v, data + (offset - lo), sizeof(T));
        return v;
    }
    template <class T> T get(const ue::Field& f) const { return get<T>(f.offset); }
    bool flag(const ue::Field& f) const { return (data[f.offset - lo] & f.mask) != 0; }
};

ue::Vec3 Forward(const ue::Quat& q) // q * (1, 0, 0)
{
    return { 1.0f - 2.0f * (q.y * q.y + q.z * q.z), 2.0f * (q.x * q.y + q.w * q.z), 2.0f * (q.x * q.z - q.w * q.y) };
}

//-----------------------------------------------------------------------------
// Resolved offsets
//-----------------------------------------------------------------------------

struct EngineFields
{
    ue::Field persistentLevel, levels, owningGameInstance;          // UWorld
    ue::Field localPlayers;                                         // UGameInstance
    ue::Field aspectAxis, playerController;                         // ULocalPlayer / UPlayer
    ue::Field cameraManager, actorRoot;                             // APlayerController / AActor
    ue::Field acknowledgedPawn;                                     // APlayerController: the local player's pawn (optional: only Ghost uses it)
    ue::Field povLocation, povRotation, povFov, povAspect, povConstrain; // CameraCachePrivate.POV.*
    uint32_t  levelActors = 0, componentToWorld = 0;                // not reflected, discovered

    uint32_t viewLo = 0, viewHi = 0; // byte range of the POV members inside APlayerCameraManager
    bool fieldsReady() const { return persistentLevel && levels && owningGameInstance && localPlayers && aspectAxis && playerController &&
                                      cameraManager && actorRoot && povLocation && povRotation && povFov && povAspect && povConstrain; }
};

struct GuardFields
{
    ue::Field health, dead, patrolling, investigating, alert, escorting, emped, tiedUp, capsule;
    ue::Field halfHeight, radius; // on the capsule component class
    uint32_t  extent = 0;
    bool      ready = false;
    double    retryAt = 0.0; // earliest time to try resolving again after a failure
};

struct CameraFields
{
    ue::Field destroyed, emped, possessed, ignored, number, lens, spotted;
    uint32_t  extent = 0;
    bool      ready = false;
    double    retryAt = 0.0; // earliest time to try resolving again after a failure
};

struct PhoneFields
{
    ue::Field active, emped, seconds;
    uint32_t  extent = 0;
    bool      ready = false;
    double    retryAt = 0.0; // earliest time to try resolving again after a failure
};

struct TerminalFields
{
    ue::Field activated;
    uint32_t  extent = 0;
    bool      ready = false;
    double    retryAt = 0.0; // earliest time to try resolving again after a failure
};

struct Tracked
{
    Kind      kind = Kind::None;
    uintptr_t cls = 0;
    int       misses = 0; // consecutive failed refreshes; dropped after kMaxMisses
};

struct ActorInfo
{
    Kind      kind = Kind::None;
    uintptr_t cls = 0;
    uint32_t  generation = 0;
    bool      settled = false; // kind is final (not the result of a failed read)
};

constexpr int kMaxMisses = 5; // ~40 ms of failed reads before an entity is let go
}

//-----------------------------------------------------------------------------
// Scanner::Impl
//-----------------------------------------------------------------------------

struct Scanner::Impl
{
    // --- shared with other threads (guarded by `mutex`) ---
    mutable std::mutex mutex;
    SnapshotPtr        snapshot = std::make_shared<const Snapshot>();
    Settings           settings;
    std::vector<Event> events;
    features::CheckIn  checkIn;
    features::Ghost    ghost;

    std::thread       thread;
    std::atomic<bool> stopping{ false };
    HANDLE            wake = nullptr; // manual-reset event, set by stop()

    // --- scanner thread only ---
    game::Process     proc;
    game::OffsetCache cache;
    ue::NamePool      names{ proc };
    ue::Reflection    refl{ proc, cache, names };
    ue::Globals       globals;
    bool              initialised = false;    // globals resolved
    bool              layoutVerified = false; // engine layout self-test passed
    std::string       initFailure;

    EngineFields   eng;
    GuardFields    guardF;
    CameraFields   cameraF;
    PhoneFields    phoneF;
    TerminalFields terminalF;

    uintptr_t world = 0;
    std::string worldName;
    uintptr_t pcm = 0;           // APlayerCameraManager of the local player
    uintptr_t pcAddr = 0;        // the local APlayerController
    features::GhostLayout ghostLayout;
    double ghostLayoutRetryAt = 0.0, lastGhost = -1.0;
    size_t ghosted = 0;          // NPCs Ghost is currently working on
    uint8_t   axisConstraint = 0;
    HWND      gameWindow = nullptr;

    std::unordered_map<uintptr_t, Kind>      classKinds;
    std::unordered_map<uintptr_t, ActorInfo> actors; // every actor seen on the last walk, including uninteresting ones
    std::unordered_map<uintptr_t, Tracked>   tracked;
    std::unordered_map<uintptr_t, int>       peakHealth; // guards have no max-health property: remember the highest value seen
    uint32_t generation = 0;
    int      initFailures = 0; // consecutive failed global resolutions: slows the retries down
    std::shared_ptr<const std::vector<DebugActor>> debugActors;

    std::vector<uint8_t> scratch;
    Snapshot entities;              // the last entity refresh (only its entity vectors are used)
    double lastEntities = -1;
    double lastDiscover = -1, lastGameWindow = -1;
    uint64_t seq = 0;
    float tickMs = 0, discoverMs = 0, tickHz = 0, readsPerSec = 0;
    double lastTickAt = 0, lastReadsAt = 0;
    uint64_t lastReads = 0;
    size_t actorsSeen = 0;

    // ---------------------------------------------------------------- thread plumbing
    void run();
    double step();
    double attach();
    double tick();

    void publishState(Link link, const std::string& detail);
    void publish(std::shared_ptr<Snapshot> s);
    void addEvent(const std::string& text, const std::string& highlight, int kind);
    void detachAll();
    void baseSnapshot(Snapshot& s);

    // ---------------------------------------------------------------- engine
    bool resolveEngineFields();
    void onWorldChanged(uintptr_t newWorld);
    bool resolveViewChain();
    bool readView(View& v);

    // ---------------------------------------------------------------- entities
    Kind kindOf(uintptr_t cls, bool& settled);
    bool ensureFields(Kind kind, uintptr_t cls, uintptr_t instance);
    bool stillAlive(uintptr_t actor);
    void discover(bool wantDebug);
    bool readTransform(uintptr_t component, ue::Transform& out) const;
    bool readBlock(uintptr_t actor, uint32_t extent, const Tracked& t);
    void refreshEntities(const Settings& cfg);
    void runGhost(const Settings& cfg);
    void resolveGhostLayout(const features::GhostWorld& world);
    bool refreshGuard(uintptr_t addr, Tracked& t, Snapshot& out);
    bool refreshCamera(uintptr_t addr, Tracked& t, Snapshot& out);
    bool refreshPhone(uintptr_t addr, Tracked& t, Snapshot& out);
    bool refreshTerminal(uintptr_t addr, Tracked& t, Snapshot& out);
};

//-----------------------------------------------------------------------------
// Thread plumbing
//-----------------------------------------------------------------------------

void Scanner::Impl::run()
{
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    while (!stopping.load(std::memory_order_relaxed))
    {
        const double t0 = util::Now();
        const double period = step();
        const double wait = period - (util::Now() - t0);
        if (wait > 0.0005 && timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(wait * 1e7);
            ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE handles[2] = { timer, wake };
            ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
    }
    if (timer) ::CloseHandle(timer);
    if (proc.attached() && ghost.engaged() && proc.alive())
        ghost.restore(proc, [this](uintptr_t actor) { return stillAlive(actor); }); // leave the game as we found it
    cache.close();
}

double Scanner::Impl::step()
{
    if (!proc.attached()) return attach();
    if (!proc.alive())
    {
        detachAll();
        addEvent("Game closed", "", 2);
        publishState(Link::Detached, "Waiting for the game to start");
        return 1.0;
    }
    if (!initialised)
    {
        if (!ue::ResolveGlobals(proc, cache, names, globals, initFailure))
        {
            publishState(Link::Incompatible, initFailure);
            // Each attempt scans the whole executable image: back off from 3 s up to 30 s.
            return std::min(30.0, 3.0 * (double)(1 << std::min(initFailures++, 4)));
        }
        initFailures = 0;
        initialised = true;
        addEvent("Attached to", "OAR (pid " + std::to_string(proc.pid()) + ")", 1);
    }
    return tick();
}

double Scanner::Impl::attach()
{
    switch (proc.attach(kGameExe))
    {
    case game::AttachResult::Ok:
        cache.open(proc.buildId());
        initialised = false;
        return 0.0;
    case game::AttachResult::NotRunning:
        publishState(Link::Detached, "Waiting for OAR-Win64-Shipping.exe");
        return 1.0;
    case game::AttachResult::AccessDenied:
        publishState(Link::AccessDenied, "The game runs elevated: start this program as administrator");
        return 1.0;
    default:
        publishState(Link::Detached, "Could not open the game process");
        return 2.0;
    }
}

void Scanner::Impl::detachAll()
{
    cache.close();
    proc.detach();
    names.setPool(0);
    refl.clearPointerCaches();
    eng = {};
    guardF = {};
    cameraF = {};
    phoneF = {};
    terminalF = {};
    globals = {};
    initialised = false;
    layoutVerified = false;
    world = pcm = pcAddr = 0;
    ghost.forget();
    lastGhost = -1;
    worldName.clear();
    classKinds.clear();
    actors.clear();
    tracked.clear();
    peakHealth.clear();
    entities = Snapshot();
    lastEntities = -1;
    debugActors.reset();
    gameWindow = nullptr;
    lastDiscover = lastGameWindow = -1;
}

void Scanner::Impl::addEvent(const std::string& text, const std::string& highlight, int kind)
{
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back({ util::Now(), text, highlight, kind });
    if (events.size() > 64) events.erase(events.begin());
}

void Scanner::Impl::baseSnapshot(Snapshot& s)
{
    s.seq = ++seq;
    s.pid = proc.pid();
    s.base = proc.base();
    s.buildId = proc.buildId();
    s.worldName = worldName;
    s.gameWindow = gameWindow;
}

void Scanner::Impl::publish(std::shared_ptr<Snapshot> s)
{
    std::lock_guard<std::mutex> lock(mutex);
    snapshot = std::move(s);
}

void Scanner::Impl::publishState(Link link, const std::string& detail)
{
    auto s = std::make_shared<Snapshot>();
    baseSnapshot(*s);
    s->link = link;
    s->detail = detail;
    publish(std::move(s));
}

//-----------------------------------------------------------------------------
// Engine: world, local player, camera
//-----------------------------------------------------------------------------

// Resolves the engine members by following the live object chain GWorld -> GameInstance -> LocalPlayer
// -> PlayerController -> PlayerCameraManager. Needs a local player to exist; retried every tick until it does.
bool Scanner::Impl::resolveEngineFields()
{
    if (eng.fieldsReady() && eng.levelActors && eng.componentToWorld) return true;

    const uintptr_t worldCls = refl.classOf(world);
    eng.persistentLevel = refl.field(worldCls, "PersistentLevel", "ObjectProperty");
    eng.levels = refl.field(worldCls, "Levels", "ArrayProperty");
    eng.owningGameInstance = refl.field(worldCls, "OwningGameInstance", "ObjectProperty");
    if (!eng.persistentLevel || !eng.levels || !eng.owningGameInstance) return false;

    const uintptr_t gi = proc.read<uintptr_t>(world + eng.owningGameInstance.offset);
    if (!gi) return false;
    eng.localPlayers = refl.field(refl.classOf(gi), "LocalPlayers", "ArrayProperty");
    if (!eng.localPlayers) return false;

    const auto players = proc.read<ue::ArrayHeader>(gi + eng.localPlayers.offset);
    const uintptr_t lp = players.num > 0 ? proc.read<uintptr_t>(players.data) : 0;
    if (!lp) return false;
    const uintptr_t lpCls = refl.classOf(lp);
    eng.aspectAxis = refl.field(lpCls, "AspectRatioAxisConstraint", "ByteProperty");
    eng.playerController = refl.field(lpCls, "PlayerController", "ObjectProperty");
    if (!eng.aspectAxis || !eng.playerController) return false;

    const uintptr_t pc = proc.read<uintptr_t>(lp + eng.playerController.offset);
    if (!pc) return false;
    const uintptr_t pcCls = refl.classOf(pc);
    eng.cameraManager = refl.field(pcCls, "PlayerCameraManager", "ObjectProperty");
    eng.actorRoot = refl.field(pcCls, "RootComponent", "ObjectProperty");
    eng.acknowledgedPawn = refl.field(pcCls, "AcknowledgedPawn", "ObjectProperty");
    if (!eng.cameraManager || !eng.actorRoot) return false;

    const uintptr_t cm = proc.read<uintptr_t>(pc + eng.cameraManager.offset);
    if (!cm) return false;
    const uintptr_t cmCls = refl.classOf(cm);
    eng.povLocation = refl.field(cmCls, "CameraCachePrivate.POV.Location", "StructProperty");
    eng.povRotation = refl.field(cmCls, "CameraCachePrivate.POV.Rotation", "StructProperty");
    eng.povFov = refl.field(cmCls, "CameraCachePrivate.POV.FOV", "FloatProperty");
    eng.povAspect = refl.field(cmCls, "CameraCachePrivate.POV.AspectRatio", "FloatProperty");
    eng.povConstrain = refl.field(cmCls, "CameraCachePrivate.POV.bConstrainAspectRatio", "BoolProperty");
    if (!eng.fieldsReady()) return false;
    eng.viewLo = std::min({ eng.povLocation.offset, eng.povRotation.offset, eng.povFov.offset, eng.povAspect.offset, eng.povConstrain.offset });
    eng.viewHi = std::max({ eng.povLocation.offset + 12, eng.povRotation.offset + 12, eng.povFov.offset + 4, eng.povAspect.offset + 4, eng.povConstrain.offset + 1 });

    const uintptr_t level = proc.read<uintptr_t>(world + eng.persistentLevel.offset);
    if (!eng.levelActors) eng.levelActors = refl.levelActorsOffset(level);
    if (eng.levelActors && !eng.componentToWorld) eng.componentToWorld = refl.componentToWorldOffset(level, eng.levelActors);
    return eng.levelActors && eng.componentToWorld;
}

void Scanner::Impl::onWorldChanged(uintptr_t newWorld)
{
    world = newWorld;
    pcm = pcAddr = 0;
    ghost.forget(); // the objects it remembered belong to the old world
    classKinds.clear();
    actors.clear();
    tracked.clear();
    peakHealth.clear();
    entities = Snapshot();
    lastEntities = -1;
    debugActors.reset();
    refl.clearPointerCaches();
    lastDiscover = -1;
    worldName = refl.objectName(newWorld);
    addEvent("World", worldName, 0);
}

bool Scanner::Impl::resolveViewChain()
{
    const uintptr_t gi = proc.read<uintptr_t>(world + eng.owningGameInstance.offset);
    const auto players = gi ? proc.read<ue::ArrayHeader>(gi + eng.localPlayers.offset) : ue::ArrayHeader{};
    const uintptr_t lp = players.num > 0 ? proc.read<uintptr_t>(players.data) : 0;
    const uintptr_t pc = lp ? proc.read<uintptr_t>(lp + eng.playerController.offset) : 0;
    pcAddr = pc;
    pcm = pc ? proc.read<uintptr_t>(pc + eng.cameraManager.offset) : 0;
    axisConstraint = lp ? proc.read<uint8_t>(lp + eng.aspectAxis.offset) : 0;
    return pcm != 0;
}

bool Scanner::Impl::readView(View& v)
{
    if (!pcm) return false;
    scratch.resize(eng.viewHi - eng.viewLo);
    if (!proc.read(pcm + eng.viewLo, scratch.data(), scratch.size())) return false;
    const Slice s{ scratch.data(), eng.viewLo };
    v.location = s.get<ue::Vec3>(eng.povLocation);
    v.rotation = s.get<ue::Rot>(eng.povRotation);
    v.fov = s.get<float>(eng.povFov);
    v.aspect = s.get<float>(eng.povAspect);
    v.constrainAspect = s.flag(eng.povConstrain);
    v.axisConstraint = axisConstraint;
    v.valid = Finite(v.location) && std::isfinite(v.fov) && v.fov > 1.0f && v.fov < 170.0f;
    return v.valid;
}

//-----------------------------------------------------------------------------
// Entities
//-----------------------------------------------------------------------------

// Classifies a class by walking its super chain. `settled` is false when a read failed on the way, in which
// case the answer must not be remembered: it would turn a transient failure into "not interesting" for good.
Kind Scanner::Impl::kindOf(uintptr_t cls, bool& settled)
{
    if (auto it = classKinds.find(cls); it != classKinds.end())
    {
        settled = true;
        return it->second;
    }
    Kind kind = Kind::None;
    bool reachedObject = false;
    settled = true;
    for (uintptr_t c = cls, depth = 0; c && depth < 24 && kind == Kind::None && !reachedObject; depth++, c = refl.superOf(c))
    {
        const std::string& name = refl.className(c);
        if (name == "?") settled = false;
        reachedObject = name == "Object";
        for (const ClassKind& ck : kClassKinds)
            if (name == ck.name) kind = ck.kind;
    }
    if (kind == Kind::None && !reachedObject) settled = false; // chain was cut short by a failed read
    if (settled) classKinds.emplace(cls, kind);
    return kind;
}

// Resolves a kind's fields the first time an instance of it shows up. Blueprint classes can be present
// before their layout is usable, so a failure is retried, but not more often than every two seconds:
// each attempt walks property lists, and there can be dozens of instances.
bool Scanner::Impl::ensureFields(Kind kind, uintptr_t cls, uintptr_t instance)
{
    constexpr double kRetryDelay = 2.0;
    const auto fail = [](double& retryAt) {
        retryAt = util::Now() + kRetryDelay;
        return false;
    };
    switch (kind)
    {
    case Kind::Guard:
    {
        GuardFields& f = guardF;
        if (f.ready) return true;
        if (util::Now() < f.retryAt) return false;
        f.health = refl.field(cls, "Health", "IntProperty");
        f.dead = refl.field(cls, "Dead?", "BoolProperty");
        f.patrolling = refl.field(cls, "patrolling?", "BoolProperty");
        f.investigating = refl.field(cls, "Investigating?", "BoolProperty");
        f.alert = refl.field(cls, "Alert?", "BoolProperty");
        f.escorting = refl.field(cls, "Escorting?", "BoolProperty");
        f.emped = refl.field(cls, "EMPed", "BoolProperty");
        f.tiedUp = refl.field(cls, "TiedUp?", "BoolProperty");
        f.capsule = refl.field(cls, "CapsuleComponent", "ObjectProperty");
        if (!(f.health && f.dead && f.patrolling && f.investigating && f.alert && f.escorting && f.emped && f.tiedUp && f.capsule)) return fail(f.retryAt);
        const uintptr_t capsule = proc.read<uintptr_t>(instance + f.capsule.offset);
        if (!capsule) return fail(f.retryAt);
        const uintptr_t capsuleCls = refl.classOf(capsule);
        f.halfHeight = refl.field(capsuleCls, "CapsuleHalfHeight", "FloatProperty");
        f.radius = refl.field(capsuleCls, "CapsuleRadius", "FloatProperty");
        if (!f.halfHeight || !f.radius) return fail(f.retryAt);
        f.extent = std::max<uint32_t>(ue::Extent({ f.health, f.dead, f.patrolling, f.investigating, f.alert, f.escorting, f.emped, f.tiedUp, f.capsule }), 0x40);
        return f.ready = true;
    }
    case Kind::Camera:
    {
        CameraFields& f = cameraF;
        if (f.ready) return true;
        if (util::Now() < f.retryAt) return false;
        f.destroyed = refl.field(cls, "Destroyed?", "BoolProperty");
        f.emped = refl.field(cls, "EMPed", "BoolProperty");
        f.possessed = refl.field(cls, "Possessed?", "BoolProperty");
        f.ignored = refl.field(cls, "Ignored?", "BoolProperty");
        f.number = refl.field(cls, "CamNumber", "IntProperty");
        f.lens = refl.field(cls, "Camera", "ObjectProperty");
        f.spotted = refl.field(cls, "SpottedPlayers", "ArrayProperty");
        if (!(f.destroyed && f.emped && f.possessed && f.ignored && f.number && f.lens && f.spotted)) return fail(f.retryAt);
        f.extent = std::max<uint32_t>(ue::Extent({ f.destroyed, f.emped, f.possessed, f.ignored, f.number, f.lens }, 8), f.spotted.offset + 16);
        f.extent = std::max(f.extent, eng.actorRoot.offset + 8);
        return f.ready = true;
    }
    case Kind::Phone:
    {
        PhoneFields& f = phoneF;
        if (f.ready) return true;
        if (util::Now() < f.retryAt) return false;
        f.active = refl.field(cls, "Active?", "BoolProperty");
        f.emped = refl.field(cls, "EMPed?", "BoolProperty");
        f.seconds = refl.field(cls, "SecondsToAlert", "IntProperty");
        if (!(f.active && f.emped && f.seconds)) return fail(f.retryAt);
        f.extent = std::max(ue::Extent({ f.active, f.emped, f.seconds }), eng.actorRoot.offset + 8);
        return f.ready = true;
    }
    case Kind::Terminal:
    {
        TerminalFields& f = terminalF;
        if (f.ready) return true;
        if (util::Now() < f.retryAt) return false;
        f.activated = refl.field(cls, "Activated?", "BoolProperty");
        if (!f.activated) return fail(f.retryAt);
        f.extent = std::max(ue::Extent({ f.activated }), eng.actorRoot.offset + 8);
        return f.ready = true;
    }
    case Kind::Police:
    case Kind::Sensor:
        return true; // tracked for Ghost only; its own fields are resolved from the first instance
    default:
        return false;
    }
}

// Walks every level's actor array. Only actors we have not classified yet cost a class lookup;
// everything seen on the previous walk is answered from `actors`.
void Scanner::Impl::discover(bool wantDebug)
{
    const double t0 = util::Now();
    generation++;

    // `complete` turns false if any list could not be read: a partial walk must not be mistaken for
    // "those actors are gone", which would drop their tracking state.
    bool complete = true;
    std::vector<uintptr_t> levels;
    const auto levelArray = proc.read<ue::ArrayHeader>(world + eng.levels.offset);
    if (levelArray.num > 0 && levelArray.num < 256)
    {
        levels.resize((size_t)levelArray.num);
        if (!proc.read(levelArray.data, levels.data(), levels.size() * sizeof(uintptr_t))) levels.clear();
    }
    if (levels.empty())
        levels.push_back(proc.read<uintptr_t>(world + eng.persistentLevel.offset));

    std::vector<uintptr_t> all, list;
    for (uintptr_t level : levels)
    {
        if (!level) { complete = false; continue; }
        const auto arr = proc.read<ue::ArrayHeader>(level + eng.levelActors);
        if (arr.num < 0 || arr.num > (1 << 20) || (arr.num > 0 && !arr.data)) { complete = false; continue; }
        if (arr.num == 0) continue;
        list.resize((size_t)arr.num);
        if (proc.read(arr.data, list.data(), list.size() * sizeof(uintptr_t)))
            all.insert(all.end(), list.begin(), list.end());
        else
            complete = false;
    }

    for (uintptr_t actor : all)
    {
        if (!actor) continue;
        auto it = actors.find(actor);
        if (it == actors.end() || !it->second.settled)
        {
            const uintptr_t cls = refl.classOf(actor);
            bool settled = false;
            const Kind kind = cls ? kindOf(cls, settled) : Kind::None;
            if (it == actors.end())
                it = actors.emplace(actor, ActorInfo{}).first;
            it->second.kind = kind;
            it->second.cls = cls;
            it->second.settled = settled; // unsettled answers are asked again at the next discovery
        }
        ActorInfo& info = it->second;
        info.generation = generation;
        if (info.kind != Kind::None && !tracked.count(actor) && ensureFields(info.kind, info.cls, actor))
            tracked.emplace(actor, Tracked{ info.kind, info.cls });
    }

    // Drop everything that is no longer in the level.
    if (complete)
    {
        for (auto it = actors.begin(); it != actors.end();)
            it = it->second.generation != generation ? actors.erase(it) : std::next(it);
        for (auto it = tracked.begin(); it != tracked.end();)
            it = actors.count(it->first) ? std::next(it) : tracked.erase(it);
        for (auto it = peakHealth.begin(); it != peakHealth.end();)
            it = actors.count(it->first) ? std::next(it) : peakHealth.erase(it);
    }
    actorsSeen = actors.size();

    if (wantDebug)
    {
        auto out = std::make_shared<std::vector<DebugActor>>();
        for (uintptr_t actor : all)
        {
            if (out->size() >= kDebugActorLimit) break;
            const uintptr_t root = actor ? proc.read<uintptr_t>(actor + eng.actorRoot.offset) : 0;
            ue::Transform tf;
            if (root && readTransform(root, tf))
                out->push_back({ tf.translation, refl.className(actors[actor].cls) });
        }
        debugActors = std::move(out);
    }
    else
        debugActors.reset();

    discoverMs = (float)((util::Now() - t0) * 1000.0);
}

bool Scanner::Impl::readTransform(uintptr_t component, ue::Transform& out) const
{
    return component && proc.read(component + eng.componentToWorld, out) && Finite(out.translation);
}

// Last look before a write: the actor is still the object we tracked (same class) and not being destroyed.
bool Scanner::Impl::stillAlive(uintptr_t actor)
{
    const auto it = tracked.find(actor);
    if (it == tracked.end()) return false;
    uint8_t head[ue::layout::UObjectClass + sizeof(uintptr_t)] = {};
    if (!proc.read(actor, head, sizeof(head))) return false;
    uint32_t flags;
    uintptr_t cls;
    memcpy(&flags, head + ue::layout::UObjectFlags, sizeof(flags));
    memcpy(&cls, head + ue::layout::UObjectClass, sizeof(cls));
    return cls == it->second.cls && (flags & kDestroyedFlags) == 0;
}

// Reads the first `extent` bytes of an actor into `scratch` and rejects dead or recycled objects.
bool Scanner::Impl::readBlock(uintptr_t actor, uint32_t extent, const Tracked& t)
{
    scratch.resize(extent);
    if (!proc.read(actor, scratch.data(), extent)) return false;
    const uint32_t flags = ue::Get<uint32_t>(scratch.data(), { ue::layout::UObjectFlags });
    const uintptr_t cls = ue::Get<uintptr_t>(scratch.data(), { ue::layout::UObjectClass });
    return cls == t.cls && (flags & kDestroyedFlags) == 0;
}

bool Scanner::Impl::refreshGuard(uintptr_t addr, Tracked& t, Snapshot& out)
{
    const GuardFields& f = guardF;
    if (!readBlock(addr, f.extent, t)) return false;
    const uint8_t* b = scratch.data();

    Guard g;
    g.id = addr;
    g.health = ue::Get<int32_t>(b, f.health);
    g.dead = ue::GetBool(b, f.dead);
    g.patrolling = ue::GetBool(b, f.patrolling);
    g.investigating = ue::GetBool(b, f.investigating);
    g.alert = ue::GetBool(b, f.alert);
    g.escorting = ue::GetBool(b, f.escorting);
    g.emped = ue::GetBool(b, f.emped);
    g.tiedUp = ue::GetBool(b, f.tiedUp);
    int& peak = peakHealth[addr]; // survives re-tracking, so a wounded guard keeps its true maximum
    peak = std::max({ peak, g.health, 1 });
    g.maxHealth = peak;

    // One read covers the capsule's world transform and its dimensions.
    const uintptr_t capsule = ue::Get<uintptr_t>(b, f.capsule);
    const uint32_t lo = std::min(eng.componentToWorld, std::min(f.halfHeight.offset, f.radius.offset));
    const uint32_t hi = std::max(eng.componentToWorld + (uint32_t)sizeof(ue::Transform), std::max(f.halfHeight.offset, f.radius.offset) + 4);
    if (!capsule || hi - lo > 0x800) return false;
    scratch.resize(hi - lo);
    if (!proc.read(capsule + lo, scratch.data(), scratch.size())) return false;
    const Slice c{ scratch.data(), lo };
    const auto tf = c.get<ue::Transform>(eng.componentToWorld);
    if (!Finite(tf.translation)) return false;
    g.pos = tf.translation;
    const float scale = std::fabs(tf.scale.z) > 0.01f ? tf.scale.z : 1.0f;
    g.halfHeight = c.get<float>(f.halfHeight) * scale;
    g.radius = c.get<float>(f.radius) * scale;
    out.guards.push_back(g);
    return true;
}

bool Scanner::Impl::refreshCamera(uintptr_t addr, Tracked& t, Snapshot& out)
{
    const CameraFields& f = cameraF;
    if (!readBlock(addr, f.extent, t)) return false;
    const uint8_t* b = scratch.data();

    Camera c;
    c.id = addr;
    c.destroyed = ue::GetBool(b, f.destroyed);
    c.emped = ue::GetBool(b, f.emped);
    c.possessed = ue::GetBool(b, f.possessed);
    c.ignored = ue::GetBool(b, f.ignored);
    c.number = ue::Get<int32_t>(b, f.number);
    c.spotting = ue::Get<ue::ArrayHeader>(b, f.spotted).num > 0;

    // The lens (CameraComponent) carries the real orientation; fall back to the actor root.
    ue::Transform tf;
    const uintptr_t lens = ue::Get<uintptr_t>(b, f.lens);
    const uintptr_t root = ue::Get<uintptr_t>(b, eng.actorRoot);
    if (!readTransform(lens, tf) && !readTransform(root, tf)) return false;
    c.pos = tf.translation;
    c.forward = Forward(tf.rotation);
    out.cameras.push_back(c);
    return true;
}

bool Scanner::Impl::refreshPhone(uintptr_t addr, Tracked& t, Snapshot& out)
{
    const PhoneFields& f = phoneF;
    if (!readBlock(addr, f.extent, t)) return false;
    const uint8_t* b = scratch.data();

    Phone p;
    p.id = addr;
    p.active = ue::GetBool(b, f.active);
    p.emped = ue::GetBool(b, f.emped);
    p.seconds = ue::Get<int32_t>(b, f.seconds);
    ue::Transform tf;
    if (!readTransform(ue::Get<uintptr_t>(b, eng.actorRoot), tf)) return false;
    p.pos = tf.translation;
    out.phones.push_back(p);
    return true;
}

bool Scanner::Impl::refreshTerminal(uintptr_t addr, Tracked& t, Snapshot& out)
{
    const TerminalFields& f = terminalF;
    if (!readBlock(addr, f.extent, t)) return false;
    const uint8_t* b = scratch.data();

    Terminal term;
    term.id = addr;
    term.activated = ue::GetBool(b, f.activated);
    ue::Transform tf;
    if (!readTransform(ue::Get<uintptr_t>(b, eng.actorRoot), tf)) return false;
    term.pos = tf.translation;
    out.terminals.push_back(term);
    return true;
}

//-----------------------------------------------------------------------------
// Ghost
//-----------------------------------------------------------------------------

// Finds the fields Ghost writes, from the first live instance of each NPC kind. Whatever cannot be resolved
// yet (no police on the map, say) stays unresolved and that part of Ghost simply does nothing until it can be.
void Scanner::Impl::resolveGhostLayout(const features::GhostWorld& w)
{
    features::GhostLayout& g = ghostLayout;
    const auto firstOf = [this](Kind kind, uintptr_t& addr, uintptr_t& cls) {
        for (const auto& [a, t] : tracked)
            if (t.kind == kind)
            {
                addr = a;
                cls = t.cls;
                return true;
            }
        return false;
    };
    // PawnSensing and SpotPlayer components are the same classes everywhere: take their layout from any instance.
    const auto learnSensing = [&](uintptr_t owner, const ue::Field& sensingField) {
        if (g.seePawns && g.hearNoises) return;
        const uintptr_t comp = sensingField ? proc.read<uintptr_t>(owner + sensingField.offset) : 0;
        if (!comp) return;
        const uintptr_t cls = refl.classOf(comp);
        g.seePawns = refl.field(cls, "bSeePawns", "BoolProperty");
        g.hearNoises = refl.field(cls, "bHearNoises", "BoolProperty");
    };
    const auto learnSpot = [&](uintptr_t owner, const ue::Field& spotField) {
        if (g.spottedPlayer) return;
        const uintptr_t comp = spotField ? proc.read<uintptr_t>(owner + spotField.offset) : 0;
        if (comp) g.spottedPlayer = refl.field(refl.classOf(comp), "SpottedPlayer", "ObjectProperty");
    };

    uintptr_t addr = 0, cls = 0;
    if (firstOf(Kind::Guard, addr, cls))
    {
        g.guardEars = refl.field(cls, "PawnSensing", "ObjectProperty");
        g.guardSpot = refl.field(cls, "SpotPlayerComponent", "ObjectProperty");
        g.guardTarget = refl.field(cls, "TargetPlayer", "ObjectProperty");
        g.guardEscorting = refl.field(cls, "EscortingPlayer", "ObjectProperty");
        g.guardAware = refl.field(cls, "Sensing?", "BoolProperty");
        learnSensing(addr, g.guardEars);
        learnSpot(addr, g.guardSpot);
    }
    if (firstOf(Kind::Sensor, addr, cls))
    {
        g.sensorEyes = refl.field(cls, "PawnSensing", "ObjectProperty");
        learnSensing(addr, g.sensorEyes);
    }
    if (firstOf(Kind::Police, addr, cls))
    {
        g.policeEyes = refl.field(cls, "PawnSensing", "ObjectProperty");
        g.policeTarget = refl.field(cls, "TargetPlayer", "ObjectProperty");
        g.policeAware = refl.field(cls, "Sensing?", "BoolProperty");
        learnSensing(addr, g.policeEyes);
    }
    if (firstOf(Kind::Camera, addr, cls))
    {
        g.cameraIgnored = cameraF.ignored;
        g.cameraSpot = refl.field(cls, "SpotPlayerComponent", "ObjectProperty");
        learnSpot(addr, g.cameraSpot);
    }
    if (!g.pawnSus && w.localPawn)
        g.pawnSus = refl.field(refl.classOf(w.localPawn), "Sus?", "BoolProperty");
}

// Applies Ghost (or undoes it) to everything tracked right now.
void Scanner::Impl::runGhost(const Settings& cfg)
{
    if (!cfg.ghost && !ghost.engaged()) return; // nothing to apply and nothing to give back

    features::GhostWorld w;
    for (const auto& [addr, t] : tracked)
        switch (t.kind)
        {
        case Kind::Guard:  w.guards.push_back(addr); break;
        case Kind::Camera: w.cameras.push_back(addr); break;
        case Kind::Police: w.police.push_back(addr); break;
        case Kind::Sensor: w.sensors.push_back(addr); break;
        default: break;
        }
    w.localPawn = pcAddr && eng.acknowledgedPawn ? proc.read<uintptr_t>(pcAddr + eng.acknowledgedPawn.offset) : 0;

    const double now = util::Now();
    if (cfg.ghost && now >= ghostLayoutRetryAt)
    {
        ghostLayoutRetryAt = now + 2.0; // resolution is cached on disk, but there is no need to redo it every tick
        resolveGhostLayout(w);
    }
    const auto alive = [this](uintptr_t actor) { return stillAlive(actor); };
    ghosted = ghost.tick(proc, ghostLayout, cfg, w, alive);
}

//-----------------------------------------------------------------------------
// One tick
//-----------------------------------------------------------------------------

// Re-reads every tracked entity into `entities` and runs the check-in feature on the fresh phone data.
void Scanner::Impl::refreshEntities(const Settings& cfg)
{
    entities.guards.clear();
    entities.cameras.clear();
    entities.phones.clear();
    entities.terminals.clear();

    for (auto it = tracked.begin(); it != tracked.end();)
    {
        if (it->second.kind == Kind::Police || it->second.kind == Kind::Sensor) // Ghost-only: validated by discovery
        {
            ++it;
            continue;
        }
        bool ok = false;
        switch (it->second.kind)
        {
        case Kind::Guard:    ok = refreshGuard(it->first, it->second, entities); break;
        case Kind::Camera:   ok = refreshCamera(it->first, it->second, entities); break;
        case Kind::Phone:    ok = refreshPhone(it->first, it->second, entities); break;
        case Kind::Terminal: ok = refreshTerminal(it->first, it->second, entities); break;
        default: break;
        }
        // A failed read is usually a freed actor, but tolerate a few in a row before letting go; a dropped
        // entity that is still alive is picked up again by the next discovery.
        it->second.misses = ok ? 0 : it->second.misses + 1;
        it = it->second.misses >= kMaxMisses ? tracked.erase(it) : std::next(it);
    }

    const auto alive = [this](uintptr_t actor) { return stillAlive(actor); };
    if (int n = checkIn.tick(proc, phoneF.active, entities.phones, cfg, alive); n > 0)
        addEvent("Guard check-in forced", std::to_string(n) + (n == 1 ? " phone" : " phones"), 1);
}

double Scanner::Impl::tick()
{
    const double t0 = util::Now();

    const uintptr_t w = proc.read<uintptr_t>(globals.worldSlot);
    if (!w)
    {
        world = 0;
        publishState(Link::Waiting, "No world loaded yet");
        return 0.25;
    }
    if (!layoutVerified)
    {
        std::string why;
        if (!refl.selfTest(w, why))
        {
            publishState(Link::Incompatible, why);
            return 2.0;
        }
        layoutVerified = true;
    }
    if (w != world)
        onWorldChanged(w);
    if (!resolveEngineFields())
    {
        publishState(Link::Waiting, "Waiting for the player to spawn");
        return 0.25;
    }

    Settings cfg;
    {
        std::lock_guard<std::mutex> lock(mutex);
        cfg = settings;
    }

    const double now = util::Now();
    if (now - lastDiscover >= kDiscoverPeriod || (cfg.debugActors && !debugActors))
    {
        lastDiscover = now;
        discover(cfg.debugActors);
        resolveViewChain();
        cache.flush();
    }
    if (now - lastGameWindow >= 1.0)
    {
        lastGameWindow = now;
        gameWindow = proc.mainWindow();
    }

    auto snap = std::make_shared<Snapshot>();
    baseSnapshot(*snap);
    snap->link = Link::Ready;

    // The camera view changes with every mouse movement, entities only as fast as people walk: the view is
    // read on every tick, entities at a lower rate, and the last entity picture is reused in between.
    if (!readView(snap->view) && resolveViewChain())
        readView(snap->view);

    if (now - lastEntities >= kEntityPeriod)
    {
        lastEntities = now;
        refreshEntities(cfg);
    }
    if (now - lastGhost >= kGhostPeriod)
    {
        lastGhost = now;
        runGhost(cfg);
    }
    snap->guards = entities.guards;
    snap->cameras = entities.cameras;
    snap->phones = entities.phones;
    snap->terminals = entities.terminals;
    snap->debugActors = debugActors;

    // Statistics
    tickMs = tickMs * 0.9f + (float)((util::Now() - t0) * 1000.0) * 0.1f;
    if (lastTickAt > 0.0)
    {
        const double dt = t0 - lastTickAt;
        if (dt > 0.0) tickHz = tickHz * 0.95f + (float)(1.0 / dt) * 0.05f;
    }
    lastTickAt = t0;
    if (t0 - lastReadsAt >= 1.0)
    {
        const uint64_t reads = proc.readCount();
        if (lastReadsAt > 0.0) readsPerSec = (float)((double)(reads - lastReads) / (t0 - lastReadsAt));
        lastReads = reads;
        lastReadsAt = t0;
    }
    Stats& st = snap->stats;
    st.tickMs = tickMs;
    st.discoverMs = discoverMs;
    st.tickHz = tickHz;
    st.reads = proc.readCount();
    st.readsPerSec = readsPerSec;
    st.names = names.cached();
    st.offsetsCached = cache.size();
    st.cacheHits = cache.hits();
    st.cacheMisses = cache.misses();
    st.actorsSeen = actorsSeen;
    st.ghosted = ghosted;

    publish(std::move(snap));
    return kTickPeriod;
}

//-----------------------------------------------------------------------------
// Public interface
//-----------------------------------------------------------------------------

Scanner::Scanner() : impl_(std::make_unique<Impl>()) {}
Scanner::~Scanner() { stop(); }

void Scanner::start()
{
    if (impl_->thread.joinable()) return;
    impl_->wake = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->stopping = false;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void Scanner::stop()
{
    if (!impl_->thread.joinable()) return;
    impl_->stopping = true;
    ::SetEvent(impl_->wake);
    impl_->thread.join();
    ::CloseHandle(impl_->wake);
    impl_->wake = nullptr;
}

SnapshotPtr Scanner::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot;
}

void Scanner::setSettings(const Settings& s)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->settings = s;
}

void Scanner::requestCheckIn() { impl_->checkIn.request(); }

std::vector<Event> Scanner::takeEvents()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<Event> out;
    out.swap(impl_->events);
    return out;
}
}
