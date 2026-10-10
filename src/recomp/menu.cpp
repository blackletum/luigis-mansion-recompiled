// In-game overlay menu (F1 toggles, Esc closes): Visuals / Sound / Controls / Enhancements / Game.
// Drawn with Aurora's Dear ImGui on the host thread between the guest's present
// and aurora_end_frame. While it is open the game receives neutral pad input.
#include "recomp/menu.h"
#include "recomp/gx_bridge.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/controls.h"
#include "recomp/crt.h"
#include "recomp/dev_tools.h"
#include "recomp/frame.h"
#include "recomp/hidden_mansion.h"
#include "recomp/skip.h"

#include <aurora/aurora.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <dolphin/vi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace recomp::menu {
namespace {

// Test aid: LM_MENU_TAB=<Enhancements sub-tab name> starts with the menu open on that tab.
const char* sEnvTab = std::getenv("LM_MENU_TAB");
bool sOpen = sEnvTab != nullptr;

// Flags that select a tab once when LM_MENU_TAB names it.
ImGuiTabItemFlags envTab(const char* name) {
    if (!sEnvTab || std::strcmp(sEnvTab, name) != 0) return 0;
    return ImGuiTabItemFlags_SetSelected;
}
bool sDirty = false;
bool sQuit = false;  // "Quit game" confirmed
std::string sOpenTab = sEnvTab ? "Enhancements" : "";  // tab to select once (LM_MENU / LM_MENU_TAB)

bool tab(const char* name) {
    const bool select = sOpenTab == name;
    if (select) sOpenTab.clear();
    return ImGui::BeginTabItem(name, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
}

void applyResolution() { VISetFrameBufferScale(float(port::settings().video.resolutionScale)); }

void applyWindowSettings() {
    const auto& v = port::settings().video;
    applyResolution();
    if (SDL_Window* w = aurora_get_window()) SDL_SetWindowFullscreen(w, v.fullscreen);
    // Automated runs (scripted --input, or LM_BACKGROUND=1) never pause when another window
    // takes focus, so several test instances can run side by side.
    static const bool sBackground = !port::options().inputScript.empty() || std::getenv("LM_BACKGROUND") != nullptr;
    aurora_set_pause_on_focus_lost(port::settings().enhancements.pauseOnFocusLoss && !sBackground);
    if (sBackground) aurora_set_background_input(true);
}

template <typename E>
bool combo(const char* label, E& value, const char* const* names, int count) {
    int i = int(value);
    const bool changed = ImGui::Combo(label, &i, names, count);
    if (changed) value = E(i);
    return changed;
}

void pending(const char* what) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", what);
}

// Feature gating (port::Avail, docs/SETUP.md): a feature missing its disc is greyed out
// with the reason; one that applies to the other installed version stays selectable.
struct Gate {
    port::Avail a;
    explicit Gate(port::Avail avail) : a(avail) { ImGui::BeginDisabled(blocked()); }
    bool blocked() const {
        return a == port::Avail::NeedsUsDisc || a == port::Avail::NeedsPalDisc || a == port::Avail::NotYet;
    }
    ~Gate() {
        ImGui::EndDisabled();
        if (const char* note = port::availNote(a)) pending(note);
    }
};

void gameTab() {
    auto& inst = port::install();
    if (inst.staticBuild) {
        ImGui::Text("Developer build: %s, recompiled at build time.", port::versionName(inst.running));
        ImGui::TextDisabled("LuigisMansionRecomp manages discs and versions (docs/SETUP.md).");
    } else {
        auto row = [](const char* label, const std::string& path, bool ready) {
            ImGui::Text("%s:", label);
            ImGui::SameLine();
            if (ready) ImGui::TextWrapped("%s", path.c_str());
            else ImGui::TextDisabled("not installed");
        };
        row("USA disc", inst.usDisc, inst.usReady);
        row("Europe disc", inst.palDisc, inst.palReady);
        ImGui::Text("Playing: %s version", inst.running == port::GameVersion::PAL ? "European" : "USA");
        if (inst.both()) {
            int base = int(inst.base);
            ImGui::Text("Version to play:");
            ImGui::SameLine();
            bool changed = ImGui::RadioButton("USA", &base, int(port::GameVersion::USA));
            ImGui::SameLine();
            changed |= ImGui::RadioButton("Europe", &base, int(port::GameVersion::PAL));
            if (changed) {
                inst.base = port::GameVersion(base);
                port::saveInstall();
            }
            if (inst.base != inst.running) {
                pending("restart to switch");
                if (ImGui::Button("Restart now")) {
                    port::saveSettings();
                    port::relaunch();
                }
            }
        }
        if (ImGui::Button("Add or change discs...")) {
            port::saveSettings();
            port::relaunch(true);
        }
    }
    ImGui::Separator();
    auto& e = port::settings().enhancements;
    {
        Gate g(port::palBaseFeature());
        static const char* kLanguages[] = {"English", "Deutsch", "Francais", "Espanol", "Italiano"};
        int lang = std::clamp(e.palLanguage, 1, 5) - 1;
        if (ImGui::Combo("Language (European version)", &lang, kLanguages, 5)) {
            e.palLanguage = lang + 1;
            sDirty = true;
        }
    }
    if (port::palBaseFeature() == port::Avail::Yes)
        ImGui::TextDisabled("Used for a new save; an existing save keeps its language (in-game Options).");
}

// Hardware-accuracy toggles (all on by default). Each entry: checkbox bound to
// settings().rendering.<flag>, sDirty |= ..., and a tooltip saying what changes visually
// and roughly what it costs.
void renderingTab() {
    ImGui::TextDisabled("Hardware-accurate rendering. Everything is on by default for the best quality;");
    ImGui::TextDisabled("turn features off only if your PC needs the speed.");
    ImGui::Separator();
    auto& r = port::settings().rendering;
    sDirty |= ImGui::Checkbox("Integer-accurate TEV", &r.integerTev);
    ImGui::SetItemTooltip(
        "Color combiner math as on the GameCube: 8-bit inputs, signed 11-bit registers and rounding\n"
        "after every stage. Slightly darker multi-stage shading and exact overflow; tiny GPU cost.");
    sDirty |= ImGui::Checkbox("EFB color depth (6-bit + dither)", &r.efbColorDepth);
    ImGui::SetItemTooltip(
        "The game renders into a 6-bit-per-channel framebuffer with an ordered dither, like the\n"
        "console. Off: smooth 8-bit color (less banding/dither grain in dark rooms). No real cost.");
    sDirty |= ImGui::Checkbox("Display copy filter", &r.displayCopyFilter);
    ImGui::SetItemTooltip(
        "The vertical filter (and gamma) the console applies when copying the frame to the TV\n"
        "image: slightly softer vertical detail, smooths the dither. One extra full-screen pass.");
    sDirty |= ImGui::Checkbox("EFB copies to RAM (accurate)", &r.efbCopyToRam);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Screen copies also exist in console memory format, like on the GameCube.\n"
                          "Textures that read a copy in another format (the shadow depth map is\n"
                          "read as an IA8 texture) get the console's exact bytes, and memory the game\n"
                          "overwrites stops showing a stale copy. Aimed at the rare Luigi mouth/hose flicker.\n"
                          "Off: copies stay GPU-only textures (saves ~0.6 ms CPU per frame).");
    ImGui::Separator();
    ImGui::TextDisabled("Shadows (flashlight and lightning shadow map)");
    sDirty |= ImGui::Checkbox("Shadow map at render resolution", &r.shadowRenderResolution);
    ImGui::SetItemTooltip(
        "The game draws its shadows from a depth map rendered from the light. On: that map uses the\n"
        "internal render resolution, so shadow edges stay sharp at high resolutions.\n"
        "Off: the console's 640x480 map (blockier, stair-stepped edges at high resolution).\n"
        "Without 'EFB copies to RAM' the map always follows the render resolution. Tiny GPU cost.");
    sDirty |= ImGui::Checkbox("Widescreen shadows", &r.shadowWidescreen);
    ImGui::SetItemTooltip(
        "The game only renders shadows for what a 4:3 screen shows. On: the light's view is widened\n"
        "with the screen, so lightning and flashlight shadows reach the left/right edges at 16:9 and\n"
        "wider (slightly lower horizontal shadow detail). Off: objects near the edges cast no\n"
        "shadows, like widescreen hacks on the original. No cost. Only matters in widescreen.");
    sDirty |= ImGui::Checkbox("Shadows at any distance", &r.shadowAnyDistance);
    ImGui::SetItemTooltip(
        "The original game only draws shadows within a limited distance band around Luigi: in big\n"
        "rooms, shadows of things farther away (or close to the camera) drop out as he walks.\n"
        "On: the shadow light's range covers everything in view, so all shadows always render.\n"
        "Off: the original behaviour. No cost.");
}

// Frame rate: 30 (the game's own rate, drawn directly) up to 320, or the display's refresh rate.
void frameRateControls(port::VideoSettings& v) {
    const float hz = recomp::frame::displayRefreshHz();
    const int target = recomp::frame::targetFps();
    char label[64];
    if (v.matchDisplay) std::snprintf(label, sizeof(label), "%d fps (display)", target);
    else if (v.targetFps <= port::kMinFps) std::snprintf(label, sizeof(label), "30 fps (original)");
    else std::snprintf(label, sizeof(label), "%d fps", v.targetFps);
    ImGui::BeginDisabled(v.matchDisplay);
    sDirty |= ImGui::SliderInt("Frame rate", &v.targetFps, port::kMinFps, port::kMaxFps, label, ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    const bool sliderHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    sDirty |= ImGui::Checkbox("Match display refresh rate", &v.matchDisplay);
    const bool checkHovered = ImGui::IsItemHovered();
    if (v.matchDisplay && hz > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f Hz)", hz);
    } else if (v.vsync && hz > 0 && float(target) > hz + 0.5f) {
        ImGui::SameLine();
        ImGui::TextDisabled("(VSync limits this to %.0f fps)", hz);
    }
    if (sliderHovered || checkHovered) {
        char vs[160] = "";
        if (v.vsync && hz > 0 && float(target) > hz + 0.5f)
            std::snprintf(vs, sizeof(vs),
                          "\n\nVSync is on: frames are limited to the display's %.0f Hz, so %d fps\n"
                          "is not reached. Turn VSync off (next launch) to go above it (with tearing).",
                          hz, target);
        ImGui::SetTooltip("30 fps is the game's own rate, drawn exactly as the console does.\n"
                          "Above 30, frames in between are interpolated (the game still runs at 30):\n"
                          "movement, the camera, lights and effects are blended between game frames,\n"
                          "shown one game frame (33 ms) behind. Cuts and teleports are not blended.\n"
                          "Match display refresh rate: the refresh rate of the monitor the window is on\n"
                          "(currently %.0f Hz).%s",
                          hz, vs);
    }
}

void visualsTab() {
    auto& v = port::settings().video;
    frameRateControls(v);
    static const char* kAspects[] = {"Original 4:3", "Match window", "16:9", "16:10", "21:9", "32:9"};
    sDirty |= combo("Aspect ratio", v.aspect, kAspects, 6);
    sDirty |= ImGui::Checkbox("Keep HUD at 4:3", &v.hudKeep4x3);
    if (!v.hudKeep4x3) pending("in-game HUD moves to the screen edges");
    sDirty |= ImGui::SliderFloat("Field of view", &v.fovScale, 0.75f, 1.5f, "%.2fx");
    ImGui::Separator();
    {
        int& rs = v.resolutionScale;
        char label[64];
        if (rs <= 0) {
            std::snprintf(label, sizeof(label), "Match window");
        } else {
            const AuroraWindowSize ws = aurora_get_window_size();
            const float aspect = ws.native_fb_height ? float(ws.native_fb_width) / float(ws.native_fb_height) : 4.0f / 3.0f;
            const int h = 480 * rs;
            const int w = int(float(h) * std::max(aspect, 4.0f / 3.0f) + 0.5f);
            std::snprintf(label, sizeof(label), "%dx  (%d x %d)", rs, w, h);
        }
        if (ImGui::SliderInt("Render resolution", &rs, 0, 8, label, ImGuiSliderFlags_AlwaysClamp)) {
            sDirty = true;
            applyResolution();
        }
    }
    if (ImGui::Checkbox("Fullscreen", &v.fullscreen)) {
        sDirty = true;
        applyWindowSettings();
    }
    sDirty |= ImGui::Checkbox("VSync", &v.vsync);
    pending("next launch");
    static const char* kMsaa[] = {"Off", "2x", "4x", "8x"};
    int msaaIdx = v.msaa >= 8 ? 3 : v.msaa >= 4 ? 2 : v.msaa >= 2 ? 1 : 0;
    if (ImGui::Combo("Anti-aliasing", &msaaIdx, kMsaa, 4)) {
        v.msaa = 1 << msaaIdx;
        sDirty = true;
    }
    pending("next launch");
    {
        // The European game paces its frames with a timer, so 50 Hz does not change its speed;
        // it is the console's PAL video timing (VI retrace and its callbacks).
        Gate g(port::palBaseFeature());
        int hz = v.pal60 ? 1 : 0;
        static const char* kRates[] = {"50 Hz (original PAL)", "60 Hz (PAL60)"};
        if (ImGui::Combo("PAL refresh rate", &hz, kRates, 2)) {
            v.pal60 = hz == 1;
            sDirty = true;
        }
    }
    ImGui::Separator();
    sDirty |= recomp::crt::drawMenu();
}

void soundTab() {
    auto& a = port::settings().audio;
    sDirty |= ImGui::SliderFloat("Master volume", &a.masterVolume, 0.0f, 1.0f);
    sDirty |= ImGui::SliderFloat("Music volume", &a.musicVolume, 0.0f, 1.0f);
    sDirty |= ImGui::SliderFloat("Sound effects volume", &a.sfxVolume, 0.0f, 1.0f);
    static const char* kOutputs[] = {"Stereo", "Mono", "Surround (Pro Logic II matrix)"};
    sDirty |= combo("Output", a.output, kOutputs, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Surround here plays like Stereo: the console's Dolby Surround matrix is always\n"
                          "reproduced in the stereo output (choose Surround in the game's own options),\n"
                          "so a Pro Logic (II) decoder can unfold it.");
    sDirty |= ImGui::Checkbox("Swap left/right", &a.swapChannels);
    ImGui::SeparatorText("DSP accuracy");
    sDirty |= ImGui::Checkbox("Reverb / echo", &a.reverb);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The DSP's effect lines (feedback delays used as room echo) and its chorus bus.\n"
                          "Off: dry sound only, slightly cheaper.");
    sDirty |= ImGui::Checkbox("Voice filters", &a.voiceFilters);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Per-voice FIR / IIR / distortion stages the game asks the DSP for (muffled or\n"
                          "distorted sounds). Off: those sounds play unfiltered, slightly cheaper.");
}

// Gameplay / difficulty (port::GameplaySettings, src/recomp/gameplay_hooks.cpp). All neutral by
// default (ctrl+click a slider to type a value).
void gameplaySection() {
    auto& g = port::settings().gameplay;
    ImGui::TextDisabled("Neutral by default: the original game is unchanged until you move something.");
    ImGui::SeparatorText("Lighting");
    sDirty |= ImGui::Checkbox("Hidden Mansion darkness everywhere", &g.palDarknessEverywhere);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Room ambient light x0.2, as in the PAL Hidden Mansion");
    sDirty |= ImGui::SliderFloat("Ambient light", &g.ambientScale, 0.05f, 1.5f, "%.2fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplies room ambient light (also on top of PAL darkness)");
    ImGui::SeparatorText("Difficulty");
    sDirty |= ImGui::SliderFloat("Damage taken", &g.damageScale, 0.0f, 5.0f, "%.2fx");
    sDirty |= ImGui::SliderFloat("Luigi's health", &g.healthScale, 0.25f, 5.0f, "%.2fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starting and maximum health (applies when Luigi is next created)");
    sDirty |= ImGui::SliderFloat("Ghost health", &g.ghostHealthScale, 0.25f, 5.0f, "%.2fx");
    sDirty |= ImGui::SliderFloat("Poltergust strength", &g.vacuumStrength, 0.25f, 4.0f, "%.2fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How fast the vacuum drains a captured ghost's HP");
    sDirty |= ImGui::Checkbox("Disable recovery hearts", &g.noHeartDrops);
    ImGui::SeparatorText("Movement");
    sDirty |= ImGui::Checkbox("Always ride the Poltergust", &g.alwaysRide);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Luigi glides with momentum everywhere (the PAL Boolossus-fight movement)");
    if (ImGui::Button("Reset gameplay options")) {
        g = port::GameplaySettings{};
        sDirty = true;
    }
}

void generalEnhancementsTab() {
    auto& e = port::settings().enhancements;
    sDirty |= ImGui::Checkbox("Show FPS", &e.showFps);
    if (ImGui::Checkbox("Pause when window loses focus", &e.pauseOnFocusLoss)) {
        sDirty = true;
        applyWindowSettings();
    }
    sDirty |= ImGui::Checkbox("Skip intro logos", &e.skipIntroLogos);
    pending("planned");
    {
        sDirty |= ImGui::Checkbox("Skippable cutscenes", &e.skipCutscenes);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Press START during a cutscene to skip it. The game plays the rest of the\n"
                              "cutscene at high speed behind a black screen, so everything it sets up\n"
                              "(items, keys, doors, story progress) is the same as after watching it.\n"
                              "Choices and the save screen still wait for you.");
    }
    sDirty |= ImGui::Checkbox("Hold Tab to fast-forward", &e.fastForwardKey);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While Tab is held the game runs as fast as it can, muted.");
    sDirty |= ImGui::Checkbox("Fix rooms vanishing near doorways", &e.portalClipFix);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game hides a whole room behind a doorway when the camera gets so close\n"
                          "to the doorway that part of it is behind the camera (an original bug, e.g.\n"
                          "the upper foyer during the cutscene when Luigi first enters). On: the doorway\n"
                          "is clipped properly and the room stays visible. Off: original behaviour.");
    ImGui::Separator();
    {
        // Switching mid-visit would leave PAL data/patches half applied, so only allow it outside.
        const bool inside = recomp::hm::hiddenActive();
        const port::Avail avail[2] = {port::hiddenMansionUs(), port::hiddenMansionPal()};
        const bool pal = recomp::hm::variant() == recomp::hm::Variant::Pal;
        // Either release's rules on either game (the other one's needs both discs; docs/SETUP.md).
        const bool usGame = port::install().running == port::GameVersion::USA;
        const char* kVariants[] = {usGame ? "USA (original)" : "USA (easier, not mirrored)",
                                   usGame ? "European (harder, mirrored)" : "European (original)"};
        auto usable = [&](int i) { return avail[i] == port::Avail::Yes; };
        if (recomp::hm::labChoiceAvailable()) {
            // Both releases' data: E. Gadd's lab asks on every visit (docs/HIDDEN_MANSION.md).
            ImGui::TextUnformatted("Hidden Mansion version: chosen in E. Gadd's lab");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Picking the Hidden Mansion in E. Gadd's lab asks which one:\n"
                                  "\"(USA)\" is the %s release's, \"(PAL)\" the %s release's\n"
                                  "(harder, mirrored, darker). The save file keeps the answer for that visit.",
                                  usGame ? "original USA" : "USA", usGame ? "European" : "original European");
            if (inside) ImGui::TextDisabled("This visit: %s", kVariants[pal ? 1 : 0]);
        } else {
            ImGui::BeginDisabled(inside);
            if (ImGui::BeginCombo("Hidden Mansion version", kVariants[pal ? 1 : 0])) {
                for (int i = 0; i < 2; i++) {
                    if (ImGui::Selectable(kVariants[i], (pal ? 1 : 0) == i,
                                          usable(i) ? 0 : ImGuiSelectableFlags_Disabled)) {
                        e.hiddenMansionPal = i == 1;
                        recomp::hm::setVariant(e.hiddenMansionPal ? recomp::hm::Variant::Pal : recomp::hm::Variant::Us);
                        sDirty = true;
                    }
                    if (const char* note = port::availNote(avail[i])) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", note);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (inside) pending("leave the Hidden Mansion to change");
        }
        if (!usable(0)) pending(port::availNote(avail[0]));
        else if (!usable(1)) pending(port::availNote(avail[1]));
    }
}

// Restored unused/beta content (docs/UNUSED_CONTENT.md). Every entry is off by default:
// the retail game is unchanged unless the player turns something on here.
void unusedContentTab() {
    auto& e = port::settings().enhancements;
    ImGui::TextDisabled("Content left unused in the retail game. Everything here is off by default.");
    ImGui::Separator();
    sDirty |= ImGui::Checkbox("Game Boy Horror camera", &e.gbhCamera);
    pending("D-pad Up in game");
    sDirty |= ImGui::Checkbox("Game Boy Horror E3 clock", &e.gbhClock);
    pending("D-pad Left in game");
    sDirty |= ImGui::Checkbox("Game Boy Horror ghost tracker", &e.gbhTracker);
    pending("D-pad Down: off / far / close");
    {
        static const char* kGhost[] = {"Retail", "Beta"};
        int gs = e.earlyGhostLighting ? 1 : 0;
        if (ImGui::Combo("Ghost shading", &gs, kGhost, 2)) {
            e.earlyGhostLighting = gs == 1;
            sDirty = true;
        }
    }
    sDirty |= ImGui::Checkbox("Green Toad (kinopio2)", &e.greenToad);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toads wear the unused green texture set of the never-placed kinopio2 character\n"
                          "(TCRF's Action Replay code), with the unused green-capped dialogue icon.");
    {
        Gate g(port::usCodeFeature());
        auto item = [&](const char* label, bool& v, const char* note) {
            sDirty |= ImGui::Checkbox(label, &v);
            if (note && g.a == port::Avail::Yes) pending(note);
        };
        item("Beta skybox", e.betaSkybox, "next map load");
        item("Heart door (Chauncey's room)", e.heartDoor, "next map load");
        item("Star door (Twins' room)", e.starDoor, "next map load");
        item("Game Boy Horror map: full Luigi model", e.betaMapLuigi, "next map load");
        item("Portrificationizer: each ghost's own model", e.unusedPortraitModels, nullptr);
        item("Early Luigi model", e.earlyLuigi, "next map load");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Luigi uses the unused early model left on the disc (/model/luige.szp): slightly\n"
                              "different face, hair and clothes. Same skeleton, so all retail animations, the\n"
                              "Poltergust, flashlight and Game Boy Horror work as usual. Also used by the opening\n"
                              "and E. Gadd cutscene Luigis.");
        sDirty |= ImGui::Checkbox("Air and water nozzles", &e.airWaterNozzles);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The Poltergust's unused nozzle models: the red wpair nozzle when blowing air\n"
                              "(L without an element) and the blue wpwater nozzle when spraying water.\n"
                              "Vacuum, fire and ice keep the retail nozzle.");
        sDirty |= ImGui::Checkbox("Early Poltergust 3000", &e.earlyPoltergust);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The E3 2001 Poltergust model still on the disc (/model/vbody.szp): a red canister\n"
                              "on a wheeled base instead of the retail tank.");
        if (g.a == port::Avail::Yes) pending("next map load");
        sDirty |= ImGui::Checkbox("Early Bogmire model", &e.earlyBogmire);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bogmire uses his unused early model (/model/shadow.szp): longer wings, thinner\n"
                              "hanging flaps, a defined chin and a wavy ghost tail, with its own animations.");
        if (g.a == port::Avail::Yes) pending("next map load");
        sDirty |= ImGui::Checkbox("2-player mode (unfinished)", &e.twoPlayer);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The game's leftover two-player mode: a second Luigi on controller port 2\n"
                              "(assign a device to port 2 in Controls). One shared camera while both are\n"
                              "close in the same room, top/bottom split screen when apart. As the game left\n"
                              "it: player 2 is left behind at doors, has no health meter, and the HUD\n"
                              "disappears in split screen.");
        if (g.a == port::Avail::Yes) pending("next map load");
        ImGui::BeginDisabled(!e.twoPlayer);
        ImGui::Indent();
        sDirty |= ImGui::Checkbox("Co-op improvements", &e.coopImprovements);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Makes the 2-player mode playable as co-op: player 2 is brought along when\n"
                              "player 1 changes rooms, the HUD stays visible in split screen, player 2 gets\n"
                              "a health meter, and a downed player is revived with half of the partner's\n"
                              "health (game over only when both are down).");
        ImGui::Unindent();
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    {
        Gate g(port::usCodeFeature());
        recomp::dev::unusedSection();
    }
}

// Gameplay toggles, difficulty and cheats.
void gameplayTab() { gameplaySection(); }

// Debug features left in the game, warps and viewers.
void debugTab() {
    Gate g(port::usCodeFeature());
    recomp::dev::debugSection();
}

void enhancementsTab() {
    if (ImGui::BeginTabBar("enhancements")) {
        if (ImGui::BeginTabItem("General", nullptr, envTab("General"))) {
            generalEnhancementsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gameplay", nullptr, envTab("Gameplay"))) {
            gameplayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Unused content", nullptr, envTab("Unused content"))) {
            unusedContentTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug", nullptr, envTab("Debug"))) {
            debugTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void fpsOverlay() {
    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    static int frames = 0;
    static float fps = 0.0f;
    frames++;
    const auto now = clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    if (dt >= 0.5f) {
        fps = frames / dt;
        frames = 0;
        last = now;
    }
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f fps", fps);
    ImGui::End();
}

}  // namespace

void init() {
    port::loadSettings();
    applyWindowSettings();
    // LM_MENU=<tab> (testing/screenshots): start with the menu open on that tab.
    if (const char* tab = std::getenv("LM_MENU"); tab && *tab) {
        sOpen = true;
        sOpenTab = tab;
    }
}

bool isOpen() { return sOpen || recomp::dev::capturesInput(); }
bool quitRequested() { return sQuit; }

void handleEvent(const SDL_Event& ev) {
    if (recomp::controls::handleEvent(ev)) return;  // binding capture owns the key
    if (recomp::dev::handleKey(ev)) return;
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return;
    if (ev.key.key == SDLK_F1) sOpen = !sOpen;
    else if (ev.key.key == SDLK_ESCAPE && sOpen) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

void draw() {
    skip::drawOverlay();
    recomp::gx::applyAccuracySettings();
    recomp::crt::update();
    if (port::settings().enhancements.showFps) fpsOverlay();
    recomp::dev::drawOverlays();
    if (!sOpen) return;
    ImGui::SetNextWindowSize(ImVec2(680, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("Luigi's Mansion PC  (F1)", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.45f);
        if (ImGui::BeginTabBar("tabs")) {
            if (tab("Visuals")) {
                if (ImGui::BeginTabBar("visuals")) {
                    if (ImGui::BeginTabItem("General")) {
                        visualsTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Rendering", nullptr, envTab("Rendering"))) {
                        renderingTab();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (tab("Sound")) {
                soundTab();
                ImGui::EndTabItem();
            }
            if (tab("Controls")) {
                sDirty |= recomp::controls::drawTab();
                ImGui::EndTabItem();
            }
            if (tab("Enhancements")) {
                enhancementsTab();
                ImGui::EndTabItem();
            }
            if (tab("Game")) {
                gameTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();
        // Quit (with confirmation): ends the host loop the same way closing the window does.
        ImGui::Separator();
        if (ImGui::Button("Quit game")) ImGui::OpenPopup("Quit?");
        if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Quit Luigi's Mansion? Unsaved progress will be lost.");
            if (ImGui::Button("Quit")) {
                if (sDirty) {
                    port::saveSettings();
                    sDirty = false;
                }
                sQuit = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    if (!open) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

}  // namespace recomp::menu
