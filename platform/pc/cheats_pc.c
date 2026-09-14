// platform/pc/cheats_pc.c — game-state cheat helpers for the ImGui overlay.
//
// Linked only into the full-game binary (pmd-red-game); the smoke binary uses
// the empty stubs in cheats_stub.c. Everything here reads/writes the live
// decompiled game state (the same globals the game logic uses), so cheats
// apply immediately from the overlay and persist when the game next saves.
#include <string.h>

#include "gba/gba.h"
#include "gba_shim.h"

#include "constants/ability.h"
#include "constants/dungeon.h"
#include "constants/friend_area.h"
#include "constants/global.h"
#include "constants/item.h"
#include "constants/monster.h"
#include "constants/move_id.h"
#include "constants/rescue_dungeon_id.h"
#include "dungeon_leveling.h"
#include "dungeon_range.h"
#include "dungeon_util.h"
#include "def_filearchives.h"
#include "file_system.h"
#include "friend_area.h"
#include "items.h"
#include "moves.h"
#include "pokemon.h"
#include "pokemon_abilities.h"
#include "rescue_scenario.h"
#include "rescue_team_info.h"
#include "run_dungeon.h"
#include "structs/dungeon_entity.h"
#include "structs/str_dungeon_location.h"
#include "structs/str_moves.h"
#include "structs/str_pokemon.h"

// Decode a GBA-charmapped string to plain ASCII. Plain ASCII letters/digits
// and '-'/' ' are single bytes; apostrophe / comma / gender symbols are
// multi-byte or non-ASCII (see charmap.txt). Unknown bytes are skipped.
static void Pc_DecodeCharmap(const u8 *src, char *out, size_t cap, int maxBytes)
{
    size_t o = 0;
    int guard = 0;

    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (src == NULL)
        return;

    while (*src != '\0' && o + 1 < cap && guard < maxBytes) {
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

// Per-frame de-dup tracker for the custom XP multiplier: we can only observe a
// member's exp after EnemyEvolution has applied it, so to multiply we grant a
// top-up bonus and remember it so the next frame's delta isn't multiplied
// again (which would otherwise run away exponentially).
static Entity *sExpTrackEnt[MAX_TEAM_MEMBERS];
static s32 sExpTrackLast[MAX_TEAM_MEMBERS];
static s32 sExpTrackBonus[MAX_TEAM_MEMBERS];

static void ApplyLevelGains(Pokemon *pokemon, s32 targetLevel)
{
    // Recreate the game's stat growth (dungeon_leveling.c LevelUp): base stats
    // are the level-1 values set by CreateLevel1Pokemon, then each level adds
    // the gains from GetLvlUpEntry.
    s32 level;

    if (targetLevel < 1)
        targetLevel = 1;
    if (targetLevel > 100)
        targetLevel = 100;

    for (level = 2; level <= targetLevel; level++) {
        LevelData leveldata;
        GetLvlUpEntry(&leveldata, pokemon->speciesNum, level);
        pokemon->pokeHP += leveldata.gainHP;
        pokemon->offense.att[0] += leveldata.gainAtt[0];
        pokemon->offense.att[1] += leveldata.gainAtt[1];
        pokemon->offense.def[0] += leveldata.gainDef[0];
        pokemon->offense.def[1] += leveldata.gainDef[1];
        if (pokemon->pokeHP > 998)
            pokemon->pokeHP = 999;
        if (pokemon->offense.att[0] > 254)
            pokemon->offense.att[0] = 255;
        if (pokemon->offense.att[1] > 254)
            pokemon->offense.att[1] = 255;
        if (pokemon->offense.def[0] > 254)
            pokemon->offense.def[0] = 255;
        if (pokemon->offense.def[1] > 254)
            pokemon->offense.def[1] = 255;
    }
    pokemon->level = targetLevel;

    {
        LevelData leveldata;
        GetLvlUpEntry(&leveldata, pokemon->speciesNum, targetLevel);
        pokemon->currExp = leveldata.expRequired;
    }
}

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
    PcCheatMonSpec spec;

    memset(&spec, 0, sizeof(spec));
    spec.species = species;
    spec.level = 1;
    spec.ability1 = -1;
    spec.ability2 = -1;
    return Pc_CheatRecruitMon(&spec);
}

const char *Pc_CheatSpeciesName(int species)
{
    return GetMonSpecies(species);
}

void Pc_CheatSpeciesDisplayName(int species, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (species < 0 || species >= NUM_MONSTERS)
        return;
    Pc_DecodeCharmap((const u8 *)GetMonSpecies(species), out, cap, 32);
}

int Pc_CheatMoveCount(void)
{
    // The move data table in the ROM spans 0x3679a0..0x36b3b4 = 0x3A14 bytes =
    // 413 entries of size 0x24 (move ids 0..412). move_id.h defines a handful
    // of extra "Unused" constants beyond that with no data table entry.
    return 413;
}

static MoveDataEntry *Pc_GetMoveData(void)
{
    // sMovesData is file-static in moves.c, so re-open the wazapara file the
    // same way LoadWazaParameters() does and cache the move table pointer.
    static MoveDataEntry *sMoveData = NULL;
    static bool8 sTried = FALSE;
    OpenedFile *file;

    if (sTried)
        return sMoveData;
    sTried = TRUE;
    file = OpenFileAndGetFileDataPtr((const u8 *)"wazapara", &gSystemFileArchive);
    if (file != NULL)
        sMoveData = ((MoveDataFile *)file->data)->moveData;
    return sMoveData;
}

void Pc_CheatMoveDisplayName(int moveId, char *out, size_t cap)
{
    MoveDataEntry *moveData;

    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (moveId <= 0 || moveId >= Pc_CheatMoveCount())
        return;

    // Some move table slots are unused and have a NULL name; skip them
    // (sub_8092C84 would crash the mini-printf on such a name).
    moveData = Pc_GetMoveData();
    if (moveData == NULL || moveData[moveId].name == NULL)
        return;

    Pc_DecodeCharmap(moveData[moveId].name, out, cap, 32);
}

int Pc_CheatAbilityCount(void)
{
    return NUM_ABILITIES;
}

void Pc_CheatAbilityDisplayName(int abilityId, char *out, size_t cap)
{
    u8 raw[0x50];

    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (abilityId < 0 || abilityId >= NUM_ABILITIES)
        return;
    memset(raw, 0, sizeof(raw));
    CopyAbilityNametoBuffer((char *)raw, (u8)abilityId);
    raw[0x4F] = '\0';
    Pc_DecodeCharmap(raw, out, cap, 0x50);
}

int Pc_CheatRecruitMon(const PcCheatMonSpec *spec)
{
    Pokemon pokemon;
    DungeonLocation location;
    u16 moves[MAX_MON_MOVES];
    u8 name[POKEMON_NAME_LENGTH + 1];
    s32 i;
    bool8 anyMove = FALSE;

    if (spec == NULL || gRecruitedPokemonRef == NULL || gFriendAreas == NULL)
        return 0;
    if (spec->species <= 0 || spec->species >= NUM_MONSTERS)
        return 0;

    // The game's own recruit logic refuses species whose friend area isn't
    // bought (see TryAddPokemonToRecruited), so unlock it first (mirrors the
    // dev debug menu, src/debug_menu1.c).
    UnlockFriendArea(GetFriendArea(spec->species));

    for (i = 0; i < MAX_MON_MOVES; i++) {
        moves[i] = spec->moves[i];
        if (moves[i] != 0)
            anyMove = TRUE;
    }

    location.id = DUNGEON_MT_THUNDER_PEAK;
    location.floor = 0;

    if (spec->name != NULL) {
        for (i = 0; i < POKEMON_NAME_LENGTH; i++) {
            if (spec->name[i] == '\0')
                break;
            name[i] = (u8)spec->name[i];
        }
        for (; i < POKEMON_NAME_LENGTH; i++)
            name[i] = ' ';
        name[POKEMON_NAME_LENGTH] = '\0';
    } else {
        name[0] = '\0';
    }

    CreateLevel1Pokemon(&pokemon, spec->species,
                        spec->name != NULL ? name : NULL,
                        (u8)(spec->heldItem > 0 ? spec->heldItem : 0),
                        &location,
                        anyMove ? moves : NULL);

    if (spec->level > 1)
        ApplyLevelGains(&pokemon, spec->level);

    if (spec->hp > 0)
        pokemon.pokeHP = (s16)(spec->hp > 999 ? 999 : spec->hp);
    if (spec->atk > 0)
        pokemon.offense.att[0] = (u8)(spec->atk > 255 ? 255 : spec->atk);
    if (spec->spAtk > 0)
        pokemon.offense.att[1] = (u8)(spec->spAtk > 255 ? 255 : spec->spAtk);
    if (spec->def > 0)
        pokemon.offense.def[0] = (u8)(spec->def > 255 ? 255 : spec->def);
    if (spec->spDef > 0)
        pokemon.offense.def[1] = (u8)(spec->spDef > 255 ? 255 : spec->spDef);
    if (spec->iq > 0)
        pokemon.IQ = (s16)spec->iq;

    return TryAddPokemonToRecruited(&pokemon) != NULL;
}

void Pc_CheatApplyAbilitiesTick(int species, int ability1, int ability2)
{
    s32 i;

    if (gDungeon == NULL)
        return;
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *e = gDungeon->teamPokemon[i];
        if (e != NULL && EntityIsValid(e)) {
            EntityInfo *info = GetEntInfo(e);
            if (info->id == species) {
                if (ability1 >= 0 && ability1 < NUM_ABILITIES)
                    info->abilities[0] = (u8)ability1;
                if (ability2 >= 0 && ability2 < NUM_ABILITIES)
                    info->abilities[1] = (u8)ability2;
            }
        }
    }
}

void Pc_CheatUnlockAllFriendAreas(void)
{
    // Index 0 is the "no area" slot; the game's own HasAllFriendAreas() starts
    // at 1 too.
    s32 i;

    if (gFriendAreas == NULL)
        return;
    for (i = 1; i < FRIEND_AREA_COUNT; i++)
        UnlockFriendArea((u8)i);
}

void Pc_CheatUnlockAllDungeons(void)
{
    // sub_80973A8 marks a dungeon available (JOB_LIST) and is what the rescue
    // scenario uses to unlock dungeons; the game's dungeon list then offers
    // every unlocked one.
    s32 i;

    for (i = 0; i < RESCUE_DUNGEON_COUNT; i++)
        sub_80973A8(i, TRUE);
}

void Pc_CheatSkipToEndTick(void)
{
    // While the leader stands on the stairs tile, rewrite the current floor to
    // the dungeon's last floor so the next stairs step clears the dungeon
    // (run_dungeon.c advances a floor only when floor+1 < total floors).
    Entity *leader;

    if (gDungeon == NULL)
        return;
    leader = GetLeader();
    if (leader == NULL || !EntityIsValid(leader))
        return;
    if (leader->pos.x == gDungeon->stairsSpawn.x && leader->pos.y == gDungeon->stairsSpawn.y) {
        if (gDungeon->unk1CEC8 > 1)
            gDungeon->unk644.dungeonLocation.floor = gDungeon->unk1CEC8 - 1;
    }
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

void Pc_CheatExpMultiplierTick(float multiplier)
{
    s32 i;

    if (gDungeon == NULL)
        return;

    if (multiplier <= 1.0f) {
        for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
            sExpTrackEnt[i] = NULL;
            sExpTrackLast[i] = 0;
            sExpTrackBonus[i] = 0;
        }
        return;
    }

    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *member = gDungeon->teamPokemon[i];
        EntityInfo *info;
        s32 delta;
        s32 real;
        s32 bonus;

        if (member == NULL || !EntityIsValid(member)) {
            sExpTrackEnt[i] = NULL;
            sExpTrackLast[i] = 0;
            sExpTrackBonus[i] = 0;
            continue;
        }
        if (sExpTrackEnt[i] != member) {
            sExpTrackEnt[i] = member;
            sExpTrackLast[i] = 0;
            sExpTrackBonus[i] = 0;
        }

        info = GetEntInfo(member);
        delta = (s32)info->exp - sExpTrackLast[i];
        if (delta < 0)
            delta = 0;
        // Last frame's top-up already landed in info->exp; subtract it so the
        // multiplier only applies to EXP the game actually handed out.
        real = delta - sExpTrackBonus[i];
        if (real < 0)
            real = 0;

        bonus = (s32)((float)real * (multiplier - 1.0f));
        if (bonus > 0)
            AddExpPoints(member, member, bonus);

        sExpTrackBonus[i] = bonus;
        sExpTrackLast[i] = (s32)info->exp;
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