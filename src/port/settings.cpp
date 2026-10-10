#include "port/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace port {
namespace {

Settings sSettings;

std::vector<SettingsExtension>& extensions() {
    static std::vector<SettingsExtension> sExtensions;
    return sExtensions;
}

// One ini key bound to a setting, with text conversion both ways.
struct Field {
    const char* section;
    const char* key;
    std::function<std::string()> get;
    std::function<void(const std::string&)> set;
};

template <typename T>
Field num(const char* section, const char* key, T& v) {
    return {section, key, [&v] { std::ostringstream s; s << v; return s.str(); },
            [&v](const std::string& s) {
                std::istringstream in(s);
                T t{};
                if (in >> t) v = t;
            }};
}
Field flag(const char* section, const char* key, bool& v) {
    return {section, key, [&v] { return std::string(v ? "true" : "false"); },
            [&v](const std::string& s) { v = s == "true" || s == "1"; }};
}
template <typename E>
Field enm(const char* section, const char* key, E& v) {
    return {section, key, [&v] { return std::to_string(int(v)); },
            [&v](const std::string& s) { v = E(std::atoi(s.c_str())); }};
}

std::vector<Field> fields() {
    auto& v = sSettings.video;
    auto& a = sSettings.audio;
    auto& e = sSettings.enhancements;
    auto& c = sSettings.crt;
    auto& g = sSettings.gameplay;
    auto& r = sSettings.rendering;
    return {
        num("video", "target_fps", v.targetFps),
        flag("video", "match_display_refresh", v.matchDisplay),
        enm("video", "aspect", v.aspect),
        num("video", "resolution_scale", v.resolutionScale),
        num("video", "fov_scale", v.fovScale),
        flag("video", "fullscreen", v.fullscreen),
        flag("video", "vsync", v.vsync),
        num("video", "msaa", v.msaa),
        flag("video", "hud_keep_4x3", v.hudKeep4x3),
        flag("video", "pal60", v.pal60),
        flag("rendering", "integer_tev", r.integerTev),
        flag("rendering", "efb_color_depth", r.efbColorDepth),
        flag("rendering", "display_copy_filter", r.displayCopyFilter),
        flag("rendering", "efb_copy_to_ram", sSettings.rendering.efbCopyToRam),
        flag("rendering", "shadow_render_resolution", r.shadowRenderResolution),
        flag("rendering", "shadow_widescreen", r.shadowWidescreen),
        flag("rendering", "shadow_any_distance", r.shadowAnyDistance),
        flag("crt", "enabled", c.enabled),
        enm("crt", "preset", c.preset),
        enm("crt", "scanlines", c.scanlines),
        num("crt", "beam_min_sigma", c.beamMinSigma),
        num("crt", "beam_max_sigma", c.beamMaxSigma),
        num("crt", "beam_spot_power", c.beamSpotPower),
        num("crt", "beam_min_shape", c.beamMinShape),
        num("crt", "beam_max_shape", c.beamMaxShape),
        num("crt", "beam_shape_power", c.beamShapePower),
        num("crt", "horizontal_softness", c.horizontalSoftness),
        enm("crt", "mask", c.mask),
        num("crt", "mask_size", c.maskSize),
        num("crt", "mask_strength", c.maskStrength),
        num("crt", "halation", c.halation),
        num("crt", "diffusion", c.diffusion),
        num("crt", "bloom", c.bloom),
        num("crt", "convergence_red_x", c.convergence[0]),
        num("crt", "convergence_red_y", c.convergence[1]),
        num("crt", "convergence_green_x", c.convergence[2]),
        num("crt", "convergence_green_y", c.convergence[3]),
        num("crt", "convergence_blue_x", c.convergence[4]),
        num("crt", "convergence_blue_y", c.convergence[5]),
        num("crt", "gamma_in", c.gammaIn),
        num("crt", "gamma_out", c.gammaOut),
        num("crt", "curvature", c.curvature),
        num("crt", "corner_radius", c.cornerRadius),
        num("crt", "brightness", c.brightness),
        num("audio", "master_volume", a.masterVolume),
        num("audio", "music_volume", a.musicVolume),
        num("audio", "sfx_volume", a.sfxVolume),
        enm("audio", "output", a.output),
        flag("audio", "swap_channels", a.swapChannels),
        flag("audio", "reverb", a.reverb),
        flag("audio", "voice_filters", a.voiceFilters),
        flag("enhancements", "show_fps", e.showFps),
        flag("enhancements", "pause_on_focus_loss", e.pauseOnFocusLoss),
        flag("enhancements", "skip_intro_logos", e.skipIntroLogos),
        flag("enhancements", "hidden_mansion_pal", e.hiddenMansionPal),
        num("enhancements", "pal_language", e.palLanguage),
        flag("enhancements", "gbh_camera", e.gbhCamera),
        flag("enhancements", "gbh_tracker", e.gbhTracker),
        flag("enhancements", "gbh_clock", e.gbhClock),
        flag("enhancements", "early_ghost_lighting", e.earlyGhostLighting),
        flag("enhancements", "beta_skybox", e.betaSkybox),
        flag("enhancements", "heart_door", e.heartDoor),
        flag("enhancements", "star_door", e.starDoor),
        flag("enhancements", "beta_map_luigi", e.betaMapLuigi),
        flag("enhancements", "early_luigi", e.earlyLuigi),
        flag("enhancements", "air_water_nozzles", e.airWaterNozzles),
        flag("enhancements", "early_poltergust", e.earlyPoltergust),
        flag("enhancements", "early_bogmire", e.earlyBogmire),
        flag("enhancements", "green_toad", e.greenToad),
        flag("enhancements", "two_player", e.twoPlayer),
        flag("enhancements", "coop_improvements", e.coopImprovements),
        flag("enhancements", "unused_portrait_models", e.unusedPortraitModels),
        flag("enhancements", "skip_cutscenes", e.skipCutscenes),
        flag("enhancements", "fast_forward_key", e.fastForwardKey),
        flag("enhancements", "portal_clip_fix", e.portalClipFix),
        flag("gameplay", "pal_darkness_everywhere", g.palDarknessEverywhere),
        num("gameplay", "ambient_scale", g.ambientScale),
        num("gameplay", "damage_scale", g.damageScale),
        num("gameplay", "health_scale", g.healthScale),
        num("gameplay", "ghost_health_scale", g.ghostHealthScale),
        flag("gameplay", "no_heart_drops", g.noHeartDrops),
        num("gameplay", "vacuum_strength", g.vacuumStrength),
        flag("gameplay", "always_ride", g.alwaysRide),
        flag("debug", "particle_report", e.dbgParticleReport),
        flag("debug", "depth_blur", e.dbgDepthBlur),
        flag("debug", "luigi_debug_flags", e.dbgLuigiFlags),
        flag("debug", "info_display", e.dbgInfoDisplay),
        flag("debug", "scan_boxes", e.dbgScanBoxes),
        flag("debug", "axes", e.dbgAxes),
        num("debug", "hud_test", e.dbgHudTest),
        flag("debug", "font_test", e.dbgFontTest),
        flag("debug", "font_test_latin", e.dbgFontTestLatin),
        flag("debug", "warp_skip_events", e.dbgWarpSkipEvents),
    };
}

}  // namespace

Settings& settings() { return sSettings; }

void addSettingsExtension(const SettingsExtension& ext) { extensions().push_back(ext); }

std::string settingsPath() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
#else
    const char* base = std::getenv("XDG_CONFIG_HOME");
#endif
    std::filesystem::path dir = base ? std::filesystem::path(base) / "Luigi's Mansion PC" : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / "settings.ini").string();
}

void loadSettings() {
    for (auto& ext : extensions()) ext.reset();
    std::ifstream in(settingsPath());
    if (!in) return;
    auto table = fields();
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (section == "video" && key == "frame_rate") {  // before target_fps: 0 = 30, 1 = 60, 2 = unlimited
            const int old = std::atoi(value.c_str());
            sSettings.video.targetFps = old == 1 ? 60 : 30;
            sSettings.video.matchDisplay = old == 2;
            continue;
        }
        bool known = false;
        for (auto& f : table)
            if (section == f.section && key == f.key) {
                f.set(value);
                known = true;
            }
        if (!known)
            for (auto& ext : extensions()) ext.load(section, key, value);
    }
    auto& v = sSettings.video;
    v.targetFps = v.targetFps < kMinFps ? kMinFps : v.targetFps > kMaxFps ? kMaxFps : v.targetFps;
}

void saveSettings() {
    std::ofstream out(settingsPath());
    if (!out) return;
    std::string section;
    for (auto& f : fields()) {
        if (section != f.section) {
            section = f.section;
            out << '[' << section << "]\n";
        }
        out << f.key << '=' << f.get() << '\n';
    }
    for (auto& ext : extensions()) ext.save(out);
}

}  // namespace port
