// platform/pc/cheats_stub.c — empty cheat helpers for the smoke binary.
//
// The smoke binary (make pc) links no game sources, so the real game-state
// helpers in cheats_pc.c (game-link only) are stubbed out here to keep the
// overlay linking in both binaries.
#include "gba/types.h"

int Pc_CheatTeamReady(void) { return 0; }
s32 Pc_CheatGetMoney(void) { return 0; }
void Pc_CheatSetMoney(s32 value) { (void)value; }
s32 Pc_CheatGetSavings(void) { return 0; }
void Pc_CheatSetSavings(s32 value) { (void)value; }
int Pc_CheatGiveItem(int itemId, int quantity) { (void)itemId; (void)quantity; return 0; }
int Pc_CheatInDungeon(void) { return 0; }
void Pc_CheatHealTeam(void) {}
void Pc_CheatInvincibleLeaderTick(void) {}
int Pc_CheatRecruitSpecies(int species) { (void)species; return 0; }
int Pc_CheatRecruitMon(const PcCheatMonSpec *spec) { (void)spec; return 0; }
void Pc_CheatApplyAbilitiesTick(int species, int ability1, int ability2) { (void)species; (void)ability1; (void)ability2; }
const char *Pc_CheatSpeciesName(int species) { (void)species; return ""; }
int Pc_CheatSpeciesCount(void) { return 0; }
void Pc_CheatSpeciesDisplayName(int species, char *out, size_t cap) { (void)species; (void)cap; if (out != NULL) out[0] = '\0'; }
int Pc_CheatMoveCount(void) { return 0; }
void Pc_CheatMoveDisplayName(int moveId, char *out, size_t cap) { (void)moveId; (void)cap; if (out != NULL) out[0] = '\0'; }
int Pc_CheatAbilityCount(void) { return 0; }
void Pc_CheatAbilityDisplayName(int abilityId, char *out, size_t cap) { (void)abilityId; (void)cap; if (out != NULL) out[0] = '\0'; }
int Pc_CheatGetTeamRank(void) { return 0; }
s32 Pc_CheatGetTeamRankPts(void) { return 0; }
void Pc_CheatSetTeamRank(int rank) { (void)rank; }
void Pc_CheatLevelUpTeam(int levels) { (void)levels; }
void Pc_CheatGiveExpToTeam(int exp) { (void)exp; }
void Pc_CheatExpBoostTick(void) {}
void Pc_CheatExpMultiplierTick(float multiplier) { (void)multiplier; }
void Pc_CheatInstantKillTick(void) {}
void Pc_CheatUnlockAllFriendAreas(void) {}
void Pc_CheatUnlockAllDungeons(void) {}
void Pc_CheatSkipToEndTick(void) {}