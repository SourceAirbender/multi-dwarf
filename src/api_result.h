// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only
//
// Small domain-to-route result type. It carries an HTTP-ready error without making domain code
// know about httplib responses or JSON serialization.
#pragma once

#include <string>
#include <utility>

namespace dfcapture {

struct ApiError {
    int status = 500;
    std::string code;
    std::string message;
};

template <typename T>
struct ApiResult {
    bool ok = false;
    T value{};
    ApiError error{};

    static ApiResult success(T result) {
        ApiResult out;
        out.ok = true;
        out.value = std::move(result);
        return out;
    }
    static ApiResult failure(int status, std::string code, std::string message) {
        ApiResult out;
        out.error = ApiError{status, std::move(code), std::move(message)};
        return out;
    }
};

} // namespace dfcapture
