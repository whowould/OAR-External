#include "game/unreal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ue
{
bool WriteBool(const game::Process& proc, uintptr_t object, const Field& f, bool value)
{
    if (!f) return false;
    const uintptr_t addr = object + f.offset;
    if (f.mask == 0xFF) // Blueprint bool: a whole byte
        return proc.write<uint8_t>(addr, value ? 1 : 0);
    uint8_t byte = 0;
    if (!proc.read(addr, byte)) return false;
    byte = value ? (uint8_t)(byte | f.mask) : (uint8_t)(byte & ~f.mask);
    return proc.write(addr, byte);
}

//-----------------------------------------------------------------------------
// NamePool
//-----------------------------------------------------------------------------

void NamePool::setPool(uintptr_t pool)
{
    pool_ = pool;
    cache_.clear();
}

bool NamePool::validate()
{
    // Entries are 2-byte aligned and indexed in 2-byte units: "None" takes 6 bytes, so entry 3 follows it.
    return pool_ && get(0) == "None" && get(3) == "ByteProperty";
}

const std::string& NamePool::get(uint32_t index)
{
    static const std::string kUnknown = "?";
    if (auto it = cache_.find(index); it != cache_.end())
        return it->second;
    if (!pool_) return kUnknown;

    const uint32_t block = index >> 16, offset = (index & 0xFFFF) * 2;
    if (block >= 8192) return kUnknown;
    const uintptr_t blockPtr = proc_.read<uintptr_t>(pool_ + layout::NamePoolBlocks + block * 8);
    if (!blockPtr) return kUnknown;

    // FNameEntryHeader: bit 0 = wide, bits 1-5 = lowercase probe hash, bits 6-15 = length.
    const uint16_t header = proc_.read<uint16_t>(blockPtr + offset);
    const uint32_t len = header >> 6;
    const bool wide = (header & 1) != 0;
    if (len == 0 || len > 1023) return kUnknown;

    std::string out;
    if (wide)
    {
        std::wstring w(len, L'\0');
        if (!proc_.read(blockPtr + offset + 2, w.data(), len * 2)) return kUnknown;
        out.resize((size_t)::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, nullptr, 0, nullptr, nullptr));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, out.data(), (int)out.size(), nullptr, nullptr);
    }
    else
    {
        out.resize(len);
        if (!proc_.read(blockPtr + offset + 2, out.data(), len)) return kUnknown;
    }
    return cache_.emplace(index, std::move(out)).first->second;
}

//-----------------------------------------------------------------------------
// Reflection
//-----------------------------------------------------------------------------

std::string Reflection::objectName(uintptr_t object)
{
    struct FName { uint32_t index, number; } n{};
    if (!proc_.read(object + layout::UObjectName, n)) return "?";
    std::string s = names_.get(n.index);
    if (n.number) s += "_" + std::to_string(n.number - 1);
    return s;
}

const std::string& Reflection::className(uintptr_t cls)
{
    static const std::string kUnknown = "?";
    if (auto it = classNames_.find(cls); it != classNames_.end())
        return it->second;
    if (!cls) return kUnknown;
    const uint32_t idx = proc_.read<uint32_t>(cls + layout::UObjectName);
    const std::string& n = names_.get(idx);
    if (n == "?") return kUnknown; // unreadable right now: don't cache the failure
    return classNames_.emplace(cls, n).first->second;
}

bool Reflection::derivesFrom(uintptr_t cls, std::string_view name)
{
    for (int depth = 0; cls && depth < 32; depth++, cls = superOf(cls))
        if (className(cls) == name) return true;
    return false;
}

bool Reflection::isActor(uintptr_t object)
{
    if (!object) return false;
    const uintptr_t cls = classOf(object);
    return cls && derivesFrom(cls, "Actor");
}

uintptr_t Reflection::findProperty(uintptr_t ustruct, std::string_view name)
{
    for (int depth = 0; ustruct && depth < 32; depth++, ustruct = superOf(ustruct))
    {
        uintptr_t f = proc_.read<uintptr_t>(ustruct + layout::UStructChildProperties);
        for (int guard = 0; f && guard < 4096; guard++, f = proc_.read<uintptr_t>(f + layout::FFieldNext))
        {
            // Match the plain name only: "Foo_1" shares Foo's name index but has a non-zero number.
            struct { uint32_t index, number; } fname{};
            if (proc_.read(f + layout::FFieldName, fname) && fname.number == 0 && names_.get(fname.index) == name)
                return f;
        }
    }
    return 0;
}

Field Reflection::resolve(uintptr_t cls, std::string_view path, std::string_view wantedType)
{
    Field out;
    uint32_t total = 0;
    while (true)
    {
        const size_t dot = path.find('.');
        const std::string_view part = path.substr(0, dot);
        const uintptr_t prop = findProperty(cls, part);
        if (!prop) return {};

        const uintptr_t fieldClass = proc_.read<uintptr_t>(prop + layout::FFieldClass);
        const std::string& type = names_.get(proc_.read<uint32_t>(fieldClass)); // FFieldClass::Name
        total += proc_.read<uint32_t>(prop + layout::FPropertyOffset);

        if (dot == std::string_view::npos)
        {
            if (!wantedType.empty() && type != wantedType) return {};
            out.found = true;
            if (type == "BoolProperty")
            {
                total += proc_.read<uint8_t>(prop + layout::FBoolByteOffset);
                out.mask = proc_.read<uint8_t>(prop + layout::FBoolFieldMask);
            }
            out.offset = total;
            return out;
        }
        if (type != "StructProperty") return {};
        cls = proc_.read<uintptr_t>(prop + layout::FStructPropertyStruct);
        path.remove_prefix(dot + 1);
    }
}

// Packed cache value: bit 63 = found, bits 32-39 = mask, low 32 bits = offset.
static uint64_t Pack(const Field& f) { return (1ull << 63) | ((uint64_t)f.mask << 32) | f.offset; }
static Field Unpack(uint64_t v) { return { (uint32_t)v, (uint8_t)(v >> 32), true }; }
// A property offset is below 64 KB; anything else in the file is corruption and must not be trusted.
static bool Plausible(uint64_t v) { return (v >> 63) == 1 && (v & ~(0x8000000000000000ull | (0xFFull << 32) | 0xFFFFFFFFull)) == 0 && (uint32_t)v < 0x10000; }

Field Reflection::field(uintptr_t cls, std::string_view path, std::string_view type)
{
    if (!cls) return {};
    const std::string& name = className(cls);
    if (name == "?") return {}; // unreadable right now; "?.X" must not become a shared cache key
    std::string key = name;
    key += '.';
    key += path;
    if (auto it = fields_.find(key); it != fields_.end())
        return it->second;

    uint64_t packed = 0;
    Field f;
    if (cache_.get(key, packed) && Plausible(packed)) // persisted from an earlier run on this build
        f = Unpack(packed);
    else if ((f = resolve(cls, path, type)))
        cache_.set(key, Pack(f));
    if (f) fields_.emplace(std::move(key), f); // failures are retried by the caller: the class may not be loaded yet
    return f;
}

// ULevel::Actors is not reflected. It is the TArray<AActor*> inside the first 0x100 bytes of ULevel
// that holds the most live actors (ActorsForGC sits next to it and is much shorter).
uint32_t Reflection::levelActorsOffset(uintptr_t level)
{
    uint64_t cached = 0;
    if (cache_.get("!Level.Actors", cached))
    {
        if (cached >= 0x28 && cached <= 0xF8 && cached % 8 == 0) return (uint32_t)cached;
        cache_.erase("!Level.Actors"); // corrupt entry: rediscover
    }

    uint8_t blob[0x100] = {};
    if (!proc_.read(level, blob, sizeof(blob))) return 0;
    uint32_t best = 0;
    int bestNum = 0;
    for (uint32_t off = 0x28; off + sizeof(ArrayHeader) <= sizeof(blob); off += 8)
    {
        ArrayHeader a;
        memcpy(&a, blob + off, sizeof(a));
        if (a.num < 4 || a.num > a.max || a.max > (1 << 22) || a.data < 0x10000) continue;
        uintptr_t first[8] = {};
        if (!proc_.read(a.data, first, sizeof(first))) continue;
        int actors = 0;
        for (uintptr_t p : first) actors += isActor(p) ? 1 : 0;
        if (actors >= 7 && a.num > bestNum)
        {
            best = off;
            bestNum = a.num;
        }
    }
    if (best) cache_.set("!Level.Actors", best);
    return best;
}

// USceneComponent::ComponentToWorld is not reflected either. Parentless root components must have a
// world transform equal to their relative location, which singles out the right 16-byte slot.
uint32_t Reflection::componentToWorldOffset(uintptr_t level, uint32_t levelActors)
{
    uint64_t cached = 0;
    if (cache_.get("!SceneComponent.ComponentToWorld", cached))
    {
        if (cached >= 0x140 && cached <= 0x1D0 && cached % 16 == 0) return (uint32_t)cached;
        cache_.erase("!SceneComponent.ComponentToWorld");
    }
    if (!levelActors) return 0;

    ArrayHeader arr;
    if (!proc_.read(level + levelActors, arr) || arr.num <= 0) return 0;
    const int n = std::min(arr.num, 600);
    std::vector<uintptr_t> actors((size_t)n);
    if (!proc_.read(arr.data, actors.data(), actors.size() * sizeof(uintptr_t))) return 0;

    Field rootF, relF, attachF;
    struct Sample { uint8_t block[0x200]; };
    std::vector<Sample> samples;
    for (uintptr_t a : actors)
    {
        if (!a) continue;
        if (!rootF && !(rootF = field(classOf(a), "RootComponent", "ObjectProperty"))) continue;
        const uintptr_t root = proc_.read<uintptr_t>(a + rootF.offset);
        if (!root) continue;
        if (!relF)
        {
            const uintptr_t rc = classOf(root);
            relF = field(rc, "RelativeLocation", "StructProperty");
            attachF = field(rc, "AttachParent", "ObjectProperty");
            if (!relF || !attachF) return 0;
        }
        Sample s;
        if (proc_.read(root, s.block, sizeof(s.block)) && Get<uintptr_t>(s.block, attachF) == 0)
            samples.push_back(s);
    }
    if (samples.size() < 5) return 0;

    uint32_t best = 0;
    size_t bestScore = 0;
    for (uint32_t off = 0x140; off + sizeof(Transform) <= 0x200; off += 0x10)
    {
        size_t score = 0;
        for (const Sample& s : samples)
        {
            Transform t;
            memcpy(&t, s.block + off, sizeof(t));
            const float q = t.rotation.x * t.rotation.x + t.rotation.y * t.rotation.y + t.rotation.z * t.rotation.z + t.rotation.w * t.rotation.w;
            if (std::fabs(q - 1.0f) > 1e-3f) continue;
            const Vec3 rel = Get<Vec3>(s.block, relF);
            if (std::fabs(t.translation.x - rel.x) < 0.05f && std::fabs(t.translation.y - rel.y) < 0.05f && std::fabs(t.translation.z - rel.z) < 0.05f)
                score++;
        }
        if (score > bestScore) { bestScore = score; best = off; }
    }
    if (best && bestScore * 10 >= samples.size() * 8) // >= 80% agreement
    {
        cache_.set("!SceneComponent.ComponentToWorld", best);
        return best;
    }
    return 0;
}

bool Reflection::selfTest(uintptr_t world, std::string& why)
{
    const uintptr_t cls = classOf(world);
    if (!cls || className(cls) != "World")
    {
        why = "GWorld does not point at a UWorld (class '" + (cls ? className(cls) : std::string("null")) + "')";
        return false;
    }
    uintptr_t top = cls;
    int depth = 0;
    for (; superOf(top) && depth < 16; depth++) top = superOf(top);
    if (className(top) != "Object")
    {
        why = "UStruct::SuperStruct chain does not end at UObject (layout mismatch)";
        return false;
    }
    const Field level = field(cls, "PersistentLevel", "ObjectProperty");
    if (!level || level.offset < 0x28 || level.offset > 0x100)
    {
        why = "UWorld::PersistentLevel was not found by reflection (layout mismatch)";
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// Globals
//-----------------------------------------------------------------------------

// Both anchors are `lea/mov reg, [rip+disp32]` instructions of length 7 with the disp32 at +3.
static constexpr const char* kSigNamePool = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 0F 10 03";
static constexpr const char* kSigWorld    = "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01 33 D2 48 8B CB";

static uintptr_t RipTarget(const game::Process& proc, uintptr_t instruction)
{
    const int32_t rel = proc.read<int32_t>(instruction + 3);
    return instruction + 7 + (intptr_t)rel;
}

bool ResolveGlobals(const game::Process& proc, game::OffsetCache& cache, NamePool& names, Globals& out, std::string& why)
{
    const uintptr_t base = proc.base();
    const auto inImage = [&](uintptr_t a) { return a >= base && a < base + proc.imageSize(); };
    out = {};

    uint64_t rva = 0;
    if (cache.get("!GNames", rva))
    {
        names.setPool(base + rva);
        if (names.validate()) out.namePool = base + rva;
    }
    if (!out.namePool)
    {
        for (uintptr_t hit : proc.scan(game::Pattern(kSigNamePool), 8))
        {
            const uintptr_t target = RipTarget(proc, hit);
            if (!inImage(target)) continue;
            names.setPool(target);
            if (names.validate())
            {
                out.namePool = target;
                cache.set("!GNames", target - base);
                break;
            }
        }
    }
    if (!out.namePool)
    {
        names.setPool(0);
        why = "FNamePool not found (signature outdated?)";
        return false;
    }

    if (cache.get("!GWorld", rva) && inImage(base + rva))
        out.worldSlot = base + rva;
    if (!out.worldSlot)
    {
        for (uintptr_t hit : proc.scan(game::Pattern(kSigWorld), 8))
        {
            const uintptr_t target = RipTarget(proc, hit);
            if (!inImage(target)) continue;
            out.worldSlot = target;
            cache.set("!GWorld", target - base);
            break;
        }
    }
    if (!out.worldSlot)
    {
        why = "GWorld not found (signature outdated?)";
        return false;
    }
    return true;
}
}
