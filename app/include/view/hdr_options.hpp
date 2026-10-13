// HDR video on the PS4 (mpv 0.36): what a file gets. Header-only and free of borealis and mpv,
// so tests/test_hdr_options.cpp builds it alone.
//
// Why: the PS4 build has no runtime shader compiler (SDL loads libSceShaccVSH only when the hint
// SDL_PS4_PIGLET_MODULES_PATH is set, and GMCA never sets it). Every shader is a precompiled
// binary: nanovg's through glShaderBinary, mpv's through a table keyed by the SHA-256 of the
// shader text (scripts/ps4/libmpv/mpv.patch, ps4_shaders_list). mpv's plain video shader is in the
// table; an HDR file makes mpv generate a different text (linearize, tone map, matrix), which is
// not, so the compile from source fails (empty log, nothing drawn). The fix is to give mpv nothing
// to generate: the filter `format=gamma=bt.1886:primaries=bt.709:sig-peak=1` re-tags the frames as
// SDR (mpv 0.36 then skips color mapping: no linearize, no tone mapping, no matrix), and the
// shader text is the SDR one. The picture is washed out (PQ code values shown as SDR gamma,
// BT.2020 colors as BT.709), so saturation and contrast are raised a little for HDR files: they only
// feed the colormatrix uniform of that shader, so the text stays the same. (The video equalizer's
// brightness is the same kind; its gamma adds a pow line, another text, and is not used.)
//
// The filter is in the chain from the start of mpv, for every file, not added when a file turns out
// to be HDR: mpv reconfigures the video output only when a frame's tags differ from the output's,
// and waits for the frame on screen first; a filter added after the first frame was drawn with
// HDR tags (the video-params event reaches the app after it) was not picked up (checked on the
// desktop build). For SDR files in the usual BT.709 tags it changes nothing: mpv's output
// colorspace follows the source. Wide-gamut SDR (BT.2020 or P3 primaries) is no longer converted
// either: on the PS4 that is better (the conversion needed a shader the console cannot make), on a
// desktop diag build slightly less accurate.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hdr {

using Option = std::pair<std::string, std::string>;

/// The label of the video filter that re-tags HDR frames as SDR: a label, so it can be put back
/// on its own after the mirror setting replaced the filter list.
inline const char* filterLabel() { return "@hdr"; }

/// The re-tag filter, with the label: the value of the `vf` option and the argument of `vf add`.
inline std::string retagFilter() {
    return std::string(filterLabel()) + ":format=gamma=bt.1886:primaries=bt.709:sig-peak=1";
}

/// The "HEVC test" choices of a diag build (the selector's index). 0 is what a release build does
/// on its own.
enum Test {
    Automatic = 0,    // re-tag, with the automatic compensation
    MpvDefaults = 1,  // no handling at all (the control: expected to draw nothing for HDR)
    RetagOnly = 2,    // re-tag, no compensation
    RetagLow = 3,
    RetagHigh = 4,
    FourKTest = 5,
    testCount = 6,
};

inline const std::vector<std::string>& testLabels() {
    static const std::vector<std::string> labels = {
        "Automatic (HDR re-tag + saturation 15, contrast 10)",
        "mpv defaults (the control)",
        "HDR re-tag only",
        "HDR re-tag + saturation 8, contrast 5",
        "HDR re-tag + saturation 30, contrast 25",
        "4K test (skip loop filter, fast, framedrop)",
    };
    return labels;
}

/// True for the transfer functions of HDR video, as mpv's video-params/gamma prints them.
inline bool isHdrTransfer(const std::string& gamma) { return gamma == "pq" || gamma == "hlg"; }

/// The value of mpv's `vf` for the mirror setting (0 off, 1 horizontal, 2 vertical) with the
/// re-tag filter first when `withRetag` (the mirror filter, a libavfilter graph, then sees SDR tags): one list, set in one step, so there is never a moment without
/// the re-tag filter (a video output configured with HDR tags freezes the picture).
inline std::string filterChain(int mirror, bool withRetag) {
    const std::string flip = mirror == 1 ? "hflip" : (mirror == 2 ? "vflip" : "");
    if (!withRetag) return flip;
    return flip.empty() ? retagFilter() : retagFilter() + "," + flip;
}

/// True when the choice runs with the re-tag filter in the chain (Automatic, and the three
/// compensation levels). The control (mpv defaults) and the 4K test do not.
inline bool usesRetag(int test) {
    return test == Automatic || test == RetagOnly || test == RetagLow || test == RetagHigh;
}

/// What one HDR file gets: the saturation and contrast added to the viewer's own settings
/// (mpv's -100 .. 100 scale).
struct Plan {
    bool retag = false;  // the choice re-tags and the file is HDR
    int saturation = 0;
    int contrast = 0;
};

/// The plan for a file whose transfer is `gamma` under the diag choice `test` (Automatic in a
/// release build). Nothing for a file that is not PQ: HLG is close to SDR-compatible, and +15/+10
/// could over-boost it (it still gets the re-tag filter, which every file has).
///
/// The automatic amounts: PQ puts reference white (203 nits) at a code value near 0.58 and
/// BT.2020 colors are shown on BT.709 primaries, so shown as SDR the picture is flat and
/// desaturated. Contrast +10 lifts the whole range by a tenth, saturation +15 gives back part of the
/// lost chroma; both are moderate on purpose, as clipped highlights and neon colors are worse than
/// flat ones. The console run compares the low and high choices.
inline Plan planFor(int test, const std::string& gamma) {
    if (gamma != "pq") return {};
    switch (test) {
    case Automatic: return {true, 15, 10};
    case RetagOnly: return {true, 0, 0};
    case RetagLow: return {true, 8, 5};
    case RetagHigh: return {true, 30, 25};
    default: return {};
    }
}

/// The options set once, when mpv starts.
inline std::vector<Option> startOptions(int test) {
    if (test == FourKTest)
        return {{"vd-lavc-skiploopfilter", "all"}, {"vd-lavc-fast", "yes"}, {"framedrop", "decoder+vo"}};
    return {};
}

/// A value kept inside mpv's -100 .. 100 range.
inline int clampEqualizer(int value) { return value < -100 ? -100 : (value > 100 ? 100 : value); }

/// "re-tag, saturation +15, contrast +10" for the diag log; "none" for no handling.
inline std::string describe(const Plan& plan) {
    if (!plan.retag) return "none";
    return "re-tag, saturation +" + std::to_string(plan.saturation) + ", contrast +" + std::to_string(plan.contrast);
}

/// "name=value name=value" for the diag log; "none" for no options.
inline std::string describe(const std::vector<Option>& options) {
    if (options.empty()) return "none";
    std::string out;
    for (const auto& o : options) out += (out.empty() ? "" : " ") + o.first + "=" + o.second;
    return out;
}

}  // namespace hdr
