// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 William <william@wilkins.co.kr>
// SPDX-License-Identifier: AGPL-3.0-only
//
// Bounded wait for runOnRenderThread marshals. During teardown the render thread can stop
// draining its task queue before HTTP workers exit, so every reachable marshal must time out.
#pragma once

#include <chrono>
#include <future>

namespace dfcapture {

template <typename T>
inline bool render_future_ready(std::future<T>& fut, int secs = 3) {
    return fut.wait_for(std::chrono::seconds(secs)) == std::future_status::ready;
}

// Returns true only when a value was obtained. A render task abandoned during teardown makes its
// promise ready with std::future_error; catch that here so request workers never unwind through
// cpp-httplib or plugin shutdown.
template <typename T>
inline bool render_future_get(std::future<T>& fut, T& value, int secs = 3) noexcept {
    try {
        if (!render_future_ready(fut, secs))
            return false;
        value = fut.get();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace dfcapture
