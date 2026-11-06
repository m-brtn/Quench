#include "Checks.h"

#include <quench/core/Json.h>

#include <SDL3/SDL_time.h>

#include <algorithm>
#include <array>
#include <format>

namespace demo {

using namespace quench;

namespace {

// The devlog's checks waiting for the user (after 0.7.0), as items a person can do in the demo on the Mac or the
// iPhone. The setup lines are scenario commands of the demo (`set`, `camera`, `hdr`).
constexpr std::array kItems{
    CheckItem{"materials.fence_holes", "materials", "Fence holes",
              "The alpha-tested fence on the right has round holes, and its shadow on the ground has the same holes "
              "(not a solid rectangle).",
              "camera -60 22 6"},
    CheckItem{"materials.torus_tint", "materials", "Tinted glass shadow",
              "The glass torus casts a light blue, see-through shadow. 'Frame and fallbacks' says 'Translucent "
              "shadows: transmittance map' and lists no fallback.",
              "camera 10 30 7"},
    CheckItem{"shadows.cascades", "shadows", "Cascades",
              "Turn the camera along the columns: shadows are sharp near and softer far away, without jumps or gaps "
              "where one cascade ends and the next begins.",
              ""},
    CheckItem{"sky.day", "sky", "Day and fog",
              "Run the day: the sky goes from night through sunrise to noon without bands or jumps; the fog brightens "
              "toward the sun.",
              "set animate 1"},
    CheckItem{"post.aa", "post", "Anti-aliasing",
              "Zoom in on the thin poles and the spinning blades and switch none, FXAA and TAA: TAA has the smoothest "
              "edges and no trails behind the blades.",
              "camera -20 10 4"},
    CheckItem{"hdr.edr", "hdr", "HDR and EDR",
              "With Mode on, on an HDR display (the iPhone, an XDR Mac) the panels above x1 are brighter than the "
              "white card and the UI. Mode off clips them to white. On an SDR display both look the same.",
              "hdr on"},
    CheckItem{"physics.bodies", "physics", "Bodies",
              "Drop boxes and balls and blast them: they fall, bounce and come to rest without jitter; nothing falls "
              "through the floor.",
              ""},
    CheckItem{"input.devices", "input", "Input devices",
              "Keys, a gamepad and touch all drive the box: a finger on the left half is a stick, on the right half "
              "it jumps, a tap fires. The table shows every action.",
              ""},
    CheckItem{"assets.loading", "assets", "Loading and streaming",
              "Gray boxes while loading, then three models; the broken and the missing one stay magenta. Tiles load "
              "and unload around the orange focus inside the rings.",
              ""},
    CheckItem{"audio.panning", "audio", "3D panning",
              "Listen: the orange emitter circling the head moves from ear to ear; turn the camera and the sides "
              "follow. The meter's left and right levels move with it.",
              "set flyby 0"},
    CheckItem{"audio.doppler", "audio", "Doppler",
              "Listen: the blue fly-by rises in pitch as it comes and falls as it leaves.", "set orbit 0"},
    CheckItem{"audio.tone", "audio", "Tone without clicks",
              "Move the Frequency slider: the tone glides without clicks or crackles.", "set tone 1"},
    CheckItem{"audio.music", "audio", "Music crossfade",
              "Switch the music from A to B: a smooth two-second crossfade; Off fades out.", ""},
    CheckItem{"audio.buses", "audio", "Buses and voices",
              "Mute, pause and the bus volumes change the sound without clicks; '40 pings' plays without distortion.",
              ""},
    CheckItem{"animation.feet", "animation", "No foot sliding",
              "The hero's feet stay planted while each touches the ground, at walking and at running speed.", ""},
    CheckItem{"animation.footsteps", "animation", "Footstep sync",
              "Listen: each footstep sound comes as a foot touches the ground.", ""},
    CheckItem{"animation.layers", "animation", "Layers and look-at",
              "Wave moves only the right arm; Lean bends the upper body; the head follows the camera; the torch "
              "stays in the left hand.",
              ""},
    CheckItem{"animation.ik", "animation", "Foot placement",
              "On the ramp and across the step both characters' feet stand on the surface, not in it or above it.",
              "camera -40 15 5"},
    CheckItem{"animation.crowd", "animation", "Crowd",
              "With Crowd at 512 the crowd walks smoothly, without pops between clips.", "set crowd 512"},
    CheckItem{"raytracing.noise", "raytracing", "Ray-traced noise",
              "Native Metal backend: with Shadow rays 1 and then 2 the soft shadows stay calm while the sun turns "
              "(little crawling noise).",
              "set shadow_rays 1"},
    CheckItem{"raytracing.ghosting", "raytracing", "MetalFX ghosting",
              "Native Metal backend: with MetalFX temporal at a render scale of 0.5 there are no trails behind the "
              "walking character and the turning torus.",
              "set upscaling temporal\nset scale 0.5"},
    CheckItem{"raytracing.fallback", "raytracing", "Fallback",
              "SDL GPU backend: the section says why there is no ray tracing and shows shadow maps instead.", ""},
    CheckItem{"raytracing.reflections", "raytracing", "Reflections",
              "Native Metal backend: the mirror shows the walking character and the scene behind the camera; the gold "
              "spheres go from a sharp to a blurred reflection, left to right, without crawling noise. Traced "
              "reflections off: only the blurred sky.",
              ""},
    CheckItem{"raytracing.indirect", "raytracing", "Indirect light",
              "Native Metal backend, Indirect light Probe grid: the white block in the corner turns red toward the red "
              "wall and green toward the green one; the glowing panel lights the ground in front of it. Move the sun "
              "elevation: the light follows within a second, without blotches, leaks through the walls or flicker.",
              "set turn_sun 0"},
    CheckItem{"raytracing.lamps", "raytracing", "Lamp shadows",
              "Native Metal backend, low sun: the spot behind the fence throws its diamonds onto the ground in front "
              "of it, and the white block shadows the corner from the lamp. Lamp shadows 0: a plain glow.",
              "set turn_sun 0\nset sun_elevation 6"},
    CheckItem{"raytracing.denoising", "raytracing", "MetalFX denoising",
              "Native Metal backend, Upscaling MetalFX denoising at 0.67: shadows, reflections and indirect light are "
              "smooth, without smears behind the walker or the torus; compare with MetalFX temporal and the Temporal "
              "denoiser (the default where MetalFX runs) and note the GPU time of both.",
              "set upscaling denoised"},
    CheckItem{"raytracing.path_tracer", "raytracing", "Path tracer",
              "Native Metal backend: the path tracer settles into a still, noise-free image within a few seconds; "
              "turned off, the real-time image with the probe grid is about as bright.",
              "set path_tracer 1"},
    CheckItem{"tunnel.lamps", "tunnel", "Area lights",
              "Native Metal backend: each lamp's glowing shape matches the light it casts (the cyan strips along the "
              "floor, the warm ceiling squares, the magenta panel, the neon tubes, the blue portal); shadows of the "
              "crates, ribs and sphere are soft near their base and wider away from it, without crawling noise. "
              "Fallback (SDL GPU): the same light without shadows.",
              ""},
    CheckItem{"tunnel.brushed", "tunnel", "Brushed aluminium",
              "The highlights on the aluminium sphere, ribs and kick plates stretch across the brushing; with "
              "Aluminium anisotropy at 0 they turn round, at 1 they stretch to streaks.",
              "set sweep 1"},
    CheckItem{"tunnel.puddle", "tunnel", "Puddle and wet floor",
              "Native Metal backend: the puddle mirrors the tunnel (the ceiling lamps, the violet tube) sharply, the "
              "wet asphalt shows blurred lamp streaks; Path traced settles to about the same image within a few "
              "seconds.",
              ""},
    CheckItem{"demo.frame_rate", "materials", "Frame rate",
              "The Performance header shows the display's rate (120 fps on a ProMotion iPhone) in every section.", ""},
    CheckItem{"demo.layout", "materials", "Layout",
              "Portrait and landscape: the windows stay inside the safe area, buttons are easy to tap, sheets "
              "scroll with a finger.",
              ""},
};

CheckStatus parseStatus(std::string_view text) {
    if (text == "ok") return CheckStatus::Ok;
    if (text == "problem") return CheckStatus::Problem;
    if (text == "skipped") return CheckStatus::Skipped;
    return CheckStatus::Pending;
}

} // namespace

std::span<const CheckItem> checkItems() { return kItems; }

const char* checkStatusName(CheckStatus status) {
    switch (status) {
    case CheckStatus::Pending: return "pending";
    case CheckStatus::Ok: return "ok";
    case CheckStatus::Problem: return "problem";
    case CheckStatus::Skipped: return "skipped";
    }
    return "pending";
}

std::vector<CheckResult> emptyCheckResults() {
    std::vector<CheckResult> results;
    for (const CheckItem& item : kItems) results.push_back({.id = std::string(item.id)});
    return results;
}

Result<std::vector<CheckResult>> parseCheckResults(const std::string& text) {
    Result<json::Value> parsed = json::parse(text);
    if (!parsed) return Error{parsed.error()};
    if (!parsed->isObject() || (*parsed)["version"].numberOr(0.0) != 1.0) return Error{"not a version 1 check file"};
    std::vector<CheckResult> results = emptyCheckResults();
    for (const json::Value& entry : (*parsed)["items"].array) {
        const std::string id = entry["id"].stringOr("");
        const auto found =
            std::find_if(results.begin(), results.end(), [&](const CheckResult& r) { return r.id == id; });
        if (found == results.end()) continue;
        found->status = parseStatus(entry["status"].stringOr("pending"));
        found->note = entry["note"].stringOr("");
        found->backend = entry["backend"].stringOr("");
        found->time = entry["time"].stringOr("");
    }
    return results;
}

std::string writeCheckResults(const CheckRun& run, std::span<const CheckResult> results) {
    json::Value v = json::Value::makeObject();
    v.object["version"] = json::Value::makeNumber(1);
    v.object["app"] = json::Value::makeString(run.app);
    v.object["app_version"] = json::Value::makeString(run.version);
    v.object["build"] = json::Value::makeString(run.build);
    v.object["backend"] = json::Value::makeString(run.backend);
    v.object["driver"] = json::Value::makeString(run.driver);
    v.object["device"] = json::Value::makeString(run.device);
    v.object["platform"] = json::Value::makeString(run.platform);
    v.object["written"] = json::Value::makeString(utcNow());
    json::Value items = json::Value::makeArray();
    int counts[4] = {};
    for (std::size_t i = 0; i < results.size() && i < kItems.size(); ++i) {
        const CheckResult& r = results[i];
        ++counts[int(r.status)];
        json::Value item = json::Value::makeObject();
        item.object["id"] = json::Value::makeString(r.id);
        item.object["section"] = json::Value::makeString(std::string(kItems[i].section));
        item.object["title"] = json::Value::makeString(std::string(kItems[i].title));
        item.object["status"] = json::Value::makeString(checkStatusName(r.status));
        item.object["note"] = json::Value::makeString(r.note);
        item.object["backend"] = json::Value::makeString(r.backend);
        item.object["time"] = json::Value::makeString(r.time);
        items.array.push_back(std::move(item));
    }
    v.object["items"] = std::move(items);
    json::Value summary = json::Value::makeObject();
    for (const CheckStatus s : {CheckStatus::Pending, CheckStatus::Ok, CheckStatus::Problem, CheckStatus::Skipped})
        summary.object[checkStatusName(s)] = json::Value::makeNumber(counts[int(s)]);
    v.object["summary"] = std::move(summary);
    return json::write(v) + "\n";
}

std::string utcNow() {
    SDL_Time now = 0;
    SDL_DateTime t{};
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &t, false)) return {};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

} // namespace demo
