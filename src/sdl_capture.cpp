#include "sdl_capture.h"

#include "diagnostics.h"
#include "image_encoder.h"
#include "modules/DFSDL.h"
#include "modules/Gui.h"

#include "df/enabler.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewport_flag.h"
#include "df/graphic_viewportst.h"
#include "df/renderer.h"
#include "df/viewscreen.h"
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
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace dfcapture_public {
namespace {

constexpr uint32_t SDL_PIXELFORMAT_ARGB8888 = 0x16362004u;
constexpr int SDL_TEXTUREACCESS_TARGET = 2;

using pfn_CreateTexture = void* (*)(void*, uint32_t, int, int, int);
using pfn_SetRenderTarget = int (*)(void*, void*);
using pfn_RenderReadPixels = int (*)(void*, const void*, uint32_t, void*, int);
using pfn_DestroyTexture = void (*)(void*);
using pfn_GetRendererOutputSize = int (*)(void*, int*, int*);

pfn_CreateTexture p_CreateTexture = nullptr;
pfn_SetRenderTarget p_SetRenderTarget = nullptr;
pfn_RenderReadPixels p_RenderReadPixels = nullptr;
pfn_DestroyTexture p_DestroyTexture = nullptr;
pfn_GetRendererOutputSize p_GetRendererOutputSize = nullptr;

std::recursive_mutex g_capture_mutex;
std::atomic<bool> g_warned_restore(false);
std::atomic<bool> g_zoom_unsafe(false);
std::atomic<int> g_ref_zoom_factor(0);
std::atomic<int> g_ref_dim_x(0);
std::atomic<int> g_ref_dim_y(0);

#ifdef _WIN32
volatile uint32_t g_seh_code = 0;
void* g_seh_at = nullptr;

static int dfcapture_public_seh_filter(_EXCEPTION_POINTERS* ep) {
    g_seh_code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    g_seh_at = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int call_viewscreen_render_seh(df::viewscreen* viewscreen) {
    int fault = 0;
    __try {
        viewscreen->render(0);
    } __except(dfcapture_public_seh_filter(GetExceptionInformation())) {
        fault = 1;
    }
    return fault;
}

static bool call_set_viewport_zoom_factor_seh(df::renderer* renderer, int32_t factor) {
    bool ok = false;
    __try {
        renderer->set_viewport_zoom_factor(factor);
        ok = true;
    } __except(dfcapture_public_seh_filter(GetExceptionInformation())) {
        ok = false;
    }
    return ok;
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

    if (p_CreateTexture && p_SetRenderTarget && p_RenderReadPixels &&
        p_DestroyTexture && p_GetRendererOutputSize) {
        return true;
    }

    if (err) *err = "could not resolve required SDL2 render-target functions";
    return false;
#else
    if (err) *err = "SDL capture is currently Windows-only";
    return false;
#endif
}

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

#ifdef _WIN32
    if (call_viewscreen_render_seh(viewscreen) != 0) {
        if (err) {
            std::ostringstream msg;
            msg << "viewscreen render fault code=0x" << std::hex << g_seh_code
                << " at=" << g_seh_at;
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
    auto gps = df::global::gps;
    auto vp = gps ? gps->main_viewport : nullptr;
    if (!gps || !vp || vp->dim_x <= 0 || vp->dim_y <= 0)
        return;
    int expected = gps->viewport_zoom_factor > 0 ? gps->viewport_zoom_factor : 192;
    int zero = 0;
    g_ref_zoom_factor.compare_exchange_strong(zero, expected);
    if (g_ref_dim_x.load() <= 0) g_ref_dim_x.store(vp->dim_x);
    if (g_ref_dim_y.load() <= 0) g_ref_dim_y.store(vp->dim_y);
}

int zoom_percent_for_camera(const Camera& camera) {
    int pct = camera.zoom_factor >= 0 ? camera.zoom_factor : 100;
    return std::max(40, std::min(300, pct));
}

void effective_zoom_dims(const Camera& camera, int host_x, int host_y, int& out_x, int& out_y) {
    int base_x = g_ref_dim_x.load() > 0 ? g_ref_dim_x.load() : host_x;
    int base_y = g_ref_dim_y.load() > 0 ? g_ref_dim_y.load() : host_y;
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
        if (g_zoom_unsafe.load())
            return true;
        capture_zoom_reference_if_needed();
        if (g_ref_dim_x.load() <= 0)
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
    });

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

bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err) {
    auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() {
        auto elapsed = std::chrono::steady_clock::now() - started;
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    };
    diagnostics_capture_attempt(camera);

    if (!capture_ready(err) || !resolve_sdl(err))
    {
        diagnostics_capture_failure(camera, err ? *err : "capture prerequisites failed", elapsed_ms());
        return false;
    }

    auto enabler = df::global::enabler;
    df::renderer* renderer = enabler ? enabler->renderer : nullptr;
    void* sdl_renderer = renderer ? renderer->get_renderer() : nullptr;
    if (!sdl_renderer) {
        if (err) *err = "renderer->get_renderer returned null";
        diagnostics_capture_failure(camera, err ? *err : "renderer unavailable", elapsed_ms());
        return false;
    }

    int width = 0;
    int height = 0;
    p_GetRendererOutputSize(sdl_renderer, &width, &height);
    if (width <= 0 || height <= 0) {
        if (err) *err = "bad renderer output size";
        diagnostics_capture_failure(camera, err ? *err : "bad renderer output size", elapsed_ms());
        return false;
    }

    void* target = p_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_TARGET, width, height);
    if (!target) {
        if (err) *err = "SDL_CreateTexture(target) failed";
        diagnostics_capture_failure(camera, err ? *err : "SDL_CreateTexture failed", elapsed_ms());
        return false;
    }

    if (p_SetRenderTarget(sdl_renderer, target) != 0) {
        p_DestroyTexture(target);
        if (err) *err = "SDL_SetRenderTarget(target) failed";
        diagnostics_capture_failure(camera, err ? *err : "SDL_SetRenderTarget failed", elapsed_ms());
        return false;
    }

    Camera saved;
    saved.x = df::global::window_x ? *df::global::window_x : 0;
    saved.y = df::global::window_y ? *df::global::window_y : 0;
    saved.z = df::global::window_z ? *df::global::window_z : 0;

    bool ok = false;
    std::string local_err;

    if (df::global::window_x && df::global::window_y && df::global::window_z) {
        *df::global::window_x = camera.x;
        *df::global::window_y = camera.y;
        *df::global::window_z = camera.z;
        if (df::global::gps)
            df::global::gps->force_full_display_count = 1;

#ifdef _WIN32
        ViewportZoomGuard zoom_guard;
        std::string zoom_err;
        if (!zoom_guard.activate(camera, &zoom_err)) {
            diagnostics_log("zoom disabled for capture: " + zoom_err);
        }
#endif

        ok = render_current_viewscreen(&local_err);
        if (ok) {
            CapturedFrame next;
            next.width = width;
            next.height = height;
            next.bgra.resize(static_cast<size_t>(width) * height * 4);
            int rc = p_RenderReadPixels(sdl_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                        next.bgra.data(), width * 4);
            if (rc == 0) {
                draw_placement_overlay(camera, next);
                frame = std::move(next);
            } else {
                ok = false;
                local_err = "SDL_RenderReadPixels failed";
            }
        }

#ifdef _WIN32
        zoom_guard.restore();
#endif

        *df::global::window_x = saved.x;
        *df::global::window_y = saved.y;
        *df::global::window_z = saved.z;
        if (df::global::gps)
            df::global::gps->force_full_display_count = 1;

        std::string restore_err;
        if (!render_current_viewscreen(&restore_err) && !g_warned_restore.exchange(true)) {
            local_err += local_err.empty() ? "" : "; ";
            local_err += "host restore render failed: " + restore_err;
        }
    } else {
        local_err = "DF window coordinates are unavailable";
    }

    p_SetRenderTarget(sdl_renderer, nullptr);
    p_DestroyTexture(target);

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

} // namespace dfcapture_public
