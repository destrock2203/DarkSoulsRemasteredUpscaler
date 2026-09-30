// DSRProbe: formats a recorded frame trace (probe_d3d11.cpp) into DSRProbe_trace_<present>.txt.

#include "game/shader_ids.h"
#include "probe/probe.h"
#include "probe/probe_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace dsru::probe::detail {

namespace {

namespace shaders = game::shaders;

struct Writer {
    const TraceData& data;
    std::ostringstream out;
    std::set<uint32_t> referenced;

    // Analysis, filled while formatting.
    struct Found {
        uint64_t present = 0;
        std::string where;
    };
    std::vector<std::string> taaReports, velocityReports, hudReports, fxaaReports, tonemapReports;
    std::map<std::string, int> viewportSizes;  // model draws
    std::map<uint32_t, int> depthTargets;      // model draws
    std::vector<std::string> jitter;

    explicit Writer(const TraceData& d) : data(d) {}

    std::string Res(uint32_t id, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN) {
        if (!id) return "-";
        referenced.insert(id);
        const auto it = data.resources.find(id);
        if (it == data.resources.end()) return std::format("R{}", id);
        const ResourceInfo& r = it->second;
        std::string s;
        if (r.dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
            s = std::format("R{}(buffer {} B)", id, r.byteWidth);
        else
            s = std::format("R{}({} {}x{}{}{})", id,
                            viewFormat != DXGI_FORMAT_UNKNOWN && viewFormat != r.format
                                ? std::format("{} as {}", FormatName(r.format), FormatName(viewFormat))
                                : std::string(FormatName(r.format)),
                            r.width, r.height, r.samples > 1 ? std::format(" x{}", r.samples) : "",
                            r.backBuffer || id == data.backBuffer ? " BACKBUFFER" : "");
        return s;
    }

    static std::string ShaderName(const ShaderRecord* s) { return s ? s->name : "-"; }

    // Float4 register `reg` of a constant buffer snapshot.
    static bool Register(const Snapshot& cb, uint32_t reg, float out[4]) {
        if (!cb || (reg + 1) * 16 > cb->size()) return false;
        std::memcpy(out, cb->data() + reg * 16, 16);
        return true;
    }

    static std::string Floats(const float* v, int n) {
        std::string s = "(";
        for (int i = 0; i < n; ++i) s += std::format("{}{:.6g}", i ? ", " : "", v[i]);
        return s + ")";
    }

    // The used constants of `shader` from its constant buffer snapshots (slots 0 and 1).
    void Constants(const ShaderRecord* shader, const Snapshot* cbs, const char* stage, int maxRegisters = 64) {
        if (!shader) return;
        for (const ReflectedVariable& v : shader->variables) {
            if (v.buffer > 1) continue;
            const Snapshot& cb = cbs[v.buffer];
            if (!cb) {
                out << std::format("        {} b{} {}: (no data captured)\n", stage, v.buffer, v.name);
                continue;
            }
            const uint32_t firstReg = v.offset / 16;
            const uint32_t regs = std::max<uint32_t>(1, (v.size + 15) / 16);
            std::string values;
            const bool integer = v.type == D3D_SVT_INT || v.type == D3D_SVT_UINT || v.type == D3D_SVT_BOOL;
            const uint32_t shown = std::min<uint32_t>(regs, static_cast<uint32_t>(maxRegisters));
            for (uint32_t r = 0; r < shown; ++r) {
                const uint32_t offset = v.offset + r * 16;
                const uint32_t bytes = std::min<uint32_t>(16, v.size > r * 16 ? v.size - r * 16 : 0);
                const int n = static_cast<int>(std::max<uint32_t>(1, bytes / 4));
                if (offset + n * 4 > cb->size()) break;
                if (integer) {
                    int32_t ints[4]{};
                    std::memcpy(ints, cb->data() + offset, n * 4);
                    values += "(";
                    for (int i = 0; i < n; ++i) values += std::format("{}{}", i ? ", " : "", ints[i]);
                    values += ")";
                } else {
                    float f[4]{};
                    std::memcpy(f, cb->data() + offset, n * 4);
                    values += Floats(f, n);
                }
                if (r + 1 < shown) values += " ";
            }
            if (shown < regs) values += std::format(" ... ({} registers)", regs);
            out << std::format("        {} b{} c{:<3} {:<32} = {}\n", stage, v.buffer, firstReg, v.name, values);
        }
    }

    void Srvs(const Event& e, const ShaderRecord* shader) {
        for (const BoundResource& b : e.srvs) {
            std::string name;
            if (shader)
                for (const ReflectedBinding& rb : shader->bindings)
                    if (rb.type == D3D_SIT_TEXTURE && rb.slot == b.slot) name = " " + rb.name;
            out << std::format("        t{}{} = {}\n", b.slot, name, Res(b.resource, b.viewFormat));
        }
        for (const BoundResource& b : e.uavs) out << std::format("        u{} = {}\n", b.slot, Res(b.resource, b.viewFormat));
    }

    std::string Targets(const Event& e) {
        std::string s;
        for (uint8_t i = 0; i < e.rtCount; ++i)
            if (e.rt[i]) s += std::format("rt{}={} ", i, Res(e.rt[i], e.rtFormat[i]));
        s += "ds=" + Res(e.ds, e.dsFormat);
        s += std::format(" vp=({:.0f},{:.0f} {:.0f}x{:.0f} z {:.2f}-{:.2f})", e.viewport.TopLeftX, e.viewport.TopLeftY,
                         e.viewport.Width, e.viewport.Height, e.viewport.MinDepth, e.viewport.MaxDepth);
        return s;
    }

    static bool SameTargets(const Event& a, const Event& b) {
        if (a.rtCount != b.rtCount || a.ds != b.ds) return false;
        for (uint8_t i = 0; i < a.rtCount; ++i)
            if (a.rt[i] != b.rt[i]) return false;
        return a.viewport.Width == b.viewport.Width && a.viewport.Height == b.viewport.Height;
    }

    // Jitter from a velocity draw: gVC_WorldViewClipMtx (c0-c3, jittered) vs gVC_CommonREG12
    // (c12-c15, this frame's unjittered view-projection).
    void Jitter(const Event& e, uint64_t present) {
        float j[4][4], u[4][4];
        for (int r = 0; r < 4; ++r)
            if (!Register(e.vsCb[0], r, j[r]) || !Register(e.vsCb[0], 12 + r, u[r])) return;
        int k = 0;
        for (int i = 1; i < 4; ++i)
            if (std::fabs(u[3][i]) > std::fabs(u[3][k])) k = i;
        if (std::fabs(u[3][k]) < 1e-6f) return;
        const float jx = (j[0][k] - u[0][k]) / u[3][k];
        const float jy = (j[1][k] - u[1][k]) / u[3][k];
        const float w = e.viewport.Width, h = e.viewport.Height;
        jitter.push_back(std::format("present {}: NDC ({:+.6f}, {:+.6f}) = pixels ({:+.4f}, {:+.4f}) at {:.0f}x{:.0f}",
                                     present, jx, jy, jx * w * 0.5f, -jy * h * 0.5f, w, h));
    }

    void TaaReport(const Event& e, uint64_t present) {
        std::string s = std::format("present {}: {}\n", present, Targets(e));
        for (const BoundResource& b : e.srvs) s += std::format("      t{} = {}\n", b.slot, Res(b.resource, b.viewFormat));
        // Registers the resolve reads (FRPG_Fil_ResolveTAA.fpo): cb0[8] depth linearization, cb0[12]
        // screen size, cb0[56-57] weights, cb0[58-61] clip -> world, cb0[74-77] world -> previous clip,
        // cb0[78] camera offset between frames.
        static constexpr std::pair<uint32_t, const char*> kRegs[] = {
            {8, "depth params (y, z, w: linear = w / (z * depth + y))"}, {12, "screen size (w, h, 1/w, 1/h)"},
            {56, "velocity weight"}, {57, "motion scale"}, {58, "clip->world row 0"}, {59, "clip->world row 1"},
            {60, "clip->world row 2"}, {61, "clip->world row 3"}, {74, "world->prev clip row 0"},
            {75, "world->prev clip row 1"}, {76, "world->prev clip row 2"}, {77, "world->prev clip row 3"},
            {78, "camera offset (prev - cur)"}};
        for (const auto& [reg, what] : kRegs) {
            float v[4];
            if (Register(e.psCb[0], reg, v)) s += std::format("      cb0[{}] {} = {}\n", reg, what, Floats(v, 4));
        }
        taaReports.push_back(std::move(s));
    }

    void FormatEvents(const std::vector<Event>& events, uint64_t present, int depth, int& counter,
                      const Event*& lastTargets, bool& hudSeen) {
        const std::string indent(depth * 2, ' ');
        // Draws not printed in full, summarized at the end of their pass.
        int skipped = 0;
        std::map<std::string, int> skippedShaders;
        int passDraws = 0;
        auto flush = [&] {
            if (!skipped) return;
            std::vector<std::pair<int, std::string>> top;
            for (const auto& [name, count] : skippedShaders) top.emplace_back(count, name);
            std::sort(top.rbegin(), top.rend());
            std::string names;
            for (size_t i = 0; i < top.size() && i < 8; ++i)
                names += std::format("{}{} x{}", i ? ", " : "", top[i].second, top[i].first);
            if (top.size() > 8) names += std::format(", ... ({} shaders)", top.size());
            out << std::format("{}    ... +{} more draws: {}\n", indent, skipped, names);
            skipped = 0;
            skippedShaders.clear();
        };

        for (const Event& e : events) {
            switch (e.type) {
                case EventType::Execute:
                    flush();
                    out << std::format("{}== ExecuteCommandList: {} ==\n", indent, e.text);
                    lastTargets = nullptr;
                    FormatEvents(e.children, present, depth + 1, counter, lastTargets, hudSeen);
                    out << std::format("{}== end of command list ==\n", indent);
                    lastTargets = nullptr;
                    break;
                case EventType::Note:
                    flush();
                    out << std::format("{}-- {}\n", indent, e.text);
                    break;
                case EventType::ClearRT:
                    flush();
                    out << std::format("{}ClearRenderTargetView {} to {}\n", indent, Res(e.dst), Floats(e.clear, 4));
                    break;
                case EventType::ClearDS:
                    flush();
                    out << std::format("{}ClearDepthStencilView {} depth={:.6g} stencil={:.0f} flags={:.0f}\n", indent,
                                       Res(e.dst), e.clear[0], e.clear[1], e.clear[2]);
                    break;
                case EventType::Copy:
                    flush();
                    out << std::format("{}CopyResource {} <- {}\n", indent, Res(e.dst), Res(e.src));
                    break;
                case EventType::CopyRegion:
                    flush();
                    out << std::format("{}CopySubresourceRegion {} <- {} {}\n", indent, Res(e.dst), Res(e.src), e.text);
                    break;
                case EventType::Resolve:
                    flush();
                    out << std::format("{}ResolveSubresource {} <- {} ({})\n", indent, Res(e.dst), Res(e.src), e.text);
                    break;
                case EventType::Dispatch: {
                    flush();
                    ++counter;
                    out << std::format("{}#{:05} Dispatch({}, {}, {}) CS={}\n", indent, counter, e.count, e.groupsY,
                                       e.groupsZ, ShaderName(e.cs));
                    Srvs(e, e.cs);
                    if (e.csCbId[0]) out << std::format("        cb: CS b0={} b1={}\n", Res(e.csCbId[0]), Res(e.csCbId[1]));
                    if (e.cs && (e.cs->flags() & shaders::kFilter)) Constants(e.cs, e.csCb, "CS");
                    break;
                }
                case EventType::Draw: {
                    ++counter;
                    if (!lastTargets || !SameTargets(*lastTargets, e)) {
                        flush();
                        out << std::format("{}-- pass: {}\n", indent, Targets(e));
                        lastTargets = &e;
                        passDraws = 0;
                    }
                    ++passDraws;
                    const uint32_t psFlags = e.ps ? e.ps->flags() : 0;
                    const uint32_t vsFlags = e.vs ? e.vs->flags() : 0;
                    const bool velocity = (psFlags | vsFlags) & shaders::kVelocity;
                    const bool filter = psFlags & (shaders::kFilter | shaders::kDeferred);
                    const bool menu = psFlags & shaders::kMenu;
                    const bool unknown = (e.ps && !e.ps->info) || (e.vs && !e.vs->info);
                    const bool model = psFlags & shaders::kModel;

                    if (model && e.viewport.Width > 0) {
                        ++viewportSizes[std::format("{:.0f}x{:.0f}", e.viewport.Width, e.viewport.Height)];
                        if (e.ds) ++depthTargets[e.ds];
                    }
                    if (velocity && jitter.size() < 16 && (passDraws == 1)) Jitter(e, present);
                    if (e.ps && e.ps->id == game::shader_id::kPsResolveTaa) TaaReport(e, present);
                    if (e.ps && e.ps->id == game::shader_id::kPsFxaa)
                        fxaaReports.push_back(std::format("present {}: {} input {}", present, Targets(e),
                                                          e.srvs.empty() ? "-" : Res(e.srvs[0].resource)));
                    if (e.ps && (e.ps->id == game::shader_id::kPsHdr || e.ps->id == game::shader_id::kPsHdrPbl ||
                                 e.ps->id == game::shader_id::kPsHdrColAdj || e.ps->id == game::shader_id::kPsHdrPblColAdj))
                        tonemapReports.push_back(std::format("present {}: {} ({}) input {}", present, Targets(e),
                                                             e.ps->name, e.srvs.empty() ? "-" : Res(e.srvs[0].resource)));
                    if (menu && !hudSeen) {
                        hudSeen = true;
                        hudReports.push_back(std::format("present {}: first HUD/menu draw #{} {} VS={} PS={}", present,
                                                         counter, Targets(e), ShaderName(e.vs), ShaderName(e.ps)));
                    }
                    if (velocity && velocityReports.size() < 4 && passDraws == 1)
                        velocityReports.push_back(std::format("present {}: {}", present, Targets(e)));

                    const bool full = filter || unknown || (menu && passDraws <= 6) || (velocity && passDraws <= 2) ||
                                      passDraws <= 2 || !e.ps;
                    if (!full) {
                        ++skipped;
                        ++skippedShaders[e.ps ? e.ps->name : "-"];
                        break;
                    }
                    out << std::format("{}#{:05} {}{}({}{}) VS={} PS={}\n", indent, counter, e.indexed ? "DrawIndexed" : "Draw",
                                       e.instances > 1 ? "Instanced" : "", e.count,
                                       e.instances > 1 ? std::format(", {} instances", e.instances) : "",
                                       ShaderName(e.vs), ShaderName(e.ps));
                    Srvs(e, e.ps);
                    if (e.vsCbId[0] || e.psCbId[0] || e.vsCbId[1] || e.psCbId[1])
                        out << std::format("        cb: VS b0={} b1={}  PS b0={} b1={}\n", Res(e.vsCbId[0]),
                                           Res(e.vsCbId[1]), Res(e.psCbId[0]), Res(e.psCbId[1]));
                    if (filter || menu || unknown) Constants(e.ps, e.psCb, "PS");
                    if (filter || velocity || passDraws == 1) Constants(e.vs, e.vsCb, "VS", velocity || filter ? 16 : 5);
                    break;
                }
            }
        }
        flush();
    }

    void Write() {
        out << "DSRProbe frame trace\n====================\n";
        if (data.frames.empty()) {
            out << "No frames recorded.\n";
            return;
        }
        out << std::format("Presents {} - {} ({} frames). Back buffer: {}.\n", data.frames.front().present,
                           data.frames.back().present, data.frames.size(), Res(data.backBuffer));
        out << "The first traced frame may lack command lists that worker threads started recording before the\n"
               "trace was armed; the last one is complete. Draws are grouped in passes (same render targets);\n"
               "filter / HUD / velocity passes are printed in full with their shader constants, the bulk of\n"
               "model draws is summarized. R<n> = resource (table at the end), shaders by their file name.\n\n";

        int counter = 0;
        for (const FrameRecord& frame : data.frames) {
            out << std::format("\n================ FRAME (present {}) ================\n", frame.present);
            const Event* lastTargets = nullptr;
            bool hudSeen = false;
            counter = 0;
            FormatEvents(frame.events, frame.present, 0, counter, lastTargets, hudSeen);
        }

        out << "\n\n================ ANALYSIS ================\n";
        auto list = [&](const char* title, const std::vector<std::string>& lines) {
            out << "\n" << title << ":\n";
            if (lines.empty()) out << "  (none)\n";
            for (const std::string& l : lines) out << "  " << l << (l.ends_with('\n') ? "" : "\n");
        };
        list("TAA resolve (FRPG_Fil_ResolveTAA: t0 color, t1 history, t2 depth, t3 velocity, t4 ?)", taaReports);
        list("FXAA", fxaaReports);
        list("Tone mapping (FRPG_Fil_HDR*)", tonemapReports);
        list("Velocity pass (first draws)", velocityReports);
        list("Jitter (from velocity draws: jittered vs unjittered view-projection)", jitter);
        list("HUD start", hudReports);
        out << "\nViewport sizes of model draws:\n";
        for (const auto& [size, count] : viewportSizes) out << std::format("  {} x{}\n", size, count);
        out << "\nDepth targets of model draws:\n";
        for (const auto& [id, count] : depthTargets) out << std::format("  {} x{}\n", Res(id), count);

        out << "\n\n================ RESOURCES ================\n";
        for (uint32_t id : referenced) {
            const auto it = data.resources.find(id);
            if (it == data.resources.end()) continue;
            const ResourceInfo& r = it->second;
            if (r.dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
                out << std::format("R{:<6} buffer {} B bind={} usage={} (created at present {} from {})\n", id,
                                   r.byteWidth, BindFlagsName(r.bind), r.usage, r.createdAtPresent, r.caller);
            else
                out << std::format("R{:<6} {}x{} {} mips={} array={} samples={} bind={} misc=0x{:x}{} (created at present "
                                   "{} from {})\n",
                                   id, r.width, r.height, FormatName(r.format), r.mips, r.arraySize, r.samples,
                                   BindFlagsName(r.bind), r.misc, r.backBuffer ? " BACKBUFFER" : "", r.createdAtPresent,
                                   r.caller);
        }
    }
};

}  // namespace

void WriteTrace(const TraceData& data) {
    Writer writer(data);
    writer.Write();
    std::ofstream file(data.file, std::ios::binary);
    const std::string text = writer.out.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace dsru::probe::detail
