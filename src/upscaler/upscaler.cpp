#include "upscaler/upscaler.h"

#include "core/log.h"
#include "upscaler/fsr/fsr.h"
#include "upscaler/streamline/streamline.h"
#include "upscaler/xess/xess.h"

#include <algorithm>
#include <atomic>

namespace dsru::upscaler {

namespace {

std::atomic<bool> g_fsrProbed{false}, g_xessProbed{false};

// FSR and XeSS read their inputs from compute shaders and don't transition them: the inputs go to
// PIXEL | NON_PIXEL_SHADER_RESOURCE and the output to UNORDERED_ACCESS for the evaluation, in one
// barrier batch, and back to the caller's states afterwards.
constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct StateScope {
    ID3D12GraphicsCommandList* list;
    D3D12_RESOURCE_BARRIER before[4]{}, after[4]{};
    UINT count = 0;

    StateScope(UpscaleInputs& in) : list(in.list) {
        Want(in.color, in.colorState, kShaderRead);
        Want(in.depth, in.depthState, kShaderRead);
        Want(in.motionVectors, in.motionVectorsState, kShaderRead);
        Want(in.output, in.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (count) list->ResourceBarrier(count, before);
    }
    ~StateScope() {
        if (count) list->ResourceBarrier(count, after);
    }

    void Want(ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES wanted) {
        if (state != wanted) {
            before[count] = Transition(r, state, wanted);
            after[count++] = Transition(r, wanted, state);
        }
        state = wanted;
    }
    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                             D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }
};

}  // namespace

void OnDeviceCreated(ID3D12Device* device) {
    fsr::OnDeviceCreated(device);
    xess::OnDeviceCreated(device);
}

void Prepare(Backend backend) {
    switch (backend) {
        case Backend::Fsr:
            if (!g_fsrProbed.exchange(true)) fsr::Load();
            break;
        case Backend::Xess:
            if (!g_xessProbed.exchange(true)) xess::Load();
            break;
        default: break;
    }
}

bool Available(Backend backend) {
    switch (backend) {
        case Backend::Dlss: return streamline::DlssAvailable();
        case Backend::Fsr: return fsr::Available();
        case Backend::Xess: return xess::Available();
    }
    return false;
}

bool Probed(Backend backend) {
    switch (backend) {
        case Backend::Fsr: return g_fsrProbed.load();
        case Backend::Xess: return g_xessProbed.load();
        default: return true;
    }
}

bool AnyAvailable() { return streamline::DlssAvailable() || fsr::Available() || xess::Available(); }

std::string Description(Backend backend) {
    switch (backend) {
        case Backend::Fsr: {
            const std::string version = fsr::ActiveVersion();
            return version.empty() ? "FSR" : "FSR " + version;
        }
        case Backend::Xess: return xess::Available() ? xess::VersionName() : "XeSS";
        default: return ToString(backend);
    }
}

float RenderScale(Backend backend, Resolution output, QualityMode mode) {
    if (mode == QualityMode::Native || !output.width || !output.height) return 1.0f;
    if (backend == Backend::Dlss) {
        Resolution render{};
        if (streamline::DlssOptimalRenderSize(output, mode, render) && render.width && render.height)
            return std::max(static_cast<float>(render.width) / static_cast<float>(output.width),
                            static_cast<float>(render.height) / static_cast<float>(output.height));
    }
    // XeSS rejects inputs below its mode's range: stay a hair above the nominal ratio so the
    // engine's rounding never lands a pixel short.
    return NominalScale(backend, mode) + (backend == Backend::Xess ? 0.002f : 0.0f);
}

bool Evaluate(Backend backend, const UpscaleInputs& in, uint32_t dlssPreset) {
    if (backend == Backend::Dlss) return streamline::EvaluateDlss(in, dlssPreset);
    UpscaleInputs local = in;
    StateScope states(local);
    return backend == Backend::Fsr ? fsr::Evaluate(local) : xess::Evaluate(local);
}

void OnPresent() {
    fsr::OnPresent();
    xess::OnPresent();
}

}  // namespace dsru::upscaler
