// platform/pc/ui_pc.cpp — Dear ImGui overlay for the PC port.
//
// Owns the ImGui context and draws a main menu bar toggled with F1:
//   Game   -> Restart / Settings / Exit
//   Cheats -> Open Cheats
//
// The Settings window is tabbed (Graphics/Audio/Controls/Boot). Controls
// rebind the actions from pmd-red.ini (see config_pc.c) — every action takes
// any number of keys and/or mouse buttons. Boot exposes the launch-arg toggles.
// The Cheats window calls the game-state helpers in cheats_pc.c (linked only
// into pmd-red-game; the smoke binary links empty stubs).
//
// Uses the SDL_Renderer backend (imgui_impl_sdlrenderer2) to match the renderer
// already owned by video_pc.c. video_pc.c blits the 240x160 game frame with an
// explicit scaled rect (no SDL logical-size transform) and calls Pc_UiRender
// right before SDL_RenderPresent, so ImGui draws in window pixels where its
// mouse/hit-testing aligns exactly with the rendered menu.
//
// Compiled as C++ (ImGui is C++); the C callers see the extern "C" API
// declared in gba_shim.h. Restart/quit flags live outside HAVE_SDL2 so the
// rest of the port always links.
#include "gba/types.h" // u8/s32 used by the save API in gba_shim.h
#include "gba_shim.h"
#include "constants/item.h" // cheat item ids (pure #defines)

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int gUiRestartRequested = 0;

extern "C" void Pc_RequestRestart(void) {
    gUiRestartRequested = 1;
    Pc_RequestQuit(); // make the game/boot loops unwind so main can re-exec
}

extern "C" int Pc_RestartRequested(void) {
    return gUiRestartRequested;
}

#ifdef HAVE_SDL2

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

static bool gUiReady = false;
static bool gMenuVisible = false;
static bool gSettingsOpen = false;
static bool gCheatsOpen = false;      // Cheats window visibility
static bool gCheatInvincible = false; // continuously top-up leader HP in dungeon
static bool gCheatInstantKill = false; // enemies at 1 HP each frame
static bool gCheatExpBoost = false;    // enemies give 1.5x EXP each frame
static int  gCaptureAction = -1; // action awaiting a key press, or -1

extern "C" void Pc_UiInit(void) {
    SDL_Window *win;
    SDL_Renderer *ren;
    ImGuiIO *io;

    if (gUiReady)
        return;
    win = (SDL_Window *)Pc_VideoGetSdlWindow();
    ren = (SDL_Renderer *)Pc_VideoGetSdlRenderer();
    if (win == NULL || ren == NULL)
        return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    io = &ImGui::GetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io->IniFilename = NULL; // don't scatter imgui.ini next to the exe
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    gUiReady = true;
}

extern "C" void Pc_UiShutdown(void) {
    if (!gUiReady)
        return;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    gUiReady = false;
    gMenuVisible = false;
    gSettingsOpen = false;
    gCheatsOpen = false;
    gCheatInvincible = false;
    gCheatInstantKill = false;
    gCheatExpBoost = false;
    gCaptureAction = -1;
}

extern "C" int Pc_UiIsActive(void) {
    return gUiReady ? 1 : 0;
}

extern "C" int Pc_UiWantsCaptureInput(void) {
    // Any visible overlay (menu bar, settings/cheats window) or an in-progress
    // bind capture means game input must not reach the GBA shadow.
    return (gMenuVisible || gSettingsOpen || gCheatsOpen || gCaptureAction >= 0) ? 1 : 0;
}

extern "C" void Pc_UiProcessEvent(const void *sdlEvent) {
    const SDL_Event *ev = (const SDL_Event *)sdlEvent;
    if (!gUiReady || ev == NULL)
        return;

    if (ev->type == SDL_KEYDOWN) {
        if (gCaptureAction >= 0) {
            // A bind is pending: the next non-Esc key becomes it.
            if (ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                gCaptureAction = -1;
            } else if (ev->key.repeat == 0) {
                Pc_ConfigAddBind(gCaptureAction, 0, (int)ev->key.keysym.scancode);
                Pc_ConfigSave();
                gCaptureAction = -1;
            }
        } else if (ev->key.repeat == 0 &&
                   ev->key.keysym.scancode == SDL_SCANCODE_F1) {
            gMenuVisible = !gMenuVisible;
        }
    }

    ImGui_ImplSDL2_ProcessEvent(ev);
}

extern "C" void Pc_UiToggle(void) {
    gMenuVisible = !gMenuVisible;
}

static void Pc_UiControlsTab(void) {
    int a;

    if (ImGui::Button("Reset to Defaults")) {
        Pc_ConfigResetBinds();
        Pc_ConfigSave();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Click a bind to remove it; + to add.");

    ImGui::Separator();
    ImGui::BeginChild("controls_list");
    for (a = 0; a < PC_ACT_COUNT; a++) {
        PcActionBinds *ab = &Pc_ConfigBinds()[a];
        char pid[40];
        int i;

        ImGui::TextUnformatted(Pc_ActionName(a));
        ImGui::SameLine();

        for (i = 0; i < ab->count; i++) {
            char label[80];
            char id[96];
            Pc_BindLabel(ab->binds[i].kind, ab->binds[i].code, label, sizeof(label));
            snprintf(id, sizeof(id), "%s##del_%d_%d", label, a, i);
            if (ImGui::SmallButton(id)) {
                Pc_ConfigRemoveBind(a, ab->binds[i].kind, ab->binds[i].code);
                Pc_ConfigSave();
            }
            ImGui::SameLine();
        }

        snprintf(pid, sizeof(pid), "+##add_%d", a);
        if (ImGui::SmallButton(pid))
            ImGui::OpenPopup(pid);
        if (ImGui::BeginPopup(pid)) {
            char mitem[64];
            if (ImGui::MenuItem("Capture next key...")) {
                gCaptureAction = a;
                ImGui::CloseCurrentPopup();
            }
            snprintf(mitem, sizeof(mitem), "Mouse Left");
            if (ImGui::MenuItem(mitem)) { Pc_ConfigAddBind(a, 1, 1); Pc_ConfigSave(); }
            if (ImGui::MenuItem("Mouse Middle")) { Pc_ConfigAddBind(a, 1, 2); Pc_ConfigSave(); }
            if (ImGui::MenuItem("Mouse Right")) { Pc_ConfigAddBind(a, 1, 3); Pc_ConfigSave(); }
            if (ImGui::MenuItem("Mouse X1")) { Pc_ConfigAddBind(a, 1, 4); Pc_ConfigSave(); }
            if (ImGui::MenuItem("Mouse X2")) { Pc_ConfigAddBind(a, 1, 5); Pc_ConfigSave(); }
            ImGui::EndPopup();
        }

        ImGui::NewLine();
    }
    ImGui::EndChild();
}

static void Pc_UiBootTab(void) {
    PcBootPrefs *boot = Pc_ConfigBootPrefs();
    bool b;

    b = boot->noConsole != 0;
    if (ImGui::Checkbox("No console (log to client.log only)", &b)) { boot->noConsole = b ? 1 : 0; Pc_ConfigSave(); }
    b = boot->skipWarning != 0;
    if (ImGui::Checkbox("Skip health & safety warning", &b)) { boot->skipWarning = b ? 1 : 0; Pc_ConfigSave(); }
    b = boot->skipLogos != 0;
    if (ImGui::Checkbox("Skip logos", &b)) { boot->skipLogos = b ? 1 : 0; Pc_ConfigSave(); }
    b = boot->skipIntro != 0;
    if (ImGui::Checkbox("Skip intro / opening", &b)) { boot->skipIntro = b ? 1 : 0; Pc_ConfigSave(); }
    b = boot->autoload != 0;
    if (ImGui::Checkbox("Auto-load save at launch", &b)) { boot->autoload = b ? 1 : 0; Pc_ConfigSave(); }
    b = boot->fpsLog != 0;
    if (ImGui::Checkbox("Log FPS", &b)) { boot->fpsLog = b ? 1 : 0; Pc_ConfigSave(); }

    ImGui::Spacing();
    ImGui::TextDisabled("These mirror launch arguments (e.g. --noconsole, SkipIntro)\nand take effect the next time the game starts (Game > Restart).");
}

static const char *const kPlayerNames[PC_AUDIO_PLAYERS] = {
    "BGM", "Fanfare", "SE1", "SE2", "SE3", "SE4", "SE5", "SE6"
};

// One row per m4a player: tempo (%) and pitch (semitones) sliders.
static void Pc_UiAudioPlayerTable(PcAudioPrefs *ap)
{
    if (ImGui::BeginTable("player_tempo", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Tempo", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pitch", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < PC_AUDIO_PLAYERS; i++)
        {
            int t = ap->tempoScale[i];
            int p = ap->pitchShift[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPlayerNames[i]);
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SliderInt("##tempo", &t, 50, 200, "%d%%"))
            {
                ap->tempoScale[i] = t;
                Pc_ConfigSave();
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SliderInt("##pitch", &p, -12, 12, "%+d st"))
            {
                ap->pitchShift[i] = p;
                Pc_ConfigSave();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void Pc_UiAudioTab(void) {
    PcAudioPrefs *ap = Pc_ConfigAudioPrefs();
    int v;

    v = ap->masterVolume;
    if (ImGui::SliderInt("Volume", &v, 0, 100, "%d%%")) {
        ap->masterVolume = v;
        Pc_ConfigSave();
    }
    ImGui::SameLine();
    {
        bool b = ap->muted != 0;
        if (ImGui::Checkbox("Mute", &b)) {
            ap->muted = b ? 1 : 0;
            Pc_ConfigSave();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Mix");
    v = ap->dsVolume;
    if (ImGui::SliderInt("DirectSound", &v, 0, 200, "%d%%")) {
        ap->dsVolume = v;
        Pc_ConfigSave();
    }
    v = ap->psgVolume;
    if (ImGui::SliderInt("PSG (square/wave/noise)", &v, 0, 200, "%d%%")) {
        ap->psgVolume = v;
        Pc_ConfigSave();
    }
    v = ap->saturate;
    if (ImGui::SliderInt("Limiter threshold", &v, 50, 100, "%d%%")) {
        ap->saturate = v;
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("DirectSound = sample voices, PSG = the 4 CGB channels.\nLower the limiter threshold for a softer, compressed sound.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Equalizer");
    v = ap->bassDb;
    if (ImGui::SliderInt("Bass", &v, -15, 15, "%+d dB")) {
        ap->bassDb = v;
        Pc_ConfigSave();
    }
    v = ap->trebleDb;
    if (ImGui::SliderInt("Treble", &v, -15, 15, "%+d dB")) {
        ap->trebleDb = v;
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("Shelving EQ on the final mix: bass at 200 Hz, treble at 4 kHz.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Stereo");
    v = ap->stereoWidth;
    if (ImGui::SliderInt("Width", &v, 0, 200, "%d%%")) {
        ap->stereoWidth = v;
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("100% is normal; 0% is mono; 200% doubles the spread.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Reverb");
    {
        static const char *modes[] = { "Off", "Follow song", "Override" };
        v = ap->reverbMode;
        if (v < 0 || v > 2)
            v = 1;
        if (ImGui::Combo("##reverb_mode", &v, modes, 3)) {
            ap->reverbMode = v;
            Pc_ConfigSave();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("current song: %d", Pc_AudioSongReverb());
        if (ap->reverbMode == 2) {
            v = ap->reverbOverride;
            if (ImGui::SliderInt("Amount", &v, 0, 127)) {
                ap->reverbOverride = v;
                Pc_ConfigSave();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    {
        bool b = ap->lowPass != 0;
        if (ImGui::Checkbox("GBA analog low-pass filter", &b)) {
            ap->lowPass = b ? 1 : 0;
            Pc_ConfigSave();
        }
        v = ap->lowPassCutoff;
        if (ImGui::SliderInt("Cutoff", &v, 1000, 16000, "%d Hz")) {
            ap->lowPassCutoff = v;
            Pc_ConfigSave();
        }
        ImGui::TextDisabled("The GBA's output is band-limited; this warms the mix toward the hardware.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Tempo & Pitch");
    ImGui::TextDisabled("Per m4a player; SE players are single-shot effects.");
    if (ImGui::BeginChild("player_prefs", ImVec2(0.0f, 230.0f), true)) {
        Pc_UiAudioPlayerTable(ap);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reset to Defaults")) {
        PcAudioPrefs d;
        int i;
        memset(&d, 0, sizeof(d));
        d.masterVolume = 100;
        d.dsVolume = 100;
        d.psgVolume = 100;
        d.saturate = 85;
        d.reverbMode = 1;
        d.reverbOverride = 60;
        d.lowPass = 1;
        d.lowPassCutoff = 2200;
        d.bassDb = 0;
        d.trebleDb = 0;
        d.stereoWidth = 100;
        for (i = 0; i < PC_AUDIO_PLAYERS; i++) {
            d.tempoScale[i] = 100;
            d.pitchShift[i] = 0;
        }
        *ap = d;
        Pc_ConfigSave();
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture 30s WAV")) {
        char dir[1024 + 1];
        char path[1024 + 1];
        Pc_ExeDir(dir, sizeof(dir), NULL);
        snprintf(path, sizeof(path), "%spmd-red-audio.wav", dir);
        Pc_AudioWavDump(path, 30);
    }
    ImGui::TextDisabled("Audio is rendered natively (GBA sample math at the host rate).\nChanges apply immediately.");
}

static void Pc_UiGraphicsTab(void) {
    PcVideoPrefs *vp = Pc_ConfigVideoPrefs();
    int v;
    bool b;

    ImGui::TextUnformatted("Window");
    v = vp->windowScale;
    if (ImGui::SliderInt("Scale", &v, PC_VIDEO_SCALE_MIN, PC_VIDEO_SCALE_MAX, "%dx")) {
        vp->windowScale = v;
        Pc_VideoResizeScale(v);
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("Multiplier of the 240x160 internal frame.\nResizes the window immediately.");

    b = vp->fullscreen != 0;
    if (ImGui::Checkbox("Fullscreen", &b)) {
        vp->fullscreen = b ? 1 : 0;
        Pc_VideoSetFullscreen(b ? 1 : 0);
        Pc_ConfigSave();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Scaling");
    {
        static const char *modes[] = { "Integer", "Fit", "Stretch" };
        v = vp->scaleMode;
        if (v < 0 || v > 2)
            v = PC_SCALE_INTEGER;
        if (ImGui::Combo("Mode", &v, modes, 3)) {
            vp->scaleMode = v;
            Pc_VideoSetScaleMode(v);
            Pc_ConfigSave();
        }
        ImGui::TextDisabled("Integer = sharp pixels, Fit = fills the window\npreserving ratio, Stretch = fills ignoring ratio.");
    }

    b = vp->smoothing != 0;
    if (ImGui::Checkbox("Bilinear smoothing", &b)) {
        vp->smoothing = b ? 1 : 0;
        Pc_VideoSetSmoothing(b ? 1 : 0);
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("Nearest is crisp and pixelated; bilinear blurs when upscaled.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Backdrop");
    {
        float col[3];
        col[0] = (float)vp->letterboxR / 255.0f;
        col[1] = (float)vp->letterboxG / 255.0f;
        col[2] = (float)vp->letterboxB / 255.0f;
        if (ImGui::ColorEdit3("Letterbox", col)) {
            vp->letterboxR = (int)(col[0] * 255.0f + 0.5f);
            vp->letterboxG = (int)(col[1] * 255.0f + 0.5f);
            vp->letterboxB = (int)(col[2] * 255.0f + 0.5f);
            Pc_VideoSetLetterbox(vp->letterboxR, vp->letterboxG, vp->letterboxB);
            Pc_ConfigSave();
        }
        ImGui::TextDisabled("Color shown around the frame when the window\ndoesn't match the 3:2 ratio.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    b = vp->vsync != 0;
    if (ImGui::Checkbox("Vertical sync", &b)) {
        vp->vsync = b ? 1 : 0;
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("Waits for the display refresh to avoid tearing.\nTakes effect the next time the game starts (Game > Restart).");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Frame rate");
    {
        static const char *hzLabels[] = {
            "Match display", "60 Hz", "120 Hz", "144 Hz", "240 Hz", "Uncapped"
        };
        static const int hzVals[] = { 0, 60, 120, 144, 240, -1 };
        int cur = 0;
        for (v = 0; v < 6; v++) {
            if (vp->presentHz == hzVals[v]) {
                cur = v;
                break;
            }
        }
        if (ImGui::Combo("Present rate", &cur, hzLabels, 6)) {
            vp->presentHz = hzVals[cur];
            Pc_ConfigSave();
        }
        ImGui::TextDisabled("Game logic stays locked at 60Hz; the frame is presented\nat this rate (or the monitor refresh) in between.");
    }

    b = vp->interpolate != 0;
    if (ImGui::Checkbox("Interpolate frames", &b)) {
        vp->interpolate = b ? 1 : 0;
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("Blend between 60Hz logic frames at high refresh.\n(Reserved - not yet implemented.)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Overlay");
    b = vp->showFps != 0;
    if (ImGui::Checkbox("Show FPS", &b)) {
        vp->showFps = b ? 1 : 0;
        Pc_ConfigSave();
    }
    b = vp->showTickrate != 0;
    if (ImGui::Checkbox("Show tickrate", &b)) {
        vp->showTickrate = b ? 1 : 0;
        Pc_ConfigSave();
    }
    ImGui::TextDisabled("Draws in the top-right corner of the frame.\nFPS = present rate, TR = game logic ticks/s.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reset to Defaults")) {
        PcVideoPrefs d;
        memset(&d, 0, sizeof(d));
        d.windowScale = 3;
        d.scaleMode = PC_SCALE_INTEGER;
        d.vsync = 1;
        *vp = d;
        Pc_VideoSetScaleMode(vp->scaleMode);
        Pc_VideoSetSmoothing(vp->smoothing);
        Pc_VideoSetFullscreen(vp->fullscreen);
        Pc_VideoSetLetterbox(vp->letterboxR, vp->letterboxG, vp->letterboxB);
        Pc_VideoResizeScale(vp->windowScale);
        Pc_ConfigSave();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Most changes apply immediately.");
}

// Cheat item quick list for the Give items combo. The in-game item table is
// charmapped, so plain-ASCII names are listed here (ids from constants/item.h).
static const int kCheatItemIds[] = {
    ITEM_REVIVER_SEED, ITEM_ORAN_BERRY, ITEM_SITRUS_BERRY, ITEM_MAX_ELIXIR,
    ITEM_APPLE, ITEM_BIG_APPLE, ITEM_HUNGER_SEED, ITEM_PLAIN_SEED,
    ITEM_ESCAPE_ORB, ITEM_PETRIFY_ORB, ITEM_TRAWL_ORB, ITEM_REVIVER_ORB,
    ITEM_X_RAY_SPECS, ITEM_PECHA_SCARF, ITEM_WARP_SCARF, ITEM_FRIEND_BOW,
    ITEM_JOY_SEED, ITEM_GOLD_RIBBON,
};
static const char *const kCheatItemNames[] = {
    "Reviver Seed", "Oran Berry", "Sitrus Berry", "Max Elixir",
    "Apple", "Big Apple", "Hunger Seed", "Plain Seed",
    "Escape Orb", "Petrify Orb", "Trawl Orb", "Reviver Orb",
    "X-Ray Specs", "Pecha Scarf", "Warp Scarf", "Friend Bow",
    "Joy Seed", "Gold Ribbon",
};
#define CHEAT_ITEM_COUNT (sizeof(kCheatItemIds) / sizeof(kCheatItemIds[0]))

// Rescue-team ranks, indexed by GetRescueTeamRank() (rescue_team_info.h).
static const char *const kTeamRankNames[] = {
    "Normal", "Bronze", "Silver", "Gold", "Platinum", "Diamond", "Lucario"
};

// Recruit list state: all species names (decoded to ASCII), a case-insensitive
// search filter and a page index over the filtered results.
static std::vector<std::string> sSpeciesNames;  // display names, index = species - 1
static bool sSpeciesNamesReady = false;
static char sRecruitSearch[64] = "";
static int  sRecruitPage = 0;
static char sRecruitMsg[128] = "";

static void Pc_UiBuildSpeciesNames(void) {
    int count = Pc_CheatSpeciesCount();
    int i;

    sSpeciesNames.clear();
    sSpeciesNames.reserve((size_t)count);
    for (i = 1; i < count; i++) { // skip MONSTER_NONE (0)
        char buf[64];
        Pc_CheatSpeciesDisplayName(i, buf, sizeof(buf));
        if (buf[0] == '\0')
            snprintf(buf, sizeof(buf), "Species %d", i);
        sSpeciesNames.push_back(buf);
    }
    if (!sSpeciesNames.empty())
        sSpeciesNamesReady = true; // otherwise retry next frame
}

static bool Pc_UiSpeciesMatches(const char *name, const char *query) {
    if (query[0] == '\0')
        return true;
    {
        std::string a(name);
        std::string b(query);
        for (size_t i = 0; i < a.size(); i++)
            a[i] = (char)std::tolower((unsigned char)a[i]);
        for (size_t i = 0; i < b.size(); i++)
            b[i] = (char)std::tolower((unsigned char)b[i]);
        return a.find(b) != std::string::npos;
    }
}

static void Pc_UiCheatsWindow(void) {
    static int sMoneyInput = 0;
    static int sSavingsInput = 0;
    static int sItemSel = 0;
    static int sItemQty = 1;
    static char sLastMsg[128] = "";

    ImGui::SetNextWindowSize(ImVec2(430, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cheats", &gCheatsOpen)) {
        ImGui::End();
        return;
    }

    if (!Pc_CheatTeamReady()) {
        ImGui::TextDisabled("Team data isn't loaded yet.\nStart a game / load a save first.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Cheats apply immediately; money/items/recruits persist\non the next in-game save.");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Team")) {
        if (ImGui::InputInt("Money", &sMoneyInput))
            Pc_CheatSetMoney(sMoneyInput);
        sMoneyInput = (int)Pc_CheatGetMoney();
        if (ImGui::InputInt("Bank savings", &sSavingsInput))
            Pc_CheatSetSavings(sSavingsInput);
        sSavingsInput = (int)Pc_CheatGetSavings();
        if (ImGui::Button("Max Money"))
            Pc_CheatSetMoney(99999);
        ImGui::SameLine();
        if (ImGui::Button("Max Savings"))
            Pc_CheatSetSavings(9999999);

        ImGui::Separator();
        {
            static int sRankSel = 6;
            int curRank = Pc_CheatGetTeamRank();
            if (sRankSel != curRank)
                sRankSel = curRank; // keep the combo preview in sync with reality
            if (ImGui::Combo("Rank", &sRankSel, kTeamRankNames, 7))
                Pc_CheatSetTeamRank(sRankSel);
            ImGui::SameLine();
            ImGui::TextDisabled("current: %s (%d pts)",
                                kTeamRankNames[curRank], Pc_CheatGetTeamRankPts());
        }
    }

    if (ImGui::CollapsingHeader("Give items")) {
        ImGui::Combo("Item", &sItemSel, kCheatItemNames, (int)CHEAT_ITEM_COUNT);
        if (ImGui::InputInt("Quantity", &sItemQty)) {
            if (sItemQty < 1)
                sItemQty = 1;
            if (sItemQty > 99)
                sItemQty = 99;
        }
        if (ImGui::Button("Give")) {
            int added = Pc_CheatGiveItem(kCheatItemIds[sItemSel], sItemQty);
            snprintf(sLastMsg, sizeof(sLastMsg), "Added %d of %d requested.", added, sItemQty);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(sLastMsg);
    }

    if (ImGui::CollapsingHeader("Dungeon")) {
        if (!Pc_CheatInDungeon()) {
            ImGui::TextDisabled("Enter a dungeon to use these.");
        } else {
            if (ImGui::Button("Heal team (HP + belly)"))
                Pc_CheatHealTeam();
            ImGui::SameLine();
            ImGui::Checkbox("Invincible leader", &gCheatInvincible);

            ImGui::Checkbox("Instant kill (enemies at 1 HP)", &gCheatInstantKill);
            ImGui::Checkbox("Boost enemy EXP (1.5x)", &gCheatExpBoost);

            ImGui::Separator();
            ImGui::TextUnformatted("Leveling");
            {
                static int sLevelAmount = 1;
                if (ImGui::InputInt("Levels", &sLevelAmount)) {
                    if (sLevelAmount < 1)
                        sLevelAmount = 1;
                    if (sLevelAmount > 99)
                        sLevelAmount = 99;
                }
                if (ImGui::Button("Level up team"))
                    Pc_CheatLevelUpTeam(sLevelAmount);
                ImGui::SameLine();
                if (ImGui::Button("Max level (100)"))
                    Pc_CheatLevelUpTeam(100);
            }
            {
                static int sExpAmount = 500;
                if (ImGui::InputInt("EXP", &sExpAmount)) {
                    if (sExpAmount < 1)
                        sExpAmount = 1;
                    if (sExpAmount > 99999)
                        sExpAmount = 99999;
                }
                if (ImGui::Button("Give EXP to team"))
                    Pc_CheatGiveExpToTeam(sExpAmount);
            }
        }
    }

    if (ImGui::CollapsingHeader("Recruit")) {
        if (!sSpeciesNamesReady)
            Pc_UiBuildSpeciesNames();

        if (ImGui::InputText("Search", sRecruitSearch, sizeof(sRecruitSearch)))
            sRecruitPage = 0;
        ImGui::TextDisabled("Case-insensitive substring match; Recruit adds the species.");

        {
            std::vector<int> matches;
            int total;
            int pageCount;
            int start, end;
            int i;

            for (i = 0; i < (int)sSpeciesNames.size(); i++) {
                if (Pc_UiSpeciesMatches(sSpeciesNames[i].c_str(), sRecruitSearch))
                    matches.push_back(i + 1); // species id = index + 1
            }
            total = (int)matches.size();
            pageCount = (total + 14) / 15; // 15 rows per page
            if (pageCount < 1)
                pageCount = 1;
            if (sRecruitPage >= pageCount)
                sRecruitPage = pageCount - 1;
            start = sRecruitPage * 15;
            end = start + 15;
            if (end > total)
                end = total;

            if (ImGui::BeginTable("recruit_table", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Dex", ImGuiTableColumnFlags_WidthFixed, 46.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 68.0f);
                ImGui::TableHeadersRow();
                for (i = start; i < end; i++) {
                    int species = matches[i];
                    char lbl[32];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", species);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(sSpeciesNames[species - 1].c_str());
                    ImGui::TableSetColumnIndex(2);
                    snprintf(lbl, sizeof(lbl), "Recruit##%d", species);
                    if (ImGui::SmallButton(lbl)) {
                        int ok = Pc_CheatRecruitSpecies(species);
                        snprintf(sRecruitMsg, sizeof(sRecruitMsg),
                                 ok ? "Recruited %s!" : "Failed (team / friend area full).",
                                 sSpeciesNames[species - 1].c_str());
                    }
                }
                ImGui::EndTable();
            }

            if (ImGui::Button("Prev") && sRecruitPage > 0)
                sRecruitPage--;
            ImGui::SameLine();
            ImGui::Text("Page %d / %d (%d match%s)", sRecruitPage + 1, pageCount, total,
                        total == 1 ? "" : "es");
            ImGui::SameLine();
            if (ImGui::Button("Next") && sRecruitPage < pageCount - 1)
                sRecruitPage++;
            ImGui::TextUnformatted(sRecruitMsg);
        }
    }

    ImGui::End();
}

static void Pc_UiSettingsWindow(void) {
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &gSettingsOpen)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("settings_tabs")) {
        if (ImGui::BeginTabItem("Graphics")) {
            ImGui::Spacing();
            Pc_UiGraphicsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            Pc_UiAudioTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            Pc_UiControlsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Boot")) {
            Pc_UiBootTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    // Bind-capture modal: shown while a "Capture next key..." is pending.
    if (gCaptureAction >= 0) {
        ImGui::OpenPopup("capture_key");
        if (ImGui::BeginPopupModal("capture_key", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Press a key for %s...", Pc_ActionName(gCaptureAction));
            ImGui::TextUnformatted(msg);
            ImGui::TextDisabled("Esc cancels");
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                gCaptureAction = -1;
            ImGui::EndPopup();
        }
    }
}

extern "C" void Pc_UiRender(void) {
    if (!gUiReady)
        return;

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (gMenuVisible && ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Game")) {
            if (ImGui::Selectable("Restart"))
                Pc_RequestRestart();
            if (ImGui::Selectable("Settings"))
                gSettingsOpen = true;
            if (ImGui::Selectable("Exit"))
                Pc_RequestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Cheats")) {
            if (ImGui::Selectable("Open Cheats"))
                gCheatsOpen = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // The continuous toggles keep working even while the Cheats window is
    // closed (they run once per logic frame, after the game tick).
    if (gCheatInvincible && Pc_CheatInDungeon())
        Pc_CheatInvincibleLeaderTick();
    if (gCheatInstantKill && Pc_CheatInDungeon())
        Pc_CheatInstantKillTick();
    if (gCheatExpBoost && Pc_CheatInDungeon())
        Pc_CheatExpBoostTick();

    if (gSettingsOpen)
        Pc_UiSettingsWindow();

    if (gCheatsOpen)
        Pc_UiCheatsWindow();

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(
        ImGui::GetDrawData(), (SDL_Renderer *)Pc_VideoGetSdlRenderer());
}

#else // !HAVE_SDL2 — headless stubs so the C callers always link.

extern "C" void Pc_UiInit(void) {}
extern "C" void Pc_UiShutdown(void) {}
extern "C" int Pc_UiIsActive(void) { return 0; }
extern "C" int Pc_UiWantsCaptureInput(void) { return 0; }
extern "C" void Pc_UiProcessEvent(const void *sdlEvent) { (void)sdlEvent; }
extern "C" void Pc_UiToggle(void) {}
extern "C" void Pc_UiRender(void) {}

#endif // HAVE_SDL2