#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Names of the game's shaders. Dark Souls Remastered creates every shader from the DXBC files of its
// shader archives (shader/*_DX11.shaderbnd.dcx), unchanged: a shader is identified by the first 8
// bytes of its DXBC checksum and named from a table generated from those archives
// (tools/gen_shader_table.py -> shader_table.inc, shader_ids.h). Identical bytecode stored under
// several names shares one id; its names are joined with '|'.
namespace dsru::game::shaders {

enum class Stage : uint8_t { kVertex, kPixel, kCompute, kOther };

// Archive the shader comes from, and pass hints from its name.
enum Flags : uint32_t {
    kFilter = 1u << 0,     // FRPG_Filter: post-processing, TAA, depth copies, SSAO...
    kMenu = 1u << 1,       // FRPG_Menu: HUD and menus
    kDeferred = 1u << 2,   // FRPG_Deferred: light probes, point lights
    kSfx = 1u << 3,        // FRPG_SfxPBL: particles / effects
    kModel = 1u << 4,      // FRPG_FlverPBL: characters, maps, objects
    kDebug = 1u << 5,      // DbgFont
    kOther = 1u << 6,
    kVelocity = 1u << 8,   // *_Vel / *_VelAlp: the object velocity pass
    kDepthOnly = 1u << 9,  // *_Dep / *_DepAlp: depth-only passes
    kShadow = 1u << 10,    // *_Sdw: shadow maps
};

struct Info {
    uint64_t id;
    Stage stage;
    uint32_t flags;
    const char* names;
};

// Id of a shader from its bytecode (DXBC checksum bytes 4..11; FNV-1a with the top bit set for
// anything that isn't DXBC, so it never matches the table).
uint64_t IdOf(const void* bytecode, size_t size);

// Table entry for `id`, or null for a shader that isn't one of the game's (ReShade, overlays...).
const Info* Find(uint64_t id);

// The first name of an entry ("FRPG_Fil_ResolveTAA"), "?" for null.
std::string_view FirstName(const Info* info);

// Number of table entries (for the probe's report).
size_t Count();

}  // namespace dsru::game::shaders
