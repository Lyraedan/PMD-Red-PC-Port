// platform/pc/gba_shim.h — PC backend declarations (host builds only).
//
// The GBA headers themselves branch on PLATFORM_PC now (see include/gba/
// defines.h, io_reg.h, macro.h), so this header only declares the C functions
// the PC backends provide: BIOS syscall replacements + video/input/audio/save.
//
// Compile game sources with -DPLATFORM_PC; no -include flag needed.
#ifndef PMDRED_PC_GBA_SHIM_H
#define PMDRED_PC_GBA_SHIM_H

#include <stddef.h>

#ifdef PLATFORM_PC

#include "config_pc.h" // rebindable controls + boot-launch prefs

#ifdef __cplusplus
extern "C" {
#endif

// ---- Crash diagnostics + client.log (crash_win.c) ----
void Pc_InstallCrashHandler(void);
void Pc_ExeDir(char *out, size_t cap, const char *argv0); // dir of running exe, trailing sep
void Pc_LogOpen(const char *exeDir, int wantConsole, const char *logFile); // client.log sink
int Pc_LogPrintf(const char *fmt, ...); // lifecycle/crash record, always flushed
void Pc_LogFlush(void);
void Pc_LogClose(const char *reason); // final "session end" line + close
void Pc_FatalMessage(const char *msg); // log + native error dialog

// ---- BIOS syscall replacements (cpu_pc.c) ----
// Signatures match libagbsyscall so game call sites need no edits.
void Pc_MemInit(void);       // zero VRAM/PLTT/OAM + regs, KEYINPUT idle
void Pc_VBlankCommit(void);  // PC replacement for the VBlank_CB register reloads
void Pc_RequestVBlank(void); // set by the host 60Hz loop; consumed by VBlankIntrWait
void Pc_SetPaced(int on);    // enable 60Hz render/pacing inside VBlankIntrWait
unsigned int Pc_VBlankFrameCount(void); // frames rendered via VBlankIntrWait
void Pc_EnableFpsLog(int on);           // print a rolling measured FPS (~60Hz)
double Pc_MeasuredFps(void);            // last measured FPS (0 until first window)
double Pc_OverlayTickrate(void);        // game logic ticks/sec (0 until first window)

// ---- Video backend (video_pc.c) ----
void Pc_VideoInit(int scale);      // init framebuffer/window (SDL if HAVE_SDL2)
void Pc_VideoPresent(void);        // composite shadows -> screen, 240x160 scaled
void Pc_VideoRenderAndUpload(void);// composite shadows + upload to texture (no present)
void Pc_VideoPresentOnly(void);    // blit+present the current texture (no re-render)
void Pc_VideoShutdown(void);
void Pc_VideoDumpPPM(const char *path); // headless framebuffer dump (no SDL needed)
void *Pc_VideoGetSdlWindow(void);   // SDL_Window* or NULL (headless / no SDL2)
void *Pc_VideoGetSdlRenderer(void); // SDL_Renderer* or NULL (headless / no SDL2)

// Live graphics settings (no-op when built headless). Called by the ImGui
// settings window after the config prefs are edited; vsync has no live path
// (renderer flag) and is applied at the next Pc_VideoInit instead.
void Pc_VideoSetFullscreen(int on); // toggle fullscreen on the current window
void Pc_VideoSetScaleMode(int mode); // PC_SCALE_INTEGER/FIT/STRETCH
void Pc_VideoSetSmoothing(int on);  // 0 nearest, 1 bilinear texture scaling
void Pc_VideoSetLetterbox(int r, int g, int b); // backdrop color (0-255 each)
void Pc_VideoResizeScale(int scale); // resize the window to 240*scale x 160*scale
int Pc_VideoDisplayHz(void);         // detected display refresh (0 = unknown/headless)

// ---- Input backend (input_pc.c) ----
void Pc_InputInit(void);
void Pc_InputPump(void); // refresh REG_KEYINPUT shadow (active-low, KEYS_MASK idle)
void Pc_InputShutdown(void);
int Pc_QuitRequested(void); // true when window close / quit requested
void Pc_RequestQuit(void);  // request a clean shutdown (window close / Exit menu)
void Pc_SetAutopress(int startFrame, int durFrames, u16 keys); // scripted key hold (CI)

// Raw key capture for the in-game Controls settings screen. While a capture is
// pending the next non-Esc keydown is recorded; the game polls the result each
// frame (the frame tick pumps SDL events via Pc_InputPump).
void Pc_InputStartKeyCapture(void);
int  Pc_InputKeyCaptureResult(void);   // 0 = waiting, 1 = captured, -1 = cancelled
int  Pc_InputKeyCaptureScancode(void); // SDL_Scancode captured (valid when result == 1)

// ---- ImGui overlay backend (ui_pc.cpp) ----
void Pc_UiInit(void);          // create context + SDL2/SDL_Renderer backends
void Pc_UiShutdown(void);
void Pc_UiProcessEvent(const void *sdlEvent); // forward one SDL_Event*; F1 toggles menu
void Pc_UiRender(void);        // NewFrame + menu/demo + RenderDrawData (draw on top)
int Pc_UiIsActive(void);       // 1 once the context is live (window/renderer present)
int Pc_UiWantsCaptureInput(void); // 1 while any overlay is shown (suppress game input)
void Pc_UiToggle(void);        // show/hide the menu bar
void Pc_RequestRestart(void);  // relaunch the game (implies quit)
int Pc_RestartRequested(void); // true once Restart was chosen

// ---- Audio backend (audio_pc.c, mute-first) ----
void Pc_AudioInit(void);
void Pc_AudioFrame(void); // called once per 60Hz tick; noop until sequencer lands
void Pc_AudioShutdown(void);
int  Pc_AudioSongReverb(void);             // current song reverb amount (0-127)
void Pc_AudioWavDump(const char *path, int maxSeconds); // capture mix to WAV
void Pc_AudioTopUp(int targetSamples);     // keep the SDL queue above a cushion

// ---- Cheats (cheats_pc.c real / cheats_stub.c empty) ----
// Game-state helpers for the ImGui Cheats window. The real implementations
// are linked only into pmd-red-game; the smoke binary links empty stubs.
typedef struct PcCheatMonSpec {
    int species;
    int level;       // 1..100
    u16 moves[4];    // move ids (0 = empty slot)
    int heldItem;    // item id (0 = none)
    int iq;
    int hp;          // 0 = auto from level
    int atk;
    int spAtk;
    int def;
    int spDef;
    const char *name; // NULL = species default
    int ability1;    // -1 = species default (applied to in-dungeon entity)
    int ability2;    // -1 = species default
} PcCheatMonSpec;

int  Pc_CheatTeamReady(void);                    // team/save state is live
s32  Pc_CheatGetMoney(void);
void Pc_CheatSetMoney(s32 value);                // clamped to MAX_TEAM_MONEY
s32  Pc_CheatGetSavings(void);
void Pc_CheatSetSavings(s32 value);              // clamped to MAX_TEAM_SAVINGS
int  Pc_CheatGiveItem(int itemId, int quantity); // items actually added
int  Pc_CheatItemCount(void);                    // NUMBER_OF_ITEM_IDS
void Pc_CheatItemDisplayName(int itemId, char *out, size_t cap); // charmapped item name -> ASCII
void Pc_CheatItemDescription(int itemId, char *out, size_t cap); // charmapped item desc -> ASCII
int  Pc_CheatInDungeon(void);                    // leader present in a dungeon
void Pc_CheatHealTeam(void);                     // full HP + belly for the team
void Pc_CheatInvincibleLeaderTick(void);         // leader HP = maxHP (per frame)
int  Pc_CheatRecruitSpecies(int species);        // unlock friend area + add lvl-1 mon
int  Pc_CheatRecruitMon(const PcCheatMonSpec *spec); // custom recruit (level/moves/item/name/stats)
void Pc_CheatApplyAbilitiesTick(int species, int ability1, int ability2); // in-dungeon override
const char *Pc_CheatSpeciesName(int species);    // species display name
int  Pc_CheatSpeciesCount(void);                 // NUM_MONSTERS
void Pc_CheatSpeciesDisplayName(int species, char *out, size_t cap); // charmapped name -> ASCII
int  Pc_CheatMoveCount(void);                    // number of move ids
void Pc_CheatMoveDisplayName(int moveId, char *out, size_t cap);    // charmapped move -> ASCII
int  Pc_CheatAbilityCount(void);                 // NUM_ABILITIES
void Pc_CheatAbilityDisplayName(int abilityId, char *out, size_t cap); // charmapped ability -> ASCII
int  Pc_CheatGetTeamRank(void);                  // 0..MAX_TEAM_RANKS-1
s32  Pc_CheatGetTeamRankPts(void);
void Pc_CheatSetTeamRank(int rank);              // Normal..Lucario
void Pc_CheatLevelUpTeam(int levels);            // in-dungeon, all team members
void Pc_CheatGiveExpToTeam(int exp);             // in-dungeon, all team members
void Pc_CheatExpBoostTick(void);                 // mark enemies 1.5x EXP (per frame)
void Pc_CheatExpMultiplierTick(float multiplier); // custom whole-team XP multiplier (1.0 = off)
void Pc_CheatInstantKillTick(void);              // enemies at 1 HP (per frame)
void Pc_CheatUnlockAllFriendAreas(void);         // buy every friend area
void Pc_CheatUnlockAllDungeons(void);            // mark every dungeon available
void Pc_CheatSkipToEndTick(void);                // on stairs, floor = last floor

// ---- Save backend (save_pc.c) ----
void Pc_SaveInit(const char *dir); // host save dir (default: exe dir / cwd)
void Pc_SaveFlush(void);
int Pc_SaveRead(s32 sector, u8 *dest, s32 size);      // read flash sector range
int Pc_SaveWrite(s32 sector, u8 *src, s32 size);      // erase+write flash sector range
int Pc_SaveEraseChip(void);                           // reset image to all-0xFF

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PLATFORM_PC
#endif // PMDRED_PC_GBA_SHIM_H
