#pragma once
// Memory model of Unreal Engine 4.27 (x64, shipping): names, reflection and the few engine globals we need.
//
// Nothing here hardcodes a game offset. Property offsets are resolved by name from the live UClass
// objects and persisted per build (OffsetCache); the engine's own data layout (UStruct / FProperty /
// FNamePool) is fixed for 4.27 and checked at startup by Reflection::selfTest().
//
// Not thread-safe: owned and used by the scanner thread only.
#include "game/cache.h"
#include "game/process.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ue
{
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Rot  { float pitch = 0, yaw = 0, roll = 0; }; // degrees
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

struct Transform // FTransform (SIMD layout: each member padded to 16 bytes)
{
    Quat  rotation;
    Vec3  translation;
    float pad0 = 0;
    Vec3  scale{ 1, 1, 1 };
    float pad1 = 0;
};
static_assert(sizeof(Transform) == 48, "FTransform layout");

struct ArrayHeader // TArray<T>
{
    uintptr_t data = 0;
    int32_t   num = 0, max = 0;
};

// Engine 4.27 data layout that reflection itself relies on.
namespace layout
{
constexpr uint32_t UObjectFlags = 0x08, UObjectClass = 0x10, UObjectName = 0x18;
constexpr uint32_t UStructSuper = 0x40, UStructChildProperties = 0x50;
constexpr uint32_t FFieldClass = 0x08, FFieldNext = 0x20, FFieldName = 0x28;
constexpr uint32_t FPropertyOffset = 0x4C, FPropertySize = 0x3C;
constexpr uint32_t FStructPropertyStruct = 0x78;
constexpr uint32_t FBoolByteOffset = 0x79, FBoolFieldMask = 0x7B;
constexpr uint32_t NamePoolBlocks = 0x10; // FNamePool: lock, current block, cursor, then Blocks[]
}

// A resolved property: byte offset inside its container and, for bools, the bit mask of that byte.
struct Field
{
    uint32_t offset = 0;
    uint8_t  mask = 0; // 0xFF = whole byte (Blueprint bool), otherwise a single bit (native bitfield)
    bool     found = false;
    explicit operator bool() const { return found; }
};

// Typed access to an object image read into local memory (block starts at the object's address).
template <class T> inline T Get(const uint8_t* object, const Field& f)
{
    T v{};
    memcpy(&v, object + f.offset, sizeof(T));
    return v;
}
inline bool GetBool(const uint8_t* object, const Field& f) { return (object[f.offset] & f.mask) != 0; }
// Writes a bool in the target process, touching only the bits that belong to it.
bool WriteBool(const game::Process& proc, uintptr_t object, const Field& f, bool value);

// Highest byte (exclusive) any of the given fields touches: how much of an object to read per refresh.
inline uint32_t Extent(std::initializer_list<Field> fields, uint32_t elemSize = 8)
{
    uint32_t hi = 0;
    for (const Field& f : fields)
        if (f && f.offset + elemSize > hi) hi = f.offset + elemSize;
    return hi;
}

//-----------------------------------------------------------------------------
// FName -> string
//-----------------------------------------------------------------------------
class NamePool
{
public:
    explicit NamePool(const game::Process& proc) : proc_(proc) {}
    void setPool(uintptr_t pool);
    bool validate();                       // block 0 starts with "None", "ByteProperty"
    const std::string& get(uint32_t comparisonIndex); // cached; "?" if unreadable
    size_t cached() const { return cache_.size(); }

private:
    const game::Process& proc_;
    uintptr_t pool_ = 0;
    std::unordered_map<uint32_t, std::string> cache_;
};

//-----------------------------------------------------------------------------
// UObject / UClass / FProperty access
//-----------------------------------------------------------------------------
class Reflection
{
public:
    Reflection(const game::Process& proc, game::OffsetCache& cache, NamePool& names)
        : proc_(proc), cache_(cache), names_(names) {}

    uintptr_t   classOf(uintptr_t object) const { return proc_.read<uintptr_t>(object + layout::UObjectClass); }
    uintptr_t   superOf(uintptr_t ustruct) const { return proc_.read<uintptr_t>(ustruct + layout::UStructSuper); }
    std::string objectName(uintptr_t object);      // "Name" or "Name_N"
    const std::string& className(uintptr_t cls);   // cached per class pointer
    bool        derivesFrom(uintptr_t cls, std::string_view name);

    // Resolves "Prop" or a dotted path through struct members ("CameraCachePrivate.POV.Location").
    // If `type` is given ("IntProperty", "BoolProperty", ...) the property must be of that type, otherwise
    // it counts as not found: a changed Blueprint variable type fails safe instead of being misread.
    // Results are cached in memory and on disk, keyed by the queried class name.
    Field field(uintptr_t cls, std::string_view path, std::string_view type = {});

    // Engine members that are not UPROPERTYs; found by inspecting live memory, then cached like fields.
    uint32_t levelActorsOffset(uintptr_t level);          // ULevel::Actors (TArray<AActor*>)
    uint32_t componentToWorldOffset(uintptr_t sampleLevel, uint32_t levelActors); // USceneComponent::ComponentToWorld

    // Sanity checks of the assumed engine layout against the live world object.
    bool selfTest(uintptr_t world, std::string& why);

    void clearPointerCaches() { classNames_.clear(); }

private:
    uintptr_t findProperty(uintptr_t ustruct, std::string_view name);
    Field     resolve(uintptr_t cls, std::string_view path, std::string_view type);
    bool      isActor(uintptr_t object);

    const game::Process& proc_;
    game::OffsetCache&   cache_;
    NamePool&            names_;
    std::unordered_map<uintptr_t, std::string> classNames_;
    std::unordered_map<std::string, Field>     fields_;
};

//-----------------------------------------------------------------------------
// Engine globals
//-----------------------------------------------------------------------------
struct Globals
{
    uintptr_t namePool = 0;  // &FNamePool
    uintptr_t worldSlot = 0; // &GWorld (UWorld*)
};
// Finds FNamePool and GWorld, by cached RVA first and by code signature otherwise.
bool ResolveGlobals(const game::Process& proc, game::OffsetCache& cache, NamePool& names, Globals& out, std::string& why);
}
