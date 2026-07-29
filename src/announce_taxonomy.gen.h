// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
// Full license: see LICENSE. Third-party credits: see NOTICE.
//
// SPDX-License-Identifier: AGPL-3.0-only


// Generated from Dwarf Fortress announcement definitions and df::announcement_type.
// Do not edit by hand.


#pragma once

#include <cstdint>

namespace dfcapture {
namespace taxonomy {

// Flag bits, from the legend at the top of announcements.txt.
enum AnnounceFlag : uint8_t {
    FLAG_A_DISPLAY = 1,    // shows in the adventure announcement log
    FLAG_D_DISPLAY = 2,    // shows in the dwarf announcement alerts
    FLAG_BOX       = 4,    // BOX / DO_MEGA -- box popup AND a hard pause
    FLAG_PAUSE     = 8,    // P / PAUSE
    FLAG_RECENTER  = 16,   // R / RECENTER
    FLAG_ALERT     = 32,   // lights the alert button
    FLAG_UCR       = 64,   // attaches to unit combat/hunting/sparring reports
    FLAG_UCR_ACTIVE = 128, // UCR_A -- attaches only to an ALREADY-ACTIVE unit report
};

// Section ids. `misc` is 0 so a zero-initialised / unknown row degrades to Misc, never to a
// section it does not belong in.
enum Section : uint8_t {
    SECTION_MISC = 0,
    SECTION_COMBAT = 1,
    SECTION_SIEGES = 2,
    SECTION_ARTIFACTS = 3,
    SECTION_TRADE = 4,
    SECTION_NOBLES = 5,
    SECTION_DEATHS = 6,
    SECTION_COUNT = 7,
};

struct SectionInfo { const char* key; const char* label; };
static constexpr SectionInfo SECTION_INFO[SECTION_COUNT] = {
    { "misc", "Misc" },
    { "combat", "Combat" },
    { "sieges", "Sieges & invasions" },
    { "artifacts", "Artifacts & masterworks" },
    { "trade", "Trade & diplomacy" },
    { "nobles", "Nobles & mandates" },
    { "deaths", "Deaths" },
};

struct AnnounceTaxon {
    const char* key;   // df::announcement_type enum key == the announcements.txt token
    uint8_t section;   // Section
    uint8_t flags;     // AnnounceFlag bitfield, as SHIPPED by DF (see the generator's header)
};

// Indexed DIRECTLY by df::announcement_type value (0 .. TAXONOMY_COUNT-1). O(1), no search --
// this is what lets the /reports scan classify every entry in the fort's report vector without
// costing anything measurable under the core lock.
static constexpr AnnounceTaxon TAXONOMY[] = {
    { "REACHED_PEAK", SECTION_MISC, 7 }, // 0
    { "ERA_CHANGE", SECTION_MISC, 7 }, // 1
    { "FEATURE_DISCOVERY", SECTION_MISC, 7 }, // 2
    { "STRUCK_DEEP_METAL", SECTION_MISC, 7 }, // 3
    { "STRUCK_MINERAL", SECTION_MISC, 3 }, // 4
    { "STRUCK_ECONOMIC_MINERAL", SECTION_MISC, 3 }, // 5
    { "COMBAT_TWIST_WEAPON", SECTION_COMBAT, 65 }, // 6
    { "COMBAT_LET_ITEM_DROP", SECTION_COMBAT, 65 }, // 7
    { "COMBAT_START_CHARGE", SECTION_COMBAT, 65 }, // 8
    { "COMBAT_SURPRISE_CHARGE", SECTION_COMBAT, 65 }, // 9
    { "COMBAT_JUMP_DODGE_PROJ", SECTION_COMBAT, 65 }, // 10
    { "COMBAT_JUMP_DODGE_STRIKE", SECTION_COMBAT, 65 }, // 11
    { "COMBAT_DODGE", SECTION_COMBAT, 65 }, // 12
    { "COMBAT_COUNTERSTRIKE", SECTION_COMBAT, 65 }, // 13
    { "COMBAT_BLOCK", SECTION_COMBAT, 65 }, // 14
    { "COMBAT_PARRY", SECTION_COMBAT, 65 }, // 15
    { "COMBAT_CHARGE_COLLISION", SECTION_COMBAT, 65 }, // 16
    { "COMBAT_CHARGE_DEFENDER_TUMBLES", SECTION_COMBAT, 65 }, // 17
    { "COMBAT_CHARGE_DEFENDER_KNOCKED_OVER", SECTION_COMBAT, 65 }, // 18
    { "COMBAT_CHARGE_ATTACKER_TUMBLES", SECTION_COMBAT, 65 }, // 19
    { "COMBAT_CHARGE_ATTACKER_BOUNCE_BACK", SECTION_COMBAT, 65 }, // 20
    { "COMBAT_CHARGE_TANGLE_TOGETHER", SECTION_COMBAT, 65 }, // 21
    { "COMBAT_CHARGE_TANGLE_TUMBLE", SECTION_COMBAT, 65 }, // 22
    { "COMBAT_CHARGE_RUSH_BY", SECTION_COMBAT, 65 }, // 23
    { "COMBAT_CHARGE_MANAGE_STOP", SECTION_COMBAT, 65 }, // 24
    { "COMBAT_CHARGE_OBSTACLE_SLAM", SECTION_COMBAT, 65 }, // 25
    { "COMBAT_WRESTLE_LOCK", SECTION_COMBAT, 65 }, // 26
    { "COMBAT_WRESTLE_CHOKEHOLD", SECTION_COMBAT, 65 }, // 27
    { "COMBAT_WRESTLE_TAKEDOWN", SECTION_COMBAT, 65 }, // 28
    { "COMBAT_WRESTLE_THROW", SECTION_COMBAT, 65 }, // 29
    { "COMBAT_WRESTLE_RELEASE_LOCK", SECTION_COMBAT, 65 }, // 30
    { "COMBAT_WRESTLE_RELEASE_CHOKE", SECTION_COMBAT, 65 }, // 31
    { "COMBAT_WRESTLE_RELEASE_GRIP", SECTION_COMBAT, 65 }, // 32
    { "COMBAT_WRESTLE_STRUGGLE", SECTION_COMBAT, 65 }, // 33
    { "COMBAT_WRESTLE_RELEASE_LATCH", SECTION_COMBAT, 65 }, // 34
    { "COMBAT_WRESTLE_STRANGLE_KO", SECTION_COMBAT, 65 }, // 35
    { "COMBAT_WRESTLE_ADJUST_GRIP", SECTION_COMBAT, 65 }, // 36
    { "COMBAT_GRAB_TEAR", SECTION_COMBAT, 65 }, // 37
    { "COMBAT_STRIKE_DETAILS", SECTION_COMBAT, 65 }, // 38
    { "COMBAT_STRIKE_DETAILS_2", SECTION_COMBAT, 65 }, // 39
    { "COMBAT_EVENT_ENRAGED", SECTION_COMBAT, 65 }, // 40
    { "COMBAT_EVENT_STUCKIN", SECTION_COMBAT, 65 }, // 41
    { "COMBAT_EVENT_LATCH_BP", SECTION_COMBAT, 65 }, // 42
    { "COMBAT_EVENT_LATCH_GENERAL", SECTION_COMBAT, 65 }, // 43
    { "COMBAT_EVENT_PROPELLED_AWAY", SECTION_COMBAT, 65 }, // 44
    { "COMBAT_EVENT_KNOCKED_OUT", SECTION_COMBAT, 65 }, // 45
    { "COMBAT_EVENT_STUNNED", SECTION_COMBAT, 65 }, // 46
    { "COMBAT_EVENT_WINDED", SECTION_COMBAT, 65 }, // 47
    { "COMBAT_EVENT_NAUSEATED", SECTION_COMBAT, 65 }, // 48
    { "MIGRANT_ARRIVAL_NAMED", SECTION_MISC, 3 }, // 49
    { "MIGRANT_ARRIVAL", SECTION_MISC, 3 }, // 50
    { "DIG_CANCEL_WARM", SECTION_MISC, 3 }, // 51
    { "DIG_CANCEL_DAMP", SECTION_MISC, 3 }, // 52
    { "AMBUSH_DEFENDER", SECTION_SIEGES, 35 }, // 53
    { "AMBUSH_RESIDENT", SECTION_SIEGES, 35 }, // 54
    { "AMBUSH_THIEF", SECTION_SIEGES, 35 }, // 55
    { "AMBUSH_THIEF_SUPPORT_SKULKING", SECTION_SIEGES, 35 }, // 56
    { "AMBUSH_THIEF_SUPPORT_NATURE", SECTION_SIEGES, 35 }, // 57
    { "AMBUSH_THIEF_SUPPORT", SECTION_SIEGES, 35 }, // 58
    { "AMBUSH_MISCHIEVOUS", SECTION_SIEGES, 35 }, // 59
    { "AMBUSH_SNATCHER", SECTION_SIEGES, 35 }, // 60
    { "AMBUSH_SNATCHER_SUPPORT", SECTION_SIEGES, 35 }, // 61
    { "AMBUSH_AMBUSHER_NATURE", SECTION_SIEGES, 35 }, // 62
    { "AMBUSH_AMBUSHER", SECTION_SIEGES, 35 }, // 63
    { "AMBUSH_INJURED", SECTION_SIEGES, 35 }, // 64
    { "AMBUSH_OTHER", SECTION_SIEGES, 35 }, // 65
    { "AMBUSH_INCAPACITATED", SECTION_SIEGES, 35 }, // 66
    { "CARAVAN_ARRIVAL", SECTION_TRADE, 35 }, // 67
    { "NOBLE_ARRIVAL", SECTION_NOBLES, 3 }, // 68
    { "D_MIGRANTS_ARRIVAL", SECTION_MISC, 3 }, // 69
    { "D_MIGRANT_ARRIVAL", SECTION_MISC, 3 }, // 70
    { "D_MIGRANT_ARRIVAL_DISCOURAGED", SECTION_MISC, 3 }, // 71
    { "D_NO_MIGRANT_ARRIVAL", SECTION_MISC, 3 }, // 72
    { "ANIMAL_TRAP_CATCH", SECTION_MISC, 3 }, // 73
    { "ANIMAL_TRAP_ROBBED", SECTION_MISC, 3 }, // 74
    { "MISCHIEF_LEVER", SECTION_SIEGES, 3 }, // 75
    { "MISCHIEF_PLATE", SECTION_SIEGES, 3 }, // 76
    { "MISCHIEF_CAGE", SECTION_SIEGES, 3 }, // 77
    { "MISCHIEF_CHAIN", SECTION_SIEGES, 3 }, // 78
    { "DIPLOMAT_ARRIVAL", SECTION_TRADE, 3 }, // 79
    { "LIAISON_ARRIVAL", SECTION_TRADE, 3 }, // 80
    { "TRADE_DIPLOMAT_ARRIVAL", SECTION_TRADE, 3 }, // 81
    { "CAVE_COLLAPSE", SECTION_MISC, 35 }, // 82
    { "BIRTH_CITIZEN", SECTION_MISC, 3 }, // 83
    { "BIRTH_ANIMAL", SECTION_MISC, 3 }, // 84
    { "STRANGE_MOOD", SECTION_ARTIFACTS, 3 }, // 85
    { "MADE_ARTIFACT", SECTION_ARTIFACTS, 7 }, // 86
    { "NAMED_ARTIFACT", SECTION_ARTIFACTS, 7 }, // 87
    { "ITEM_ATTACHMENT", SECTION_ARTIFACTS, 3 }, // 88
    { "VERMIN_CAGE_ESCAPE", SECTION_MISC, 3 }, // 89
    { "TRIGGER_WEB", SECTION_MISC, 3 }, // 90
    { "MOOD_BUILDING_CLAIMED", SECTION_ARTIFACTS, 3 }, // 91
    { "ARTIFACT_BEGUN", SECTION_ARTIFACTS, 3 }, // 92
    { "MEGABEAST_ARRIVAL", SECTION_SIEGES, 7 }, // 93
    { "WEREBEAST_ARRIVAL", SECTION_SIEGES, 7 }, // 94
    { "BEAST_AMBUSH", SECTION_SIEGES, 35 }, // 95
    { "BERSERK_CITIZEN", SECTION_MISC, 3 }, // 96
    { "MAGMA_DEFACES_ENGRAVING", SECTION_ARTIFACTS, 3 }, // 97
    { "ENGRAVING_MELTS", SECTION_ARTIFACTS, 3 }, // 98
    { "MASTERPIECE_CONSTRUCTION", SECTION_ARTIFACTS, 3 }, // 99
    { "MASTER_ARCHITECTURE_LOST", SECTION_ARTIFACTS, 3 }, // 100
    { "MASTER_CONSTRUCTION_LOST", SECTION_ARTIFACTS, 3 }, // 101
    { "ADV_AWAKEN", SECTION_MISC, 3 }, // 102
    { "ADV_SLEEP_INTERRUPTED", SECTION_MISC, 3 }, // 103
    { "CANCEL_JOB", SECTION_MISC, 3 }, // 104
    { "ADV_CREATURE_DEATH", SECTION_DEATHS, 131 }, // 105
    { "CITIZEN_DEATH", SECTION_DEATHS, 163 }, // 106
    { "PET_DEATH", SECTION_DEATHS, 131 }, // 107
    { "ENDGAME_EVENT_1", SECTION_MISC, 7 }, // 108
    { "ENDGAME_EVENT_1B", SECTION_MISC, 7 }, // 109
    { "ENDGAME_EVENT_2", SECTION_MISC, 7 }, // 110
    { "FALL_OVER", SECTION_COMBAT, 129 }, // 111
    { "CAUGHT_IN_FLAMES", SECTION_COMBAT, 129 }, // 112
    { "CAUGHT_IN_WEB", SECTION_COMBAT, 129 }, // 113
    { "UNIT_PROJECTILE_SLAM_BLOW_APART", SECTION_COMBAT, 129 }, // 114
    { "UNIT_PROJECTILE_SLAM", SECTION_COMBAT, 129 }, // 115
    { "UNIT_PROJECTILE_SLAM_INTO_UNIT", SECTION_COMBAT, 129 }, // 116
    { "VOMIT", SECTION_COMBAT, 129 }, // 117
    { "LOSE_HOLD_OF_ITEM", SECTION_COMBAT, 129 }, // 118
    { "REGAIN_CONSCIOUSNESS", SECTION_COMBAT, 129 }, // 119
    { "FREE_FROM_WEB", SECTION_COMBAT, 129 }, // 120
    { "PARALYZED", SECTION_COMBAT, 129 }, // 121
    { "OVERCOME_PARALYSIS", SECTION_COMBAT, 129 }, // 122
    { "NOT_STUNNED", SECTION_COMBAT, 129 }, // 123
    { "EXHAUSTION", SECTION_COMBAT, 129 }, // 124
    { "PAIN_KO", SECTION_COMBAT, 129 }, // 125
    { "BREAK_GRIP", SECTION_COMBAT, 129 }, // 126
    { "NO_BREAK_GRIP", SECTION_COMBAT, 129 }, // 127
    { "BLOCK_FIRE", SECTION_COMBAT, 129 }, // 128
    { "BREATHE_FIRE", SECTION_COMBAT, 129 }, // 129
    { "SHOOT_WEB", SECTION_COMBAT, 129 }, // 130
    { "PULL_OUT_DROP", SECTION_COMBAT, 129 }, // 131
    { "STAND_UP", SECTION_COMBAT, 129 }, // 132
    { "MARTIAL_TRANCE", SECTION_MISC, 3 }, // 133
    { "MAT_BREATH", SECTION_COMBAT, 129 }, // 134
    { "ADV_REACTION_PRODUCTS", SECTION_MISC, 3 }, // 135
    { "NIGHT_ATTACK_STARTS", SECTION_SIEGES, 7 }, // 136
    { "NIGHT_ATTACK_ENDS", SECTION_SIEGES, 7 }, // 137
    { "NIGHT_ATTACK_TRAVEL", SECTION_SIEGES, 3 }, // 138
    { "GHOST_ATTACK", SECTION_SIEGES, 131 }, // 139
    { "FLAME_HIT", SECTION_COMBAT, 65 }, // 140
    { "TRAVEL_SITE_DISCOVERY", SECTION_MISC, 7 }, // 141
    { "TRAVEL_SITE_BUMP", SECTION_MISC, 3 }, // 142
    { "ADVENTURE_INTRO", SECTION_MISC, 4 }, // 143
    { "CREATURE_SOUND", SECTION_MISC, 1 }, // 144
    { "CREATURE_STEALS_OBJECT", SECTION_SIEGES, 3 }, // 145
    { "FOUND_TRAP", SECTION_MISC, 3 }, // 146
    { "BODY_TRANSFORMATION", SECTION_MISC, 3 }, // 147
    { "INTERACTION_ACTOR", SECTION_COMBAT, 65 }, // 148
    { "INTERACTION_TARGET", SECTION_COMBAT, 65 }, // 149
    { "UNDEAD_ATTACK", SECTION_SIEGES, 7 }, // 150
    { "CITIZEN_MISSING", SECTION_DEATHS, 163 }, // 151
    { "PET_MISSING", SECTION_DEATHS, 131 }, // 152
    { "EMBRACE", SECTION_MISC, 1 }, // 153
    { "STRANGE_RAIN_SNOW", SECTION_MISC, 3 }, // 154
    { "STRANGE_CLOUD", SECTION_MISC, 3 }, // 155
    { "SIMPLE_ANIMAL_ACTION", SECTION_MISC, 1 }, // 156
    { "FLOUNDER_IN_LIQUID", SECTION_MISC, 1 }, // 157
    { "TRAINING_DOWN_TO_SEMI_WILD", SECTION_MISC, 3 }, // 158
    { "TRAINING_FULL_REVERSION", SECTION_MISC, 3 }, // 159
    { "ANIMAL_TRAINING_KNOWLEDGE", SECTION_MISC, 3 }, // 160
    { "SKIP_ON_LIQUID", SECTION_COMBAT, 129 }, // 161
    { "DODGE_FLYING_OBJECT", SECTION_COMBAT, 129 }, // 162
    { "REGULAR_CONVERSATION", SECTION_MISC, 1 }, // 163
    { "BANDIT_EMPTY_CONTAINER", SECTION_MISC, 1 }, // 164
    { "BANDIT_GRAB_ITEM", SECTION_MISC, 1 }, // 165
    { "COMBAT_EVENT_ATTACK_INTERRUPTED", SECTION_COMBAT, 65 }, // 166
    { "COMBAT_WRESTLE_CATCH_ATTACK", SECTION_COMBAT, 65 }, // 167
    { "FAIL_TO_GRAB_SURFACE", SECTION_COMBAT, 129 }, // 168
    { "LOSE_HOLD_OF_SURFACE", SECTION_COMBAT, 129 }, // 169
    { "TRAVEL_COMPLAINT", SECTION_MISC, 7 }, // 170
    { "LOSE_EMOTION", SECTION_COMBAT, 129 }, // 171
    { "REORGANIZE_POSSESSIONS", SECTION_MISC, 1 }, // 172
    { "PUSH_ITEM", SECTION_COMBAT, 129 }, // 173
    { "DRAW_ITEM", SECTION_MISC, 1 }, // 174
    { "STRAP_ITEM", SECTION_MISC, 1 }, // 175
    { "GAIN_SITE_CONTROL", SECTION_NOBLES, 7 }, // 176
    { "CONFLICT_CONVERSATION", SECTION_COMBAT, 129 }, // 177
    { "FORT_POSITION_SUCCESSION", SECTION_NOBLES, 7 }, // 178
    { "MECHANISM_SOUND", SECTION_MISC, 1 }, // 179
    { "BIRTH_WILD_ANIMAL", SECTION_MISC, 3 }, // 180
    { "STRESSED_CITIZEN", SECTION_MISC, 3 }, // 181
    { "CITIZEN_LOST_TO_STRESS", SECTION_DEATHS, 3 }, // 182
    { "CITIZEN_TANTRUM", SECTION_MISC, 3 }, // 183
    { "MOVED_OUT_OF_RANGE", SECTION_MISC, 3 }, // 184
    { "CANNOT_JUMP", SECTION_MISC, 3 }, // 185
    { "NO_TRACKS", SECTION_MISC, 3 }, // 186
    { "ALREADY_SEARCHED_AREA", SECTION_MISC, 3 }, // 187
    { "SEARCH_FOUND_SOMETHING", SECTION_MISC, 3 }, // 188
    { "SEARCH_FOUND_NOTHING", SECTION_MISC, 3 }, // 189
    { "NOTHING_TO_INTERACT", SECTION_MISC, 3 }, // 190
    { "NOTHING_TO_EXAMINE", SECTION_MISC, 3 }, // 191
    { "YOU_YIELDED", SECTION_MISC, 3 }, // 192
    { "YOU_UNYIELDED", SECTION_MISC, 3 }, // 193
    { "YOU_STRAP_ITEM", SECTION_MISC, 3 }, // 194
    { "YOU_DRAW_ITEM", SECTION_MISC, 3 }, // 195
    { "NO_GRASP_TO_DRAW_ITEM", SECTION_MISC, 3 }, // 196
    { "NO_ITEM_TO_STRAP", SECTION_MISC, 3 }, // 197
    { "NO_INV_TO_REMOVE", SECTION_MISC, 3 }, // 198
    { "NO_INV_TO_WEAR", SECTION_MISC, 3 }, // 199
    { "NO_INV_TO_EAT", SECTION_MISC, 3 }, // 200
    { "NO_INV_TO_CONTAIN", SECTION_MISC, 3 }, // 201
    { "NO_INV_TO_DROP", SECTION_MISC, 3 }, // 202
    { "NOTHING_TO_PICK_UP", SECTION_MISC, 3 }, // 203
    { "NO_INV_TO_THROW", SECTION_MISC, 3 }, // 204
    { "NO_INV_TO_FIRE", SECTION_MISC, 3 }, // 205
    { "CURRENT_SMELL", SECTION_MISC, 3 }, // 206
    { "CURRENT_WEATHER", SECTION_MISC, 3 }, // 207
    { "CURRENT_TEMPERATURE", SECTION_MISC, 3 }, // 208
    { "CURRENT_DATE", SECTION_MISC, 3 }, // 209
    { "NO_GRASP_FOR_PICKUP", SECTION_MISC, 3 }, // 210
    { "CANNOT_CHOP_TREE", SECTION_MISC, 3 }, // 211
    { "CANNOT_CLIMB", SECTION_MISC, 3 }, // 212
    { "CANNOT_STAND", SECTION_MISC, 3 }, // 213
    { "MUST_UNRETRACT_FIRST", SECTION_MISC, 3 }, // 214
    { "CANNOT_REST", SECTION_MISC, 3 }, // 215
    { "CANNOT_MAKE_CAMPFIRE", SECTION_MISC, 3 }, // 216
    { "MADE_CAMPFIRE", SECTION_MISC, 3 }, // 217
    { "CANNOT_SET_FIRE", SECTION_MISC, 3 }, // 218
    { "SET_FIRE", SECTION_MISC, 3 }, // 219
    { "DAWN_BREAKS", SECTION_MISC, 3 }, // 220
    { "NOON", SECTION_MISC, 3 }, // 221
    { "NIGHTFALL", SECTION_MISC, 3 }, // 222
    { "UNUSED_0001", SECTION_MISC, 0 }, // 223
    { "EMPTY_CONTAINER", SECTION_MISC, 3 }, // 224
    { "TAKE_OUT_OF_CONTAINER", SECTION_MISC, 3 }, // 225
    { "UNUSED_0002", SECTION_MISC, 0 }, // 226
    { "PUT_INTO_CONTAINER", SECTION_MISC, 3 }, // 227
    { "EAT_ITEM", SECTION_MISC, 1 }, // 228
    { "DRINK_ITEM", SECTION_MISC, 1 }, // 229
    { "CONSUME_FAILURE", SECTION_MISC, 1 }, // 230
    { "DROP_ITEM", SECTION_MISC, 1 }, // 231
    { "PICK_UP_ITEM", SECTION_MISC, 1 }, // 232
    { "YOU_BUILDING_INTERACTION", SECTION_MISC, 3 }, // 233
    { "YOU_ITEM_INTERACTION", SECTION_MISC, 3 }, // 234
    { "YOU_TEMPERATURE_EFFECTS", SECTION_MISC, 3 }, // 235
    { "PROFESSION_CHANGES", SECTION_MISC, 3 }, // 236
    { "RECRUIT_PROMOTED", SECTION_MISC, 3 }, // 237
    { "SOLDIER_BECOMES_MASTER", SECTION_MISC, 3 }, // 238
    { "RESOLVE_SHARED_ITEMS", SECTION_COMBAT, 65 }, // 239
    { "COUGH_BLOOD", SECTION_MISC, 3 }, // 240
    { "VOMIT_BLOOD", SECTION_MISC, 3 }, // 241
    { "MERCHANTS_UNLOADING", SECTION_TRADE, 3 }, // 242
    { "MERCHANTS_NEED_DEPOT", SECTION_TRADE, 35 }, // 243
    { "MERCHANT_WAGONS_BYPASSED", SECTION_TRADE, 35 }, // 244
    { "MERCHANTS_LEAVING_SOON", SECTION_TRADE, 3 }, // 245
    { "MERCHANTS_EMBARKED", SECTION_TRADE, 3 }, // 246
    { "PET_LOSES_DEAD_OWNER", SECTION_DEATHS, 3 }, // 247
    { "PET_ADOPTS_OWNER", SECTION_MISC, 3 }, // 248
    { "VERMIN_BITE", SECTION_MISC, 3 }, // 249
    { "UNABLE_TO_COMPLETE_BUILDING", SECTION_MISC, 3 }, // 250
    { "JOBS_REMOVED_FROM_UNPOWERED_BUILDING", SECTION_MISC, 3 }, // 251
    { "CITIZEN_SNATCHED", SECTION_SIEGES, 3 }, // 252
    { "VERMIN_DISTURBED", SECTION_MISC, 3 }, // 253
    { "LAND_GAINS_STATUS", SECTION_NOBLES, 3 }, // 254
    { "LAND_ELEVATED_STATUS", SECTION_NOBLES, 3 }, // 255
    { "MASTERPIECE_CRAFTED", SECTION_ARTIFACTS, 3 }, // 256
    { "ARTWORK_DEFACED", SECTION_ARTIFACTS, 3 }, // 257
    { "POWER_LEARNED", SECTION_MISC, 3 }, // 258
    { "YOU_FEED_ON_SUCKEE", SECTION_MISC, 3 }, // 259
    { "ANIMAL_TRAINED", SECTION_MISC, 3 }, // 260
    { "DYED_MASTERPIECE", SECTION_ARTIFACTS, 3 }, // 261
    { "COOKED_MASTERPIECE", SECTION_ARTIFACTS, 3 }, // 262
    { "MANDATE_ENDS", SECTION_NOBLES, 3 }, // 263
    { "SLOWDOWN_ENDS", SECTION_NOBLES, 3 }, // 264
    { "FAREWELL_HELPER", SECTION_MISC, 3 }, // 265
    { "ELECTION_RESULTS", SECTION_NOBLES, 3 }, // 266
    { "SITE_PRESENT", SECTION_MISC, 3 }, // 267
    { "CONSTRUCTION_SUSPENDED", SECTION_MISC, 3 }, // 268
    { "LINKAGE_SUSPENDED", SECTION_MISC, 3 }, // 269
    { "QUOTA_FILLED", SECTION_NOBLES, 3 }, // 270
    { "JOB_OVERWRITTEN", SECTION_MISC, 3 }, // 271
    { "NOTHING_TO_CATCH_IN_WATER", SECTION_MISC, 3 }, // 272
    { "DEMAND_FORGOTTEN", SECTION_NOBLES, 3 }, // 273
    { "NEW_DEMAND", SECTION_NOBLES, 3 }, // 274
    { "NEW_MANDATE", SECTION_NOBLES, 3 }, // 275
    { "PRICES_ALTERED", SECTION_NOBLES, 3 }, // 276
    { "NAMED_RESIDENT_CREATURE", SECTION_MISC, 3 }, // 277
    { "SOMEBODY_GROWS_UP", SECTION_MISC, 3 }, // 278
    { "GUILD_REQUEST_TAKEN", SECTION_NOBLES, 3 }, // 279
    { "GUILD_WAGES_CHANGED", SECTION_NOBLES, 3 }, // 280
    { "NEW_WORK_MANDATE", SECTION_NOBLES, 3 }, // 281
    { "CITIZEN_BECOMES_SOLDIER", SECTION_MISC, 3 }, // 282
    { "CITIZEN_BECOMES_NONSOLDIER", SECTION_MISC, 3 }, // 283
    { "PARTY_ORGANIZED", SECTION_MISC, 3 }, // 284
    { "POSSESSED_TANTRUM", SECTION_ARTIFACTS, 3 }, // 285
    { "BUILDING_TOPPLED_BY_GHOST", SECTION_MISC, 3 }, // 286
    { "MASTERFUL_IMPROVEMENT", SECTION_ARTIFACTS, 3 }, // 287
    { "MASTERPIECE_ENGRAVING", SECTION_ARTIFACTS, 3 }, // 288
    { "MARRIAGE", SECTION_MISC, 3 }, // 289
    { "NO_MARRIAGE_CELEBRATION", SECTION_MISC, 3 }, // 290
    { "CURIOUS_GUZZLER", SECTION_MISC, 3 }, // 291
    { "WEATHER_BECOMES_CLEAR", SECTION_MISC, 3 }, // 292
    { "WEATHER_BECOMES_SNOW", SECTION_MISC, 3 }, // 293
    { "WEATHER_BECOMES_RAIN", SECTION_MISC, 3 }, // 294
    { "SEASON_WET", SECTION_MISC, 3 }, // 295
    { "SEASON_DRY", SECTION_MISC, 3 }, // 296
    { "SEASON_SPRING", SECTION_MISC, 3 }, // 297
    { "SEASON_SUMMER", SECTION_MISC, 3 }, // 298
    { "SEASON_AUTUMN", SECTION_MISC, 3 }, // 299
    { "SEASON_WINTER", SECTION_MISC, 3 }, // 300
    { "GUEST_ARRIVAL", SECTION_TRADE, 3 }, // 301
    { "CANNOT_SPEAK", SECTION_MISC, 3 }, // 302
    { "RESEARCH_BREAKTHROUGH", SECTION_MISC, 3 }, // 303
    { "SERVICE_ORDER_DELIVERY", SECTION_MISC, 1 }, // 304
    { "PERFORMANCE_START_FAILURE", SECTION_MISC, 1 }, // 305
    { "BEGIN_ACTIVITY", SECTION_MISC, 1 }, // 306
    { "MIDDLE_OF_ACTIVITY", SECTION_MISC, 1 }, // 307
    { "ACTIVITY_SECTION_CHANGE", SECTION_MISC, 1 }, // 308
    { "CONCLUDE_ACTIVITY", SECTION_MISC, 1 }, // 309
    { "LEARNED_WRITTEN_CONTENT", SECTION_MISC, 1 }, // 310
    { "LEARNED_ART_FORM", SECTION_MISC, 1 }, // 311
    { "PERFORMER_UPDATE", SECTION_MISC, 1 }, // 312
    { "BUILDING_DESTROYED_OR_TOPPLED", SECTION_MISC, 3 }, // 313
    { "DEITY_CURSE", SECTION_MISC, 7 }, // 314
    { "COMPOSITION_COMPLETE", SECTION_MISC, 1 }, // 315
    { "COMPOSITION_FAILED", SECTION_MISC, 1 }, // 316
    { "NEW_APPRENTICESHIP", SECTION_TRADE, 3 }, // 317
    { "PETITION_IGNORED", SECTION_TRADE, 3 }, // 318
    { "CHOP_TREE", SECTION_MISC, 3 }, // 319
    { "CANNOT_CONSTRUCT", SECTION_MISC, 3 }, // 320
    { "RUMOR_SPREAD", SECTION_MISC, 3 }, // 321
    { "AMBUSH_HERO", SECTION_SIEGES, 39 }, // 322
    { "SERVICE_ORDER_RUMOR_RECEIVED", SECTION_MISC, 3 }, // 323
    { "RETURNING_RUMOR_RECEIVED", SECTION_MISC, 3 }, // 324
    { "NEW_HOLDING", SECTION_NOBLES, 7 }, // 325
    { "NEW_MARKET_LINK", SECTION_TRADE, 7 }, // 326
    { "EMERGENCY_TACTICAL_CONTROL", SECTION_SIEGES, 7 }, // 327
    { "AGREEMENT_SATISFIED", SECTION_TRADE, 3 }, // 328
    { "AGREEMENT_WARNING", SECTION_TRADE, 3 }, // 329
    { "AGREEMENT_ABANDONED", SECTION_TRADE, 3 }, // 330
    { "NEW_GUILD", SECTION_NOBLES, 3 }, // 331
    { "CRIME_WITNESS_HANDOFF", SECTION_MISC, 7 }, // 332
    { "CRIME_WITNESS_STOLEN", SECTION_MISC, 7 }, // 333
    { "CRIME_WITNESS_ITEM_MOVED", SECTION_MISC, 7 }, // 334
    { "CRIME_WITNESS_ITEM_MISSING", SECTION_MISC, 7 }, // 335
    { "MOUNT", SECTION_COMBAT, 65 }, // 336
    { "CANNOT_MOUNT", SECTION_MISC, 1 }, // 337
    { "FAILED_MOUNT", SECTION_COMBAT, 65 }, // 338
    { "DISMOUNT", SECTION_COMBAT, 65 }, // 339
    { "FAILED_DISMOUNT", SECTION_COMBAT, 65 }, // 340
    { "DIPLOMAT_LEFT_UNHAPPY", SECTION_TRADE, 3 }, // 341
    { "EMBARK_MESSAGE", SECTION_MISC, 4 }, // 342
    { "FIRST_CARAVAN_ARRIVAL", SECTION_TRADE, 4 }, // 343
    { "MONARCH_ARRIVAL", SECTION_NOBLES, 7 }, // 344
    { "HASTY_MONARCH", SECTION_NOBLES, 7 }, // 345
    { "SATISFIED_MONARCH", SECTION_NOBLES, 7 }, // 346
    { "MOUNTAINHOME", SECTION_NOBLES, 7 }, // 347
    { "FOOD_WARNING", SECTION_MISC, 35 }, // 348
    { "PUT_ON_ITEM", SECTION_MISC, 3 }, // 349
    { "TAKE_OFF_ITEM", SECTION_MISC, 3 }, // 350
    { "DEITY_PRONOUNCEMENT", SECTION_MISC, 7 }, // 351
    { "CREATURE_STUCK", SECTION_MISC, 3 }, // 352
    { "CITIZEN_STUCK", SECTION_MISC, 3 }, // 353
    { "UNUSED_49", SECTION_MISC, 0 }, // 354
    { "UNUSED_50", SECTION_MISC, 0 }, // 355
};
static constexpr int TAXONOMY_COUNT = 356;

// Rescue table: applied ONLY when TAXONOMY put a report in Misc. Keyed by
// df::announcement_alert_type. Fail-open by construction -- it can move a row OUT of Misc but can
// never move it between two named sections. This is the backstop for DF's HARDCODED siege banner
// (there is no SIEGE announcement token) and for any token a future DF version adds.
struct AlertRescue { int16_t alert_type; uint8_t section; };
static constexpr AlertRescue ALERT_RESCUE[] = {
    { 4, SECTION_SIEGES },
    { 5, SECTION_SIEGES },
    { 6, SECTION_TRADE },
    { 7, SECTION_NOBLES },
    { 10, SECTION_ARTIFACTS },
    { 18, SECTION_ARTIFACTS },
    { 19, SECTION_ARTIFACTS },
    { 21, SECTION_DEATHS },
    { 23, SECTION_SIEGES },
    { 28, SECTION_TRADE },
    { 29, SECTION_NOBLES },
    { 31, SECTION_TRADE },
    { 34, SECTION_COMBAT },
    { 35, SECTION_COMBAT },
    { 36, SECTION_COMBAT },
};
static constexpr int ALERT_RESCUE_COUNT = 15;

// The two calls the server makes. Both are branch-light and allocation-free.
inline const AnnounceTaxon* taxon_for(int announcement_type) {
    if (announcement_type < 0 || announcement_type >= TAXONOMY_COUNT)
        return nullptr;
    return &TAXONOMY[announcement_type];
}

inline uint8_t section_for(int announcement_type, int alert_type) {
    const AnnounceTaxon* taxon = taxon_for(announcement_type);
    uint8_t section = taxon ? taxon->section : SECTION_MISC;
    if (section != SECTION_MISC)
        return section;
    for (int i = 0; i < ALERT_RESCUE_COUNT; ++i) {
        if (ALERT_RESCUE[i].alert_type == alert_type)
            return ALERT_RESCUE[i].section;
    }
    return SECTION_MISC;
}

inline uint8_t flags_for(int announcement_type) {
    const AnnounceTaxon* taxon = taxon_for(announcement_type);
    return taxon ? taxon->flags : 0;
}

// -1 when `key` is not a section key ("all", "", garbage) -- the route reads that as "no filter".
inline int section_from_key(const char* key, size_t len) {
    if (!key || len == 0)
        return -1;
    for (int i = 0; i < SECTION_COUNT; ++i) {
        const char* candidate = SECTION_INFO[i].key;
        size_t n = 0;
        while (candidate[n] && n < len && candidate[n] == key[n]) ++n;
        if (!candidate[n] && n == len)
            return i;
    }
    return -1;
}

} // namespace taxonomy
} // namespace dfcapture
