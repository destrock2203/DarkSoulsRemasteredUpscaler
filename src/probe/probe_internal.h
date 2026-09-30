#pragma once

// Between the probe's D3D11 hooks (probe_d3d11.cpp) and its trace writer (probe_trace.cpp).

#include "game/shader_db.h"

#include <d3d11.h>
#include <d3dcommon.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dsru::probe::detail {

struct ResourceInfo {
    uint32_t id = 0;  // R<id> in traces, 0 = unknown
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    UINT width = 0, height = 0, arraySize = 0, mips = 0, samples = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0, misc = 0, usage = 0, cpuAccess = 0;
    UINT byteWidth = 0;  // buffers
    uint64_t createdAtPresent = 0;
    std::string caller;  // code address that created it
    bool backBuffer = false;
};

// A constant-buffer variable of a shader's reflection (used ones only).
struct ReflectedVariable {
    std::string name;
    uint32_t buffer = 0;  // cbuffer bind slot
    uint32_t offset = 0, size = 0;
    uint8_t cls = 0;      // D3D_SHADER_VARIABLE_CLASS
    uint8_t type = 0;     // D3D_SHADER_VARIABLE_TYPE
    uint16_t rows = 0, columns = 0, elements = 0;
};

struct ReflectedBinding {
    std::string name;
    D3D_SHADER_INPUT_TYPE type{};
    UINT slot = 0;
    UINT count = 1;
};

struct ShaderRecord {
    uint32_t index = 0;  // S<index>
    uint64_t id = 0;
    game::shaders::Stage stage = game::shaders::Stage::kOther;
    const game::shaders::Info* info = nullptr;  // null: not one of the game's shaders
    std::string name;                           // first name, or "unknown_<id>"
    std::vector<ReflectedVariable> variables;
    std::vector<ReflectedBinding> bindings;
    uint32_t flags() const { return info ? info->flags : 0u; }
};

using Snapshot = std::shared_ptr<const std::vector<uint8_t>>;

enum class EventType : uint8_t { Draw, Dispatch, ClearRT, ClearDS, Copy, CopyRegion, Resolve, Execute, Note };

struct BoundResource {
    uint8_t slot = 0;
    uint32_t resource = 0;  // R id
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
};

struct Event {
    EventType type = EventType::Draw;
    uint8_t context = 0;  // 0 = immediate
    bool indexed = false;
    uint32_t count = 0, instances = 0;  // vertices / indices; dispatch x in count, y z below
    uint32_t groupsY = 0, groupsZ = 0;
    const ShaderRecord* vs = nullptr;
    const ShaderRecord* ps = nullptr;
    const ShaderRecord* cs = nullptr;
    uint32_t rt[8]{};
    DXGI_FORMAT rtFormat[8]{};
    uint8_t rtCount = 0;
    uint32_t ds = 0;
    DXGI_FORMAT dsFormat = DXGI_FORMAT_UNKNOWN;
    std::vector<BoundResource> srvs;
    std::vector<BoundResource> uavs;
    D3D11_VIEWPORT viewport{};
    Snapshot vsCb[2], psCb[2], csCb[2];
    uint32_t vsCbId[2]{}, psCbId[2]{}, csCbId[2]{};  // R ids of the bound constant buffers
    uint32_t src = 0, dst = 0;  // copies, clears (dst)
    float clear[4]{};
    std::string text;              // Execute: command list, Note: the note
    std::vector<Event> children;   // Execute: the command list's events
};

struct FrameRecord {
    uint64_t present = 0;
    std::vector<Event> events;
};

struct TraceData {
    std::vector<FrameRecord> frames;
    std::unordered_map<uint32_t, ResourceInfo> resources;  // by id, those the frames refer to
    uint32_t backBuffer = 0;
    std::filesystem::path file;
};

// Formats and writes a trace (worker thread).
void WriteTrace(const TraceData& data);

}  // namespace dsru::probe::detail
