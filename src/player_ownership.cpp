// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "player_ownership.h"

#include "Core.h"
#include "MiscUtils.h"
#include "attribution.h"
#include "httplib.h"
#include "json_util.h"
#include "sdl_capture.h"
#include "session_policy.h"

#include "df/global_objects.h"
#include "df/building.h"
#include "df/building_workshopst.h"
#include "df/burrow.h"
#include "df/job.h"
#include "df/job_postingst.h"
#include "df/job_skill.h"
#include "df/job_type.h"
#include "df/mood_type.h"
#include "df/profession.h"
#include "df/reaction.h"
#include "df/unit.h"
#include "df/unit_health_info.h"
#include "df/unit_labor.h"
#include "df/workshop_type.h"
#include "df/world.h"
#include "modules/Burrows.h"
#include "modules/Job.h"
#include "modules/Maps.h"
#include "modules/Units.h"

#include "json/json.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace DFHack;
namespace fs = std::filesystem;

namespace dfcapture {
namespace {

constexpr int kSchema = 1;
constexpr size_t kMaxHistory = 1024;
constexpr size_t kMaxDecisions = 80;
constexpr long long kDispatchIntervalMs = 2000;
constexpr long long kUnitCooldownMs = 10000;
constexpr long long kDecisionRepeatMs = 5000;
constexpr long long kAssignmentGraceMs = 1500;
constexpr long long kAssignmentStableMs = 15000;
constexpr int kCircuitBreakerFailures = 3;

struct Record {
    int32_t unit_id = -1;
    int32_t historical_figure_id = -1;
    std::string player_id;
    std::string player_name;
    std::string assigned_by;
    long long assigned_at_ms = 0;
    std::string notes;
};

struct History {
    int32_t unit_id = -1;
    std::string action;
    std::string from_player;
    std::string to_player;
    std::string actor;
    long long timestamp_ms = 0;
};

struct SchedulerDecision {
    long long timestamp_ms = 0;
    std::string outcome;
    std::string reason;
    std::string player_id;
    std::string player_name;
    int32_t order_id = -1;
    int32_t job_id = -1;
    std::string job_name;
    int32_t unit_id = -1;
    std::string unit_name;
};

struct PendingAssignment {
    int32_t job_id = -1;
    int32_t unit_id = -1;
    int32_t order_id = -1;
    std::string player_id;
    long long assigned_at_ms = 0;
};

std::mutex g_mutex;
std::string g_save_dir;
bool g_loaded = false;
bool g_scheduler_enabled = false;
bool g_scheduler_auto_disabled = false;
int g_scheduler_consecutive_failures = 0;
long long g_scheduler_last_check_ms = 0;
long long g_scheduler_last_dispatch_ms = 0;
std::string g_scheduler_status = "Off";
std::unordered_map<int32_t, Record> g_records;
std::vector<History> g_history;
std::deque<SchedulerDecision> g_scheduler_decisions;
std::vector<PendingAssignment> g_pending_assignments;
std::unordered_map<int32_t, long long> g_unit_cooldowns;
std::unordered_map<std::string, long long> g_skip_cooldowns;

long long system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool safe_save_dir(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    // DF permits user-facing save names such as "autosave 1" and "mining setup".
    // The name is used as exactly one path component, so reject traversal/separators and Windows
    // alternate-stream/invalid characters instead of incorrectly restricting it to identifiers.
    if (value == "." || value == "..") return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch != '/' && ch != '\\' && ch != ':' &&
               ch != '<' && ch != '>' && ch != '"' && ch != '|' &&
               ch != '?' && ch != '*';
    });
}

fs::path ownership_path(const std::string& save_dir) {
    return fs::path("save") / save_dir / "dfcapture-ownership.json";
}

Json::Value record_json(const Record& record) {
    Json::Value value(Json::objectValue);
    value["unitId"] = record.unit_id;
    value["historicalFigureId"] = record.historical_figure_id;
    value["playerId"] = record.player_id;
    value["playerName"] = record.player_name;
    value["assignedBy"] = record.assigned_by;
    value["assignedAt"] = Json::Int64(record.assigned_at_ms);
    value["notes"] = record.notes;
    return value;
}

Json::Value history_json(const History& history) {
    Json::Value value(Json::objectValue);
    value["unitId"] = history.unit_id;
    value["action"] = history.action;
    value["fromPlayerId"] = history.from_player;
    value["toPlayerId"] = history.to_player;
    value["actorPlayerId"] = history.actor;
    value["timestamp"] = Json::Int64(history.timestamp_ms);
    return value;
}

bool persist_state(const std::string& save_dir,
                   const std::unordered_map<int32_t, Record>& records,
                   const std::vector<History>& history,
                   bool scheduler_enabled,
                   std::string* err) {
    if (!safe_save_dir(save_dir)) {
        if (err) *err = "no valid fortress save is loaded";
        return false;
    }
    Json::Value root(Json::objectValue);
    root["schema"] = kSchema;
    root["saveDir"] = save_dir;
    root["records"] = Json::Value(Json::arrayValue);
    std::vector<int32_t> ids;
    ids.reserve(records.size());
    for (const auto& entry : records) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    for (int32_t id : ids) root["records"].append(record_json(records.at(id)));
    root["history"] = Json::Value(Json::arrayValue);
    const size_t begin = history.size() > kMaxHistory ? history.size() - kMaxHistory : 0;
    for (size_t i = begin; i < history.size(); ++i)
        root["history"].append(history_json(history[i]));
    root["scheduler"]["preferOwnedDwarves"] = scheduler_enabled;

    const fs::path target = ownership_path(save_dir);
    const fs::path temporary = target.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        if (err) *err = "cannot create ownership save directory: " + ec.message();
        return false;
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            if (err) *err = "cannot write ownership temporary file";
            return false;
        }
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
        if (writer->write(root, &file) != 0) {
            if (err) *err = "ownership serialization failed";
            return false;
        }
        file << "\n";
        file.flush();
        if (!file.good()) {
            if (err) *err = "ownership write failed";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    fs::rename(temporary, target, ec);
#endif
    if (ec) {
        if (err) *err = "ownership atomic replace failed: " + ec.message();
        return false;
    }
    return true;
}

void load_state_locked(const std::string& save_dir) {
    g_save_dir = save_dir;
    g_loaded = true;
    g_records.clear();
    g_history.clear();
    g_scheduler_enabled = false;
    g_scheduler_auto_disabled = false;
    g_scheduler_consecutive_failures = 0;
    g_scheduler_last_check_ms = 0;
    g_scheduler_last_dispatch_ms = 0;
    g_scheduler_status = "Off";
    g_scheduler_decisions.clear();
    g_pending_assignments.clear();
    g_unit_cooldowns.clear();
    g_skip_cooldowns.clear();
    if (!safe_save_dir(save_dir)) return;
    std::ifstream file(ownership_path(save_dir), std::ios::binary);
    if (!file) return;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, file, &root, &errors) ||
            root.get("schema", 0).asInt() != kSchema ||
            root.get("saveDir", "").asString() != save_dir)
        return;
    g_scheduler_enabled =
        root["scheduler"].get("preferOwnedDwarves", false).asBool();
    g_scheduler_status = g_scheduler_enabled ? "Watching manager-order jobs" : "Off";
    for (const auto& value : root["records"]) {
        Record record;
        record.unit_id = value.get("unitId", -1).asInt();
        record.historical_figure_id = value.get("historicalFigureId", -1).asInt();
        record.player_id = value.get("playerId", "").asString();
        record.player_name = value.get("playerName", "").asString();
        record.assigned_by = value.get("assignedBy", "").asString();
        record.assigned_at_ms = value.get("assignedAt", Json::Int64(0)).asInt64();
        record.notes = value.get("notes", "").asString();
        if (record.unit_id >= 0 && is_safe_player_id(record.player_id))
            g_records[record.unit_id] = std::move(record);
    }
    for (const auto& value : root["history"]) {
        History history;
        history.unit_id = value.get("unitId", -1).asInt();
        history.action = value.get("action", "").asString();
        history.from_player = value.get("fromPlayerId", "").asString();
        history.to_player = value.get("toPlayerId", "").asString();
        history.actor = value.get("actorPlayerId", "").asString();
        history.timestamp_ms = value.get("timestamp", Json::Int64(0)).asInt64();
        if (history.unit_id >= 0) g_history.push_back(std::move(history));
    }
    if (g_history.size() > kMaxHistory)
        g_history.erase(g_history.begin(), g_history.end() - kMaxHistory);
}

std::string current_save_dir() {
    return df::global::world ? df::global::world->cur_savegame.save_dir : std::string();
}

void ensure_world_locked(const std::string& save_dir) {
    if (!g_loaded || g_save_dir != save_dir) load_state_locked(save_dir);
}

bool assignable_citizen(df::unit* unit) {
    return unit && Units::isCitizen(unit, true) && Units::isActive(unit) &&
           !Units::isDead(unit) && !Units::isUndead(unit);
}

void add_scheduler_decision_locked(SchedulerDecision decision) {
    g_scheduler_decisions.push_back(std::move(decision));
    while (g_scheduler_decisions.size() > kMaxDecisions)
        g_scheduler_decisions.pop_front();
}

df::job* find_world_job(int32_t job_id) {
    if (!df::global::world) return nullptr;
    for (df::job_list_link* link = df::global::world->jobs.list.next;
         link; link = link->next)
        if (link->item && link->item->id == job_id)
            return link->item;
    return nullptr;
}

df::coord job_target(df::job* job, df::building* holder) {
    if (job && job->pos.isValid()) return job->pos;
    if (holder) return df::coord(holder->centerx, holder->centery, holder->z);
    return df::coord();
}

struct JobRequirement {
    df::unit_labor labor = df::unit_labor::NONE;
    df::job_skill skill = df::job_skill::NONE;
};

df::job_skill first_skill_for_labor(df::unit_labor labor) {
    FOR_ENUM_ITEMS(job_skill, skill)
        if (skill != df::job_skill::NONE &&
                ENUM_ATTR(job_skill, labor, skill) == labor)
            return skill;
    return df::job_skill::NONE;
}

JobRequirement resolve_job_requirement(df::job* job,
                                       df::building_workshopst* workshop) {
    JobRequirement requirement;
    if (!job) return requirement;
    requirement.labor = ENUM_ATTR(job_type, labor, job->job_type);
    requirement.skill = ENUM_ATTR(job_type, skill, job->job_type);

    if (job->job_type == df::job_type::CustomReaction) {
        for (auto reaction : df::reaction::get_vector()) {
            if (!reaction || reaction->code != job->reaction_name) continue;
            requirement.skill = reaction->skill;
            requirement.labor =
                ENUM_ATTR(job_skill, labor, requirement.skill);
            return requirement;
        }
        return {};
    }

    if (requirement.labor == df::unit_labor::NONE &&
            requirement.skill != df::job_skill::NONE)
        requirement.labor =
            ENUM_ATTR(job_skill, labor, requirement.skill);
    if (requirement.labor != df::unit_labor::NONE || !workshop)
        return requirement;

    df::job_skill material_skill = df::job_skill::NONE;
    switch (workshop->type) {
    case df::workshop_type::MetalsmithsForge:
    case df::workshop_type::MagmaForge:
        material_skill = ENUM_ATTR(job_type, skill_metal, job->job_type);
        break;
    case df::workshop_type::Carpenters:
    case df::workshop_type::Bowyers:
        material_skill = ENUM_ATTR(job_type, skill_wood, job->job_type);
        break;
    case df::workshop_type::Masons:
        material_skill = ENUM_ATTR(job_type, skill_stone, job->job_type);
        break;
    default:
        break;
    }
    if (material_skill != df::job_skill::NONE) {
        requirement.skill = material_skill;
        requirement.labor = ENUM_ATTR(job_skill, labor, material_skill);
        return requirement;
    }

    // A small set of workshops has one unambiguous fallback labor. Multi-labor
    // workshops remain unsupported unless the job or reaction declares its skill.
    switch (workshop->type) {
    case df::workshop_type::Carpenters:
        requirement.labor = df::unit_labor::CARPENTER; break;
    case df::workshop_type::Masons:
        requirement.labor = df::unit_labor::MASON; break;
    case df::workshop_type::Bowyers:
        requirement.labor = df::unit_labor::BOWYER; break;
    case df::workshop_type::Mechanics:
        requirement.labor = df::unit_labor::MECHANIC; break;
    case df::workshop_type::Siege:
        requirement.labor = df::unit_labor::SIEGECRAFT; break;
    case df::workshop_type::Butchers:
        requirement.labor = df::unit_labor::BUTCHER; break;
    case df::workshop_type::Leatherworks:
        requirement.labor = df::unit_labor::LEATHER; break;
    case df::workshop_type::Tanners:
        requirement.labor = df::unit_labor::TANNER; break;
    case df::workshop_type::Clothiers:
        requirement.labor = df::unit_labor::CLOTHESMAKER; break;
    case df::workshop_type::Still:
        requirement.labor = df::unit_labor::BREWER; break;
    case df::workshop_type::Loom:
        requirement.labor = df::unit_labor::WEAVER; break;
    case df::workshop_type::Quern:
    case df::workshop_type::Millstone:
        requirement.labor = df::unit_labor::MILLER; break;
    case df::workshop_type::Kitchen:
        requirement.labor = df::unit_labor::COOK; break;
    case df::workshop_type::Dyers:
        requirement.labor = df::unit_labor::DYER; break;
    default:
        break;
    }
    requirement.skill = first_skill_for_labor(requirement.labor);
    return requirement;
}

std::string unit_scheduler_rejection(df::unit* unit, df::job* job,
                                     df::building_workshopst* workshop,
                                     const JobRequirement& requirement,
                                     const df::coord& target) {
    const df::unit_labor labor = requirement.labor;
    if (!unit || !Units::isCitizen(unit) || !Units::isOwnCiv(unit) ||
            !Units::isOwnGroup(unit) || !Units::isActive(unit) ||
            Units::isDead(unit) || Units::isUndead(unit))
        return "not an active sane fortress citizen";
    if (unit->job.current_job) return "not idle";
    if (unit->flags2.bits.visitor || unit->flags3.bits.ghostly ||
            unit->flags1.bits.caged || unit->flags1.bits.chained)
        return "not freely available";
    if (!ENUM_ATTR(profession, can_assign_labor, unit->profession) ||
            ENUM_ATTR(profession, military, unit->profession))
        return "military or labor-ineligible";
    if (unit->mood != df::mood_type::None)
        return "in a mood";
    if (unit->following || !unit->social_activities.empty())
        return "following someone or in a social activity";
    if (unit->counters.unconscious > 0 || unit->status2.limbs_grasp_count == 0)
        return "unconscious or unable to work";
    if (unit->health &&
            (unit->health->flags.bits.needs_healthcare ||
             unit->health->flags.bits.should_not_move))
        return "needs healthcare";
    if (unit->counters2.thirst_timer >= 25000)
        return "needs to drink";
    if (unit->counters2.hunger_timer >= 50000)
        return "needs to eat";
    if (unit->counters2.sleepiness_timer >= 57600)
        return "needs to sleep";
    if (labor == df::unit_labor::NONE || !Units::isValidLabor(unit, labor))
        return "job has no safe labor mapping for this dwarf";
    if (!unit->status.labors[labor])
        return "required labor is disabled";
    if (!workshop)
        return "job is not held by a workshop";
    const auto& profile = workshop->profile;
    if (!profile.permitted_workers.empty() &&
            std::find(profile.permitted_workers.begin(),
                      profile.permitted_workers.end(), unit->id) ==
                profile.permitted_workers.end())
        return "not permitted by the workshop worker profile";
    if (profile.blocked_labors[labor])
        return "labor is blocked by the workshop profile";
    const df::job_skill skill = requirement.skill;
    if (skill == df::job_skill::NONE) {
        if (profile.min_level > 0 || profile.max_level < 20)
            return "cannot safely evaluate the workshop skill restriction";
    } else {
        const int level = Units::getNominalSkill(unit, skill, false);
        if (level < profile.min_level || level > profile.max_level)
            return "outside the workshop skill range";
    }
    if (!target.isValid())
        return "job has no reachable target";
    if (!unit->burrows.empty()) {
        bool target_in_burrow = false;
        for (int32_t burrow_id : unit->burrows) {
            auto burrow = df::burrow::find(burrow_id);
            if (burrow && !burrow->flags.bits.suspended &&
                    Burrows::isAssignedTile(burrow, target)) {
                target_in_burrow = true;
                break;
            }
        }
        if (!target_in_burrow)
            return "job target is outside the dwarf's active burrows";
    }
    if (!Maps::canWalkBetween(Units::getPosition(unit), target))
        return "job target is unreachable";
    return {};
}

void note_skip_locked(long long now, df::job* job, const std::string& actor,
                      const std::string& reason, int32_t unit_id = -1,
                      const std::string& unit_name = {}) {
    const std::string key = std::to_string(job ? job->id : -1) + ":" + reason;
    const auto found = g_skip_cooldowns.find(key);
    if (found != g_skip_cooldowns.end() &&
            now - found->second < kDecisionRepeatMs)
        return;
    g_skip_cooldowns[key] = now;
    SchedulerDecision decision;
    decision.timestamp_ms = now;
    decision.outcome = "skipped";
    decision.reason = reason;
    decision.player_id = actor;
    decision.player_name = session_display_name(actor);
    decision.order_id = job ? job->order_id : -1;
    decision.job_id = job ? job->id : -1;
    decision.job_name = job ? DF2UTF(Job::getName(job)) : std::string();
    decision.unit_id = unit_id;
    decision.unit_name = unit_name;
    add_scheduler_decision_locked(std::move(decision));
}

void disable_scheduler_after_failure_locked(const std::string& save_dir) {
    if (g_scheduler_consecutive_failures < kCircuitBreakerFailures) return;
    g_scheduler_enabled = false;
    g_scheduler_auto_disabled = true;
    g_scheduler_status =
        "Automatically disabled after repeated rejected or cancelled assignments";
    std::string persist_error;
    if (!persist_state(save_dir, g_records, g_history, false, &persist_error))
        g_scheduler_status += "; could not persist: " + persist_error;
}

void monitor_pending_assignments_locked(long long now,
                                        const std::string& save_dir) {
    for (auto it = g_pending_assignments.begin();
         it != g_pending_assignments.end();) {
        df::job* job = find_world_job(it->job_id);
        df::unit* unit = df::unit::find(it->unit_id);
        const long long age = now - it->assigned_at_ms;
        if (!job) {
            g_scheduler_consecutive_failures = 0;
            SchedulerDecision decision;
            decision.timestamp_ms = now;
            decision.outcome = "completed";
            decision.reason = "The assigned job left DF's active job list.";
            decision.player_id = it->player_id;
            decision.player_name = session_display_name(it->player_id);
            decision.order_id = it->order_id;
            decision.job_id = it->job_id;
            decision.unit_id = it->unit_id;
            decision.unit_name =
                unit ? DF2UTF(Units::getReadableName(unit)) : std::string();
            add_scheduler_decision_locked(std::move(decision));
            it = g_pending_assignments.erase(it);
            continue;
        }
        if (unit && unit->job.current_job == job) {
            if (age >= kAssignmentStableMs) {
                g_scheduler_consecutive_failures = 0;
                SchedulerDecision decision;
                decision.timestamp_ms = now;
                decision.outcome = "confirmed";
                decision.reason = "DF kept the owned dwarf on the job.";
                decision.player_id = it->player_id;
                decision.player_name = session_display_name(it->player_id);
                decision.order_id = it->order_id;
                decision.job_id = it->job_id;
                decision.job_name = DF2UTF(Job::getName(job));
                decision.unit_id = it->unit_id;
                decision.unit_name = DF2UTF(Units::getReadableName(unit));
                add_scheduler_decision_locked(std::move(decision));
                it = g_pending_assignments.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (age < kAssignmentGraceMs) {
            ++it;
            continue;
        }
        ++g_scheduler_consecutive_failures;
        SchedulerDecision decision;
        decision.timestamp_ms = now;
        decision.outcome = "failed";
        decision.reason =
            "DF rejected or cancelled the worker attachment before it stabilized.";
        decision.player_id = it->player_id;
        decision.player_name = session_display_name(it->player_id);
        decision.order_id = it->order_id;
        decision.job_id = it->job_id;
        decision.job_name = DF2UTF(Job::getName(job));
        decision.unit_id = it->unit_id;
        decision.unit_name =
            unit ? DF2UTF(Units::getReadableName(unit)) : std::string();
        add_scheduler_decision_locked(std::move(decision));
        it = g_pending_assignments.erase(it);
        disable_scheduler_after_failure_locked(save_dir);
    }
}

Json::Value snapshot_json(bool host) {
    Json::Value root(Json::objectValue);
    root["ok"] = true;
    root["schema"] = kSchema;
    root["saveDir"] = g_save_dir;
    root["host"] = host;
    root["advisory"] = !g_scheduler_enabled;
    root["scheduler"]["mode"] =
        g_scheduler_enabled ? "guarded-manager-orders" : "off";
    root["scheduler"]["enabled"] = g_scheduler_enabled;
    root["scheduler"]["available"] = true;
    root["scheduler"]["autoDisabled"] = g_scheduler_auto_disabled;
    root["scheduler"]["consecutiveFailures"] = g_scheduler_consecutive_failures;
    root["scheduler"]["failureLimit"] = kCircuitBreakerFailures;
    root["scheduler"]["lastCheckAt"] = Json::Int64(g_scheduler_last_check_ms);
    root["scheduler"]["lastDispatchAt"] = Json::Int64(g_scheduler_last_dispatch_ms);
    root["scheduler"]["status"] = g_scheduler_status;
    root["scheduler"]["scope"] =
        "Attributed manager-order workshop jobs; unassigned jobs and idle owned dwarves only.";
    root["scheduler"]["recentDecisions"] = Json::Value(Json::arrayValue);
    for (auto it = g_scheduler_decisions.rbegin();
         it != g_scheduler_decisions.rend(); ++it) {
        Json::Value row(Json::objectValue);
        row["timestamp"] = Json::Int64(it->timestamp_ms);
        row["outcome"] = it->outcome;
        row["reason"] = it->reason;
        row["playerId"] = it->player_id;
        row["playerName"] = it->player_name;
        row["orderId"] = it->order_id;
        row["jobId"] = it->job_id;
        row["jobName"] = it->job_name;
        row["unitId"] = it->unit_id;
        row["unitName"] = it->unit_name;
        root["scheduler"]["recentDecisions"].append(row);
    }

    std::unordered_map<std::string, std::string> player_names;
    std::set<std::string> online;
    for (const auto& player : session_players_snapshot()) {
        player_names[player.player_id] =
            player.name.empty() ? player.player_id : player.name;
        online.insert(player.player_id);
    }
    for (const auto& entry : g_records)
        if (!entry.second.player_id.empty())
            player_names.emplace(entry.second.player_id,
                                 entry.second.player_name.empty()
                                     ? entry.second.player_id : entry.second.player_name);

    root["players"] = Json::Value(Json::arrayValue);
    for (const auto& entry : player_names) {
        Json::Value player(Json::objectValue);
        player["playerId"] = entry.first;
        player["name"] = entry.second;
        player["online"] = online.count(entry.first) != 0;
        root["players"].append(player);
    }

    std::set<int32_t> active_ids;
    std::unordered_map<std::string, int> owned_counts;
    int unowned = 0;
    int aligned = 0;
    int mismatched = 0;
    int untracked_orders = 0;
    root["units"] = Json::Value(Json::arrayValue);
    if (df::global::world) {
        for (auto unit : df::global::world->units.active) {
            if (!assignable_citizen(unit)) continue;
            active_ids.insert(unit->id);
            Json::Value row(Json::objectValue);
            row["unitId"] = unit->id;
            row["historicalFigureId"] = unit->hist_figure_id;
            row["name"] = DF2UTF(Units::getReadableName(unit));
            row["profession"] = DF2UTF(Units::getProfessionName(unit));
            row["active"] = true;
            auto found = g_records.find(unit->id);
            if (found != g_records.end()) {
                const Record& record = found->second;
                row["owner"] = record_json(record);
                owned_counts[record.player_id]++;
            } else {
                row["owner"] = Json::nullValue;
                ++unowned;
            }
            if (unit->job.current_job) {
                row["currentJob"]["id"] = unit->job.current_job->id;
                row["currentJob"]["name"] = DF2UTF(Job::getName(unit->job.current_job));
                row["currentJob"]["orderId"] = unit->job.current_job->order_id;
                if (unit->job.current_job->order_id >= 0) {
                    std::string actor;
                    if (attrib_lookup(AttribKind::Order,
                                      unit->job.current_job->order_id, actor)) {
                        row["currentJob"]["orderActorPlayerId"] = actor;
                        row["currentJob"]["orderActorDisplayName"] =
                            session_display_name(actor);
                        if (found != g_records.end()) {
                            const bool same = actor == found->second.player_id;
                            row["currentJob"]["ownerAligned"] = same;
                            same ? ++aligned : ++mismatched;
                        }
                    } else {
                        ++untracked_orders;
                    }
                }
            } else {
                row["currentJob"] = Json::nullValue;
            }
            root["units"].append(row);
        }
    }

    root["historical"] = Json::Value(Json::arrayValue);
    for (const auto& entry : g_records) {
        if (active_ids.count(entry.first)) continue;
        Json::Value row = record_json(entry.second);
        row["active"] = false;
        root["historical"].append(row);
    }
    root["history"] = Json::Value(Json::arrayValue);
    const size_t begin = g_history.size() > 100 ? g_history.size() - 100 : 0;
    for (size_t i = begin; i < g_history.size(); ++i)
        root["history"].append(history_json(g_history[i]));

    root["analytics"]["activeCitizens"] = Json::UInt64(active_ids.size());
    root["analytics"]["unownedCitizens"] = unowned;
    root["analytics"]["alignedActiveOrders"] = aligned;
    root["analytics"]["mismatchedActiveOrders"] = mismatched;
    root["analytics"]["untrackedActiveOrders"] = untracked_orders;
    root["analytics"]["ownedByPlayer"] = Json::Value(Json::objectValue);
    for (const auto& count : owned_counts)
        root["analytics"]["ownedByPlayer"][count.first] = count.second;
    return root;
}

std::string json_text(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value) + "\n";
}

void json_error(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_content("{\"ok\":false,\"error\":" + json_string(error) + "}\n",
                    "application/json; charset=utf-8");
}

} // namespace

void ownership_scheduler_update() {
    std::unique_lock<std::recursive_mutex> capture_lock(
        capture_state_mutex(), std::try_to_lock);
    if (!capture_lock.owns_lock() || !df::global::world ||
            (df::global::pause_state && *df::global::pause_state))
        return;

    const long long now = system_now_ms();
    const std::string save_dir = current_save_dir();
    std::unordered_map<int32_t, Record> records;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensure_world_locked(save_dir);
        monitor_pending_assignments_locked(now, save_dir);
        if (!g_scheduler_enabled || g_scheduler_auto_disabled ||
                now - g_scheduler_last_check_ms < kDispatchIntervalMs)
            return;
        for (auto it = g_skip_cooldowns.begin();
             it != g_skip_cooldowns.end();) {
            if (now - it->second > 60000)
                it = g_skip_cooldowns.erase(it);
            else
                ++it;
        }
        g_scheduler_last_check_ms = now;
        g_scheduler_status = "Checking attributed manager-order jobs";
        records = g_records;
    }

    int skipped_jobs = 0;
    for (df::job_postingst* posting : df::global::world->jobs.postings) {
        if (!posting || posting->flags.bits.dead || !posting->job) continue;
        df::job* job = posting->job;
        if (job->posting_index != posting->idx ||
                Job::getWorker(job) || job->flags.bits.suspend ||
                job->flags.bits.special || !job->flags.bits.by_manager ||
                job->order_id < 0)
            continue;

        std::string actor;
        if (!attrib_lookup(AttribKind::Order, job->order_id, actor))
            continue;

        auto holder = Job::getHolder(job);
        auto workshop = virtual_cast<df::building_workshopst>(holder);
        const JobRequirement requirement =
            resolve_job_requirement(job, workshop);
        const df::coord target = job_target(job, holder);
        df::unit* chosen = nullptr;
        long long best_distance = std::numeric_limits<long long>::max();
        std::unordered_map<std::string, int> rejection_counts;
        int32_t only_rejected_unit = -1;
        std::string only_rejected_name;
        int owned_candidates = 0;

        for (const auto& entry : records) {
            if (entry.second.player_id != actor) continue;
            ++owned_candidates;
            df::unit* unit = df::unit::find(entry.first);
            const std::string rejection =
                unit_scheduler_rejection(unit, job, workshop, requirement, target);
            if (!rejection.empty()) {
                rejection_counts[rejection]++;
                only_rejected_unit = unit ? unit->id : entry.first;
                only_rejected_name =
                    unit ? DF2UTF(Units::getReadableName(unit)) : std::string();
                continue;
            }
            long long cooldown_at = 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto cooldown = g_unit_cooldowns.find(unit->id);
                if (cooldown != g_unit_cooldowns.end())
                    cooldown_at = cooldown->second;
            }
            if (now - cooldown_at < kUnitCooldownMs) {
                rejection_counts["assignment rate limit"]++;
                only_rejected_unit = unit->id;
                only_rejected_name = DF2UTF(Units::getReadableName(unit));
                continue;
            }
            const df::coord pos = Units::getPosition(unit);
            const long long distance =
                std::abs(static_cast<long long>(pos.x) - target.x) +
                std::abs(static_cast<long long>(pos.y) - target.y) +
                10LL * std::abs(static_cast<long long>(pos.z) - target.z);
            if (!chosen || distance < best_distance) {
                chosen = unit;
                best_distance = distance;
            }
        }

        if (!chosen) {
            std::string reason;
            if (!owned_candidates) {
                reason = "The requesting player has no owned dwarves.";
            } else {
                std::vector<std::pair<std::string, int>> counts(
                    rejection_counts.begin(), rejection_counts.end());
                std::sort(counts.begin(), counts.end(),
                          [](const auto& left, const auto& right) {
                              if (left.second != right.second)
                                  return left.second > right.second;
                              return left.first < right.first;
                          });
                reason = "No eligible owned dwarf";
                if (!counts.empty()) {
                    reason += ": ";
                    for (size_t i = 0; i < counts.size(); ++i) {
                        if (i) reason += "; ";
                        reason += counts[i].first;
                        if (counts[i].second > 1)
                            reason += " (" + std::to_string(counts[i].second) + ")";
                    }
                }
                reason += ".";
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                note_skip_locked(now, job, actor, reason,
                                 owned_candidates == 1 ? only_rejected_unit : -1,
                                 owned_candidates == 1 ? only_rejected_name
                                                       : std::string());
                g_scheduler_status = "Waiting; an attributed job was skipped by the safety guards";
            }
            if (++skipped_jobs >= 4) break;
            continue;
        }

        // Revalidate the two mutation-critical facts immediately before attaching.
        if (chosen->job.current_job || Job::getWorker(job) ||
                posting->flags.bits.dead || job->posting_index != posting->idx) {
            std::lock_guard<std::mutex> lock(g_mutex);
            note_skip_locked(now, job, actor,
                             "Job or dwarf changed before assignment; will retry.");
            continue;
        }

        const bool attached = Job::addWorker(job, chosen);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            SchedulerDecision decision;
            decision.timestamp_ms = now;
            decision.outcome = attached ? "assigned" : "failed";
            decision.reason = attached
                ? "Idle owned dwarf passed labor, health, needs, profile, burrow, and reachability guards."
                : "DFHack refused the worker attachment.";
            decision.player_id = actor;
            decision.player_name = session_display_name(actor);
            decision.order_id = job->order_id;
            decision.job_id = job->id;
            decision.job_name = DF2UTF(Job::getName(job));
            decision.unit_id = chosen->id;
            decision.unit_name = DF2UTF(Units::getReadableName(chosen));
            add_scheduler_decision_locked(std::move(decision));
            g_scheduler_last_dispatch_ms = now;
            g_unit_cooldowns[chosen->id] = now;
            if (attached) {
                g_scheduler_status = "Last assignment applied; monitoring DF";
                g_pending_assignments.push_back(
                    {job->id, chosen->id, job->order_id, actor, now});
            } else {
                ++g_scheduler_consecutive_failures;
                g_scheduler_status = "DFHack refused the last assignment";
                disable_scheduler_after_failure_locked(save_dir);
            }
        }
        break; // One guarded assignment per scheduler interval.
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_scheduler_enabled && g_scheduler_status == "Checking attributed manager-order jobs")
        g_scheduler_status = "Watching; no eligible attributed job is waiting";
}

void ownership_note_world(const std::string& save_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_loaded || g_save_dir != save_dir) load_state_locked(save_dir);
}

void ownership_clear_world() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_save_dir.clear();
    g_loaded = false;
    g_scheduler_enabled = false;
    g_scheduler_auto_disabled = false;
    g_scheduler_consecutive_failures = 0;
    g_scheduler_last_check_ms = 0;
    g_scheduler_last_dispatch_ms = 0;
    g_scheduler_status = "Off";
    g_records.clear();
    g_history.clear();
    g_scheduler_decisions.clear();
    g_pending_assignments.clear();
    g_unit_cooldowns.clear();
    g_skip_cooldowns.clear();
}

bool ownership_lookup_unit(int32_t unit_id, UnitOwnership& ownership) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_records.find(unit_id);
        if (found == g_records.end()) {
            ownership = {};
            return false;
        }
        ownership.owned = true;
        ownership.player_id = found->second.player_id;
        ownership.player_name = found->second.player_name;
        ownership.assigned_by = found->second.assigned_by;
        ownership.assigned_at_ms = found->second.assigned_at_ms;
        ownership.notes = found->second.notes;
    }
    for (const auto& player : session_players_snapshot())
        if (player.player_id == ownership.player_id) {
            ownership.online = true;
            if (!player.name.empty()) ownership.player_name = player.name;
            break;
        }
    return true;
}

void register_player_ownership_routes(httplib::Server& server) {
    server.Get("/ownership", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
        CoreSuspender suspend;
        const std::string save_dir = current_save_dir();
        if (save_dir.empty()) {
            json_error(res, 409, "load a fortress before managing dwarf ownership");
            return;
        }
        Json::Value snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ensure_world_locked(save_dir);
            snapshot = snapshot_json(session_request_is_host(req));
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json_text(snapshot), "application/json; charset=utf-8");
    });

    server.Post("/ownership-action", [](const httplib::Request& req, httplib::Response& res) {
        if (!session_request_is_host(req)) {
            json_error(res, 403, "host only");
            return;
        }
        const std::string action =
            req.has_param("action") ? req.get_param_value("action") : std::string();
        if (action != "assign" && action != "transfer" && action != "clear" &&
                action != "scheduler-toggle") {
            json_error(res, 400,
                       "action must be assign, transfer, clear, or scheduler-toggle");
            return;
        }

        std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
        CoreSuspender suspend;
        const std::string save_dir = current_save_dir();
        if (save_dir.empty()) {
            json_error(res, 409, "load a fortress before managing dwarf ownership");
            return;
        }

        if (action == "scheduler-toggle") {
            const std::string raw_enabled = req.has_param("enabled")
                ? req.get_param_value("enabled") : std::string();
            if (raw_enabled != "true" && raw_enabled != "false" &&
                    raw_enabled != "1" && raw_enabled != "0") {
                json_error(res, 400, "enabled must be true or false");
                return;
            }
            const bool enabled = raw_enabled == "true" || raw_enabled == "1";
            std::string err;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ensure_world_locked(save_dir);
                if (!persist_state(save_dir, g_records, g_history, enabled, &err)) {
                    json_error(res, 500, err);
                    return;
                }
                g_scheduler_enabled = enabled;
                g_scheduler_auto_disabled = false;
                g_scheduler_consecutive_failures = 0;
                g_scheduler_last_check_ms = 0;
                g_scheduler_last_dispatch_ms = 0;
                g_scheduler_status =
                    enabled ? "Watching manager-order jobs" : "Off";
                g_scheduler_decisions.clear();
                g_pending_assignments.clear();
                g_unit_cooldowns.clear();
                g_skip_cooldowns.clear();
                SchedulerDecision decision;
                decision.timestamp_ms = system_now_ms();
                decision.outcome = enabled ? "enabled" : "disabled";
                decision.reason = enabled
                    ? "Host enabled guarded preference for attributed manager-order jobs."
                    : "Host disabled owned-dwarf task preference.";
                decision.player_id = session_request_player_id(req);
                decision.player_name = session_display_name(decision.player_id);
                add_scheduler_decision_locked(std::move(decision));
            }
            res.set_header("Cache-Control", "no-store");
            res.set_content("{\"ok\":true}\n",
                            "application/json; charset=utf-8");
            return;
        }

        int unit_id = -1;
        if (!query_int(req, "unit", unit_id) || unit_id < 0) {
            json_error(res, 400, "invalid unit id");
            return;
        }
        const std::string owner =
            req.has_param("owner") ? req.get_param_value("owner") : std::string();
        if (action != "clear" && !is_safe_player_id(owner)) {
            json_error(res, 400, "invalid player id");
            return;
        }
        const std::string owner_name = req.has_param("ownerName")
            ? req.get_param_value("ownerName").substr(0, 32)
            : session_display_name(owner).substr(0, 32);
        const std::string notes = req.has_param("notes")
            ? req.get_param_value("notes").substr(0, 128) : std::string();

        df::unit* unit = df::unit::find(unit_id);
        if (!assignable_citizen(unit)) {
            json_error(res, 409, "unit is not an active fortress citizen");
            return;
        }

        std::string err;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ensure_world_locked(save_dir);
            auto next_records = g_records;
            auto next_history = g_history;
            const auto previous = next_records.find(unit_id);
            const std::string from_player =
                previous == next_records.end() ? std::string() : previous->second.player_id;
            const std::string actor = session_request_player_id(req);
            const long long timestamp = system_now_ms();
            if (action == "clear") {
                if (previous == next_records.end()) {
                    json_error(res, 404, "unit has no owner");
                    return;
                }
                next_records.erase(unit_id);
            } else {
                Record record;
                record.unit_id = unit_id;
                record.historical_figure_id = unit->hist_figure_id;
                record.player_id = owner;
                record.player_name = owner_name.empty() ? owner : owner_name;
                record.assigned_by = actor;
                record.assigned_at_ms = timestamp;
                record.notes = notes;
                next_records[unit_id] = std::move(record);
            }
            next_history.push_back({unit_id, action, from_player,
                                    action == "clear" ? std::string() : owner,
                                    actor, timestamp});
            if (next_history.size() > kMaxHistory)
                next_history.erase(next_history.begin(),
                                   next_history.end() - kMaxHistory);
            if (!persist_state(save_dir, next_records, next_history,
                               g_scheduler_enabled, &err)) {
                json_error(res, 500, err);
                return;
            }
            g_records.swap(next_records);
            g_history.swap(next_history);
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
