// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
// SPDX-License-Identifier: AGPL-3.0-only

#include "lever_link.h"

#include "fort_stock.h"
#include "http_server.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "Core.h"
#include "modules/Buildings.h"
#include "modules/Items.h"
#include "modules/Job.h"

#include "df/building.h"
#include "df/building_bars_floorst.h"
#include "df/building_bars_verticalst.h"
#include "df/building_bridgest.h"
#include "df/building_cagest.h"
#include "df/building_chainst.h"
#include "df/building_doorst.h"
#include "df/building_floodgatest.h"
#include "df/building_gear_assemblyst.h"
#include "df/building_grate_floorst.h"
#include "df/building_grate_wallst.h"
#include "df/building_hatchst.h"
#include "df/building_rollersst.h"
#include "df/building_trapst.h"
#include "df/building_weaponst.h"
#include "df/general_ref_building_holderst.h"
#include "df/general_ref_building_triggertargetst.h"
#include "df/global_objects.h"
#include "df/item.h"
#include "df/item_trappartsst.h"
#include "df/job.h"
#include "df/job_role_type.h"
#include "df/job_type.h"
#include "df/trap_type.h"
#include "df/world.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <new>
#include <sstream>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_lever_link_mutex;

template <typename Fn>
bool run_lever_link_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> link_lock(g_lever_link_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    CoreSuspender suspend;
    if (save_barrier_active())
        return false;
    return fn();
}

struct MechanismRow {
    int32_t id = -1;
    std::string name;
    int32_t x = 0, y = 0, z = 0;
};

struct TargetRow {
    int32_t id = -1;
    std::string name;
    std::string type;
    int32_t x = 0, y = 0, z = 0;
    int32_t distance = 0;
};

bool built_actual(df::building* building) {
    return building && building->isActual() &&
        building->getBuildStage() >= building->getMaxBuildStage() &&
        !Buildings::markedForRemoval(building);
}

bool is_lever(df::building* building) {
    auto trap = virtual_cast<df::building_trapst>(building);
    return trap && trap->trap_type == df::trap_type::Lever && built_actual(building);
}

std::string building_name(df::building* building, const std::string& fallback) {
    std::string name = Buildings::getName(building);
    return name.empty() ? fallback + " #" + std::to_string(building ? building->id : -1)
                        : name;
}

bool linkable_target(df::building* building, std::string& label) {
    if (!built_actual(building))
        return false;
    switch (building->getType()) {
    case df::building_type::Floodgate:
        label = "Floodgate"; return virtual_cast<df::building_floodgatest>(building);
    case df::building_type::Bridge: {
        auto bridge = virtual_cast<df::building_bridgest>(building);
        if (bridge && bridge->direction == df::building_bridgest::Retracting) {
            label = "Bridge"; return true;
        }
        return false;
    }
    case df::building_type::Door:
        label = "Door"; return virtual_cast<df::building_doorst>(building);
    case df::building_type::Hatch:
        label = "Hatch"; return virtual_cast<df::building_hatchst>(building);
    case df::building_type::GrateFloor:
        label = "Floor Grate"; return virtual_cast<df::building_grate_floorst>(building);
    case df::building_type::GrateWall:
        label = "Wall Grate"; return virtual_cast<df::building_grate_wallst>(building);
    case df::building_type::BarsFloor:
        label = "Floor Bars"; return virtual_cast<df::building_bars_floorst>(building);
    case df::building_type::BarsVertical:
        label = "Vertical Bars"; return virtual_cast<df::building_bars_verticalst>(building);
    case df::building_type::Cage:
        label = "Cage"; return virtual_cast<df::building_cagest>(building);
    case df::building_type::Chain:
        label = "Chain"; return virtual_cast<df::building_chainst>(building);
    case df::building_type::GearAssembly:
        label = "Gear Assembly"; return virtual_cast<df::building_gear_assemblyst>(building);
    case df::building_type::Weapon:
        label = "Spike"; return virtual_cast<df::building_weaponst>(building);
    case df::building_type::Trap: {
        auto trap = virtual_cast<df::building_trapst>(building);
        if (trap && trap->trap_type == df::trap_type::TrackStop) {
            label = "Track Stop"; return true;
        }
        return false;
    }
    case df::building_type::Rollers:
        label = "Roller"; return virtual_cast<df::building_rollersst>(building);
    default:
        return false;
    }
}

bool available_mechanism(df::item* item) {
    return is_fort_stock_item(item, FortItemPurpose::Available) &&
        item->getType() == df::item_type::TRAPPARTS && !item->flags.bits.hidden;
}

std::vector<MechanismRow> collect_mechanisms() {
    std::vector<MechanismRow> rows;
    auto world = df::global::world;
    if (!world)
        return rows;
    for (auto item : world->items.other.TRAPPARTS) {
        if (!available_mechanism(item))
            continue;
        const df::coord pos = Items::getPosition(item);
        rows.push_back({item->id, Items::getDescription(item, 0, false),
                        pos.x, pos.y, pos.z});
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    return rows;
}

std::vector<TargetRow> collect_targets(df::building* lever) {
    std::vector<TargetRow> rows;
    auto world = df::global::world;
    if (!world || !lever)
        return rows;
    for (auto building : world->buildings.all) {
        if (!building || building->id == lever->id)
            continue;
        std::string type;
        if (!linkable_target(building, type))
            continue;
        const int dz = std::abs(building->z - lever->z);
        const int distance = std::abs(building->centerx - lever->centerx) +
            std::abs(building->centery - lever->centery) + dz * 10;
        rows.push_back({building->id, building_name(building, type), type,
                        building->centerx, building->centery, building->z, distance});
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.id < b.id;
    });
    return rows;
}

std::string lever_link_json(int32_t id, std::string* err) {
    std::string json;
    bool ok = run_lever_link_locked([&]() {
        auto lever = df::building::find(id);
        if (!lever) {
            if (err) *err = "building not found";
            return false;
        }
        if (!is_lever(lever)) {
            json = "{\"ok\":true,\"id\":" + std::to_string(id) +
                ",\"isLever\":false}\n";
            return true;
        }
        const auto mechanisms = collect_mechanisms();
        const auto targets = collect_targets(lever);
        std::ostringstream out;
        out << "{\"ok\":true,\"id\":" << id << ",\"isLever\":true"
            << ",\"name\":" << json_string(building_name(lever, "Lever"))
            << ",\"mechanismCount\":" << mechanisms.size()
            << ",\"needsMechanisms\":" << (mechanisms.size() < 2 ? "true" : "false")
            << ",\"targets\":[";
        for (size_t i = 0; i < targets.size(); ++i) {
            if (i) out << ",";
            const auto& target = targets[i];
            out << "{\"id\":" << target.id << ",\"name\":" << json_string(target.name)
                << ",\"type\":" << json_string(target.type)
                << ",\"x\":" << target.x << ",\"y\":" << target.y << ",\"z\":" << target.z
                << ",\"distance\":" << target.distance << "}";
        }
        out << "]}\n";
        json = out.str();
        return true;
    });
    return ok ? json : "";
}

void delete_unlinked_job(df::job* job) {
    if (!job)
        return;
    while (!job->items.empty())
        Job::disconnectJobItem(job, job->items.back());
    Job::deleteJobStruct(job, true);
}

bool queue_link_job(int32_t lever_id, int32_t target_id, int32_t& job_id, std::string* err) {
    return run_lever_link_locked([&]() {
        auto lever = df::building::find(lever_id);
        auto target = df::building::find(target_id);
        std::string target_label;
        if (!is_lever(lever)) {
            if (err) *err = "building is not a built lever";
            return false;
        }
        if (!target || !linkable_target(target, target_label)) {
            if (err) *err = "target is not linkable";
            return false;
        }
        auto mechanisms = collect_mechanisms();
        if (mechanisms.size() < 2) {
            if (err) *err = "needs two available mechanisms";
            return false;
        }
        auto trigger_mech = df::item::find(mechanisms[0].id);
        auto target_mech = df::item::find(mechanisms[1].id);
        if (!available_mechanism(trigger_mech) || !available_mechanism(target_mech) ||
                trigger_mech == target_mech) {
            if (err) *err = "mechanisms are no longer available";
            return false;
        }

        auto holder_ref = df::allocate<df::general_ref_building_holderst>();
        auto target_ref = df::allocate<df::general_ref_building_triggertargetst>();
        auto job = new (std::nothrow) df::job();
        if (!holder_ref || !target_ref || !job) {
            delete holder_ref;
            delete target_ref;
            delete job;
            if (err) *err = "allocation failed";
            return false;
        }
        holder_ref->building_id = lever->id;
        target_ref->building_id = target->id;
        job->job_type = df::job_type::LinkBuildingToTrigger;
        job->pos = df::coord(lever->centerx, lever->centery, lever->z);
        if (!Job::attachJobItem(job, trigger_mech, df::job_role_type::LinkToTrigger) ||
                !Job::attachJobItem(job, target_mech, df::job_role_type::LinkToTarget)) {
            delete_unlinked_job(job);
            delete holder_ref;
            delete target_ref;
            if (err) *err = "failed to reserve mechanisms";
            return false;
        }
        job->general_refs.push_back(holder_ref);
        job->general_refs.push_back(target_ref);
        lever->jobs.push_back(job);
        if (!Job::linkIntoWorld(job)) {
            lever->jobs.erase(std::remove(lever->jobs.begin(), lever->jobs.end(), job),
                              lever->jobs.end());
            delete_unlinked_job(job);
            if (err) *err = "failed to queue link job";
            return false;
        }
        job_id = job->id;
        return true;
    });
}

} // namespace

void register_lever_link_routes(httplib::Server& server) {
    server.Get("/lever-link", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = lever_link_json(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });
    server.Post("/lever-link", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, target = -1;
        if (!query_int(req, "id", id) || !query_int(req, "target", target)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id/target\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        int32_t job_id = -1;
        std::string err;
        if (!queue_link_job(id, target, job_id, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"jobId\":" + std::to_string(job_id) + "}\n",
                        "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
