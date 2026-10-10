#pragma once
// User settings, edited in the in-game overlay menu (F1) and saved to
// %APPDATA%\Luigi's Mansion PC\settings.ini. Read them wherever they apply;
// changes take effect immediately unless noted.
#include <iosfwd>
#include <string>

namespace port {

enum class AspectMode { Original4x3, Auto, Wide16x9, Wide16x10, Ultra21x9, Ultra32x9 };
enum class AudioOutput { Stereo, Mono, Surround };

constexpr int kMinFps = 30, kMaxFps = 320;

struct VideoSettings {
    // Presentation frame rate. 30 = the game's own rate, drawn directly (no interpolation);
    // above 30 the frames in between are interpolated (src/recomp/interp.*). matchDisplay
    // uses the refresh rate of the display the window is on instead of targetFps.
    int targetFps = 30;         // kMinFps..kMaxFps
    bool matchDisplay = false;
    AspectMode aspect = AspectMode::Auto;
    int resolutionScale = 0;    // internal render resolution: 0 = match window, N = N x 480p (aspect-widened)
    float fovScale = 1.0f;      // multiplies the game's vertical field of view
    bool fullscreen = false;
    bool vsync = true;          // applied at next launch
    int msaa = 1;               // 1, 2, 4, 8; applied at next launch
    bool hudKeep4x3 = true;     // keep 2D/HUD at 4:3 (centered) in widescreen
    bool pal60 = false;         // PAL game only: 59.94 Hz retrace (PAL60, 20% faster) instead of 50 Hz
};

// CRT post-process filter (src/recomp/crt.cpp). Beam sigmas are fractions of the
// scanline spacing, convergence offsets are in game pixels / lines, mask size in
// output pixels, corner radius a fraction of the screen height.
enum class CrtPreset { ApertureGrille, SlotMask, ShadowMask, ConsumerTv, Custom };
enum class CrtScanlines { Progressive480, DoubleStrike240, Interlaced480i };
enum class CrtMask { ApertureGrille, SlotMask, ShadowMask, None };

struct CrtSettings {
    bool enabled = false;
    CrtPreset preset = CrtPreset::ApertureGrille;
    CrtScanlines scanlines = CrtScanlines::Progressive480;
    float beamMinSigma = 0.20f, beamMaxSigma = 0.36f, beamSpotPower = 0.6f;
    float beamMinShape = 2.0f, beamMaxShape = 3.0f, beamShapePower = 0.5f;
    float horizontalSoftness = 0.45f;
    CrtMask mask = CrtMask::ApertureGrille;
    float maskSize = 3.0f, maskStrength = 0.75f;
    float halation = 0.04f, diffusion = 0.06f, bloom = 0.15f;
    float convergence[6] = {0, 0, 0, 0, 0, 0};  // R x,y  G x,y  B x,y
    float gammaIn = 2.4f, gammaOut = 2.2f;
    float curvature = 0.0f, cornerRadius = 0.02f;
    float brightness = 1.2f;
};

struct AudioSettings {
    float masterVolume = 1.0f;  // 0..1
    float musicVolume = 1.0f;   // streamed/sequenced music
    float sfxVolume = 1.0f;     // everything else
    AudioOutput output = AudioOutput::Stereo;
    bool swapChannels = false;
    bool reverb = true;        // DSP effect lines (echo/reverb) and the chorus bus
    bool voiceFilters = true;  // per-voice FIR / IIR / distortion stages of the DSP microcode
};

struct EnhancementSettings {
    bool showFps = false;
    bool pauseOnFocusLoss = true;
    bool skipIntroLogos = false;
    bool hiddenMansionPal = true;  // default Hidden Mansion rules for saves that have not chosen in E. Gadd's lab
    int palLanguage = 1;           // PAL game only: 1 English, 2 German, 3 French, 4 Spanish, 5 Italian (next launch)
    bool gbhClock = false;         // cut E3 GBH clock/radar screen, toggled in game with D-pad Left
    bool gbhTracker = false;       // unused GBH ghost tracker, cycled in game with D-pad Down (off/far/close)
    bool gbhCamera = false;        // unused Game Boy Horror live camera, toggled in game with D-pad Up (docs/UNUSED_CONTENT.md)
    bool betaSkybox = false;            // unused sky in the game archive (iwamoto/vrbox) on map 2
    bool heartDoor = false;             // unused heart door (type 07) on Chauncey's room
    bool starDoor = false;              // unused star door (type 08) on the Twins' room
    bool betaMapLuigi = false;          // unused full Luigi model on the Game Boy Horror map
    bool unusedPortraitModels = false;  // each ghost's own model in the Portrificationizer scene
    bool earlyLuigi = false;            // unused early Luigi model /model/luige.szp (src/recomp/early_luigi.cpp)
    bool airWaterNozzles = false;       // unused wpair/wpwater Poltergust nozzles (src/recomp/nozzles.cpp)
    bool earlyPoltergust = false;       // E3 2001 Poltergust body /model/vbody.szp (src/recomp/unused_models.cpp)
    bool earlyBogmire = false;          // early Bogmire model /model/shadow.szp (src/recomp/unused_models.cpp)
    bool greenToad = false;             // Toads use kinopio2's green texture set (src/recomp/unused_models.cpp)
    bool twoPlayer = false;             // unused 2-player mode, P2 on controller port 2 (src/recomp/coop.cpp)
    bool coopImprovements = false;      // co-op fixes on top of the 2-player mode (src/recomp/coop_fixes.cpp)
    bool earlyGhostLighting = false;  // "Ghost shading: Beta" = the known beta-shading cheat (fn_800581E8 channel setup)
    bool skipCutscenes = false;  // START skips cutscenes (src/recomp/skip.cpp)
    bool fastForwardKey = true;  // holding Tab runs the game at high speed (src/recomp/skip.cpp)
    bool portalClipFix = true;   // rooms behind a doorway near the camera don't vanish (src/recomp/portal_fix.cpp)
    // Debug: the game's own compiled-in debug switches (src/recomp/dev_tools.cpp)
    bool dbgParticleReport = false;  // particle manager JUTReport text
    bool dbgDepthBlur = false;       // unused depth blur screen pass
    bool dbgLuigiFlags = false;      // "Debug Mode" AR code: Luigi debug flags 0xCB
    bool dbgInfoDisplay = false;     // Luigi's debug text (dust, aura, water, heart)
    bool dbgScanBoxes = false;       // Game Boy Horror scan boxes
    bool dbgAxes = false;            // XYZ axes at Luigi
    int dbgHudTest = 0;              // 0 off, 1 icons, 2 numbers, 3 element meter
    bool dbgFontTest = false;        // font test screen text
    bool dbgFontTestLatin = false;   // PAL fix: last font-test line shows "alphabet 123"
    bool dbgWarpSkipEvents = true;   // warps don't start the destination's entry cutscene/event
};

// Gameplay / difficulty options (F1 > Enhancements > Gameplay). Defaults are neutral: the
// original game is unchanged until one is moved. They apply in every mansion and variant
// (docs/GAMEPLAY_OPTIONS.md).
struct GameplaySettings {
    bool palDarknessEverywhere = false;  // PAL Hidden Mansion lighting (ambient x0.2) in every mansion
    float ambientScale = 1.0f;           // room ambient light multiplier, 0.05..1.5 (composes with the above)
    float damageScale = 1.0f;            // damage Luigi takes, 0..5
    float healthScale = 1.0f;            // Luigi's starting and maximum health, 0.25..5
    float ghostHealthScale = 1.0f;       // ghost HP, 0.25..5
    bool noHeartDrops = false;           // recovery hearts never appear (and any that do heal nothing)
    float vacuumStrength = 1.0f;         // Poltergust drain rate on ghost HP, 0.25..4
    bool alwaysRide = false;             // Luigi always uses the Poltergust-riding movement (PAL Boolossus mode)
};

// Hardware-accuracy rendering features (F1 > Visuals > Rendering). Every costly accuracy
// feature gets a flag here, ON by default (best quality); weaker PCs can turn them off.
struct RenderingSettings {
    // Integer TEV: the color combiner computes with the hardware's 8-bit inputs, signed 11-bit
    // registers and per-stage rounding instead of floats (Aurora GX_AURORA_ACCURACY_INT_TEV).
    bool integerTev = true;
    // EFB color depth: pixels are stored the way the game's RGBA6 framebuffer stores them,
    // 6 bits per channel with the ordered dither the game enables (GX_AURORA_ACCURACY_EFB_DEPTH).
    bool efbColorDepth = true;
    // Display copy filter: the vertical filter (and gamma) GXCopyDisp applies on the way to the TV.
    bool displayCopyFilter = true;
    // GXCopyTex results also exist as GX-format bytes in main memory, so textures that read a
    // copy with another format/size (the shadow depth map) and CPU reads see what the
    // console sees (docs/RENDERING.md). Off: copies stay GPU-only textures.
    bool efbCopyToRam = true;
    // Shadows (docs/RENDERING.md, "Shadows"). The game's shadow map (light's-view depth, copied
    // as Z16 and compared in the TEV) at the internal render resolution instead of 640x480.
    bool shadowRenderResolution = true;
    // Widescreen: the shadow light's frustum is widened by the same factor as the view, so
    // shadows reach the extra side areas (the game only covers what a 4:3 screen shows).
    bool shadowWidescreen = true;
    // The game's shadow light only covers a depth band around Luigi (e.g. 619..2322 units from the
    // room light), so shadows vanish on things farther away. On: the band is extended to what the
    // player's view shows (enhancement). Off: the original range.
    bool shadowAnyDistance = true;
};

struct Settings {
    VideoSettings video;
    RenderingSettings rendering;
    CrtSettings crt;
    AudioSettings audio;
    EnhancementSettings enhancements;
    GameplaySettings gameplay;
};

Settings& settings();
void loadSettings();
void saveSettings();
std::string settingsPath();

// Ini sections owned by another module (e.g. controller bindings, src/recomp/controls.cpp).
// reset() runs before every load, load() gets each key the table above does not know,
// save() appends the module's own sections. Register from a static initializer.
struct SettingsExtension {
    void (*reset)();
    void (*load)(const std::string& section, const std::string& key, const std::string& value);
    void (*save)(std::ostream& out);
};
void addSettingsExtension(const SettingsExtension& ext);

}  // namespace port
