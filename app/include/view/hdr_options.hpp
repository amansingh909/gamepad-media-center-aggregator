// HDR video on the PS4 (mpv 0.36, a GLES 2.0 driver): which mpv options a file gets. Header-only
// and free of borealis and mpv, so tests/test_hdr_options.cpp builds it alone.
//
// Why: mpv maps PQ and HLG to SDR in the fragment shader, and on the console that shader does
// not link (the driver gives an empty log and nothing is drawn). The tone-mapping pass of
// mpv 0.36 always carries `color[sig_idx]`, a vector indexed by a variable, which GLSL ES 1.00
// does not require a fragment shader to support, whatever the curve or mode (the options only
// change the lines around it). That pass exists only while the video's peak is above the
// target's: `src.sig_peak > dst.sig_peak` in pass_color_map(). So the automatic handling sets
// target-peak just above the video's own peak (from video-params/sig-peak): no tone-mapping
// pass at all, the signal is scaled linearly into the SDR range, and gamut-mapping-mode=clip
// drops the out-of-gamut block. The shader is then the PQ linearize, one matrix, a scale and
// the 1/2.2 delinearize. If even that does not link (the log says "shader link log"), the next
// level is passthrough: the output keeps the video's own transfer function and primaries.
#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace hdr {

using Option = std::pair<std::string, std::string>;

/// mpv's reference white in nits (MP_REF_WHITE): target-peak is in nits, video-params/sig-peak
/// is relative to it.
constexpr double refWhiteNits = 203.0;

/// The "HEVC test" choices of a diag build (the selector's index). 0 is what a release build
/// does on its own.
enum Test {
    Automatic = 0,       // HDR handling per file, with the fallback levels
    MpvDefaults = 1,     // no HDR handling at all (what diag H ran)
    ToneMappingClip = 2,
    GamutClip = 3,
    ToneAndGamutClip = 4,
    Passthrough = 5,
    DumbMode = 6,
    FourKTest = 7,
    testCount = 8,
};

inline const std::vector<std::string>& testLabels() {
    static const std::vector<std::string> labels = {
        "Automatic (HDR per file)",
        "mpv defaults (no HDR handling)",
        "tone-mapping=clip",
        "gamut-mapping-mode=clip",
        "tone-mapping=clip + gamut clip",
        "Passthrough (target-trc/prim)",
        "gpu-dumb-mode=yes",
        "4K test (skip loop filter, fast, framedrop)",
    };
    return labels;
}

/// Automatic handling has this many levels; the next one is tried when the shader fails to link.
constexpr int autoLevels = 2;

/// True for the transfer functions of HDR video, as mpv's video-params/gamma prints them.
inline bool isHdrTransfer(const std::string& gamma) { return gamma == "pq" || gamma == "hlg"; }

/// The target-peak that keeps mpv from tone mapping a video whose peak is `sigPeak` (relative to
/// reference white), in nits: one nit above it, so float rounding cannot put the video above the
/// target. An unknown peak (<= 0) means the transfer's own maximum, so the largest value.
inline int targetPeakNits(double sigPeak) {
    if (!(sigPeak > 0)) return 10000;
    const double nits = std::ceil(sigPeak * refWhiteNits) + 1.0;
    if (nits < 10.0) return 10;
    if (nits > 10000.0) return 10000;
    return int(nits);
}

/// The options set once, when mpv starts (a test that is not about one HDR file).
inline std::vector<Option> startOptions(int test) {
    if (test == DumbMode) return {{"gpu-dumb-mode", "yes"}};
    if (test == FourKTest)
        return {{"vd-lavc-skiploopfilter", "all"}, {"vd-lavc-fast", "yes"}, {"framedrop", "decoder+vo"}};
    return {};
}

/// The options for one file whose transfer is `gamma`, with the video's `primaries` and
/// `sigPeak` (video-params/sig-peak; <= 0 when mpv has none). `test` is the diag choice (Automatic
/// in a release build); `level` is the automatic level (0 .. autoLevels - 1). Empty for a file
/// that is not HDR, and for the choices that are not about HDR.
inline std::vector<Option> hdrOptions(int test, int level, const std::string& gamma, const std::string& primaries,
    double sigPeak) {
    if (!isHdrTransfer(gamma)) return {};
    const std::string peak = std::to_string(targetPeakNits(sigPeak));
    const std::string prim = primaries.empty() || primaries == "auto" ? "bt.2020" : primaries;
    const std::vector<Option> passthrough = {{"target-trc", gamma}, {"target-prim", prim}, {"target-peak", peak}};
    switch (test) {
    case Automatic:
        if (level <= 0)
            return {{"target-peak", peak}, {"gamut-mapping-mode", "clip"}, {"tone-mapping", "clip"},
                {"hdr-compute-peak", "no"}};
        return passthrough;
    case ToneMappingClip: return {{"tone-mapping", "clip"}};
    case GamutClip: return {{"gamut-mapping-mode", "clip"}};
    case ToneAndGamutClip: return {{"tone-mapping", "clip"}, {"gamut-mapping-mode", "clip"}};
    case Passthrough: return passthrough;
    default: return {};
    }
}

/// What undoes hdrOptions() for the next file: every option it can set, back to mpv's default.
inline std::vector<Option> resetOptions() {
    return {{"target-peak", "auto"}, {"target-trc", "auto"}, {"target-prim", "auto"},
        {"gamut-mapping-mode", "auto"}, {"tone-mapping", "auto"}, {"hdr-compute-peak", "auto"}};
}

/// "name=value name=value" for the diag log; "none" for no options.
inline std::string describe(const std::vector<Option>& options) {
    if (options.empty()) return "none";
    std::string out;
    for (const auto& o : options) out += (out.empty() ? "" : " ") + o.first + "=" + o.second;
    return out;
}

/// True when mpv's log line says a shader did not link (the PS4 driver's failure, error level).
inline bool isShaderLinkFailure(const std::string& text) { return text.find("shader link log") != std::string::npos; }

}  // namespace hdr
