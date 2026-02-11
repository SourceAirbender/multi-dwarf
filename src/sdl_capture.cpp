#include "sdl_capture.h"

#include "image_encoder.h"
#include "modules/DFSDL.h"
#include "modules/Gui.h"

#include "df/enabler.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/graphic.h"
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
#include <future>
#include <memory>
#include <mutex>
#include <sstream>

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

bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err) {
    if (!capture_ready(err) || !resolve_sdl(err))
        return false;

    auto enabler = df::global::enabler;
    df::renderer* renderer = enabler ? enabler->renderer : nullptr;
    void* sdl_renderer = renderer ? renderer->get_renderer() : nullptr;
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
        if (err) *err = "SDL_SetRenderTarget(target) failed";
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

        ok = render_current_viewscreen(&local_err);
        if (ok) {
            CapturedFrame next;
            next.width = width;
            next.height = height;
            next.bgra.resize(static_cast<size_t>(width) * height * 4);
            int rc = p_RenderReadPixels(sdl_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                        next.bgra.data(), width * 4);
            if (rc == 0) {
                frame = std::move(next);
            } else {
                ok = false;
                local_err = "SDL_RenderReadPixels failed";
            }
        }

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
    return ok;
}

bool capture_camera_jpeg(const Camera& camera, std::vector<uint8_t>& jpeg, std::string* err) {
    CapturedFrame frame;
    if (!capture_camera_frame_on_render_thread(camera, frame, err))
        return false;
    return encode_jpeg(frame, jpeg, DEFAULT_JPEG_QUALITY, err);
}

} // namespace dfcapture_public
