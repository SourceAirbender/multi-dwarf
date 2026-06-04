#include "unit_portrait.h"

#include "diagnostics.h"
#include "sdl_capture.h"
#include "modules/DFSDL.h"
#include "modules/Gui.h"

#include "df/enabler.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/main_interface.h"
#include "df/renderer.h"
#include "df/unit.h"
#include "df/unit_flags4.h"
#include "df/view_sheet_type.h"
#include "df/view_sheets_context_type.h"
#include "df/view_sheets_interfacest.h"
#include "df/viewscreen.h"
#include "df/widget_unit_portrait.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace dfcapture_public {
namespace {

constexpr uint32_t SDL_PIXELFORMAT_ARGB8888 = 0x16362004u;
constexpr int SDL_TEXTUREACCESS_TARGET = 2;

struct SDLSurfaceLite {
    uint32_t flags;
    void* format;
    int w;
    int h;
    int pitch;
    void* pixels;
    void* userdata;
    int locked;
    void* list_blitmap;
    struct { int x, y, w, h; } clip_rect;
    void* map;
    int refcount;
};

using pfn_CreateTexture = void* (*)(void*, uint32_t, int, int, int);
using pfn_SetRenderTarget = int (*)(void*, void*);
using pfn_RenderReadPixels = int (*)(void*, const void*, uint32_t, void*, int);
using pfn_DestroyTexture = void (*)(void*);
using pfn_GetRendererOutputSize = int (*)(void*, int*, int*);
using pfn_SetRenderDrawColor = int (*)(void*, uint8_t, uint8_t, uint8_t, uint8_t);
using pfn_RenderClear = int (*)(void*);
using pfn_ConvertSurfaceFormat = void* (*)(void*, uint32_t, uint32_t);
using pfn_LockSurface = int (*)(void*);
using pfn_UnlockSurface = void (*)(void*);
using pfn_FreeSurface = void (*)(void*);

pfn_CreateTexture p_CreateTexture = nullptr;
pfn_SetRenderTarget p_SetRenderTarget = nullptr;
pfn_RenderReadPixels p_RenderReadPixels = nullptr;
pfn_DestroyTexture p_DestroyTexture = nullptr;
pfn_GetRendererOutputSize p_GetRendererOutputSize = nullptr;
pfn_SetRenderDrawColor p_SetRenderDrawColor = nullptr;
pfn_RenderClear p_RenderClear = nullptr;
pfn_ConvertSurfaceFormat p_ConvertSurfaceFormat = nullptr;
pfn_LockSurface p_LockSurface = nullptr;
pfn_UnlockSurface p_UnlockSurface = nullptr;
pfn_FreeSurface p_FreeSurface = nullptr;

std::atomic<bool> g_warned_portrait_diag(false);
std::atomic<bool> g_warned_portrait_widget_success(false);
std::atomic<bool> g_warned_portrait_widget_fail(false);
std::atomic<bool> g_warned_portrait_view_sheet_disabled(false);
std::atomic<int> g_portrait_view_sheet_fault_count(0);
std::atomic<bool> g_portrait_view_sheet_busy(false);
std::mutex g_portrait_view_sheet_disabled_mutex;
std::unordered_set<int32_t> g_portrait_view_sheet_disabled_units;

#ifdef _WIN32
volatile uint32_t g_seh_code = 0;
void* g_seh_at = nullptr;
void* g_seh_access = nullptr;

int seh_filter(_EXCEPTION_POINTERS* ep) {
    g_seh_code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    g_seh_at = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    g_seh_access = (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2)
        ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1])
        : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

constexpr DWORD DFCAPTURE_INVALID_PARAMETER_EXCEPTION = 0xE0424643u;

void __cdecl invalid_parameter_handler(const wchar_t*, const wchar_t*,
                                       const wchar_t*, unsigned int, uintptr_t) {
    RaiseException(DFCAPTURE_INVALID_PARAMETER_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

int call_widget_seh(df::widget_unit_portrait* widget) {
    int stage = 0;
    int result = 0;
    _invalid_parameter_handler old_handler =
        _set_thread_local_invalid_parameter_handler(invalid_parameter_handler);
    __try {
        stage = 1; widget->arrange();
        stage = 2; widget->logic();
        stage = 3; widget->render(0);
    } __except(seh_filter(GetExceptionInformation())) {
        result = stage;
    }
    _set_thread_local_invalid_parameter_handler(old_handler);
    return result;
}

int call_viewscreen_logic_seh(df::viewscreen* viewscreen) {
    int result = 0;
    __try {
        viewscreen->logic();
    } __except(seh_filter(GetExceptionInformation())) {
        result = 1;
    }
    return result;
}

int call_viewscreen_render_seh(df::viewscreen* viewscreen) {
    int result = 0;
    _invalid_parameter_handler old_handler =
        _set_thread_local_invalid_parameter_handler(invalid_parameter_handler);
    __try {
        viewscreen->render(0);
    } __except(seh_filter(GetExceptionInformation())) {
        result = 1;
    }
    _set_thread_local_invalid_parameter_handler(old_handler);
    return result;
}
#endif

bool resolve_sdl(std::string* err = nullptr) {
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
    p_ConvertSurfaceFormat = reinterpret_cast<pfn_ConvertSurfaceFormat>(GetProcAddress(sdl, "SDL_ConvertSurfaceFormat"));
    p_LockSurface = reinterpret_cast<pfn_LockSurface>(GetProcAddress(sdl, "SDL_LockSurface"));
    p_UnlockSurface = reinterpret_cast<pfn_UnlockSurface>(GetProcAddress(sdl, "SDL_UnlockSurface"));
    p_FreeSurface = reinterpret_cast<pfn_FreeSurface>(GetProcAddress(sdl, "SDL_FreeSurface"));

    if (p_CreateTexture && p_SetRenderTarget && p_RenderReadPixels &&
        p_DestroyTexture && p_GetRendererOutputSize && p_SetRenderDrawColor &&
        p_RenderClear && p_ConvertSurfaceFormat && p_LockSurface &&
        p_UnlockSurface && p_FreeSurface) {
        return true;
    }

    if (err) *err = "could not resolve SDL2 portrait surface/render-target functions";
    return false;
#else
    if (err) *err = "native portrait rendering is Windows-only";
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
        auto renderer = enabler ? enabler->renderer : nullptr;
        if (!renderer) {
            if (err) *err = "portrait target: no renderer";
            return false;
        }

        sdl_ = renderer->get_renderer();
        if (!sdl_) {
            if (err) *err = "portrait target: get_renderer returned null";
            return false;
        }

        int w = 0;
        int h = 0;
        p_GetRendererOutputSize(sdl_, &w, &h);
        if (requested_w > 0) w = requested_w;
        if (requested_h > 0) h = requested_h;
        if (w <= 0 || h <= 0) {
            if (err) *err = "portrait target: bad renderer output size";
            return false;
        }

        target_ = p_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!target_) {
            if (err) *err = "portrait target: SDL_CreateTexture failed";
            return false;
        }
        if (p_SetRenderTarget(sdl_, target_) != 0) {
            p_DestroyTexture(target_);
            target_ = nullptr;
            if (err) *err = "portrait target: SDL_SetRenderTarget failed";
            return false;
        }

        w_ = w;
        h_ = h;
        active_ = true;
        return true;
    }

    bool clear(std::string* err = nullptr) {
        if (!active_ || !sdl_) {
            if (err) *err = "portrait target: target inactive";
            return false;
        }
        if (p_SetRenderDrawColor(sdl_, 0, 0, 0, 0) != 0 || p_RenderClear(sdl_) != 0) {
            if (err) *err = "portrait target: SDL_RenderClear failed";
            return false;
        }
        return true;
    }

    bool read_frame(CapturedFrame& frame, std::string* err = nullptr) {
        if (!active_ || !sdl_ || w_ <= 0 || h_ <= 0) {
            if (err) *err = "portrait target: target inactive";
            return false;
        }
        CapturedFrame next;
        next.width = w_;
        next.height = h_;
        next.bgra.resize(static_cast<size_t>(w_) * h_ * 4);
        int rc = p_RenderReadPixels(sdl_, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                    next.bgra.data(), w_ * 4);
        if (rc != 0) {
            if (err) *err = "portrait target: SDL_RenderReadPixels failed";
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
#endif

bool copy_sdl_surface_to_frame(void* surface_ptr, CapturedFrame& frame, std::string* err = nullptr) {
    if (!surface_ptr) {
        if (err) *err = "portrait surface unavailable";
        return false;
    }
    if (!resolve_sdl(err))
        return false;

    void* converted = p_ConvertSurfaceFormat(surface_ptr, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!converted) {
        if (err) *err = "SDL_ConvertSurfaceFormat failed";
        return false;
    }

    auto* surface = reinterpret_cast<SDLSurfaceLite*>(converted);
    if (surface->w <= 0 || surface->h <= 0 || surface->pitch < surface->w * 4 || !surface->pixels) {
        p_FreeSurface(converted);
        if (err) *err = "portrait surface has invalid dimensions";
        return false;
    }

    if (p_LockSurface(converted) != 0) {
        p_FreeSurface(converted);
        if (err) *err = "SDL_LockSurface failed";
        return false;
    }

    CapturedFrame next;
    next.width = surface->w;
    next.height = surface->h;
    next.bgra.resize(static_cast<size_t>(next.width) * next.height * 4);
    const auto* src = reinterpret_cast<const uint8_t*>(surface->pixels);
    for (int y = 0; y < next.height; ++y) {
        std::memcpy(next.bgra.data() + static_cast<size_t>(y) * next.width * 4,
                    src + static_cast<size_t>(y) * surface->pitch,
                    static_cast<size_t>(next.width) * 4);
    }

    p_UnlockSurface(converted);
    p_FreeSurface(converted);
    frame = std::move(next);
    return true;
}

bool frame_has_visible_pixels(const CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.bgra.empty())
        return false;
    size_t visible = 0;
    size_t sampled = 0;
    for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4) {
        uint8_t b = frame.bgra[i + 0];
        uint8_t g = frame.bgra[i + 1];
        uint8_t r = frame.bgra[i + 2];
        uint8_t a = frame.bgra[i + 3];
        if (a > 8 && (static_cast<int>(r) + g + b) > 24)
            ++visible;
        ++sampled;
    }
    return sampled > 0 && visible >= std::max<size_t>(8, sampled / 200);
}

bool crop_visible_bounds(const CapturedFrame& src, CapturedFrame& dst) {
    if (src.width <= 0 || src.height <= 0 || src.bgra.empty())
        return false;

    int min_x = src.width;
    int min_y = src.height;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            size_t i = (static_cast<size_t>(y) * src.width + x) * 4;
            uint8_t b = src.bgra[i + 0];
            uint8_t g = src.bgra[i + 1];
            uint8_t r = src.bgra[i + 2];
            uint8_t a = src.bgra[i + 3];
            if (a > 8 && (static_cast<int>(r) + g + b) > 24) {
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
    }
    if (max_x < min_x || max_y < min_y)
        return false;

    int pad = 2;
    min_x = std::max(0, min_x - pad);
    min_y = std::max(0, min_y - pad);
    max_x = std::min(src.width - 1, max_x + pad);
    max_y = std::min(src.height - 1, max_y + pad);

    CapturedFrame cropped;
    cropped.width = max_x - min_x + 1;
    cropped.height = max_y - min_y + 1;
    cropped.bgra.resize(static_cast<size_t>(cropped.width) * cropped.height * 4);
    for (int y = 0; y < cropped.height; ++y) {
        const uint8_t* row = src.bgra.data() +
            (static_cast<size_t>(min_y + y) * src.width + min_x) * 4;
        std::memcpy(cropped.bgra.data() + static_cast<size_t>(y) * cropped.width * 4,
                    row, static_cast<size_t>(cropped.width) * 4);
    }
    dst = std::move(cropped);
    return true;
}

bool copy_texture_to_frame(df::enabler* enabler, int32_t texpos, const std::string& label,
                           CapturedFrame& frame, std::string& last_err) {
    if (!enabler) {
        last_err = "enabler unavailable";
        return false;
    }
    if (texpos < 0)
        return false;
    if (static_cast<size_t>(texpos) >= enabler->textures.raws.size()) {
        last_err = label + " texture out of range";
        return false;
    }

    CapturedFrame candidate;
    std::string copy_err;
    if (!copy_sdl_surface_to_frame(enabler->textures.raws[texpos], candidate, &copy_err)) {
        last_err = label + ": " + copy_err;
        return false;
    }
    if (!frame_has_visible_pixels(candidate)) {
        last_err = label + " surface is blank";
        return false;
    }
    frame = std::move(candidate);
    return true;
}

bool copy_unit_portrait_candidate(df::unit* unit, df::enabler* enabler,
                                  CapturedFrame& frame, int32_t& texpos,
                                  std::string& source, std::string& last_err,
                                  bool allow_icon_fallbacks = false) {
    if (!unit)
        return false;

    std::vector<std::pair<int32_t, std::string>> candidates;
    candidates.emplace_back(unit->portrait_texpos, "portrait");
    if (allow_icon_fallbacks) {
        candidates.emplace_back(unit->sheet_icon_texpos, "sheet-icon");
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 2; ++j) {
                if (unit->texpos_currently_in_use[i][j])
                    candidates.emplace_back(unit->texpos[i][j],
                                            "sprite-inuse-" + std::to_string(i) + std::to_string(j));
            }
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 2; ++j)
                candidates.emplace_back(unit->texpos[i][j],
                                        "sprite-" + std::to_string(i) + std::to_string(j));
        }
    }

    for (auto& candidate : candidates) {
        CapturedFrame candidate_frame;
        if (!copy_texture_to_frame(enabler, candidate.first, candidate.second,
                                   candidate_frame, last_err))
            continue;
        if (allow_icon_fallbacks) {
            CapturedFrame cropped;
            if (crop_visible_bounds(candidate_frame, cropped) && frame_has_visible_pixels(cropped))
                candidate_frame = std::move(cropped);
        }
        texpos = candidate.first;
        source = candidate.second;
        frame = std::move(candidate_frame);
        return true;
    }
    return false;
}

bool portrait_error_is_native_fault(const std::string& err) {
    return err.find("fault") != std::string::npos ||
           err.find("0xc0000005") != std::string::npos ||
           err.find("access=0x") != std::string::npos;
}

bool portrait_view_sheet_allowed_for_unit(int32_t unit_id) {
    if (g_portrait_view_sheet_fault_count.load() >= 8)
        return false;
    std::lock_guard<std::mutex> lock(g_portrait_view_sheet_disabled_mutex);
    return g_portrait_view_sheet_disabled_units.find(unit_id) ==
           g_portrait_view_sheet_disabled_units.end();
}

int portrait_disable_view_sheet_for_unit(int32_t unit_id) {
    {
        std::lock_guard<std::mutex> lock(g_portrait_view_sheet_disabled_mutex);
        g_portrait_view_sheet_disabled_units.insert(unit_id);
    }
    return ++g_portrait_view_sheet_fault_count;
}

bool render_viewscreen_isolated(std::string* err = nullptr, int target_w = 0, int target_h = 0) {
#ifdef _WIN32
    auto viewscreen = DFHack::Gui::getCurViewscreen(true);
    if (!viewscreen) {
        if (err) *err = "no current viewscreen";
        return false;
    }
    TemporaryRenderTarget target;
    std::string target_err;
    if (!target.begin(&target_err, target_w, target_h)) {
        if (err) *err = target_err;
        return false;
    }
    target.clear(nullptr);
    int fault = call_viewscreen_render_seh(viewscreen);
    if (fault != 0) {
        if (err) {
            std::ostringstream ss;
            ss << "isolated viewscreen render fault code=0x" << std::hex << g_seh_code
               << " at=" << g_seh_at
               << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
            *err = ss.str();
        }
        return false;
    }
    return true;
#else
    (void)target_w; (void)target_h;
    if (err) *err = "isolated viewscreen rendering is Windows-only";
    return false;
#endif
}

bool generate_unit_portrait_with_widget(df::unit* unit, df::enabler* enabler,
                                        CapturedFrame& frame, int32_t& texpos,
                                        std::string& source, std::string& last_err,
                                        bool allow_icon_fallbacks,
                                        bool allow_readback_fallback) {
#ifdef _WIN32
    if (!unit) {
        last_err = "unit not found";
        return false;
    }
    if (!enabler) {
        last_err = "enabler unavailable";
        return false;
    }

    std::unique_ptr<df::widget_unit_portrait> widget(df::allocate<df::widget_unit_portrait>());
    if (!widget) {
        last_err = "could not allocate native unit portrait widget";
        return false;
    }

    bool saved_refresh = unit->flags4.bits.portrait_must_be_refreshed;
    unit->flags4.bits.portrait_must_be_refreshed = true;
    auto restore_refresh = [&]() {
        unit->flags4.bits.portrait_must_be_refreshed = saved_refresh;
    };

    constexpr int PORTRAIT_TARGET = 128;
    widget->u = unit;
    widget->rect.x1 = 0;
    widget->rect.y1 = 0;
    widget->rect.x2 = PORTRAIT_TARGET - 1;
    widget->rect.y2 = PORTRAIT_TARGET - 1;
    widget->min_w = PORTRAIT_TARGET;
    widget->min_h = PORTRAIT_TARGET;

    TemporaryRenderTarget target;
    std::string target_err;
    if (!target.begin(&target_err, PORTRAIT_TARGET, PORTRAIT_TARGET) ||
        !target.clear(&target_err)) {
        restore_refresh();
        last_err = target_err;
        return false;
    }

    int fault = call_widget_seh(widget.get());
    if (fault != 0) {
        restore_refresh();
        std::ostringstream ss;
        ss << "native unit portrait widget fault at stage " << fault
           << " code=0x" << std::hex << g_seh_code
           << " at=" << g_seh_at
           << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
        last_err = ss.str();
        if (!g_warned_portrait_widget_fail.exchange(true))
            diagnostics_log("DIAG portrait widget failed: " + last_err);
        return false;
    }

    CapturedFrame generated;
    int32_t generated_texpos = -1;
    std::string generated_source;
    if (copy_unit_portrait_candidate(unit, enabler, generated, generated_texpos,
                                     generated_source, last_err, allow_icon_fallbacks)) {
        restore_refresh();
        frame = std::move(generated);
        texpos = generated_texpos;
        source = "widget-" + generated_source;
        if (!g_warned_portrait_widget_success.exchange(true))
            diagnostics_log("DIAG portrait widget generated native texture source=" + source +
                            " texpos=" + std::to_string(texpos));
        return true;
    }

    if (allow_readback_fallback) {
        CapturedFrame target_frame;
        if (target.read_frame(target_frame, &target_err) &&
            crop_visible_bounds(target_frame, generated) &&
            frame_has_visible_pixels(generated)) {
            restore_refresh();
            frame = std::move(generated);
            texpos = unit->portrait_texpos;
            source = "widget-readback";
            if (!g_warned_portrait_widget_success.exchange(true))
                diagnostics_log("DIAG portrait widget generated offscreen readback " +
                                std::to_string(frame.width) + "x" + std::to_string(frame.height));
            return true;
        }
    }

    restore_refresh();
    if (!target_err.empty())
        last_err = target_err;
    if (last_err.empty())
        last_err = "native unit portrait widget produced no visible pixels";
    if (!g_warned_portrait_widget_fail.exchange(true))
        diagnostics_log("DIAG portrait widget failed: " + last_err);
    return false;
#else
    (void)unit; (void)enabler; (void)frame; (void)texpos; (void)source;
    (void)allow_icon_fallbacks; (void)allow_readback_fallback;
    last_err = "native portrait widget rendering is Windows-only";
    return false;
#endif
}

bool generate_unit_portrait_with_view_sheet(df::unit* unit, df::enabler* enabler,
                                            CapturedFrame& frame, int32_t& texpos,
                                            std::string& source, std::string& last_err) {
#ifdef _WIN32
    if (!unit) {
        last_err = "unit not found";
        return false;
    }
    auto game = df::global::game;
    auto viewscreen = DFHack::Gui::getCurViewscreen(true);
    if (!game || !viewscreen) {
        last_err = "game/viewscreen unavailable";
        return false;
    }

    auto& sheets = game->main_interface.view_sheets;
    if (sheets.open) {
        last_err = "host view sheet is open; skipping native unit-sheet portrait generation";
        return false;
    }
    bool expected = false;
    if (!g_portrait_view_sheet_busy.compare_exchange_strong(expected, true)) {
        last_err = "native unit-sheet portrait generation is already running";
        return false;
    }

    df::view_sheets_interfacest saved = sheets;
    bool saved_refresh = unit->flags4.bits.portrait_must_be_refreshed;
    bool restored = false;
    auto restore_sheets = [&]() {
        if (!restored) {
            unit->flags4.bits.portrait_must_be_refreshed = saved_refresh;
            sheets = saved;
            g_portrait_view_sheet_busy.store(false);
            restored = true;
        }
    };

    unit->flags4.bits.portrait_must_be_refreshed = true;
    sheets.open = true;
    sheets.context = df::view_sheets_context_type::REGULAR_PLAY;
    sheets.active_sheet = df::view_sheet_type::UNIT;
    sheets.active_id = unit->id;
    sheets.viewing_unid.clear();
    sheets.viewing_unid.push_back(unit->id);
    sheets.viewing_itid.clear();
    sheets.viewing_bldid = -1;
    sheets.viewing_vermin_combined_id.clear();
    sheets.viewing_x = unit->pos.x;
    sheets.viewing_y = unit->pos.y;
    sheets.viewing_z = unit->pos.z;
    sheets.scroll_position = 0;
    sheets.scrolling = false;
    sheets.active_sub_tab = 0;
    sheets.last_tick_update = 0;

    for (int attempt = 1; attempt <= 3; ++attempt) {
        unit->flags4.bits.portrait_must_be_refreshed = true;
        sheets.last_tick_update = 0;

        int logic_fault = call_viewscreen_logic_seh(viewscreen);
        if (logic_fault != 0) {
            std::ostringstream ss;
            ss << "native view-sheet logic fault code=0x" << std::hex << g_seh_code
               << " at=" << g_seh_at
               << " access=0x" << reinterpret_cast<uintptr_t>(g_seh_access);
            last_err = ss.str();
            restore_sheets();
            if (!g_warned_portrait_widget_fail.exchange(true))
                diagnostics_log("DIAG portrait view-sheet failed: " + last_err);
            return false;
        }

        std::string render_err;
        if (!render_viewscreen_isolated(&render_err)) {
            last_err = "native view-sheet render failed: " + render_err;
            restore_sheets();
            if (!g_warned_portrait_widget_fail.exchange(true))
                diagnostics_log("DIAG portrait view-sheet failed: " + last_err);
            return false;
        }

        CapturedFrame generated;
        int32_t generated_texpos = -1;
        std::string generated_source;
        if (copy_unit_portrait_candidate(unit, enabler, generated,
                                         generated_texpos, generated_source,
                                         last_err, false)) {
            restore_sheets();
            frame = std::move(generated);
            texpos = generated_texpos;
            source = "view-sheet-" + generated_source;
            if (!g_warned_portrait_widget_success.exchange(true))
                diagnostics_log("DIAG portrait view-sheet generated native texture source=" +
                                source + " texpos=" + std::to_string(texpos) +
                                " attempt=" + std::to_string(attempt));
            return true;
        }
    }
    restore_sheets();

    if (last_err.empty())
        last_err = "native unit view-sheet did not populate portrait_texpos";
    if (!g_warned_portrait_widget_fail.exchange(true))
        diagnostics_log("DIAG portrait view-sheet failed: " + last_err +
                        " portrait_texpos=" + std::to_string(unit->portrait_texpos) +
                        " refresh=" +
                        std::to_string(unit->flags4.bits.portrait_must_be_refreshed ? 1 : 0));
    return false;
#else
    (void)unit; (void)enabler; (void)frame; (void)texpos; (void)source;
    last_err = "native portrait view-sheet rendering is Windows-only";
    return false;
#endif
}

struct RenderThreadPortraitRequest {
    int32_t unit_id = -1;
    bool allow_icon_fallbacks = false;
    bool allow_view_sheet_generation = false;
    int32_t texpos = -1;
    std::string source;
    CapturedFrame frame;
    std::string err;
    std::promise<bool> done;
};

} // namespace

bool unit_portrait_on_render_thread(int32_t unit_id,
                                    bool allow_icon_fallbacks,
                                    bool allow_view_sheet_generation,
                                    CapturedFrame& frame,
                                    int32_t& texpos,
                                    std::string& source,
                                    std::string* err) {
    std::lock_guard<std::recursive_mutex> render_lock(capture_state_mutex());
    auto request = std::make_shared<RenderThreadPortraitRequest>();
    request->unit_id = unit_id;
    request->allow_icon_fallbacks = allow_icon_fallbacks;
    request->allow_view_sheet_generation = allow_view_sheet_generation;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        auto unit = df::unit::find(request->unit_id);
        if (!unit) {
            request->err = "unit not found";
            request->done.set_value(false);
            return;
        }
        auto enabler = df::global::enabler;
        if (!enabler) {
            request->err = "enabler unavailable";
            request->done.set_value(false);
            return;
        }

        std::string last_err;
        if (copy_unit_portrait_candidate(unit, enabler, request->frame,
                                         request->texpos, request->source, last_err,
                                         request->allow_icon_fallbacks)) {
            request->done.set_value(true);
            return;
        }

        if (!request->allow_icon_fallbacks && request->allow_view_sheet_generation) {
            if (portrait_view_sheet_allowed_for_unit(request->unit_id)) {
                if (generate_unit_portrait_with_view_sheet(unit, enabler, request->frame,
                                                           request->texpos, request->source,
                                                           last_err)) {
                    request->done.set_value(true);
                    return;
                }
                if (portrait_error_is_native_fault(last_err)) {
                    int faults = portrait_disable_view_sheet_for_unit(request->unit_id);
                    diagnostics_log("DIAG portrait view-sheet generation disabled for unit " +
                                    std::to_string(request->unit_id) +
                                    " after explicit native fault (" +
                                    std::to_string(faults) + "/8): " + last_err);
                    if (faults >= 8 && !g_warned_portrait_view_sheet_disabled.exchange(true))
                        diagnostics_log("DIAG portrait view-sheet generation disabled globally after repeated native faults; "
                                        "using widget/readback and existing texture fallbacks only.");
                }
            } else if (!g_warned_portrait_view_sheet_disabled.exchange(true)) {
                diagnostics_log("DIAG explicit portrait view-sheet generation skipped by fault guard; "
                                "using widget/readback and existing texture fallbacks only.");
            }
        } else if (!request->allow_icon_fallbacks &&
                   !g_warned_portrait_view_sheet_disabled.exchange(true)) {
            diagnostics_log("DIAG portrait view-sheet generation requires explicit generate=1; "
                            "automatic unit-window portrait loads use widget/readback and existing texture fallbacks only.");
        }

        if (generate_unit_portrait_with_widget(unit, enabler, request->frame,
                                               request->texpos, request->source,
                                               last_err, request->allow_icon_fallbacks, true)) {
            request->done.set_value(true);
            return;
        }

        if (!g_warned_portrait_diag.exchange(true)) {
            std::ostringstream ss;
            ss << "DIAG portrait fail unit " << request->unit_id
               << ": portrait_texpos=" << unit->portrait_texpos
               << " sheet_icon=" << unit->sheet_icon_texpos
               << " sprite[0][0]=" << unit->texpos[0][0]
               << " [0][1]=" << unit->texpos[0][1]
               << " [1][0]=" << unit->texpos[1][0]
               << " [2][0]=" << unit->texpos[2][0]
               << " inUse00=" << (unit->texpos_currently_in_use[0][0] ? 1 : 0)
               << " raws.size=" << enabler->textures.raws.size()
               << " lastErr='" << last_err << "'";
            diagnostics_log(ss.str());
        }

        request->err = last_err.empty() ? "unit has no usable native portrait surface" : last_err;
        request->done.set_value(false);
    });

    bool ok = future.get();
    if (ok) {
        frame = std::move(request->frame);
        texpos = request->texpos;
        source = request->source;
    } else if (err) {
        *err = request->err;
    }
    return ok;
}

} // namespace dfcapture_public
