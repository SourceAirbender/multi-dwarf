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

#include "sdl_capture.h"

#include "diagnostics.h"
#include "image_encoder.h"
#include "Core.h"
#include "DataDefs.h"
#include "PluginManager.h"
#include "VersionInfo.h"
#include "modules/DFSDL.h"
#include "modules/Gui.h"

#include "df/buildreq.h"
#include "df/enabler.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewport_flag.h"
#include "df/graphic_viewportst.h"
#include "df/main_interface.h"
#include "df/renderer.h"
#include "df/viewscreen.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/world.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace dfcapture {
namespace {

constexpr uint32_t SDL_PIXELFORMAT_ARGB8888 = 0x16362004u;
constexpr int SDL_TEXTUREACCESS_TARGET = 2;

using pfn_CreateTexture = void* (*)(void*, uint32_t, int, int, int);
using pfn_SetRenderTarget = int (*)(void*, void*);
using pfn_RenderReadPixels = int (*)(void*, const void*, uint32_t, void*, int);
using pfn_DestroyTexture = void (*)(void*);
using pfn_GetRendererOutputSize = int (*)(void*, int*, int*);
using pfn_SetRenderDrawColor = int (*)(void*, uint8_t, uint8_t, uint8_t, uint8_t);
using pfn_RenderClear = int (*)(void*);
#ifdef _WIN32
using pfn_RenderMapLegacy = void (__stdcall *)(int);
using pfn_NativeClear = void (*)(int64_t, int32_t, int32_t);
using pfn_NativeSetup = void (*)();
using pfn_NativeFill = void (*)(int64_t, int32_t);
using pfn_NativeBlit = void (*)(uintptr_t);
#endif

pfn_CreateTexture p_CreateTexture = nullptr;
pfn_SetRenderTarget p_SetRenderTarget = nullptr;
pfn_RenderReadPixels p_RenderReadPixels = nullptr;
pfn_DestroyTexture p_DestroyTexture = nullptr;
pfn_GetRendererOutputSize p_GetRendererOutputSize = nullptr;
pfn_SetRenderDrawColor p_SetRenderDrawColor = nullptr;
pfn_RenderClear p_RenderClear = nullptr;

#ifdef _WIN32
enum class RenderMapMode {
    None,
    LegacySymbol,
    NativeSequence,
};

pfn_RenderMapLegacy p_RenderMapLegacy = nullptr;
pfn_NativeClear p_NativeClear = nullptr;
pfn_NativeSetup p_NativeSetup = nullptr;
pfn_NativeFill p_NativeFill = nullptr;
pfn_NativeBlit p_NativeBlit = nullptr;
uintptr_t g_map_renderer_addr = 0;
uintptr_t g_exe_base = 0;
RenderMapMode g_render_map_mode = RenderMapMode::None;
#endif

std::recursive_mutex g_capture_mutex;
std::atomic<bool> g_warned_restore(false);
std::atomic<bool> g_warned_capture_target_bind(false);
std::atomic<bool> g_warned_host_buffer_restore(false);
std::atomic<bool> g_warned_host_viewscreen_restore(false);
std::atomic<bool> g_warned_host_interaction_skip(false);
std::atomic<bool> g_warned_recovered_render_map(false);
std::atomic<bool> g_warned_native_target_guard(false);
std::atomic<bool> g_warned_viewscreen_target_guard(false);
std::atomic<bool> g_warned_viewscreen_render_fallback(false);
std::atomic<bool> g_warned_ui_overlay_skip(false);
std::atomic<bool> g_zoom_unsafe(false);
std::atomic<bool> g_grid_unsafe(false);
std::atomic<int> g_ref_zoom_factor(0);
std::atomic<int> g_ref_dim_x(0);
std::atomic<int> g_ref_dim_y(0);

constexpr const char* HOST_INTERACTION_SKIP =
    "host interaction active; independent frame skipped";

#ifdef _WIN32
constexpr int32_t GRID_TEXPOS = 128903;
constexpr int32_t CURSOR_TEXPOS = 128909;
constexpr int32_t RECT_TL = 128777;
constexpr int32_t RECT_T = 128778;
constexpr int32_t RECT_TR = 128779;
constexpr int32_t RECT_L = 128783;
constexpr int32_t RECT_C = 128784;
constexpr int32_t RECT_R = 128785;
constexpr int32_t RECT_BL = 128789;
constexpr int32_t RECT_B = 128790;
constexpr int32_t RECT_BR = 128791;
#endif

#ifdef _WIN32
volatile uint32_t g_seh_code = 0;
void* g_seh_at = nullptr;
void* g_seh_access = nullptr;

const DWORD DFCAPTURE_INVALID_PARAMETER_EXCEPTION = 0xE0424643u;

static int dfcapture_seh_filter(_EXCEPTION_POINTERS* ep) {
    g_seh_code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    g_seh_at = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    g_seh_access = (ep && ep->ExceptionRecord &&
                    ep->ExceptionRecord->NumberParameters >= 2)
        ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1])
        : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void __cdecl dfcapture_invalid_parameter_handler(
        const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    RaiseException(DFCAPTURE_INVALID_PARAMETER_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

static int call_viewscreen_render_seh(df::viewscreen* viewscreen) {
    int fault = 0;
    _invalid_parameter_handler old_handler =
        _set_thread_local_invalid_parameter_handler(dfcapture_invalid_parameter_handler);
    __try {
        viewscreen->render(0);
    } __except(dfcapture_seh_filter(GetExceptionInformation())) {
        fault = 1;
    }
    _set_thread_local_invalid_parameter_handler(old_handler);
    return fault;
}

static bool call_set_viewport_zoom_factor_seh(df::renderer* renderer, int32_t factor) {
    bool ok = false;
    __try {
        renderer->set_viewport_zoom_factor(factor);
        ok = true;
    } __except(dfcapture_seh_filter(GetExceptionInformation())) {
        ok = false;
    }
    return ok;
}

static bool call_update_full_viewport_seh(df::renderer* renderer, df::graphic_viewportst* vp) {
    bool ok = false;
    __try {
        renderer->update_full_viewport(vp);
        ok = true;
    } __except(dfcapture_seh_filter(GetExceptionInformation())) {
        ok = false;
    }
    return ok;
}

static uint8_t g_dummy_follow[0x4000] = {0};
constexpr uintptr_t VIEW_FOLLOW_RVA = 0x23d9db0; // DF 53.15 (was 0x23d2df0 on Steam 53.14)

static int call_native_sequence_seh(uintptr_t map_renderer) {
    void** p_follow = reinterpret_cast<void**>(g_exe_base + VIEW_FOLLOW_RVA);
    void* saved_follow = *p_follow;
    std::memset(g_dummy_follow, 0, sizeof(g_dummy_follow));
    *reinterpret_cast<void**>(g_dummy_follow + 0x5f8) = g_dummy_follow + 0x700;
    *p_follow = g_dummy_follow;

    int stage = 0;
    int result = 0;
    __try {
        stage = 1;
        p_NativeClear(0, 0xFF, 0);
        stage = 3;
        p_NativeFill(0, static_cast<int32_t>(GetTickCount()));
        stage = 4;
        p_NativeBlit(map_renderer);
        result = 0;
    } __except(dfcapture_seh_filter(GetExceptionInformation())) {
        result = stage;
    }

    *p_follow = saved_follow;
    return result;
}
#endif

bool resolve_sdl(std::string* err) {
#ifdef _WIN32
    HMODULE sdl = GetModuleHandleA("SDL2.dll");
    if (!sdl) {
        if (err) *err = "SDL2.dll is not loaded";
        return false;
    }

    p_CreateTexture = reinterpret_cast<pfn_CreateTexture>(GetProcAddress(sdl, "SDL_CreateTexture"));
    p_SetRenderTarget = reinterpret_cast<pfn_SetRenderTarget>(GetProcAddress(sdl, "SDL_SetRenderTarget"));
    p_RenderReadPixels = reinterpret_cast<pfn_RenderReadPixels>(GetProcAddress(sdl, "SDL_RenderReadPixels"));
    p_DestroyTexture = reinterpret_cast<pfn_DestroyTexture>(GetProcAddress(sdl, "SDL_DestroyTexture"));
    p_GetRendererOutputSize = reinterpret_cast<pfn_GetRendererOutputSize>(GetProcAddress(sdl, "SDL_GetRendererOutputSize"));
    p_SetRenderDrawColor = reinterpret_cast<pfn_SetRenderDrawColor>(GetProcAddress(sdl, "SDL_SetRenderDrawColor"));
    p_RenderClear = reinterpret_cast<pfn_RenderClear>(GetProcAddress(sdl, "SDL_RenderClear"));

    if (p_CreateTexture && p_SetRenderTarget && p_RenderReadPixels &&
        p_DestroyTexture && p_GetRendererOutputSize &&
        p_SetRenderDrawColor && p_RenderClear) {
        return true;
    }

    if (err) *err = "could not resolve required SDL2 render-target functions";
    return false;
#else
    if (err) *err = "SDL capture is currently Windows-only";
    return false;
#endif
}

#ifdef _WIN32
class TemporaryRenderTarget {
public:
    bool begin(std::string* err = nullptr, int requested_w = 0, int requested_h = 0) {
        if (!resolve_sdl(err))
            return false;

        auto enabler = df::global::enabler;
        df::renderer* renderer = enabler ? enabler->renderer : nullptr;
        if (!renderer) {
            if (err) *err = "native map render target: no renderer";
            return false;
        }

        sdl_ = renderer->get_renderer();
        if (!sdl_) {
            if (err) *err = "native map render target: get_renderer() returned null";
            return false;
        }

        int w = 0;
        int h = 0;
        p_GetRendererOutputSize(sdl_, &w, &h);
        if (w <= 0 || h <= 0) {
            if (err) *err = "native map render target: bad renderer output size";
            return false;
        }
        if (requested_w > 0) w = requested_w;
        if (requested_h > 0) h = requested_h;

        target_ = p_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_TARGET, w, h);
        if (!target_) {
            if (err) *err = "native map render target: SDL_CreateTexture failed";
            return false;
        }

        if (p_SetRenderTarget(sdl_, target_) != 0) {
            p_DestroyTexture(target_);
            target_ = nullptr;
            if (err) *err = "native map render target: SDL_SetRenderTarget failed";
            return false;
        }

        w_ = w;
        h_ = h;
        active_ = true;
        return true;
    }

    bool clear(std::string* err = nullptr) {
        if (!active_ || !sdl_) {
            if (err) *err = "native map render target: target is not active";
            return false;
        }
        if (p_SetRenderDrawColor(sdl_, 0, 0, 0, 0) != 0 || p_RenderClear(sdl_) != 0) {
            if (err) *err = "native map render target: SDL_RenderClear failed";
            return false;
        }
        return true;
    }

    bool read_frame(CapturedFrame& frame, std::string* err = nullptr) {
        if (!active_ || !sdl_ || w_ <= 0 || h_ <= 0) {
            if (err) *err = "native map render target: target is not active";
            return false;
        }
        CapturedFrame next;
        next.width = w_;
        next.height = h_;
        next.bgra.resize(static_cast<size_t>(w_) * h_ * 4);
        int rc = p_RenderReadPixels(sdl_, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                    next.bgra.data(), w_ * 4);
        if (rc != 0) {
            if (err) *err = "native map render target: SDL_RenderReadPixels failed";
            return false;
        }
        frame = std::move(next);
        return true;
    }

    void reset() {
        if (active_ && sdl_)
            p_SetRenderTarget(sdl_, nullptr);
        active_ = false;
        if (target_) {
            p_DestroyTexture(target_);
            target_ = nullptr;
        }
        sdl_ = nullptr;
        w_ = 0;
        h_ = 0;
    }

    ~TemporaryRenderTarget() {
        reset();
    }

private:
    void* sdl_ = nullptr;
    void* target_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    bool active_ = false;
};

bool resolve_render_map(std::string* err = nullptr) {
    if (g_render_map_mode != RenderMapMode::None)
        return true;

    auto& core = DFHack::Core::getInstance();
    auto legacy = reinterpret_cast<pfn_RenderMapLegacy>(
        core.vinfo->getAddress("twbt_render_map"));
    if (legacy) {
        p_RenderMapLegacy = legacy;
        g_render_map_mode = RenderMapMode::LegacySymbol;
        return true;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) {
        if (err) *err = "native map render: could not get Dwarf Fortress.exe module";
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    // DF 53.15 (Steam) native map-render RVAs. Re-derived 2026-06-27 after DF updated
    // 53.14 -> 53.15 (the engine moved this machine code, which silently broke per-player
    // pan/zoom). The prologue signatures below are byte-identical across the two builds;
    // only the addresses moved. Old 53.14 values kept in comments for reference.
    const uintptr_t CLEAR_RVA = 0x8bcb60; // 53.14 was 0x8ba0f0
    const uintptr_t SETUP_RVA = 0x801630; // 53.14 was 0x7febc0
    const uintptr_t FILL_RVA = 0xe949e0;  // 53.14 was 0xe915c0
    const uintptr_t BLIT_RVA = 0xaeead0;  // 53.14 was 0xaec020

    struct Probe {
        uintptr_t rva;
        const uint8_t* sig;
        size_t n;
        const char* name;
    };
    static const uint8_t sig_clear[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
    static const uint8_t sig_setup[] = {0x48,0x83,0xec,0x48,0x48,0x0f,0xbf,0x05};
    static const uint8_t sig_fill[]  = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x83,0x3d};
    static const uint8_t sig_blit[]  = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18};
    const Probe probes[] = {
        {CLEAR_RVA, sig_clear, sizeof(sig_clear), "clear"},
        {SETUP_RVA, sig_setup, sizeof(sig_setup), "setup"},
        {FILL_RVA, sig_fill, sizeof(sig_fill), "fill"},
        {BLIT_RVA, sig_blit, sizeof(sig_blit), "blit"},
    };
    for (const auto& p : probes) {
        if (std::memcmp(reinterpret_cast<const uint8_t*>(base + p.rva), p.sig, p.n) != 0) {
            if (err) {
                *err = std::string("native map render: prologue mismatch for ") +
                       p.name + " (binary differs from Steam 53.15)";
            }
            return false;
        }
    }

    uintptr_t map_renderer = core.vinfo->getAddress("map_renderer");
    if (!map_renderer)
        map_renderer = base + 0x238f2c0; // DF 53.15 (was 0x2388300); normally resolved via the symbol above

    g_map_renderer_addr = map_renderer;
    g_exe_base = base;
    p_NativeClear = reinterpret_cast<pfn_NativeClear>(base + CLEAR_RVA);
    p_NativeSetup = reinterpret_cast<pfn_NativeSetup>(base + SETUP_RVA);
    p_NativeFill = reinterpret_cast<pfn_NativeFill>(base + FILL_RVA);
    p_NativeBlit = reinterpret_cast<pfn_NativeBlit>(base + BLIT_RVA);
    g_render_map_mode = RenderMapMode::NativeSequence;
    if (!g_warned_recovered_render_map.exchange(true)) {
        diagnostics_log("DIAG: resolved DF 53.15 native map-render sequence "
                        "(clear/setup/fill/blit) -- independent map render, no UI state.");
    }
    return true;
}
#endif

bool capture_ready(std::string* err) {
    auto world = df::global::world;
    auto gps = df::global::gps;
    auto enabler = df::global::enabler;
    if (!world || !gps || !gps->main_viewport || !enabler || !enabler->renderer) {
        if (err) *err = "DF fortress renderer is not ready";
        return false;
    }
    if (world->map.x_count <= 0 || world->map.y_count <= 0 || world->map.z_count <= 0) {
        if (err) *err = "DF map is not loaded";
        return false;
    }
    if (!DFHack::Gui::getCurViewscreen(true)) {
        if (err) *err = "no current DF viewscreen";
        return false;
    }
    return true;
}

bool render_current_viewscreen(std::string* err) {
    df::viewscreen* viewscreen = DFHack::Gui::getCurViewscreen(true);
    if (!viewscreen) {
        if (err) *err = "no current DF viewscreen";
        return false;
    }

    auto* plugins = DFHack::Core::getInstance().getPluginManager();
    DFHack::Plugin* overlay = plugins ? plugins->getPluginByName("overlay") : nullptr;
    if (overlay && overlay->is_enabled()) {
        if (err) {
            *err = "overlay plugin is enabled; independent capture refuses to render "
                   "through overlay Lua hooks";
        }
        return false;
    }

#ifdef _WIN32
    if (call_viewscreen_render_seh(viewscreen) != 0) {
        if (err) {
            std::ostringstream msg;
            msg << "viewscreen render fault code=0x" << std::hex << g_seh_code
                << " at=" << g_seh_at
                << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
            *err = msg.str();
        }
        return false;
    }
    return true;
#else
    viewscreen->render(0);
    return true;
#endif
}

void capture_zoom_reference_if_needed() {
#ifdef _WIN32
    if (g_ref_dim_x.load() > 0)
        return;
#endif
    auto gps = df::global::gps;
    auto vp = gps ? gps->main_viewport : nullptr;
    if (!gps || !vp || vp->dim_x <= 0 || vp->dim_y <= 0)
        return;
    int expected = gps->viewport_zoom_factor > 0 ? gps->viewport_zoom_factor : 192;
    g_ref_zoom_factor.store(expected);
    g_ref_dim_x.store(vp->dim_x);
    g_ref_dim_y.store(vp->dim_y);
#ifdef _WIN32
    diagnostics_log("DIAG zoom: captured fixed reference zoom=" +
                    std::to_string(g_ref_zoom_factor.load()) + " dims=" +
                    std::to_string(vp->dim_x) + "x" + std::to_string(vp->dim_y) +
                    " (per-player zoom is now independent of host zoom).");
#endif
}

bool per_player_zoom_active(const Camera& camera) {
#ifdef _WIN32
    (void)camera;
    return !g_zoom_unsafe.load() && g_ref_dim_x.load() > 0;
#else
    (void)camera;
    return false;
#endif
}

int zoom_percent_for_camera(const Camera& camera) {
    int pct = camera.zoom_factor >= 0 ? camera.zoom_factor : 100;
    return std::max(40, std::min(300, pct));
}

void effective_zoom_dims(const Camera& camera, int host_x, int host_y, int& out_x, int& out_y) {
    int base_x = g_ref_dim_x.load() > 0 ? g_ref_dim_x.load() : host_x;
    int base_y = g_ref_dim_y.load() > 0 ? g_ref_dim_y.load() : host_y;
    out_x = std::max(1, base_x);
    out_y = std::max(1, base_y);
    if (!per_player_zoom_active(camera))
        return;
    int pct = zoom_percent_for_camera(camera);
    out_x = std::max(4, std::min(512, (base_x * pct + 50) / 100));
    out_y = std::max(4, std::min(512, (base_y * pct + 50) / 100));
}

#ifdef _WIN32
struct ViewportPointerField {
    void** slot = nullptr;
    size_t elem_size = 0;
};

template <typename T>
void add_viewport_pointer_field(std::vector<ViewportPointerField>& fields, T*& field) {
    fields.push_back({reinterpret_cast<void**>(&field), sizeof(T)});
}

std::vector<ViewportPointerField> viewport_pointer_fields(df::graphic_viewportst* vp) {
    std::vector<ViewportPointerField> fields;
    if (!vp)
        return fields;
    fields.reserve(52);

    add_viewport_pointer_field(fields, vp->screentexpos_background);
    add_viewport_pointer_field(fields, vp->screentexpos_floor_flag);
    add_viewport_pointer_field(fields, vp->screentexpos_background_two);
    add_viewport_pointer_field(fields, vp->screentexpos_liquid_flag);
    add_viewport_pointer_field(fields, vp->screentexpos_spatter_flag);
    add_viewport_pointer_field(fields, vp->screentexpos_spatter);
    add_viewport_pointer_field(fields, vp->screentexpos_ramp_flag);
    add_viewport_pointer_field(fields, vp->screentexpos_shadow_flag);
    add_viewport_pointer_field(fields, vp->screentexpos_building_one);
    add_viewport_pointer_field(fields, vp->screentexpos_item);
    add_viewport_pointer_field(fields, vp->screentexpos_vehicle);
    add_viewport_pointer_field(fields, vp->screentexpos_vermin);
    add_viewport_pointer_field(fields, vp->screentexpos_left_creature);
    add_viewport_pointer_field(fields, vp->screentexpos);
    add_viewport_pointer_field(fields, vp->screentexpos_right_creature);
    add_viewport_pointer_field(fields, vp->screentexpos_building_two);
    add_viewport_pointer_field(fields, vp->screentexpos_projectile);
    add_viewport_pointer_field(fields, vp->screentexpos_high_flow);
    add_viewport_pointer_field(fields, vp->screentexpos_top_shadow);
    add_viewport_pointer_field(fields, vp->screentexpos_signpost);
    add_viewport_pointer_field(fields, vp->screentexpos_upleft_creature);
    add_viewport_pointer_field(fields, vp->screentexpos_up_creature);
    add_viewport_pointer_field(fields, vp->screentexpos_upright_creature);
    add_viewport_pointer_field(fields, vp->screentexpos_designation);
    add_viewport_pointer_field(fields, vp->screentexpos_interface);
    add_viewport_pointer_field(fields, vp->screentexpos_background_old);
    add_viewport_pointer_field(fields, vp->screentexpos_floor_flag_old);
    add_viewport_pointer_field(fields, vp->screentexpos_background_two_old);
    add_viewport_pointer_field(fields, vp->screentexpos_liquid_flag_old);
    add_viewport_pointer_field(fields, vp->screentexpos_spatter_flag_old);
    add_viewport_pointer_field(fields, vp->screentexpos_spatter_old);
    add_viewport_pointer_field(fields, vp->screentexpos_ramp_flag_old);
    add_viewport_pointer_field(fields, vp->screentexpos_shadow_flag_old);
    add_viewport_pointer_field(fields, vp->screentexpos_building_one_old);
    add_viewport_pointer_field(fields, vp->screentexpos_item_old);
    add_viewport_pointer_field(fields, vp->screentexpos_vehicle_old);
    add_viewport_pointer_field(fields, vp->screentexpos_vermin_old);
    add_viewport_pointer_field(fields, vp->screentexpos_left_creature_old);
    add_viewport_pointer_field(fields, vp->screentexpos_old);
    add_viewport_pointer_field(fields, vp->screentexpos_right_creature_old);
    add_viewport_pointer_field(fields, vp->screentexpos_building_two_old);
    add_viewport_pointer_field(fields, vp->screentexpos_projectile_old);
    add_viewport_pointer_field(fields, vp->screentexpos_high_flow_old);
    add_viewport_pointer_field(fields, vp->screentexpos_top_shadow_old);
    add_viewport_pointer_field(fields, vp->screentexpos_signpost_old);
    add_viewport_pointer_field(fields, vp->screentexpos_upleft_creature_old);
    add_viewport_pointer_field(fields, vp->screentexpos_up_creature_old);
    add_viewport_pointer_field(fields, vp->screentexpos_upright_creature_old);
    add_viewport_pointer_field(fields, vp->screentexpos_designation_old);
    add_viewport_pointer_field(fields, vp->screentexpos_interface_old);
    add_viewport_pointer_field(fields, vp->core_tree_species_plus_one);
    add_viewport_pointer_field(fields, vp->shadow_tree_species_plus_one);
    return fields;
}

struct CachedViewportBuffers {
    int dim_x = 0;
    int dim_y = 0;
    std::vector<std::vector<uint64_t>> storage;

    void prepare(int new_dim_x, int new_dim_y, const std::vector<ViewportPointerField>& fields) {
        int tiles = std::max(1, new_dim_x * new_dim_y);
        if (dim_x != new_dim_x || dim_y != new_dim_y || storage.size() != fields.size()) {
            dim_x = new_dim_x;
            dim_y = new_dim_y;
            storage.assign(fields.size(), {});
            for (size_t i = 0; i < fields.size(); ++i) {
                size_t bytes = static_cast<size_t>(tiles) * fields[i].elem_size;
                storage[i].resize((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
            }
        }
        for (auto& buffer : storage)
            std::fill(buffer.begin(), buffer.end(), 0);
    }
};

std::unordered_map<uintptr_t, std::unique_ptr<CachedViewportBuffers>> g_zoom_buffers;

CachedViewportBuffers& cached_viewport_buffers(df::graphic_viewportst* vp) {
    auto& ptr = g_zoom_buffers[reinterpret_cast<uintptr_t>(vp)];
    if (!ptr)
        ptr.reset(new CachedViewportBuffers());
    return *ptr;
}

void add_unique_viewport(std::vector<df::graphic_viewportst*>& viewports, df::graphic_viewportst* vp) {
    if (vp && std::find(viewports.begin(), viewports.end(), vp) == viewports.end())
        viewports.push_back(vp);
}

std::vector<df::graphic_viewportst*> collect_viewports(df::graphic* gps) {
    std::vector<df::graphic_viewportst*> viewports;
    if (!gps)
        return viewports;
    add_unique_viewport(viewports, gps->main_viewport);
    for (auto vp : gps->lower_viewport)
        add_unique_viewport(viewports, vp);
    for (auto vp : gps->viewport)
        add_unique_viewport(viewports, vp);
    return viewports;
}

struct SavedViewportState {
    df::graphic_viewportst* vp = nullptr;
    df::graphic_viewport_flag flag;
    int dim_x = 0;
    int dim_y = 0;
    int clipx[2] = {};
    int clipy[2] = {};
    int screen_x = 0;
    int screen_y = 0;
    std::vector<void*> pointers;
};

class ViewportZoomGuard {
public:
    bool activate(const Camera& camera, std::string* err) {
        capture_zoom_reference_if_needed();
        if (!per_player_zoom_active(camera))
            return true;

        gps_ = df::global::gps;
        auto enabler = df::global::enabler;
        renderer_ = enabler ? enabler->renderer : nullptr;
        auto main_vp = gps_ ? gps_->main_viewport : nullptr;
        if (!gps_ || !main_vp || !renderer_) {
            if (err) *err = "zoom unavailable: gps/viewport/renderer missing";
            return false;
        }

        live_host_zoom_ = gps_->viewport_zoom_factor > 0 ? gps_->viewport_zoom_factor : 192;
        int ref_zoom = g_ref_zoom_factor.load() > 0 ? g_ref_zoom_factor.load() : live_host_zoom_;
        int percent = zoom_percent_for_camera(camera);
        target_zoom_ = std::max(16, std::min(2048, (ref_zoom * 100 + percent / 2) / percent));
        effective_zoom_dims(camera, main_vp->dim_x, main_vp->dim_y, target_dim_x_, target_dim_y_);

        if (target_dim_x_ == main_vp->dim_x && target_dim_y_ == main_vp->dim_y &&
                target_zoom_ == live_host_zoom_)
            return true;

        viewports_ = collect_viewports(gps_);
        for (auto vp : viewports_) {
            auto fields = viewport_pointer_fields(vp);
            if (fields.size() != 52) {
                if (err) *err = "zoom unavailable: unexpected viewport buffer layout";
                restore();
                return false;
            }

            SavedViewportState saved;
            saved.vp = vp;
            saved.flag = vp->flag;
            saved.dim_x = vp->dim_x;
            saved.dim_y = vp->dim_y;
            saved.clipx[0] = vp->clipx[0];
            saved.clipx[1] = vp->clipx[1];
            saved.clipy[0] = vp->clipy[0];
            saved.clipy[1] = vp->clipy[1];
            saved.screen_x = vp->screen_x;
            saved.screen_y = vp->screen_y;
            saved.pointers.reserve(fields.size());
            for (auto& field : fields)
                saved.pointers.push_back(*field.slot);

            auto& cache = cached_viewport_buffers(vp);
            cache.prepare(target_dim_x_, target_dim_y_, fields);
            for (size_t i = 0; i < fields.size(); ++i)
                *fields[i].slot = cache.storage[i].empty() ? nullptr : cache.storage[i].data();

            vp->dim_x = target_dim_x_;
            vp->dim_y = target_dim_y_;
            vp->clipx[0] = 0;
            vp->clipx[1] = target_dim_x_ - 1;
            vp->clipy[0] = 0;
            vp->clipy[1] = target_dim_y_ - 1;
            saved_.push_back(std::move(saved));
        }

        gps_->viewport_zoom_factor = target_zoom_;
        if (!call_set_viewport_zoom_factor_seh(renderer_, target_zoom_)) {
            g_zoom_unsafe.store(true);
            if (err) *err = "zoom unsafe: set_viewport_zoom_factor faulted";
            restore();
            return false;
        }
        gps_->force_full_display_count = 1;
        active_ = true;
        return true;
    }

    bool active() const {
        return active_;
    }

    void restore() {
        if (gps_) {
            gps_->viewport_zoom_factor = live_host_zoom_ > 0 ? live_host_zoom_ : 192;
            gps_->force_full_display_count = 1;
        }
        if (renderer_ && live_host_zoom_ > 0)
            call_set_viewport_zoom_factor_seh(renderer_, live_host_zoom_);

        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            auto vp = it->vp;
            if (!vp)
                continue;
            auto fields = viewport_pointer_fields(vp);
            size_t n = std::min(fields.size(), it->pointers.size());
            for (size_t i = 0; i < n; ++i)
                *fields[i].slot = it->pointers[i];
            vp->flag = it->flag;
            vp->dim_x = it->dim_x;
            vp->dim_y = it->dim_y;
            vp->clipx[0] = it->clipx[0];
            vp->clipx[1] = it->clipx[1];
            vp->clipy[0] = it->clipy[0];
            vp->clipy[1] = it->clipy[1];
            vp->screen_x = it->screen_x;
            vp->screen_y = it->screen_y;
        }
        saved_.clear();
        viewports_.clear();
        active_ = false;
    }

    ~ViewportZoomGuard() {
        restore();
    }

private:
    df::graphic* gps_ = nullptr;
    df::renderer* renderer_ = nullptr;
    bool active_ = false;
    int live_host_zoom_ = 0;
    int target_zoom_ = 0;
    int target_dim_x_ = 0;
    int target_dim_y_ = 0;
    std::vector<df::graphic_viewportst*> viewports_;
    std::vector<SavedViewportState> saved_;
};
#endif

#ifdef _WIN32
int32_t rect_9slice(int x, int y, int x0, int x1, int y0, int y1) {
    bool left = x == x0;
    bool right = x == x1;
    bool top = y == y0;
    bool bottom = y == y1;
    if (top && left) return RECT_TL;
    if (top && right) return RECT_TR;
    if (bottom && left) return RECT_BL;
    if (bottom && right) return RECT_BR;
    if (top) return RECT_T;
    if (bottom) return RECT_B;
    if (left) return RECT_L;
    if (right) return RECT_R;
    return RECT_C;
}

bool seh_copy_ints(int32_t* dst, const int32_t* src, int n) {
    __try {
        std::memcpy(dst, src, static_cast<size_t>(n) * sizeof(int32_t));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_fill_ints(int32_t* dst, int n, int32_t val) {
    __try {
        for (int i = 0; i < n; ++i)
            dst[i] = val;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool draw_designation_grid_seh(df::graphic_viewportst* vp, const Camera& camera) {
    bool ok = false;
    __try {
        int32_t* iface = vp ? vp->screentexpos_interface : nullptr;
        int dimx = vp ? vp->dim_x : 0;
        int dimy = vp ? vp->dim_y : 0;
        if (iface && dimx > 0 && dimy > 0) {
            int x0 = std::max(0, vp->clipx[0]);
            int y0 = std::max(0, vp->clipy[0]);
            int x1 = std::min(dimx - 1, vp->clipx[1]);
            int y1 = std::min(dimy - 1, vp->clipy[1]);
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    iface[x * dimy + y] = GRID_TEXPOS;

            auto to_idx = [](int p, int dim, int frame) {
                if (frame <= 0 || dim <= 0) return 0;
                int i = (p * dim) / frame;
                if (i < 0) i = 0;
                if (i > dim - 1) i = dim - 1;
                return i;
            };
            auto in_clip = [&](int x, int y) {
                return x >= x0 && x <= x1 && y >= y0 && y <= y1;
            };

            if (camera.ui_frame_w > 0 && camera.ui_frame_h > 0) {
                if (camera.build_w * camera.build_h > 1 &&
                        camera.hover_px >= 0 && camera.hover_py >= 0) {
                    int cx = to_idx(camera.hover_px, dimx, camera.ui_frame_w);
                    int cy = to_idx(camera.hover_py, dimy, camera.ui_frame_h);
                    int rx0 = cx - camera.build_w / 2;
                    int rx1 = rx0 + camera.build_w - 1;
                    int ry0 = cy - camera.build_h / 2;
                    int ry1 = ry0 + camera.build_h - 1;
                    for (int x = rx0; x <= rx1; ++x)
                        for (int y = ry0; y <= ry1; ++y)
                            if (in_clip(x, y))
                                iface[x * dimy + y] = rect_9slice(x, y, rx0, rx1, ry0, ry1);
                } else if (camera.drag_active) {
                    int ax = to_idx(camera.drag_px, dimx, camera.ui_frame_w);
                    int ay = to_idx(camera.drag_py, dimy, camera.ui_frame_h);
                    int bx = to_idx(camera.hover_px, dimx, camera.ui_frame_w);
                    int by = to_idx(camera.hover_py, dimy, camera.ui_frame_h);
                    int rx0 = std::min(ax, bx);
                    int rx1 = std::max(ax, bx);
                    int ry0 = std::min(ay, by);
                    int ry1 = std::max(ay, by);
                    for (int x = rx0; x <= rx1; ++x)
                        for (int y = ry0; y <= ry1; ++y)
                            if (in_clip(x, y))
                                iface[x * dimy + y] = rect_9slice(x, y, rx0, rx1, ry0, ry1);
                } else if (camera.hover_px >= 0 && camera.hover_py >= 0) {
                    int cx = to_idx(camera.hover_px, dimx, camera.ui_frame_w);
                    int cy = to_idx(camera.hover_py, dimy, camera.ui_frame_h);
                    if (in_clip(cx, cy))
                        iface[cx * dimy + cy] = CURSOR_TEXPOS;
                }
            }
        }
        ok = true;
    } __except(dfcapture_seh_filter(GetExceptionInformation())) {
        ok = false;
    }
    return ok;
}

bool render_map_for_current_window(std::string* err = nullptr) {
    if (!resolve_render_map(err))
        return false;
    auto gps = df::global::gps;
    if (gps)
        gps->force_full_display_count = 1;

    if (g_render_map_mode == RenderMapMode::LegacySymbol) {
        p_RenderMapLegacy(0);
        return true;
    }

    if (g_render_map_mode != RenderMapMode::NativeSequence) {
        if (err) *err = "direct map render was not initialized";
        return false;
    }

    if (!gps || !gps->main_viewport) {
        if (err) *err = "native map render: gps->main_viewport unavailable";
        return false;
    }

    TemporaryRenderTarget native_target;
    std::string target_err;
    if (!native_target.begin(&target_err)) {
        if (err) *err = target_err;
        return false;
    }
    if (!g_warned_native_target_guard.exchange(true)) {
        diagnostics_log("DIAG: native map-render sequence is isolated on a throwaway SDL target.");
    }

    int fault = call_native_sequence_seh(g_map_renderer_addr);
    if (fault != 0) {
        static const char* stage_name[] = {
            "none", "clear(0x8bcb60)", "setup(0x801630)",
            "fill(0xe949e0)", "blit(0xaeead0)"
        };
        static std::atomic<uint32_t> fault_count{0};
        if ((fault_count.fetch_add(1) % 600) == 0) {
            std::ostringstream msg;
            msg << "DIAG NATIVE FAULT @ stage " << fault << " "
                << stage_name[fault] << ": code=0x" << std::hex << g_seh_code
                << " ip=exe+0x" << (reinterpret_cast<uintptr_t>(g_seh_at) - g_exe_base)
                << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
            diagnostics_log(msg.str());
        }
        if (err) *err = "native sequence faulted this frame (caught; will retry next frame)";
        return false;
    }

    return true;
}

bool render_viewscreen_without_overlay(std::string* err = nullptr) {
    df::viewscreen* viewscreen = DFHack::Gui::getCurViewscreen(true);
    if (!viewscreen) {
        if (err) *err = "no current viewscreen";
        return false;
    }

    auto* plugins = DFHack::Core::getInstance().getPluginManager();
    DFHack::Plugin* overlay = plugins ? plugins->getPluginByName("overlay") : nullptr;
    if (overlay && overlay->is_enabled()) {
        if (err) *err = "overlay plugin is enabled; independent fallback needs it disabled before viewscreen rendering";
        return false;
    }

    TemporaryRenderTarget target;
    std::string target_err;
    if (!target.begin(&target_err)) {
        if (err) *err = target_err;
        return false;
    }
    if (!g_warned_viewscreen_target_guard.exchange(true)) {
        diagnostics_log("DIAG: viewscreen fallback render is isolated on a throwaway SDL target (host-flash fix).");
    }

    if (call_viewscreen_render_seh(viewscreen) != 0) {
        if (err) {
            std::ostringstream msg;
            msg << "viewscreen render fault code=0x" << std::hex << g_seh_code
                << " at=" << g_seh_at
                << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
            *err = msg.str();
        }
        return false;
    }
    return true;
}
#endif

bool host_interacting() {
    auto game = df::global::game;
    if (!game)
        return false;

    auto& main = game->main_interface;
    bool main_tool_active =
        main.bottom_mode_selected != df::main_bottom_mode_type::NONE ||
        main.main_designation_selected != df::main_designation_type::NONE;
    if (main_tool_active)
        return true;

    bool building_resize_active =
        df::global::ui_building_in_resize && *df::global::ui_building_in_resize;
    bool building_assign_active =
        df::global::ui_building_in_assign && *df::global::ui_building_in_assign;
    bool workshop_add_active =
        df::global::ui_workshop_in_add && *df::global::ui_workshop_in_add;

    auto build = df::global::buildreq;
    (void)build;
    if (building_resize_active || building_assign_active || workshop_add_active) {
        static std::atomic<bool> warned_build_state{false};
        if (!warned_build_state.exchange(true)) {
            int btype = build ? static_cast<int>(build->building_type) : -999;
            int stage = build ? static_cast<int>(build->stage) : -999;
            diagnostics_log("DIAG host_interacting build state: btype=" +
                            std::to_string(btype) + " stage=" + std::to_string(stage) +
                            " resize=" + std::to_string(building_resize_active ? 1 : 0) +
                            " assign=" + std::to_string(building_assign_active ? 1 : 0) +
                            " workshop_add=" + std::to_string(workshop_add_active ? 1 : 0));
        }
        return true;
    }

    return false;
}

struct RenderThreadCameraRequest {
    Camera camera;
    std::string err;
    std::promise<bool> done;
};

bool run_read_host_camera(Camera& camera, std::string* err) {
    auto request = std::make_shared<RenderThreadCameraRequest>();
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        if (!df::global::window_x || !df::global::window_y || !df::global::window_z) {
            request->err = "DF window coordinates are unavailable";
            request->done.set_value(false);
            return;
        }
        request->camera.x = *df::global::window_x;
        request->camera.y = *df::global::window_y;
        request->camera.z = *df::global::window_z;
        request->done.set_value(true);
    });

    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        if (err) *err = "timed out reading host camera on render thread";
        diagnostics_log("WARN: timed out reading host camera on render thread");
        return false;
    }

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    camera = request->camera;
    return true;
}

struct RenderThreadCaptureRequest {
    Camera camera;
    CapturedFrame frame;
    std::string err;
    std::promise<bool> done;
};

struct OverlayColor {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
};

void blend_pixel(CapturedFrame& frame, int x, int y, OverlayColor color, int alpha) {
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height || alpha <= 0)
        return;
    alpha = std::max(0, std::min(255, alpha));
    size_t off = (static_cast<size_t>(y) * frame.width + x) * 4;
    auto blend = [alpha](uint8_t dst, uint8_t src) -> uint8_t {
        return static_cast<uint8_t>((dst * (255 - alpha) + src * alpha + 127) / 255);
    };
    frame.bgra[off + 0] = blend(frame.bgra[off + 0], color.b);
    frame.bgra[off + 1] = blend(frame.bgra[off + 1], color.g);
    frame.bgra[off + 2] = blend(frame.bgra[off + 2], color.r);
    frame.bgra[off + 3] = 255;
}

void draw_hline(CapturedFrame& frame, int x1, int x2, int y, OverlayColor color, int alpha) {
    if (y < 0 || y >= frame.height)
        return;
    if (x1 > x2)
        std::swap(x1, x2);
    x1 = std::max(0, x1);
    x2 = std::min(frame.width - 1, x2);
    for (int x = x1; x <= x2; ++x)
        blend_pixel(frame, x, y, color, alpha);
}

void draw_vline(CapturedFrame& frame, int x, int y1, int y2, OverlayColor color, int alpha) {
    if (x < 0 || x >= frame.width)
        return;
    if (y1 > y2)
        std::swap(y1, y2);
    y1 = std::max(0, y1);
    y2 = std::min(frame.height - 1, y2);
    for (int y = y1; y <= y2; ++y)
        blend_pixel(frame, x, y, color, alpha);
}

void draw_rect(CapturedFrame& frame, int x1, int y1, int x2, int y2,
               OverlayColor color, int alpha) {
    draw_hline(frame, x1, x2, y1, color, alpha);
    draw_hline(frame, x1, x2, y2, color, alpha);
    draw_vline(frame, x1, y1, y2, color, alpha);
    draw_vline(frame, x2, y1, y2, color, alpha);
}

void fill_rect(CapturedFrame& frame, int x1, int y1, int x2, int y2,
               OverlayColor color, int alpha) {
    if (x1 > x2)
        std::swap(x1, x2);
    if (y1 > y2)
        std::swap(y1, y2);
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(frame.width - 1, x2);
    y2 = std::min(frame.height - 1, y2);
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x)
            blend_pixel(frame, x, y, color, alpha);
}

int tile_to_pixel_x(int tile, int tiles, int width) {
    return tiles <= 0 ? 0 : (tile * width) / tiles;
}

int tile_to_pixel_y(int tile, int tiles, int height) {
    return tiles <= 0 ? 0 : (tile * height) / tiles;
}

int pixel_to_tile_coord(int pixel, int frame_pixels, int tiles) {
    if (frame_pixels <= 0 || tiles <= 0)
        return 0;
    return std::max(0, std::min(tiles - 1, (pixel * tiles) / frame_pixels));
}

bool overlay_viewport_dims(const Camera& camera, int& tiles_x, int& tiles_y) {
    auto gps = df::global::gps;
    auto vp = gps ? gps->main_viewport : nullptr;
    if (!gps || !vp || vp->dim_x <= 0 || vp->dim_y <= 0)
        return false;
    capture_zoom_reference_if_needed();
    effective_zoom_dims(camera, vp->dim_x, vp->dim_y, tiles_x, tiles_y);
    return tiles_x > 0 && tiles_y > 0;
}

void draw_designation_grid(CapturedFrame& frame, int tiles_x, int tiles_y) {
    OverlayColor grid{24, 89, 143};
    int tile_px = std::max(1, frame.width / std::max(1, tiles_x));
    int tile_py = std::max(1, frame.height / std::max(1, tiles_y));
    int alpha = tile_px >= 8 && tile_py >= 8 ? 82 : 44;
    for (int tx = 0; tx <= tiles_x; ++tx)
        draw_vline(frame, tile_to_pixel_x(tx, tiles_x, frame.width), 0, frame.height - 1, grid, alpha);
    for (int ty = 0; ty <= tiles_y; ++ty)
        draw_hline(frame, 0, frame.width - 1, tile_to_pixel_y(ty, tiles_y, frame.height), grid, alpha);
}

void draw_tile_highlight(CapturedFrame& frame, int tx1, int ty1, int tx2, int ty2,
                         int tiles_x, int tiles_y, OverlayColor color) {
    tx1 = std::max(0, std::min(tiles_x - 1, tx1));
    tx2 = std::max(0, std::min(tiles_x - 1, tx2));
    ty1 = std::max(0, std::min(tiles_y - 1, ty1));
    ty2 = std::max(0, std::min(tiles_y - 1, ty2));
    if (tx1 > tx2)
        std::swap(tx1, tx2);
    if (ty1 > ty2)
        std::swap(ty1, ty2);

    int x1 = tile_to_pixel_x(tx1, tiles_x, frame.width);
    int y1 = tile_to_pixel_y(ty1, tiles_y, frame.height);
    int x2 = tile_to_pixel_x(tx2 + 1, tiles_x, frame.width) - 1;
    int y2 = tile_to_pixel_y(ty2 + 1, tiles_y, frame.height) - 1;
    fill_rect(frame, x1, y1, x2, y2, color, 34);
    draw_rect(frame, x1, y1, x2, y2, color, 210);
    if (x2 - x1 > 7 && y2 - y1 > 7)
        draw_rect(frame, x1 + 1, y1 + 1, x2 - 1, y2 - 1, color, 210);
}

void draw_placement_overlay(const Camera& camera, CapturedFrame& frame) {
    if (!camera.placement_mode || frame.width <= 0 || frame.height <= 0)
        return;

    int tiles_x = 0;
    int tiles_y = 0;
    if (!overlay_viewport_dims(camera, tiles_x, tiles_y))
        return;

    draw_designation_grid(frame, tiles_x, tiles_y);
    if (camera.hover_px < 0 || camera.hover_py < 0 ||
            camera.ui_frame_w <= 0 || camera.ui_frame_h <= 0)
        return;

    int hx = pixel_to_tile_coord(camera.hover_px, camera.ui_frame_w, tiles_x);
    int hy = pixel_to_tile_coord(camera.hover_py, camera.ui_frame_h, tiles_y);
    int dx = camera.drag_active ? pixel_to_tile_coord(camera.drag_px, camera.ui_frame_w, tiles_x) : hx;
    int dy = camera.drag_active ? pixel_to_tile_coord(camera.drag_py, camera.ui_frame_h, tiles_y) : hy;
    OverlayColor amber{28, 198, 255};
    draw_tile_highlight(frame, dx, dy, hx, hy, tiles_x, tiles_y, amber);
}

bool capture_camera_frame_on_render_thread(const Camera& requested,
                                           CapturedFrame& frame,
                                           std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_capture_mutex);

    auto request = std::make_shared<RenderThreadCaptureRequest>();
    request->camera = requested;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        request->done.set_value(capture_camera_frame(request->camera, request->frame, &request->err));
    });

    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        if (err) *err = "timed out capturing frame on render thread";
        diagnostics_log("WARN: timed out capturing frame on render thread");
        return false;
    }

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    frame = std::move(request->frame);
    return true;
}

} // namespace

bool read_host_camera(Camera& camera, std::string* err) {
    return run_read_host_camera(camera, err);
}

std::recursive_mutex& capture_state_mutex() {
    return g_capture_mutex;
}

bool clamp_camera(Camera& camera, std::string* err) {
    auto request = std::make_shared<RenderThreadCameraRequest>();
    request->camera = camera;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
#ifdef _WIN32
        __try {
#endif
        auto world = df::global::world;
        if (!world) {
            request->err = "world/map not available";
            request->done.set_value(false);
            return;
        }

        request->camera.x = std::max(0, std::min(request->camera.x, std::max(0, world->map.x_count - 1)));
        request->camera.y = std::max(0, std::min(request->camera.y, std::max(0, world->map.y_count - 1)));
        request->camera.z = std::max(0, std::min(request->camera.z, std::max(0, world->map.z_count - 1)));
        request->done.set_value(true);
#ifdef _WIN32
        } __except(dfcapture_seh_filter(GetExceptionInformation())) {
            request->err = "SEH fault while clamping camera";
            request->done.set_value(false);
        }
#endif
    });

    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        if (err) *err = "timed out clamping camera on render thread";
        diagnostics_log("WARN: timed out clamping camera on render thread");
        return false;
    }

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    camera = request->camera;
    return true;
}

bool effective_capture_viewport_dims(const Camera& camera, int& width_tiles,
                                     int& height_tiles, std::string* err) {
    auto gps = df::global::gps;
    auto vp = gps ? gps->main_viewport : nullptr;
    if (!gps || !vp || vp->dim_x <= 0 || vp->dim_y <= 0) {
        if (err) *err = "viewport unavailable";
        return false;
    }

    capture_zoom_reference_if_needed();
    effective_zoom_dims(camera, vp->dim_x, vp->dim_y, width_tiles, height_tiles);
    return width_tiles > 0 && height_tiles > 0;
}

bool capture_current_frame(CapturedFrame& frame, bool include_ui = true,
                           std::string* err = nullptr) {
    if (!capture_ready(err) || !resolve_sdl(err))
        return false;

    auto enabler = df::global::enabler;
    df::renderer* renderer = enabler ? enabler->renderer : nullptr;
    if (!renderer) {
        if (err) *err = "no enabler/renderer";
        return false;
    }

    void* sdl_renderer = renderer->get_renderer();
    if (!sdl_renderer) {
        if (err) *err = "renderer->get_renderer returned null";
        return false;
    }

    int width = 0;
    int height = 0;
    p_GetRendererOutputSize(sdl_renderer, &width, &height);
    if (width <= 0 || height <= 0) {
        if (err) *err = "bad renderer output size";
        return false;
    }

    void* target = p_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_TARGET, width, height);
    if (!target) {
        if (err) *err = "SDL_CreateTexture(target) failed";
        return false;
    }

    if (p_SetRenderTarget(sdl_renderer, target) != 0) {
        p_DestroyTexture(target);
        if (!g_warned_capture_target_bind.exchange(true)) {
            diagnostics_log("DIAG: SDL_SetRenderTarget failed for capture target; "
                            "dropping frame to avoid drawing into the host window.");
        }
        if (err) *err = "SDL_SetRenderTarget(capture target) failed";
        return false;
    }

    auto gps = df::global::gps;
    if (gps && gps->main_viewport) {
#ifdef _WIN32
        bool viewport_ok = call_update_full_viewport_seh(renderer, gps->main_viewport);
        if (!viewport_ok) {
            std::ostringstream msg;
            msg << "DIAG update_full_viewport FAULT: code=0x" << std::hex << g_seh_code
                << " ip=exe+0x" << (reinterpret_cast<uintptr_t>(g_seh_at) - g_exe_base)
                << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
            diagnostics_log(msg.str());
            g_zoom_unsafe.store(true);
        }
#else
        renderer->update_full_viewport(gps->main_viewport);
#endif
    }

    if (include_ui && !g_warned_ui_overlay_skip.exchange(true)) {
        diagnostics_log("DIAG: skipping renderer::update_all() during offscreen capture; "
                        "the DFHack overlay render hook runs Lua here and crashed in the "
                        "2026-06-16 stack.");
    }

    CapturedFrame next;
    next.width = width;
    next.height = height;
    next.bgra.resize(static_cast<size_t>(width) * height * 4);
    int rc = p_RenderReadPixels(sdl_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                next.bgra.data(), width * 4);

    p_SetRenderTarget(sdl_renderer, nullptr);
    p_DestroyTexture(target);

    if (rc != 0) {
        if (err) *err = "SDL_RenderReadPixels failed";
        return false;
    }

    frame = std::move(next);
    return true;
}

bool capture_shifted(const Camera& camera, CapturedFrame& frame,
                     bool include_ui = true, std::string* err = nullptr,
                     bool restore_host_buffers = true) {
    // capture_shifted always runs ON the render thread (via capture_camera_frame_on_render_thread,
    // or the capture/capture-at commands' runOnRenderThread). Read the host camera DIRECTLY here:
    // calling the marshaling read_host_camera() would queue ANOTHER render-thread task behind this
    // one, which can never run while we're occupying the render thread -> 3s self-deadlock timeout
    // ("timed out reading host camera on render thread") -> every frame fails -> black webview.
#ifdef _WIN32
    // HARD GATE (crash fix 2026-06-27): only run the native map render while a fortress map view
    // is actually live. During title/load/save -- and especially world TEARDOWN when a fort is
    // exited (top viewscreen becomes viewscreen_titlest / viewscreen_game_cleanerst) -- DF's MAIN
    // thread frees the map + renderer while this render thread would still be reading them. That
    // produced the null/wild-pointer "NATIVE FAULT @ fill/blit" entries in dfcapture.log AND, more
    // seriously, a crash on DF's own main thread mid-teardown ("Advancing unit moves") that our
    // SEH cannot catch (SEH only guards THIS thread; it can't stop the main thread from faulting
    // on a map we race). getViewscreenByType<dwarfmodest>(0) scans the whole viewscreen stack, so
    // it stays valid under menus/overlays during play and is null only when there is genuinely no
    // live fort -- exactly the moments we must not touch the map.
    if (!DFHack::Gui::getViewscreenByType<df::viewscreen_dwarfmodest>(0)) {
        if (err) *err = "no live fortress view (capture skipped during menu/load/teardown)";
        return false;
    }
#endif
    Camera saved;
    if (!df::global::window_x || !df::global::window_y || !df::global::window_z) {
        if (err) *err = "DF window coordinates are unavailable";
        return false;
    }
    saved.x = *df::global::window_x;
    saved.y = *df::global::window_y;
    saved.z = *df::global::window_z;

    bool needs_full_host_restore = false;

#ifdef _WIN32
    {
        df::viewscreen* viewscreen = DFHack::Gui::getCurViewscreen(true);
        const char* vname = viewscreen ? DFHack::virtual_identity::get(viewscreen)->getName() : "(null)";
        static std::string s_last_vs;
        if (vname && s_last_vs != vname) {
            s_last_vs = vname;
            int gametype = df::global::gametype ? static_cast<int>(*df::global::gametype) : -999;
            diagnostics_log(std::string("DIAG host-flash: top viewscreen = ") + vname +
                            ", gametype=" + std::to_string(gametype));
        }
    }
#endif

    auto gps = df::global::gps;
    if (!df::global::window_x || !df::global::window_y || !df::global::window_z) {
        if (err) *err = "DF window coordinates are unavailable";
        return false;
    }

    *df::global::window_x = camera.x;
    *df::global::window_y = camera.y;
    *df::global::window_z = camera.z;
    if (gps)
        gps->force_full_display_count = 1;

#ifdef _WIN32
    capture_zoom_reference_if_needed();
    ViewportZoomGuard zoom_guard;
    std::string zoom_err;
    if (!zoom_guard.activate(camera, &zoom_err)) {
        diagnostics_log("DIAG: " + zoom_err + "; per-player zoom disabled.");
        g_zoom_unsafe.store(true);
    }
    {
        static std::atomic<int> s_last_zf{-999};
        int zf = camera.zoom_factor;
        if (s_last_zf.exchange(zf) != zf) {
            diagnostics_log("DIAG zoom: cam.zoom_factor=" + std::to_string(zf) +
                            " guard_active=" + std::to_string(zoom_guard.active() ? 1 : 0) +
                            " g_zoom_unsafe=" + std::to_string(g_zoom_unsafe.load() ? 1 : 0));
        }
    }
#endif

    std::string map_err;
#ifdef _WIN32
    if (!render_map_for_current_window(&map_err)) {
        if (host_interacting()) {
            *df::global::window_x = saved.x;
            *df::global::window_y = saved.y;
            *df::global::window_z = saved.z;
            if (gps)
                gps->force_full_display_count = 1;
            if (!g_warned_host_interaction_skip.exchange(true)) {
                diagnostics_log("DIAG: host dig/build/designation interaction active and direct "
                                "map renderer unavailable; holding last independent frame.");
            }
            if (err) *err = HOST_INTERACTION_SKIP;
            return false;
        }
        if (!g_warned_viewscreen_render_fallback.exchange(true)) {
            diagnostics_log("DIAG: " + map_err + "; rendering shifted view through viewscreen fallback.");
        }
        if (!render_viewscreen_without_overlay(err)) {
            *df::global::window_x = saved.x;
            *df::global::window_y = saved.y;
            *df::global::window_z = saved.z;
            if (gps)
                gps->force_full_display_count = 1;
            return false;
        }
        needs_full_host_restore = true;
    } else {
        needs_full_host_restore = g_render_map_mode == RenderMapMode::NativeSequence;
    }
#else
    if (!render_current_viewscreen(&map_err)) {
        if (err) *err = map_err;
        return false;
    }
    needs_full_host_restore = true;
#endif

#ifdef _WIN32
    bool iface_saved = false;
    std::vector<int32_t> saved_iface;
    if (!g_grid_unsafe.load() && gps && gps->main_viewport &&
            gps->main_viewport->screentexpos_interface) {
        auto vp = gps->main_viewport;
        int n = vp->dim_x * vp->dim_y;
        if (n > 0 && n < 4000000) {
            saved_iface.resize(n);
            if (!seh_copy_ints(saved_iface.data(), vp->screentexpos_interface, n)) {
                g_grid_unsafe.store(true);
                saved_iface.clear();
                diagnostics_log("DIAG: designation grid save faulted (caught); grid disabled.");
            } else {
                iface_saved = true;
                if (camera.placement_mode != 0) {
                    if (!draw_designation_grid_seh(vp, camera)) {
                        g_grid_unsafe.store(true);
                        diagnostics_log("DIAG: designation grid write faulted (caught); grid disabled.");
                    }
                } else if (!seh_fill_ints(vp->screentexpos_interface, n, 0)) {
                    g_grid_unsafe.store(true);
                    diagnostics_log("DIAG: designation grid clear faulted (caught); grid disabled.");
                }
            }
        }
    }
#endif

    bool ok = capture_current_frame(frame, include_ui, err);

#ifdef _WIN32
    if (iface_saved && gps && gps->main_viewport && gps->main_viewport->screentexpos_interface &&
            static_cast<int>(saved_iface.size()) == gps->main_viewport->dim_x * gps->main_viewport->dim_y) {
        seh_copy_ints(gps->main_viewport->screentexpos_interface,
                      saved_iface.data(), static_cast<int>(saved_iface.size()));
    }
    if (zoom_guard.active())
        zoom_guard.restore();
#endif

    *df::global::window_x = saved.x;
    *df::global::window_y = saved.y;
    *df::global::window_z = saved.z;
    if (gps)
        gps->force_full_display_count = 1;

    if (restore_host_buffers) {
#ifdef _WIN32
        bool full_restore_ok = false;
        if (needs_full_host_restore) {
            std::string vs_restore_err;
            full_restore_ok = render_viewscreen_without_overlay(&vs_restore_err);
            if (!full_restore_ok && !g_warned_host_viewscreen_restore.exchange(true)) {
                diagnostics_log("DIAG: host viewscreen restore failed after private capture: " +
                                vs_restore_err);
            }
        }
        if (!full_restore_ok) {
            std::string restore_err;
            if (!render_map_for_current_window(&restore_err) &&
                    !g_warned_host_buffer_restore.exchange(true)) {
                diagnostics_log("DIAG: host map-buffer restore failed after private capture: " +
                                restore_err);
            }
        }
#else
        std::string restore_err;
        if (!render_current_viewscreen(&restore_err) && !g_warned_restore.exchange(true)) {
            diagnostics_log("DIAG: host viewscreen restore failed after private capture: " +
                            restore_err);
        }
#endif
        if (gps)
            gps->force_full_display_count = 1;
    }

    return ok;
}

std::atomic<bool> g_warned_seedown_host_restore(false);
std::atomic<bool> g_warned_seedown_viewscreen_restore(false);

void restore_host_buffers_after_aux_capture(const char* reason) {
    auto gps = df::global::gps;
    if (gps)
        gps->force_full_display_count = 1;

#ifdef _WIN32
    std::string restore_err;
    if (!render_map_for_current_window(&restore_err) &&
            !g_warned_seedown_host_restore.exchange(true)) {
        diagnostics_log(std::string("DIAG: host map-buffer restore failed after ") + reason +
                        " auxiliary capture: " + restore_err);
    }

    std::string vs_restore_err;
    if (!render_viewscreen_without_overlay(&vs_restore_err) &&
            !g_warned_seedown_viewscreen_restore.exchange(true)) {
        diagnostics_log(std::string("DIAG: host viewscreen restore failed after ") + reason +
                        " auxiliary capture: " + vs_restore_err);
    }
#else
    (void)reason;
#endif

    if (gps)
        gps->force_full_display_count = 1;
}

bool composite_seedown_into(const Camera& camera, CapturedFrame& top, bool include_ui) {
    if (top.bgra.size() < 4)
        return false;

    auto is_black = [&](size_t i) {
        return top.bgra[i] < 16 && top.bgra[i + 1] < 16 && top.bgra[i + 2] < 16;
    };

    size_t total_px = top.bgra.size() / 4;
    size_t black = 0;
    for (size_t i = 0; i < top.bgra.size(); i += 4)
        if (is_black(i))
            ++black;
    if (black < total_px / 50)
        return false;

    bool captured_aux_level = false;
    for (int depth = 1; depth <= 5 && camera.z - depth >= 0; ++depth) {
        Camera below = camera;
        below.z = camera.z - depth;
        CapturedFrame lower;
        std::string lower_err;
        if (!capture_shifted(below, lower, false, &lower_err, false))
            break;
        if (lower.bgra.size() != top.bgra.size())
            break;
        captured_aux_level = true;

        float bright = 0.74f - 0.14f * (depth - 1);
        if (bright < 0.20f)
            bright = 0.20f;

        size_t remaining = 0;
        for (size_t i = 0; i < top.bgra.size(); i += 4) {
            if (!is_black(i))
                continue;
            top.bgra[i] = static_cast<uint8_t>(lower.bgra[i] * bright);
            top.bgra[i + 1] = static_cast<uint8_t>(lower.bgra[i + 1] * bright * 0.90f);
            top.bgra[i + 2] = static_cast<uint8_t>(lower.bgra[i + 2] * bright * 0.74f);
            if (is_black(i))
                ++remaining;
        }
        if (remaining < total_px / 200)
            break;
    }
    return captured_aux_level;
}

bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err) {
    auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() {
        auto elapsed = std::chrono::steady_clock::now() - started;
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    };
    diagnostics_capture_attempt(camera);

    std::string local_err;
    bool ok = capture_shifted(camera, frame, true, &local_err);
    if (ok && composite_seedown_into(camera, frame, true))
        restore_host_buffers_after_aux_capture("see-down");

    if (!ok && err)
        *err = local_err;
    if (ok) {
        diagnostics_capture_success(camera, frame.width, frame.height,
                                    static_cast<uint64_t>(frame.bgra.size()), elapsed_ms());
    } else {
        diagnostics_capture_failure(camera, local_err.empty() ? "capture failed" : local_err, elapsed_ms());
    }
    return ok;
}

bool capture_camera_jpeg(const Camera& camera, std::vector<uint8_t>& jpeg, std::string* err) {
    CapturedFrame frame;
    if (!capture_camera_frame_on_render_thread(camera, frame, err))
        return false;
    return encode_jpeg(frame, jpeg, DEFAULT_JPEG_QUALITY, err);
}

} // namespace dfcapture
