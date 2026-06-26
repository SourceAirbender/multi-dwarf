// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
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

#include "placement.h"

#include "diagnostics.h"
#include "sdl_capture.h"

#include "Core.h"
#include "TileTypes.h"
#include "modules/Designations.h"
#include "modules/DFSDL.h"
#include "modules/Job.h"
#include "modules/MapCache.h"

#include "df/global_objects.h"
#include "df/job.h"
#include "df/job_list_link.h"
#include "df/job_type.h"
#include "df/plant.h"
#include "df/plant_tree_info.h"
#include "df/tile_designation.h"
#include "df/tile_dig_designation.h"
#include "df/tile_occupancy.h"
#include "df/tiletype.h"
#include "df/tiletype_material.h"
#include "df/tiletype_shape.h"
#include "df/tiletype_shape_basic.h"
#include "df/tiletype_special.h"
#include "df/world.h"

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace dfcapture {
namespace {

using DFHack::DFCoord;

enum class DesignationKind {
    Dig,
    Smooth,
    Engrave,
    Track,
    Fortify,
    Chop,
    Gather,
    Clear,
};

std::recursive_mutex g_designation_mutex;

int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(high, value));
}

// Job types DF queues from dig/chop/gather/track/engrave designations. Once you press play,
// designations turn into these jobs and the tiles' designation bits clear -- so the eraser must
// cancel these jobs too, not just clear the bits. This is what DF's native erase does.
bool is_designation_job(df::job_type jt) {
    switch (jt) {
    case df::job_type::Dig:
    case df::job_type::CarveUpwardStaircase:
    case df::job_type::CarveDownwardStaircase:
    case df::job_type::CarveUpDownStaircase:
    case df::job_type::CarveRamp:
    case df::job_type::DigChannel:
    case df::job_type::CarveFortification:
    case df::job_type::CarveTrack:
    case df::job_type::DetailWall:
    case df::job_type::DetailFloor:
    case df::job_type::FellTree:
    case df::job_type::GatherPlants:
        return true;
    default:
        return false;
    }
}

bool dig_from_tool(const std::string& tool, df::tile_dig_designation& out) {
    if (tool.empty() || tool == "dig" || tool == "mine") {
        out = df::tile_dig_designation::Default;
        return true;
    }
    if (tool == "channel") {
        out = df::tile_dig_designation::Channel;
        return true;
    }
    if (tool == "ramp") {
        out = df::tile_dig_designation::Ramp;
        return true;
    }
    if (tool == "stairs" || tool == "updown") {
        out = df::tile_dig_designation::UpDownStair;
        return true;
    }
    if (tool == "up" || tool == "upstair") {
        out = df::tile_dig_designation::UpStair;
        return true;
    }
    if (tool == "down" || tool == "downstair") {
        out = df::tile_dig_designation::DownStair;
        return true;
    }
    if (tool == "clear" || tool == "erase" || tool == "none" || tool == "off") {
        out = df::tile_dig_designation::No;
        return true;
    }
    return false;
}

bool kind_from_tool(const std::string& tool, DesignationKind& kind,
                    df::tile_dig_designation& dig) {
    dig = df::tile_dig_designation::No;
    if (tool == "smooth") {
        kind = DesignationKind::Smooth;
        return true;
    }
    if (tool == "engrave") {
        kind = DesignationKind::Engrave;
        return true;
    }
    if (tool == "track" || tool == "carve_track") {
        kind = DesignationKind::Track;
        return true;
    }
    if (tool == "fortify" || tool == "fortification") {
        kind = DesignationKind::Fortify;
        return true;
    }
    if (tool == "chop") {
        kind = DesignationKind::Chop;
        return true;
    }
    if (tool == "gather") {
        kind = DesignationKind::Gather;
        return true;
    }
    if (tool == "clear" || tool == "erase" || tool == "remove" || tool == "none" || tool == "off") {
        kind = DesignationKind::Clear;
        return true;
    }
    if (dig_from_tool(tool, dig)) {
        kind = DesignationKind::Dig;
        return true;
    }
    return false;
}

bool is_visible_natural_stone(MapExtras::MapCache& map, const DFCoord& pos, df::tiletype& tt) {
    df::tile_designation des = map.designationAt(pos);
    if (des.bits.hidden)
        return false;
    tt = map.tiletypeAt(pos);
    df::tiletype_material mat = DFHack::tileMaterial(tt);
    return mat == df::tiletype_material::STONE ||
           mat == df::tiletype_material::MINERAL ||
           mat == df::tiletype_material::FEATURE ||
           mat == df::tiletype_material::LAVA_STONE;
}

bool natural_wall_or_floor(df::tiletype tt) {
    df::tiletype_shape_basic basic =
        ENUM_ATTR(tiletype_shape, basic_shape, DFHack::tileShape(tt));
    return basic == df::tiletype_shape_basic::Wall ||
           basic == df::tiletype_shape_basic::Floor;
}

bool can_smooth_tile(MapExtras::MapCache& map, const DFCoord& pos) {
    df::tiletype tt = df::tiletype::Void;
    if (!is_visible_natural_stone(map, pos, tt) || !natural_wall_or_floor(tt))
        return false;
    return DFHack::tileSpecial(tt) != df::tiletype_special::SMOOTH;
}

bool can_engrave_tile(MapExtras::MapCache& map, const DFCoord& pos) {
    df::tiletype tt = df::tiletype::Void;
    if (!is_visible_natural_stone(map, pos, tt) || !natural_wall_or_floor(tt))
        return false;
    return DFHack::tileSpecial(tt) == df::tiletype_special::SMOOTH;
}

bool can_fortify_tile(MapExtras::MapCache& map, const DFCoord& pos) {
    df::tiletype tt = df::tiletype::Void;
    if (!is_visible_natural_stone(map, pos, tt))
        return false;
    return DFHack::tileShape(tt) == df::tiletype_shape::WALL &&
           DFHack::tileSpecial(tt) == df::tiletype_special::SMOOTH;
}

bool can_track_tile(MapExtras::MapCache& map, const DFCoord& pos) {
    df::tiletype tt = df::tiletype::Void;
    if (!is_visible_natural_stone(map, pos, tt))
        return false;
    df::tiletype_shape shape = DFHack::tileShape(tt);
    return shape == df::tiletype_shape::FLOOR || shape == df::tiletype_shape::RAMP;
}

bool can_apply_dig_designation(MapExtras::MapCache& map, const DFCoord& pos,
                               df::tile_dig_designation dig) {
    auto world = df::global::world;
    if (!world)
        return false;

    if (pos.x <= 0 || pos.y <= 0 ||
            pos.x >= world->map.x_count * 16 - 1 ||
            pos.y >= world->map.y_count * 16 - 1)
        return false;

    auto block = map.BlockAt(pos / 16);
    if (!block || !block->is_valid())
        return false;

    df::tiletype tt = map.tiletypeAt(pos);
    df::tile_designation des = map.designationAt(pos);
    if (DFHack::tileMaterial(tt) == df::tiletype_material::CONSTRUCTION && !des.bits.hidden)
        return false;

    df::tiletype_shape shape = DFHack::tileShape(tt);
    if (shape == df::tiletype_shape::EMPTY && !des.bits.hidden)
        return false;

    if (!des.bits.hidden) {
        df::tiletype_shape_basic basic = ENUM_ATTR(tiletype_shape, basic_shape, shape);
        if (basic == df::tiletype_shape_basic::Wall)
            return true;
        if (basic == df::tiletype_shape_basic::Floor &&
                (dig == df::tile_dig_designation::DownStair ||
                 dig == df::tile_dig_designation::Channel) &&
                shape != df::tiletype_shape::BRANCH &&
                shape != df::tiletype_shape::TRUNK_BRANCH &&
                shape != df::tiletype_shape::TWIG)
            return true;
        if (basic == df::tiletype_shape_basic::Stair &&
                dig == df::tile_dig_designation::Channel)
            return true;
        return false;
    }

    return true;
}

int pixel_to_tile(int pixel, int dim, int frame) {
    if (frame <= 0)
        return 0;
    return std::max(0, std::min(dim - 1, (pixel * dim) / frame));
}

struct RenderDesignationRequest {
    Camera camera;
    DesignationRequest request;
    DesignationKind kind = DesignationKind::Dig;
    df::tile_dig_designation dig = df::tile_dig_designation::Default;
    DesignationResult result;
    std::string err;
    std::promise<bool> done;
    // World-tile box of the selection, recorded by apply_designation so the post-render
    // eraser job-cancel pass can run under CoreSuspender. box_x2 < box_x1 means "not set".
    int box_x1 = 0, box_y1 = 0, box_x2 = -1, box_y2 = -1, box_z = 0;
};

bool apply_tile_designations(RenderDesignationRequest& req, MapExtras::MapCache& map,
                             int tx1, int ty1, int tx2, int ty2, int wz) {
    int changed_count = 0;
    const int sel_w = tx2 - tx1 + 1;
    const int sel_h = ty2 - ty1 + 1;
    const bool track_ns = sel_h >= sel_w;
    const bool track_ew = sel_w >= sel_h;
    int priority = clamp_int(req.request.priority, 1, 7) * 1000;

    for (int ty = ty1; ty <= ty2; ++ty) {
        for (int tx = tx1; tx <= tx2; ++tx) {
            DFCoord pos(req.camera.x + tx, req.camera.y + ty, wz);
            df::tile_designation des = map.designationAt(pos);
            bool changed = false;
            bool touch_occ = false;
            int des_priority = 0;

            if (req.kind == DesignationKind::Dig) {
                if (req.dig != df::tile_dig_designation::No &&
                        !can_apply_dig_designation(map, pos, req.dig))
                    continue;
                if (req.request.mine_mode != 0 &&
                        req.dig != df::tile_dig_designation::No &&
                        DFHack::tileMaterial(map.tiletypeAt(pos)) != df::tiletype_material::MINERAL)
                    continue;
                if (des.bits.dig != req.dig) {
                    des.bits.dig = req.dig;
                    changed = true;
                }
                des_priority = priority;
                touch_occ = true;
            } else if (req.kind == DesignationKind::Smooth ||
                       req.kind == DesignationKind::Engrave ||
                       req.kind == DesignationKind::Fortify) {
                if (req.kind == DesignationKind::Smooth && !can_smooth_tile(map, pos))
                    continue;
                if (req.kind == DesignationKind::Engrave && !can_engrave_tile(map, pos))
                    continue;
                if (req.kind == DesignationKind::Fortify && !can_fortify_tile(map, pos))
                    continue;
                uint32_t want = req.kind == DesignationKind::Engrave ? 2u : 1u;
                if (des.bits.smooth != want) {
                    des.bits.smooth = want;
                    changed = true;
                }
                des_priority = priority;
            } else if (req.kind == DesignationKind::Track) {
                if (!can_track_tile(map, pos))
                    continue;
                df::tile_occupancy occ = map.occupancyAt(pos);
                if (occ.bits.carve_track_north != track_ns ||
                        occ.bits.carve_track_south != track_ns ||
                        occ.bits.carve_track_east != track_ew ||
                        occ.bits.carve_track_west != track_ew) {
                    occ.bits.carve_track_north = track_ns;
                    occ.bits.carve_track_south = track_ns;
                    occ.bits.carve_track_east = track_ew;
                    occ.bits.carve_track_west = track_ew;
                    map.setOccupancyAt(pos, occ);
                    changed = true;
                }
                des_priority = priority;
            } else if (req.kind == DesignationKind::Clear) {
                if (des.bits.dig != df::tile_dig_designation::No) {
                    des.bits.dig = df::tile_dig_designation::No;
                    changed = true;
                }
                if (des.bits.smooth != 0) {
                    des.bits.smooth = 0;
                    changed = true;
                }
                touch_occ = true;
            }

            if (touch_occ) {
                df::tile_occupancy occ = map.occupancyAt(pos);
                bool want_marked = req.kind == DesignationKind::Clear ? false : req.request.marker;
                bool want_auto = req.kind == DesignationKind::Clear ? false : req.request.warm_damp;
                if (occ.bits.dig_marked != want_marked || occ.bits.dig_auto != want_auto) {
                    occ.bits.dig_marked = want_marked;
                    occ.bits.dig_auto = want_auto;
                    map.setOccupancyAt(pos, occ);
                    changed = true;
                }
                if (req.kind == DesignationKind::Clear &&
                        (occ.bits.carve_track_north || occ.bits.carve_track_south ||
                         occ.bits.carve_track_east || occ.bits.carve_track_west)) {
                    occ.bits.carve_track_north = 0;
                    occ.bits.carve_track_south = 0;
                    occ.bits.carve_track_east = 0;
                    occ.bits.carve_track_west = 0;
                    map.setOccupancyAt(pos, occ);
                    changed = true;
                }
            }

            if (changed && map.setDesignationAt(pos, des, des_priority))
                ++changed_count;
        }
    }

    req.result.count += changed_count;
    return changed_count > 0;
}

bool apply_plant_designations(RenderDesignationRequest& req, MapExtras::MapCache& map,
                              int wx1, int wy1, int wx2, int wy2, int wz) {
    auto world = df::global::world;
    if (!world)
        return false;

    int changed_count = 0;
    int priority = clamp_int(req.request.priority, 1, 7) * 1000;
    for (df::plant* plant : world->plants.all) {
        if (!plant)
            continue;
        bool is_tree = plant->tree_info != nullptr;
        if (req.kind == DesignationKind::Chop && !is_tree)
            continue;
        if (req.kind == DesignationKind::Gather && is_tree)
            continue;

        df::coord pos = DFHack::Designations::getPlantDesignationTile(plant);
        if (pos.z != wz || pos.x < wx1 || pos.x > wx2 || pos.y < wy1 || pos.y > wy2)
            continue;

        bool ok = req.kind == DesignationKind::Clear
            ? DFHack::Designations::unmarkPlant(plant)
            : DFHack::Designations::markPlant(plant);
        if (!ok)
            continue;

        if (req.kind != DesignationKind::Clear) {
            df::tile_designation des = map.designationAt(pos);
            df::tile_occupancy occ = map.occupancyAt(pos);
            if (occ.bits.dig_marked != req.request.marker) {
                occ.bits.dig_marked = req.request.marker;
                map.setOccupancyAt(pos, occ);
            }
            map.setDesignationAt(pos, des, priority);
        }
        ++changed_count;
    }

    req.result.count += changed_count;
    return changed_count > 0;
}

bool apply_designation(RenderDesignationRequest& req) {
    int view_w = 0;
    int view_h = 0;
    if (!effective_capture_viewport_dims(req.camera, view_w, view_h, &req.err) ||
            req.request.frame_w <= 0 || req.request.frame_h <= 0) {
        if (req.err.empty())
            req.err = "viewport/frame unavailable";
        return false;
    }

    int tx1 = pixel_to_tile(std::min(req.request.px, req.request.px2), view_w, req.request.frame_w);
    int ty1 = pixel_to_tile(std::min(req.request.py, req.request.py2), view_h, req.request.frame_h);
    int tx2 = pixel_to_tile(std::max(req.request.px, req.request.px2), view_w, req.request.frame_w);
    int ty2 = pixel_to_tile(std::max(req.request.py, req.request.py2), view_h, req.request.frame_h);

    int wx1 = req.camera.x + tx1;
    int wy1 = req.camera.y + ty1;
    int wx2 = req.camera.x + tx2;
    int wy2 = req.camera.y + ty2;
    int wz = req.camera.z;

    // Record the world box so the eraser's post-render job-cancel pass knows what to clear,
    // even when the tile pass below changes nothing (the "designation became a job" case).
    req.box_x1 = wx1; req.box_y1 = wy1;
    req.box_x2 = wx2; req.box_y2 = wy2;
    req.box_z = wz;

    MapExtras::MapCache map;
    bool changed = false;

    if (req.kind != DesignationKind::Chop && req.kind != DesignationKind::Gather)
        changed = apply_tile_designations(req, map, tx1, ty1, tx2, ty2, wz) || changed;

    if (req.kind == DesignationKind::Chop ||
            req.kind == DesignationKind::Gather ||
            req.kind == DesignationKind::Clear)
        changed = apply_plant_designations(req, map, wx1, wy1, wx2, wy2, wz) || changed;

    if (changed) {
        map.WriteAll();
        return true;
    }

    req.err = "no valid tiles for that designation";
    std::ostringstream diag;
    diag << "designation had no effect: tool=" << req.request.tool
         << " box=" << wx1 << "," << wy1 << ".." << wx2 << "," << wy2
         << "," << wz << " viewport=" << view_w << "x" << view_h
         << " frame=" << req.request.frame_w << "x" << req.request.frame_h;
    diagnostics_log(diag.str());
    return false;
}

} // namespace

bool designate_on_render_thread(const Camera& camera, const DesignationRequest& request,
                                DesignationResult& result, std::string* err) {
    DesignationKind kind = DesignationKind::Dig;
    df::tile_dig_designation dig = df::tile_dig_designation::Default;
    if (!kind_from_tool(request.tool, kind, dig)) {
        if (err) *err = "unsupported designation tool: " + request.tool;
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_designation_mutex);
    auto req = std::make_shared<RenderDesignationRequest>();
    req->camera = camera;
    req->request = request;
    req->kind = kind;
    req->dig = dig;
    req->result.tool = request.tool;
    auto future = req->done.get_future();

    DFHack::runOnRenderThread([req]() {
        req->done.set_value(apply_designation(*req));
    });

    bool ok = future.get();

    // Eraser: cancel the dig/chop/gather JOBS in the box -- off the render thread, under
    // CoreSuspender (main parked) so unlinking from the job list can't race DF's job manager
    // during play. Runs even when the tile pass found nothing (n==0): that is exactly the case
    // where pressing play turned the designations into jobs and cleared the tiles' dig bits, so
    // only clearing designation bits did nothing.
    if (kind == DesignationKind::Clear &&
            req->box_x2 >= req->box_x1 && req->box_y2 >= req->box_y1) {
        int canceled = 0;
        {
            std::lock_guard<std::recursive_mutex> cap_lock(capture_state_mutex());
            DFHack::CoreSuspender suspend;
            auto world = df::global::world;
            if (world) {
                std::vector<df::job*> to_cancel;
                for (auto* link = world->jobs.list.next; link; link = link->next) {
                    df::job* job = link->item;
                    if (!job)
                        continue;
                    if (job->pos.z == req->box_z &&
                            job->pos.x >= req->box_x1 && job->pos.x <= req->box_x2 &&
                            job->pos.y >= req->box_y1 && job->pos.y <= req->box_y2 &&
                            is_designation_job(job->job_type)) {
                        to_cancel.push_back(job);
                    }
                }
                for (df::job* job : to_cancel)
                    if (DFHack::Job::removeJob(job))
                        ++canceled;
            }
        }
        if (canceled > 0) {
            ok = true;
            req->result.count += canceled;
        }
    }

    result = req->result;
    if (!ok && err)
        *err = req->err;
    return ok;
}

} // namespace dfcapture
