// platform/pc/cheats_pc.c — game-state cheat helpers for the ImGui overlay.
//
// Linked only into the full-game binary (pmd-red-game); the smoke binary uses
// the empty stubs in cheats_stub.c. Everything here reads/writes the live
// decompiled game state (the same globals the game logic uses), so cheats
// apply immediately from the overlay and persist when the game next saves.
#include "gba/gba.h"
#include "gba_shim.h"

#include "constants/dungeon.h"
#include "constants/global.h"
#include "constants/item.h"
#include "constants/monster.h"
#include "dungeon_leveling.h"
#include "dungeon_range.h"
#include "dungeon_util.h"
#include "friend_area.h"
#include "items.h"
#include "pokemon.h"
#include "rescue_team_info.h"
#include "run_dungeon.h"
#include "structs/dungeon_entity.h"
#include "structs/str_dungeon_location.h"
#include "structs/str_pokemon.h"

int Pc_CheatTeamReady(void)
{
    return (gTeamInventoryRef != NULL && gRecruitedPokemonRef != NULL && gFriendAreas != NULL);
}

s32 Pc_CheatGetMoney(void)
{
    if (gTeamInventoryRef == NULL)
        return 0;
    return gTeamInventoryRef->teamMoney;
}

void Pc_CheatSetMoney(s32 value)
{
    if (gTeamInventoryRef == NULL)
        return;
    if (value < 0)
        value = 0;
    if (value > MAX_TEAM_MONEY)
        value = MAX_TEAM_MONEY;
    gTeamInventoryRef->teamMoney = value;
}

s32 Pc_CheatGetSavings(void)
{
    if (gTeamInventoryRef == NULL)
        return 0;
    return gTeamInventoryRef->teamSavings;
}

void Pc_CheatSetSavings(s32 value)
{
    if (gTeamInventoryRef == NULL)
        return;
    if (value < 0)
        value = 0;
    if (value > MAX_TEAM_SAVINGS)
        value = MAX_TEAM_SAVINGS;
    gTeamInventoryRef->teamSavings = value;
}

int Pc_CheatGiveItem(int itemId, int quantity)
{
    int added = 0;
    int i;

    if (gTeamInventoryRef == NULL || quantity <= 0)
        return 0;
    if (itemId <= 0 || itemId >= NUMBER_OF_ITEM_IDS)
        return 0;

    for (i = 0; i < quantity; i++) {
        if (AddItemIdToInventory((u8)itemId, FALSE))
            added++;
        else
            break; // bag full
    }
    FillInventoryGaps();
    return added;
}

int Pc_CheatInDungeon(void)
{
    return (gDungeon != NULL && GetLeader() != NULL);
}

void Pc_CheatHealTeam(void)
{
    s32 i;

    if (gDungeon == NULL)
        return;
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *entity = gDungeon->teamPokemon[i];
        if (entity != NULL && EntityIsValid(entity)) {
            EntityInfo *info = GetEntInfo(entity);
            info->HP = info->maxHPStat;
            info->belly = info->maxBelly;
        }
    }
}

void Pc_CheatInvincibleLeaderTick(void)
{
    Entity *leader = GetLeader();

    if (leader != NULL && EntityIsValid(leader)) {
        EntityInfo *info = GetEntInfo(leader);
        info->HP = info->maxHPStat;
    }
}

int Pc_CheatRecruitSpecies(int species)
{
    DungeonLocation location;

    if (gRecruitedPokemonRef == NULL || gFriendAreas == NULL)
        return 0;
    if (species <= 0 || species >= NUM_MONSTERS)
        return 0;

    // The game's own recruit logic refuses species whose friend area isn't
    // bought (see TryAddPokemonToRecruited), so unlock it first (mirrors the
    // dev debug menu, src/debug_menu1.c).
    UnlockFriendArea(GetFriendArea(species));

    location.id = DUNGEON_MT_THUNDER_PEAK;
    location.floor = 0;
    if (TryAddLevel1PokemonToRecruited(species, NULL, ITEM_NOTHING, &location, NULL) != NULL)
        return 1;
    return 0;
}

const char *Pc_CheatSpeciesName(int species)
{
    return GetMonSpecies(species);
}

void Pc_CheatSpeciesDisplayName(int species, char *out, size_t cap)
{
    // The species table stores names in the GBA charmap: plain ASCII letters,
    // digits and '-'./' ' are single bytes, but apostrophe / comma / gender
    // symbols are multi-byte or non-ASCII (see charmap.txt). Decode those to
    // plain text so ImGui can display and search them.
    const char *src;
    size_t o = 0;
    int guard = 0;

    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (species < 0 || species >= NUM_MONSTERS)
        return;
    src = GetMonSpecies(species);
    if (src == NULL)
        return;

    while (*src != '\0' && o + 1 < cap && guard < 32) {
        unsigned char c = (unsigned char)*src;
        guard++;
        if (c == 0x7E && (unsigned char)src[1] == 0x32 && (unsigned char)src[2] == 0x32) {
            out[o++] = '"';
            src += 3;
        } else if (c == 0x7E && (unsigned char)src[1] == 0x32 && (unsigned char)src[2] == 0x37) {
            out[o++] = '\'';
            src += 3;
        } else if (c == 0x7E && (unsigned char)src[1] == 0x32 && (unsigned char)src[2] == 0x63) {
            out[o++] = ',';
            src += 3;
        } else if (c == 0x7E || c == 0x81 || c == 0xBD || c == 0xBE) {
            src++; // ~, gender symbols, or unknown multi-byte runs: skip
        } else if (c >= 0x20 && c <= 0x7E) {
            out[o++] = (char)c;
            src++;
        } else {
            src++; // skip any other non-ASCII byte
        }
    }
    out[o] = '\0';
}

int Pc_CheatGetTeamRank(void)
{
    if (gRescueTeamInfoRef == NULL)
        return 0;
    return GetRescueTeamRank();
}

s32 Pc_CheatGetTeamRankPts(void)
{
    if (gRescueTeamInfoRef == NULL)
        return 0;
    return gRescueTeamInfoRef->teamRankPts;
}

void Pc_CheatSetTeamRank(int rank)
{
    // Points chosen so GetRescueTeamRank() lands exactly on the requested rank
    // (a rank is current while pts < the next rank's threshold).
    static const s32 sRankPoints[MAX_TEAM_RANKS] = {
        [NORMAL_RANK] = 0,
        [BRONZE_RANK] = 499,
        [SILVER_RANK] = 1499,
        [GOLD_RANK] = 2999,
        [PLATINUM_RANK] = 7499,
        [DIAMOND_RANK] = 14999,
        [LUCARIO_RANK] = 99999999,
    };

    if (gRescueTeamInfoRef == NULL)
        return;
    if (rank < 0)
        rank = 0;
    if (rank >= MAX_TEAM_RANKS)
        rank = MAX_TEAM_RANKS - 1;
    gRescueTeamInfoRef->teamRankPts = sRankPoints[rank];
}

void Pc_CheatLevelUpTeam(int levels)
{
    s32 i;

    if (gDungeon == NULL)
        return;
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *member = gDungeon->teamPokemon[i];
        if (member != NULL && EntityIsValid(member))
            LevelUpTarget(member, member, levels, 0, 0);
    }
}

void Pc_CheatGiveExpToTeam(int exp)
{
    s32 i;

    if (gDungeon == NULL || exp <= 0)
        return;
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *member = gDungeon->teamPokemon[i];
        if (member != NULL && EntityIsValid(member))
            AddExpPoints(member, member, exp);
    }
}

void Pc_CheatExpBoostTick(void)
{
    // EXP_BOOSTED is applied to the defeated target in dungeon_damage.c, so
    // marking every enemy boosted gives 1.5x EXP for the whole floor.
    s32 i;

    if (gDungeon == NULL)
        return;
    for (i = 0; i < DUNGEON_MAX_POKEMON; i++) {
        Entity *e = gDungeon->activePokemon[i];
        if (e != NULL && EntityIsValid(e)) {
            EntityInfo *info = GetEntInfo(e);
            if (info->isNotTeamMember)
                info->expMultiplier = EXP_BOOSTED;
        }
    }
}

void Pc_CheatInstantKillTick(void)
{
    // Drop every non-team monster to 1 HP each frame so any hit is fatal.
    s32 i;

    if (gDungeon == NULL)
        return;
    for (i = 0; i < DUNGEON_MAX_POKEMON; i++) {
        Entity *e = gDungeon->activePokemon[i];
        if (e != NULL && EntityIsValid(e)) {
            EntityInfo *info = GetEntInfo(e);
            if (info->isNotTeamMember && info->HP > 1)
                info->HP = 1;
        }
    }
}

int Pc_CheatSpeciesCount(void)
{
    return NUM_MONSTERS;
}