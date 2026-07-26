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

#include "hospital.h"

#include "Core.h"
#include "http_server.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"
#include "unit_activity.h"

#include "modules/Buildings.h"
#include "modules/Job.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/abstract_building.h"
#include "df/abstract_building_contents.h"
#include "df/abstract_building_hospitalst.h"
#include "df/abstract_building_type.h"
#include "df/building.h"
#include "df/building_civzonest.h"
#include "df/building_type.h"
#include "df/civzone_type.h"
#include "df/entity_position.h"
#include "df/entity_position_assignment.h"
#include "df/entity_position_responsibility.h"
#include "df/general_ref.h"
#include "df/general_ref_type.h"
#include "df/global_objects.h"
#include "df/historical_entity.h"
#include "df/historical_figure.h"
#include "df/job.h"
#include "df/job_list_link.h"
#include "df/job_type.h"
#include "df/plotinfost.h"
#include "df/unit.h"
#include "df/unit_health_flags.h"
#include "df/unit_health_info.h"
#include "df/unit_labor.h"
#include "df/unit_wound.h"
#include "df/world.h"
#include "df/world_data.h"
#include "df/world_site.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_hospital_mutex;

// Same lock discipline as trade_depot.cpp / fort_admin.cpp: panel mutex -> capture-state mutex ->
// CoreSuspender. Reads and mutations share the guard so walking sites / units / jobs never races
// the sim.
template <typename Fn>
bool run_hospital_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> hospital_lock(g_hospital_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    if (save_barrier_active())
        return false;
    return fn();
}

void set_no_store_json(httplib::Response& res, const std::string& json) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(json, "application/json; charset=utf-8");
}

void json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"error\":" + json_string(message) + "}\n",
                    "application/json; charset=utf-8");
}

// ---------------------------------------------------------------------------
// Supply table: the 7 hospital supplies. `scale` is DF's internal multiplier (thread x15000,
// cloth x10000, plaster/soap x150, others x1), as defined by abstract_building_contents and
// quickfort defaults (5 splints -> desired_splints=5; 5 thread -> desired_thread=75000
// = 5*15000; 5 cloth -> 50000 = 5*10000; 5 plaster -> desired_powder=750 = 5*150; 5 soap -> 750;
// 2 buckets -> 2). "Level" (what the player sets, what DF's Locations screen shows) = raw/scale.
// ---------------------------------------------------------------------------
struct SupplyDef {
    const char* key;
    const char* label;
    int scale;
    int32_t df::abstract_building_contents::* desired;
    int32_t df::abstract_building_contents::* count;
};

const SupplyDef kSupplies[] = {
    {"splints",  "Splints",  1,     &df::abstract_building_contents::desired_splints,  &df::abstract_building_contents::count_splints},
    {"thread",   "Thread",   15000, &df::abstract_building_contents::desired_thread,   &df::abstract_building_contents::count_thread},
    {"cloth",    "Cloth",    10000, &df::abstract_building_contents::desired_cloth,    &df::abstract_building_contents::count_cloth},
    {"crutches", "Crutches", 1,     &df::abstract_building_contents::desired_crutches, &df::abstract_building_contents::count_crutches},
    {"plaster",  "Plaster",  150,   &df::abstract_building_contents::desired_powder,   &df::abstract_building_contents::count_powder},
    {"buckets",  "Buckets",  1,     &df::abstract_building_contents::desired_buckets,  &df::abstract_building_contents::count_buckets},
    {"soap",     "Soap",     150,   &df::abstract_building_contents::desired_soap,     &df::abstract_building_contents::count_soap},
};

const SupplyDef* find_supply(const std::string& key) {
    for (const auto& s : kSupplies)
        if (key == s.key)
            return &s;
    return nullptr;
}

// need_more is a bitfield union (location_info_flag) -- no pointer-to-member of a bit, so switch.
void set_need_more(df::abstract_building_contents* c, const std::string& key, bool on) {
    auto& f = c->need_more.bits;
    if (key == "splints")       f.splints = on;
    else if (key == "thread")   f.thread = on;
    else if (key == "cloth")    f.cloth = on;
    else if (key == "crutches") f.crutches = on;
    else if (key == "plaster")  f.powder = on;
    else if (key == "buckets")  f.buckets = on;
    else if (key == "soap")     f.soap = on;
}

bool get_need_more(df::abstract_building_contents* c, const std::string& key) {
    const auto& f = c->need_more.bits;
    if (key == "splints")  return f.splints;
    if (key == "thread")   return f.thread;
    if (key == "cloth")    return f.cloth;
    if (key == "crutches") return f.crutches;
    if (key == "plaster")  return f.powder;
    if (key == "buckets")  return f.buckets;
    if (key == "soap")     return f.soap;
    return false;
}

// ---------------------------------------------------------------------------
// Resolution: a hospital is a location; the panel is entered via a zone click, so the primary key
// is a zone id (zone.location_id -> the abstract_building_hospitalst). `?location=` resolves the
// site from the current fort (plotinfo->site_id) and scans its buildings.
// ---------------------------------------------------------------------------
df::world_site* find_site(int32_t site_id) {
    auto wd = df::global::world ? df::global::world->world_data : nullptr;
    if (!wd)
        return nullptr;
    for (auto site : wd->sites)
        if (site && site->id == site_id)
            return site;
    return nullptr;
}

df::abstract_building* find_location_in_site(df::world_site* site, int32_t location_id) {
    if (!site || location_id < 0)
        return nullptr;
    for (auto loc : site->buildings)
        if (loc && loc->id == location_id)
            return loc;
    return nullptr;
}

struct HospitalRef {
    df::abstract_building_hospitalst* hosp = nullptr;
    df::abstract_building_contents* contents = nullptr;
    df::building_civzonest* zone = nullptr;       // the queried zone (null when resolved by location)
    df::world_site* site = nullptr;
    int32_t location_id = -1;
};

// Resolve by zone id (preferred) or, if zone_id<0, by location id within the current fort site.
bool resolve_hospital(int32_t zone_id, int32_t location_id, HospitalRef& out, std::string* err) {
    if (zone_id >= 0) {
        auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
        if (!zone) { if (err) *err = "zone not found"; return false; }
        out.zone = zone;
        if (zone->location_id < 0 || zone->site_id < 0) {
            if (err) *err = "zone has no location attached";
            return false;
        }
        out.site = find_site(zone->site_id);
        out.location_id = zone->location_id;
    } else if (location_id >= 0) {
        auto plotinfo = df::global::plotinfo;
        out.site = plotinfo ? find_site(plotinfo->site_id) : nullptr;
        out.location_id = location_id;
    } else {
        if (err) *err = "missing zone or location id";
        return false;
    }
    if (!out.site) { if (err) *err = "site unavailable"; return false; }
    auto loc = find_location_in_site(out.site, out.location_id);
    out.hosp = virtual_cast<df::abstract_building_hospitalst>(loc);
    if (!out.hosp) { if (err) *err = "location is not a hospital"; return false; }
    out.contents = out.hosp->getContents();
    if (!out.contents) { if (err) *err = "hospital has no contents"; return false; }
    return true;
}

std::string hospital_name(df::abstract_building_hospitalst* hosp) {
    if (!hosp)
        return "Hospital";
    std::string name = DFHack::Translation::translateName(&hosp->name, true);
    return name.empty() ? std::string("Hospital") : name;
}

// ---------------------------------------------------------------------------
// Chief medical dwarf: the fort noble position carrying HEALTH_MANAGEMENT responsibility. Same
// positions/assignments walk as trade_depot.cpp find_broker (the broker == TRADE responsibility).
// ---------------------------------------------------------------------------
struct ChiefMedical {
    bool found = false;         // a HEALTH_MANAGEMENT position exists in this fort
    int32_t position_id = -1;
    std::string position;       // "Chief Medical Dwarf"
    bool filled = false;
    int32_t unit_id = -1;
    std::string name;
    int8_t profession_color = -1;
};

ChiefMedical find_chief_medical() {
    ChiefMedical out;
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo)
        return out;
    auto fort = df::historical_entity::find(plotinfo->group_id);
    if (!fort)
        return out;
    for (auto position : fort->positions.own) {
        if (!position)
            continue;
        if (!position->responsibilities[df::entity_position_responsibility::HEALTH_MANAGEMENT])
            continue;
        out.found = true;
        out.position_id = position->id;
        out.position = position->name[0].empty() ? std::string("Chief Medical Dwarf")
                                                  : position->name[0];
        int32_t holder_hf = -1;
        for (auto asn : fort->positions.assignments) {
            if (asn && asn->position_id == position->id && asn->histfig != -1) {
                holder_hf = asn->histfig;
                break;
            }
        }
        if (holder_hf != -1) {
            for (auto u : df::global::world->units.active) {
                if (u && u->hist_figure_id == holder_hf) {
                    out.filled = true;
                    out.unit_id = u->id;
                    out.name = DFHack::Units::getReadableName(u);
                    out.profession_color = DFHack::Units::getProfessionColor(u);
                    break;
                }
            }
            if (!out.filled) {
                if (auto hf = df::historical_figure::find(holder_hf)) {
                    out.filled = true;
                    out.name = DFHack::Translation::translateName(&hf->name, true);
                }
            }
        }
        break; // one HEALTH_MANAGEMENT position (the Chief Medical Dwarf)
    }
    return out;
}

std::string chief_medical_json(const ChiefMedical& c) {
    std::ostringstream js;
    js << "{\"found\":" << (c.found ? "true" : "false")
       << ",\"positionId\":" << c.position_id
       << ",\"position\":" << json_string(c.position)
       << ",\"filled\":" << (c.filled ? "true" : "false")
       << ",\"unitId\":" << c.unit_id
       << ",\"name\":" << json_string(c.name)
       << ",\"professionColor\":" << static_cast<int>(c.profession_color) << "}";
    return js.str();
}

// ---------------------------------------------------------------------------
// Doctors: citizens with >=1 medical labor enabled. The medical labor set is exactly what
// labor.cpp maps to work_detail_icon_type::ORDERLIES.
// ---------------------------------------------------------------------------
struct MedLabor { df::unit_labor labor; const char* key; };
const MedLabor kMedLabors[] = {
    {df::unit_labor::DIAGNOSE,             "diagnose"},
    {df::unit_labor::SURGERY,              "surgery"},
    {df::unit_labor::BONE_SETTING,         "bonesetting"},
    {df::unit_labor::SUTURING,             "suturing"},
    {df::unit_labor::DRESSING_WOUNDS,      "dressing"},
    {df::unit_labor::FEED_WATER_CIVILIANS, "feedwater"},
    {df::unit_labor::RECOVER_WOUNDED,      "recover"},
};

// ---------------------------------------------------------------------------
// Medical job types (the treatment queue). Read-only visibility -- DF schedules healthcare itself.
// ---------------------------------------------------------------------------
const df::job_type kMedJobs[] = {
    df::job_type::RecoverWounded,
    df::job_type::DiagnosePatient,
    df::job_type::DressWound,
    df::job_type::CleanPatient,
    df::job_type::Surgery,
    df::job_type::Suture,
    df::job_type::PlaceInTraction,
    df::job_type::GiveWater,
    df::job_type::GiveFood,
    df::job_type::ConstructCrutch,
    df::job_type::ConstructTractionBench,
    df::job_type::BringCrutch,
    df::job_type::ApplyCast,
};

bool is_medical_job(df::job_type type) {
    for (auto medical_type : kMedJobs)
        if (medical_type == type)
            return true;
    return false;
}

df::unit* job_patient(df::job* job) {
    if (!job)
        return nullptr;
    for (auto* ref : job->general_refs) {
        if (ref && ref->getType() == df::general_ref_type::UNIT_PATIENT)
            return df::unit::find(ref->getID());
    }
    return nullptr;
}

// True iff `unit_id` is the patient of a live PlaceInTraction job (traction is scheduled, not a
// standing flag once benched -- rq_traction clears when the job is created).
bool has_traction_job(int32_t unit_id) {
    auto world = df::global::world;
    if (!world)
        return false;
    for (df::job_list_link* link = world->jobs.list.next; link; link = link->next) {
        df::job* job = link->item;
        if (!job || job->job_type != df::job_type::PlaceInTraction)
            continue;
        for (auto* ref : job->general_refs)
            if (ref && ref->getType() == df::general_ref_type::UNIT_PATIENT && ref->getID() == unit_id)
                return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------
std::string build_hospital_info_json(int32_t zone_id, int32_t location_id, std::string* err) {
    std::ostringstream js;
    bool ok = run_hospital_locked([&]() -> bool {
        HospitalRef ref;
        if (!resolve_hospital(zone_id, location_id, ref, err))
            return false;
        auto* c = ref.contents;

        // Hospital furniture: count building types across every zone attached to this location.
        int beds = 0, tables = 0, traction = 0, containers = 0;
        std::vector<int32_t> zone_ids;
        for (auto zid : c->building_ids) {
            zone_ids.push_back(zid);
            auto z = virtual_cast<df::building_civzonest>(df::building::find(zid));
            if (!z)
                continue;
            for (auto b : z->contained_buildings) {
                if (!b)
                    continue;
                switch (b->getType()) {
                case df::building_type::Bed: ++beds; break;
                case df::building_type::Table: ++tables; break;
                case df::building_type::TractionBench: ++traction; break;
                case df::building_type::Box:
                case df::building_type::Cabinet: ++containers; break;
                default: break;
                }
            }
        }

        js << "{\"ok\":true,\"locationId\":" << ref.location_id
           << ",\"zoneId\":" << (ref.zone ? ref.zone->id : -1)
           << ",\"name\":" << json_string(hospital_name(ref.hosp))
           << ",\"value\":" << c->location_value
           << ",\"tier\":" << c->location_tier
           << ",\"zoneIds\":[";
        for (size_t i = 0; i < zone_ids.size(); ++i) {
            if (i) js << ",";
            js << zone_ids[i];
        }
        js << "],\"furniture\":{\"beds\":" << beds << ",\"tables\":" << tables
           << ",\"tractionBenches\":" << traction << ",\"containers\":" << containers << "}";

        // Supplies.
        js << ",\"supplies\":[";
        for (size_t i = 0; i < (sizeof(kSupplies) / sizeof(kSupplies[0])); ++i) {
            const auto& s = kSupplies[i];
            int32_t desired_raw = c->*(s.desired);
            int32_t count_raw = c->*(s.count);
            if (i) js << ",";
            js << "{\"key\":" << json_string(s.key)
               << ",\"label\":" << json_string(s.label)
               << ",\"scale\":" << s.scale
               << ",\"desiredRaw\":" << desired_raw
               << ",\"countRaw\":" << count_raw
               << ",\"desiredLevel\":" << (desired_raw / s.scale)
               << ",\"countLevel\":" << (count_raw / s.scale)
               << ",\"needMore\":" << (get_need_more(c, s.key) ? "true" : "false")
               << "}";
        }
        js << "]";

        // Chief medical dwarf (noble).
        js << ",\"chiefMedical\":" << chief_medical_json(find_chief_medical());

        // Doctors: citizens with >=1 medical labor.
        js << ",\"doctors\":[";
        auto world = df::global::world;
        bool first = true;
        if (world) {
            for (auto u : world->units.active) {
                if (!u || !DFHack::Units::isCitizen(u, true))
                    continue;
                std::vector<std::string> labs;
                for (const auto& ml : kMedLabors) {
                    if (u->status.labors[ml.labor])
                        labs.push_back(ml.key);
                }
                if (labs.empty())
                    continue;
                if (!first) js << ",";
                first = false;
                js << "{\"unitId\":" << u->id
                   << ",\"name\":" << json_string(DFHack::Units::getReadableName(u))
                   << ",\"profession\":" << json_string(DFHack::Units::getProfessionName(u))
                   << ",\"professionColor\":" << static_cast<int>(DFHack::Units::getProfessionColor(u))
                   << ",\"labors\":";
                append_json_string_array(js, labs);
                js << "}";
            }
        }
        js << "]}\n";
        return true;
    });
    if (!ok)
        return "";
    return js.str();
}

std::string build_hospital_patients_json(int32_t zone_id, int32_t location_id, std::string* err) {
    std::ostringstream js;
    bool ok = run_hospital_locked([&]() -> bool {
        HospitalRef ref;
        if (!resolve_hospital(zone_id, location_id, ref, err))
            return false;
        auto world = df::global::world;
        if (!world) { if (err) *err = "world unavailable"; return false; }

        // Patients: active fort units whose health carries a request OR who have active wounds.
        js << "{\"ok\":true,\"locationId\":" << ref.location_id << ",\"patients\":[";
        bool first = true;
        for (auto u : world->units.active) {
            if (!u || !DFHack::Units::isCitizen(u, true))
                continue;
            auto health = u->health;
            bool wounded = !u->body.wounds.empty();
            bool needs = health && health->flags.whole != 0;
            if (!needs && !wounded)
                continue;
            std::vector<std::string> flags;
            if (health) {
                const auto& b = health->flags.bits;
                if (b.rq_diagnosis)     flags.push_back("Needs diagnosis");
                if (b.rq_immobilize)    flags.push_back("Needs immobilization");
                if (b.rq_dressing)      flags.push_back("Needs dressing");
                if (b.rq_cleaning)      flags.push_back("Needs cleaning");
                if (b.rq_surgery)       flags.push_back("Needs surgery");
                if (b.rq_suture)        flags.push_back("Needs suturing");
                if (b.rq_setting)       flags.push_back("Needs a bone set");
                if (b.rq_traction)      flags.push_back("Needs traction");
                if (b.rq_crutch)        flags.push_back("Needs a crutch");
                if (b.needs_healthcare) flags.push_back("Under hospital care");
            }
            if (!first) js << ",";
            first = false;
            js << "{\"unitId\":" << u->id
               << ",\"name\":" << json_string(DFHack::Units::getReadableName(u))
               << ",\"profession\":" << json_string(DFHack::Units::getProfessionName(u))
               << ",\"professionColor\":" << static_cast<int>(DFHack::Units::getProfessionColor(u))
               << ",\"woundCount\":" << static_cast<int>(u->body.wounds.size())
               << ",\"inTraction\":" << (has_traction_job(u->id) ? "true" : "false")
               << ",\"flags\":";
            append_json_string_array(js, flags);
            js << "}";
        }
        js << "]";

        // Treatment queue: active medical jobs in the fort.
        js << ",\"queue\":[";
        first = true;
        for (df::job_list_link* link = world->jobs.list.next; link; link = link->next) {
            df::job* job = link->item;
            if (!job)
                continue;
            if (!is_medical_job(job->job_type))
                continue;
            const std::string label = native_job_name(job);
            df::unit* worker = DFHack::Job::getWorker(job);
            df::unit* patient = job_patient(job);
            if (!first) js << ",";
            first = false;
            js << "{\"jobType\":" << json_string(label)
               << ",\"worker\":" << json_string(worker ? DFHack::Units::getReadableName(worker) : "")
               << ",\"workerProfessionColor\":" << (worker ? static_cast<int>(DFHack::Units::getProfessionColor(worker)) : -1)
               << ",\"patient\":" << json_string(patient ? DFHack::Units::getReadableName(patient) : "")
               << ",\"patientProfessionColor\":" << (patient ? static_cast<int>(DFHack::Units::getProfessionColor(patient)) : -1)
               << "}";
        }
        js << "]}\n";
        return true;
    });
    if (!ok)
        return "";
    return js.str();
}

// Mutation: set one supply's desired maximum. Mirrors DF's Locations-screen +/- and quickfort:
// desired_<field> = level * scale, and need_more.<bit> = (level > 0). Level clamped 0..99.
bool do_hospital_supply(int32_t zone_id, int32_t location_id, const std::string& key, int level,
                        std::string* err) {
    return run_hospital_locked([&]() -> bool {
        HospitalRef ref;
        if (!resolve_hospital(zone_id, location_id, ref, err))
            return false;
        const SupplyDef* s = find_supply(key);
        if (!s) { if (err) *err = "unknown supply key"; return false; }
        if (level < 0) level = 0;
        if (level > 99) level = 99;
        ref.contents->*(s->desired) = level * s->scale;
        set_need_more(ref.contents, key, level > 0);
        return true;
    });
}

} // namespace

void register_hospital_routes(httplib::Server& server) {
    // GET /hospitals -> every hospital location in the current fort site (id + name), so the panel
    // can discover and pick one. Empty list (never an error) when the fort has no hospital yet.
    server.Get("/hospitals", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::ostringstream body;
        bool ok = run_hospital_locked([&]() -> bool {
            auto plotinfo = df::global::plotinfo;
            auto site = plotinfo ? find_site(plotinfo->site_id) : nullptr;
            body << "{\"player\":" << json_string(player) << ",\"hospitals\":[";
            bool first = true;
            if (site) {
                for (auto loc : site->buildings) {
                    auto hosp = virtual_cast<df::abstract_building_hospitalst>(loc);
                    if (!hosp) continue;
                    if (!first) body << ",";
                    first = false;
                    body << "{\"locationId\":" << hosp->id
                         << ",\"name\":" << json_string(hospital_name(hosp)) << "}";
                }
            }
            body << "]}\n";
            return true;
        });
        if (!ok) { json_error(res, 503, "hospital list unavailable"); return; }
        set_no_store_json(res, body.str());
    });

    // GET /hospital-info?zone=<id> (or ?location=<id>) -> supplies, furniture, doctors, chief medic.
    server.Get("/hospital-info", [](const httplib::Request& req, httplib::Response& res) {
        int zone = -1, location = -1;
        query_int(req, "zone", zone);
        query_int(req, "location", location);
        if (zone < 0 && location < 0) { json_error(res, 400, "missing zone or location"); return; }
        std::string err;
        std::string json = build_hospital_info_json(zone, location, &err);
        if (json.empty()) { json_error(res, 400, err.empty() ? "hospital unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // GET /hospital-patients?zone=<id> -> patient rows + active medical-job queue (auto-empty when
    // the fort has no wounded -- precondition-gated, never an error).
    server.Get("/hospital-patients", [](const httplib::Request& req, httplib::Response& res) {
        int zone = -1, location = -1;
        query_int(req, "zone", zone);
        query_int(req, "location", location);
        if (zone < 0 && location < 0) { json_error(res, 400, "missing zone or location"); return; }
        std::string err;
        std::string json = build_hospital_patients_json(zone, location, &err);
        if (json.empty()) { json_error(res, 400, err.empty() ? "patients unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /hospital-supply?zone=<id>&supply=<key>&level=<n> -> set a supply maximum.
    auto supply_handler = [](const httplib::Request& req, httplib::Response& res) {
        int zone = -1, location = -1, level = 0;
        query_int(req, "zone", zone);
        query_int(req, "location", location);
        if (zone < 0 && location < 0) { json_error(res, 400, "missing zone or location"); return; }
        std::string key = req.get_param_value("supply");
        if (key.empty()) { json_error(res, 400, "missing supply"); return; }
        if (!query_int(req, "level", level)) { json_error(res, 400, "missing level"); return; }
        std::string err;
        if (!do_hospital_supply(zone, location, key, level, &err)) {
            json_error(res, 400, err.empty() ? "supply update failed" : err); return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/hospital-supply", supply_handler);
    server.Post("/hospital-supply", supply_handler);
}

} // namespace dfcapture
