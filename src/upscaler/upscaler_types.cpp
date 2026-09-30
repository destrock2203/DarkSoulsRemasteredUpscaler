#include "upscaler/upscaler_types.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace dsru {

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

constexpr QualityMode kDlssModes[] = {QualityMode::Native, QualityMode::Quality, QualityMode::Balanced,
                                      QualityMode::Performance, QualityMode::UltraPerformance};
constexpr QualityMode kFsrModes[] = {QualityMode::Native, QualityMode::Quality, QualityMode::Balanced,
                                     QualityMode::Performance, QualityMode::UltraPerformance};
constexpr QualityMode kXessModes[] = {QualityMode::Native,      QualityMode::UltraQuality,
                                      QualityMode::Quality,     QualityMode::Balanced,
                                      QualityMode::Performance, QualityMode::UltraPerformance};

// sl::DLSSPreset. A-D were removed from DLSS 310+, G-I / N-O revert to the default.
constexpr DlssPresetInfo kDlssPresets[] = {
    {0, "Default", "NVIDIA's choice per mode (K for DLAA / Quality / Balanced, M Performance, L Ultra Performance)"},
    {11, "K", "Transformer (default for DLAA, Quality, Balanced)"},
    {13, "M", "Newer transformer: most of L's gains at close to K's cost (default for Performance)"},
    {12, "L", "Newer transformer: sharper, more stable, less ghosting than K; the slowest (default for Ultra Performance)"},
    {10, "J", "Transformer: like K, slightly less ghosting but more flicker"},
    {6, "F", "CNN (deprecated): the old convolutional model, for Ultra Performance / DLAA"},
    {5, "E", "CNN (deprecated): the old convolutional model, the last CNN default"},
};

}  // namespace

const char* ToString(QualityMode mode) {
    switch (mode) {
        case QualityMode::UltraPerformance: return "UltraPerformance";
        case QualityMode::Performance: return "Performance";
        case QualityMode::Balanced: return "Balanced";
        case QualityMode::Quality: return "Quality";
        case QualityMode::UltraQuality: return "UltraQuality";
        case QualityMode::Native: return "Native";
    }
    return "?";
}

QualityMode ParseQualityMode(std::string_view text, QualityMode fallback) {
    for (auto m : {QualityMode::UltraPerformance, QualityMode::Performance, QualityMode::Balanced,
                   QualityMode::Quality, QualityMode::UltraQuality, QualityMode::Native})
        if (IEquals(text, ToString(m))) return m;
    if (IEquals(text, "DLAA") || IEquals(text, "NativeAA") || IEquals(text, "AA")) return QualityMode::Native;
    return fallback;
}

const char* ToString(Backend backend) {
    switch (backend) {
        case Backend::Dlss: return "DLSS";
        case Backend::Fsr: return "FSR";
        case Backend::Xess: return "XeSS";
    }
    return "?";
}

Backend ParseBackend(std::string_view text, Backend fallback) {
    for (Backend b : kBackends)
        if (IEquals(text, ToString(b))) return b;
    return fallback;
}

const char* ToString(SharpenFilter filter) {
    switch (filter) {
        case SharpenFilter::Game: return "Game";
        case SharpenFilter::Amd: return "AMD";
        case SharpenFilter::Nvidia: return "NVIDIA";
    }
    return "?";
}

SharpenFilter ParseSharpenFilter(std::string_view text, SharpenFilter fallback) {
    for (SharpenFilter f : kSharpenFilters)
        if (IEquals(text, ToString(f))) return f;
    return fallback;
}

std::span<const QualityMode> Modes(Backend backend) {
    switch (backend) {
        case Backend::Fsr: return kFsrModes;
        case Backend::Xess: return kXessModes;
        default: return kDlssModes;
    }
}

QualityMode SupportedMode(Backend backend, QualityMode mode) {
    const auto modes = Modes(backend);
    if (std::ranges::find(modes, mode) != modes.end()) return mode;
    return mode == QualityMode::UltraQuality ? QualityMode::Quality : QualityMode::Native;
}

float NominalScale(Backend backend, QualityMode mode) {
    // Upscale factors per dimension: DLSS (NVIDIA programming guide), FSR (fixed by FSR 2+), XeSS legacy.
    float factor = 1.0f;
    switch (backend) {
        case Backend::Dlss:
            switch (mode) {
                case QualityMode::Quality: factor = 1.5f; break;
                case QualityMode::Balanced: factor = 1.0f / 0.58f; break;
                case QualityMode::Performance: factor = 2.0f; break;
                case QualityMode::UltraPerformance: factor = 3.0f; break;
                case QualityMode::UltraQuality: factor = 1.3f; break;
                default: break;
            }
            break;
        case Backend::Fsr:
            switch (mode) {
                case QualityMode::Quality: factor = 1.5f; break;
                case QualityMode::Balanced: factor = 1.7f; break;
                case QualityMode::Performance: factor = 2.0f; break;
                case QualityMode::UltraPerformance: factor = 3.0f; break;
                case QualityMode::UltraQuality: factor = 1.5f; break;
                default: break;
            }
            break;
        case Backend::Xess:
            switch (mode) {
                // Legacy factors (xessForceLegacyScaleFactors, see xess.cpp).
                case QualityMode::UltraQuality: factor = 1.3f; break;
                case QualityMode::Quality: factor = 1.5f; break;
                case QualityMode::Balanced: factor = 1.7f; break;
                case QualityMode::Performance: factor = 2.0f; break;
                case QualityMode::UltraPerformance: factor = 3.0f; break;
                default: break;
            }
            break;
    }
    return 1.0f / factor;
}

QualityMode ModeForRatio(Backend backend, float ratio) {
    if (ratio >= 0.99f) return QualityMode::Native;
    const auto modes = Modes(backend);
    if (backend == Backend::Xess) {
        // Best quality first: the first (non-native) mode whose smallest input still fits.
        for (QualityMode m : modes)
            if (m != QualityMode::Native && ratio >= NominalScale(backend, m) - 1e-4f) return m;
        return QualityMode::UltraPerformance;
    }
    QualityMode best = QualityMode::Quality;
    float bestDistance = 1e9f;
    for (QualityMode m : modes) {
        if (m == QualityMode::Native) continue;
        const float distance = std::fabs(NominalScale(backend, m) - ratio);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = m;
        }
    }
    return best;
}

const char* ModeName(Backend backend, QualityMode mode) {
    if (mode == QualityMode::Native) {
        switch (backend) {
            case Backend::Dlss: return "DLAA";
            case Backend::Fsr: return "Native AA";
            case Backend::Xess: return "Native AA";
        }
    }
    switch (mode) {
        case QualityMode::UltraPerformance: return "Ultra Performance";
        case QualityMode::Performance: return "Performance";
        case QualityMode::Balanced: return "Balanced";
        case QualityMode::Quality: return "Quality";
        case QualityMode::UltraQuality: return "Ultra Quality";
        default: return "?";
    }
}

std::span<const DlssPresetInfo> DlssPresets() { return kDlssPresets; }

const char* DlssPresetName(uint32_t value) {
    for (const DlssPresetInfo& p : kDlssPresets)
        if (p.value == value) return p.name;
    return "Default";
}

uint32_t ParseDlssPreset(std::string_view text, uint32_t fallback) {
    if (text.empty()) return fallback;
    for (const DlssPresetInfo& p : kDlssPresets)
        if (IEquals(text, p.name)) return p.value;
    return fallback;
}

}  // namespace dsru
