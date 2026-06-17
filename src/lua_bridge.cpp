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

#include "lua_bridge.h"

#include "Core.h"
#include "LuaTools.h"
#include "diagnostics.h"
#include "sdl_capture.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <tuple>
#include <utility>

using namespace DFHack;

namespace dfcapture {
namespace {

constexpr const char* LUA_MODULE = "plugins.dfcapture";

std::recursive_mutex g_lua_bridge_mutex;

template <typename Fn>
bool run_lua_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> module_lock(g_lua_bridge_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    return fn();
}

int pixel_to_tile(int pixel, int dim, int frame) {
    if (frame <= 0 || dim <= 0)
        return 0;
    return std::max(0, std::min(dim - 1, (pixel * dim) / frame));
}

bool pixel_rect_to_world_tiles(const Camera& camera, int px, int py, int px2, int py2,
                               int frame_w, int frame_h, int& x1, int& y1,
                               int& x2, int& y2, std::string* err) {
    int view_w = 0;
    int view_h = 0;
    if (!effective_capture_viewport_dims(camera, view_w, view_h, err) ||
            frame_w <= 0 || frame_h <= 0) {
        if (err && err->empty())
            *err = "viewport/frame unavailable";
        return false;
    }

    int tx1 = pixel_to_tile(std::min(px, px2), view_w, frame_w);
    int ty1 = pixel_to_tile(std::min(py, py2), view_h, frame_h);
    int tx2 = pixel_to_tile(std::max(px, px2), view_w, frame_w);
    int ty2 = pixel_to_tile(std::max(py, py2), view_h, frame_h);

    x1 = camera.x + tx1;
    y1 = camera.y + ty1;
    x2 = camera.x + tx2;
    y2 = camera.y + ty2;
    return true;
}

std::string lua_output_text(DFHack::buffered_color_ostream& out) {
    std::string text;
    for (const auto& frag : out.fragments())
        text += frag.second;
    return text;
}

template <typename Args, typename ResultFn>
bool call_lua(const char* function_name, Args&& args, int returns,
              ResultFn&& result_fn, std::string* err) {
    DFHack::buffered_color_ostream lua_out;
    bool called = Lua::CallLuaModuleFunction(lua_out, LUA_MODULE, function_name,
                                             std::forward<Args>(args), returns,
                                             std::forward<ResultFn>(result_fn));
    if (!called) {
        if (err) {
            std::string details = lua_output_text(lua_out);
            *err = details.empty()
                ? std::string("lua bridge call failed: ") + function_name
                : details;
        }
        return false;
    }
    return true;
}

std::string json_returning_lua(const char* function_name, std::string* err) {
    std::string json;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua(function_name, std::make_tuple(), 1,
            [&](lua_State* L) {
                if (lua_isstring(L, -1))
                    json = lua_tostring(L, -1);
            }, err);
    });
    return ok ? json : "";
}

std::string json_returning_lua_int(const char* function_name, int32_t id, std::string* err) {
    std::string json;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua(function_name, std::make_tuple(id), 1,
            [&](lua_State* L) {
                if (lua_isstring(L, -1))
                    json = lua_tostring(L, -1);
            }, err);
    });
    return ok ? json : "";
}

bool bool_error_lua_int_string(const char* function_name, int32_t id,
                               const std::string& value, std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua(function_name, std::make_tuple(id, value), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? std::string(function_name) + " failed" : result_err;
    return result_ok;
}

} // namespace

bool lua_ping(int value, int& out_value, std::string* err) {
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("ping", std::make_tuple(value), 1,
            [&](lua_State* L) {
                if (lua_isnumber(L, -1))
                    out_value = static_cast<int>(lua_tointeger(L, -1));
            }, err);
    });
    return ok;
}

std::string building_catalog_json_via_lua(std::string* err) {
    return json_returning_lua("building_catalog", err);
}

std::string build_materials_json_via_lua(const std::string& token, std::string* err) {
    std::string json;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("build_materials", std::make_tuple(token), 1,
            [&](lua_State* L) {
                if (lua_isstring(L, -1))
                    json = lua_tostring(L, -1);
            }, err);
    });
    return ok ? json : "";
}

bool place_building_via_lua(const Camera& camera, int px, int py, int px2, int py2,
                            int frame_w, int frame_h, const std::string& token,
                            int direction, const std::string& options,
                            int& out_count, int& out_id, std::string* err) {
    return run_lua_locked([&]() -> bool {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!pixel_rect_to_world_tiles(camera, px, py, px2, py2, frame_w, frame_h,
                                       x1, y1, x2, y2, err))
            return false;

        int result_count = 0;
        int result_id = -1;
        std::string result_err;
        diagnostics_log("build-place lua token=" + token + " rect=" +
                        std::to_string(x1) + "," + std::to_string(y1) + ".." +
                        std::to_string(x2) + "," + std::to_string(y2) +
                        " z=" + std::to_string(camera.z));
        bool ok = call_lua("place_building",
            std::make_tuple(x1, y1, x2, y2, camera.z, token, direction, options), 3,
            [&](lua_State* L) {
                if (lua_isnumber(L, -3))
                    result_count = static_cast<int>(lua_tointeger(L, -3));
                if (lua_isnumber(L, -2))
                    result_id = static_cast<int>(lua_tointeger(L, -2));
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
        if (!ok)
            return false;
        out_count = result_count;
        out_id = result_id;
        if (result_count <= 0 || result_id < 0) {
            if (err) *err = result_err.empty() ? "building placement failed" : result_err;
            return false;
        }
        return true;
    });
}

bool create_stockpile_via_lua(const Camera& camera, int px, int py, int px2, int py2,
                              int frame_w, int frame_h, const std::string& preset,
                              int& out_id, std::string* err) {
    return run_lua_locked([&]() -> bool {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!pixel_rect_to_world_tiles(camera, px, py, px2, py2, frame_w, frame_h,
                                       x1, y1, x2, y2, err))
            return false;

        int result_id = -1;
        std::string result_err;
        bool ok = call_lua("create_stockpile",
            std::make_tuple(x1, y1, x2, y2, camera.z, preset), 2,
            [&](lua_State* L) {
                if (lua_isnumber(L, -2))
                    result_id = static_cast<int>(lua_tointeger(L, -2));
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
        if (!ok)
            return false;
        out_id = result_id;
        if (result_id < 0) {
            if (err) *err = result_err.empty() ? "stockpile creation failed" : result_err;
            return false;
        }
        return true;
    });
}

bool create_zone_via_lua(const Camera& camera, int px, int py, int px2, int py2,
                         int frame_w, int frame_h, const std::string& zone_type,
                         int& out_id, std::string* err) {
    return run_lua_locked([&]() -> bool {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!pixel_rect_to_world_tiles(camera, px, py, px2, py2, frame_w, frame_h,
                                       x1, y1, x2, y2, err))
            return false;

        int result_id = -1;
        std::string result_err;
        bool ok = call_lua("create_zone",
            std::make_tuple(x1, y1, x2, y2, camera.z, zone_type), 2,
            [&](lua_State* L) {
                if (lua_isnumber(L, -2))
                    result_id = static_cast<int>(lua_tointeger(L, -2));
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
        if (!ok)
            return false;
        out_id = result_id;
        if (result_id < 0) {
            if (err) *err = result_err.empty() ? "zone creation failed" : result_err;
            return false;
        }
        return true;
    });
}

std::string stockpile_groups_via_lua(const std::string& cat, std::string* err) {
    std::string json;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("stockpile_cat_groups", std::make_tuple(cat), 1,
            [&](lua_State* L) {
                if (lua_isstring(L, -1))
                    json = lua_tostring(L, -1);
            }, err);
    });
    return ok ? json : "";
}

std::string stockpile_items_via_lua(int32_t id, const std::string& cat,
                                    const std::string& group, std::string* err) {
    std::string json;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("stockpile_item_list", std::make_tuple(id, cat, group), 1,
            [&](lua_State* L) {
                if (lua_isstring(L, -1))
                    json = lua_tostring(L, -1);
            }, err);
    });
    return ok ? json : "";
}

bool stockpile_toggle_item_via_lua(int32_t id, const std::string& cat,
                                   const std::string& group, int idx, bool on,
                                   std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("stockpile_toggle_item", std::make_tuple(id, cat, group, idx, on), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? "toggle failed" : result_err;
    return result_ok;
}

bool stockpile_toggle_all_via_lua(int32_t id, const std::string& cat,
                                  const std::string& group, bool on,
                                  std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("stockpile_toggle_all", std::make_tuple(id, cat, group, on), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? "toggle-all failed" : result_err;
    return result_ok;
}

std::string workshop_info_json_via_lua(int32_t id, std::string* err) {
    return json_returning_lua_int("workshop_info", id, err);
}

bool workshop_add_job_via_lua(int32_t id, const std::string& task, std::string* err) {
    return bool_error_lua_int_string("workshop_add_job", id, task, err);
}

bool workshop_job_action_via_lua(int32_t id, int32_t job_id, const std::string& action,
                                 std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("workshop_job_action", std::make_tuple(id, job_id, action), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? "workshop job action failed" : result_err;
    return result_ok;
}

bool workshop_worker_action_via_lua(int32_t id, int32_t unit_id, bool assign,
                                    std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("workshop_worker_action", std::make_tuple(id, unit_id, assign), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? "worker action failed" : result_err;
    return result_ok;
}

bool workshop_workers_clear_via_lua(int32_t id, std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("workshop_workers_clear", std::make_tuple(id), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? "clear workers failed" : result_err;
    return result_ok;
}

std::string zone_locations_json_via_lua(int32_t zone_id, std::string* err) {
    return json_returning_lua_int("zone_locations_json", zone_id, err);
}

bool zone_location_action_via_lua(int32_t zone_id, const std::string& action,
                                  const std::string& kind, int32_t location_id,
                                  std::string* err) {
    bool result_ok = false;
    std::string result_err;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("zone_location_action",
            std::make_tuple(zone_id, action, kind, location_id), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_err = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_err.empty() ? "zone location action failed" : result_err;
    return result_ok;
}

std::string order_json_via_lua(const char* function_name, std::string* err) {
    diagnostics_log(std::string("orders endpoint begin: ") + function_name);
    std::string json = json_returning_lua(function_name, err);
    if (json.empty()) {
        diagnostics_log(std::string("orders endpoint failed: ") + function_name +
                        ": " + (err ? *err : ""));
    } else {
        diagnostics_log(std::string("orders endpoint end: ") + function_name +
                        " bytes=" + std::to_string(json.size()));
    }
    return json;
}

std::string order_json_via_lua_str(const char* function_name, const std::string& arg,
                                   std::string* err) {
    std::string json;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua(function_name, std::make_tuple(arg), 1,
            [&](lua_State* L) {
                if (lua_isstring(L, -1))
                    json = lua_tostring(L, -1);
            }, err);
    });
    return ok ? json : "";
}

bool create_order_via_lua(const std::string& key, int32_t amount,
                          const std::string& frequency, int32_t workshop_id,
                          std::string* msg, std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("create_order", std::make_tuple(key, amount, frequency, workshop_id), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok) {
        if (err) *err = result_msg.empty() ? "create order failed" : result_msg;
        return false;
    }
    if (msg) *msg = result_msg;
    return true;
}

bool import_order_preset_via_lua(const std::string& name, std::string* msg,
                                 std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("import_order_preset", std::make_tuple(name), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok) {
        if (err) *err = result_msg.empty() ? "import failed" : result_msg;
        return false;
    }
    if (msg) *msg = result_msg;
    return true;
}

bool cancel_order_via_lua(int32_t id, std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("cancel_order", std::make_tuple(id), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "cancel failed" : result_msg;
    return result_ok;
}

bool adjust_order_via_lua(int32_t id, int32_t amount, const std::string& frequency,
                          std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("adjust_order", std::make_tuple(id, amount, frequency), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "adjust failed" : result_msg;
    return result_ok;
}

bool add_item_condition_via_lua(int32_t id, const std::string& compare, int32_t value,
                                const std::string& item, const std::string& material,
                                const std::string& adjective, std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("add_item_condition",
            std::make_tuple(id, compare, value, item, material, adjective), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "add condition failed" : result_msg;
    return result_ok;
}

bool add_order_condition_via_lua(int32_t id, int32_t other_id, const std::string& type,
                                 std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("add_order_condition", std::make_tuple(id, other_id, type), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "add dependency failed" : result_msg;
    return result_ok;
}

bool remove_condition_via_lua(int32_t id, const std::string& kind, int32_t index,
                              std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("remove_condition", std::make_tuple(id, kind, index), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "remove condition failed" : result_msg;
    return result_ok;
}

bool set_order_max_workshops_via_lua(int32_t id, int32_t max_workshops,
                                     std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("set_order_max_workshops", std::make_tuple(id, max_workshops), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "update failed" : result_msg;
    return result_ok;
}

bool set_order_workshop_via_lua(int32_t id, int32_t workshop_id, std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("set_order_workshop", std::make_tuple(id, workshop_id), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "update failed" : result_msg;
    return result_ok;
}

bool reorder_order_via_lua(int32_t id, int32_t direction, std::string* err) {
    bool result_ok = false;
    std::string result_msg;
    bool ok = run_lua_locked([&]() -> bool {
        return call_lua("reorder_order", std::make_tuple(id, direction), 2,
            [&](lua_State* L) {
                result_ok = lua_toboolean(L, -2) != 0;
                if (lua_isstring(L, -1))
                    result_msg = lua_tostring(L, -1);
            }, err);
    });
    if (!ok)
        return false;
    if (!result_ok && err)
        *err = result_msg.empty() ? "reorder failed" : result_msg;
    return result_ok;
}

} // namespace dfcapture
