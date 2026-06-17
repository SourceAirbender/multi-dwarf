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

#include "image_encoder.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#endif

#include <algorithm>
#include <fstream>
#include <mutex>

namespace dfcapture {
namespace {

#ifdef _WIN32
std::mutex g_gdiplus_mutex;
ULONG_PTR g_gdiplus_token = 0;

bool ensure_gdiplus(std::string* err) {
    std::lock_guard<std::mutex> lock(g_gdiplus_mutex);
    if (g_gdiplus_token)
        return true;

    Gdiplus::GdiplusStartupInput input;
    Gdiplus::Status st = Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
    if (st == Gdiplus::Ok)
        return true;

    if (err) *err = "GdiplusStartup failed: " + std::to_string(static_cast<int>(st));
    g_gdiplus_token = 0;
    return false;
}

bool get_encoder_clsid(const WCHAR* mime, CLSID& clsid) {
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
        return false;

    std::vector<uint8_t> storage(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok)
        return false;

    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mime) == 0) {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

bool save_bitmap_to_memory(const CapturedFrame& frame, const CLSID& encoder,
                           const Gdiplus::EncoderParameters* params,
                           std::vector<uint8_t>& bytes, std::string* err) {
    Gdiplus::Bitmap bitmap(frame.width, frame.height, frame.width * 4,
                           PixelFormat32bppARGB,
                           const_cast<BYTE*>(frame.bgra.data()));

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        if (err) *err = "CreateStreamOnHGlobal failed";
        return false;
    }

    Gdiplus::Status st = bitmap.Save(stream, &encoder, const_cast<Gdiplus::EncoderParameters*>(params));
    if (st != Gdiplus::Ok) {
        stream->Release();
        if (err) *err = "Bitmap::Save failed: " + std::to_string(static_cast<int>(st));
        return false;
    }

    STATSTG stat = {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        stream->Release();
        if (err) *err = "IStream::Stat failed";
        return false;
    }

    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream, &global)) || !global) {
        stream->Release();
        if (err) *err = "GetHGlobalFromStream failed";
        return false;
    }

    SIZE_T size = static_cast<SIZE_T>(stat.cbSize.QuadPart);
    void* data = GlobalLock(global);
    if (!data) {
        stream->Release();
        if (err) *err = "GlobalLock failed";
        return false;
    }

    const auto* begin = reinterpret_cast<const uint8_t*>(data);
    bytes.assign(begin, begin + size);
    GlobalUnlock(global);
    stream->Release();
    return true;
}
#endif

bool validate_frame(const CapturedFrame& frame, std::string* err) {
    if (frame.width <= 0 || frame.height <= 0 || frame.bgra.empty()) {
        if (err) *err = "empty frame";
        return false;
    }

    const size_t expected = static_cast<size_t>(frame.width) * frame.height * 4;
    if (frame.bgra.size() < expected) {
        if (err) *err = "frame buffer is smaller than width*height*4";
        return false;
    }
    return true;
}

} // namespace

bool write_bmp(const std::string& path, const CapturedFrame& frame, std::string* err) {
    if (!validate_frame(frame, err))
        return false;

    uint32_t imgsize = static_cast<uint32_t>(frame.width) * frame.height * 4;
    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    *reinterpret_cast<uint32_t*>(&header[2]) = 54 + imgsize;
    *reinterpret_cast<uint32_t*>(&header[10]) = 54;
    *reinterpret_cast<uint32_t*>(&header[14]) = 40;
    *reinterpret_cast<int32_t*>(&header[18]) = frame.width;
    *reinterpret_cast<int32_t*>(&header[22]) = frame.height;
    *reinterpret_cast<uint16_t*>(&header[26]) = 1;
    *reinterpret_cast<uint16_t*>(&header[28]) = 32;
    *reinterpret_cast<uint32_t*>(&header[34]) = imgsize;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (err) *err = "failed to open BMP output path";
        return false;
    }

    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (int y = frame.height - 1; y >= 0; --y) {
        const size_t row = static_cast<size_t>(y) * frame.width * 4;
        out.write(reinterpret_cast<const char*>(&frame.bgra[row]),
                  static_cast<std::streamsize>(frame.width * 4));
    }
    return true;
}

bool encode_jpeg(const CapturedFrame& frame, std::vector<uint8_t>& jpeg,
                 int quality, std::string* err) {
    if (!validate_frame(frame, err))
        return false;

#ifdef _WIN32
    if (!ensure_gdiplus(err))
        return false;

    CLSID jpeg_clsid;
    if (!get_encoder_clsid(L"image/jpeg", jpeg_clsid)) {
        if (err) *err = "JPEG encoder not available";
        return false;
    }

    ULONG q = static_cast<ULONG>(std::max(1, std::min(100, quality)));
    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &q;

    return save_bitmap_to_memory(frame, jpeg_clsid, &params, jpeg, err);
#else
    if (err) *err = "JPEG encoding is currently Windows-only";
    return false;
#endif
}

bool encode_png(const CapturedFrame& frame, std::vector<uint8_t>& png, std::string* err) {
    if (!validate_frame(frame, err))
        return false;

#ifdef _WIN32
    if (!ensure_gdiplus(err))
        return false;

    CLSID png_clsid;
    if (!get_encoder_clsid(L"image/png", png_clsid)) {
        if (err) *err = "PNG encoder not available";
        return false;
    }

    return save_bitmap_to_memory(frame, png_clsid, nullptr, png, err);
#else
    if (err) *err = "PNG encoding is currently Windows-only";
    return false;
#endif
}

void shutdown_image_encoder() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_gdiplus_mutex);
    if (g_gdiplus_token) {
        Gdiplus::GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_token = 0;
    }
#endif
}

} // namespace dfcapture
