// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
//
// Native fortress barter bridge for the exact supported Steam DF executable. The global UI dispatcher
// is never called: typed C++ constructs the lists and DF's standalone routine commits the complete
// transaction.
//
// SPDX-License-Identifier: AGPL-3.0-only

#include "native_trade.h"

#include "modules/Items.h"

#include "df/building_item_role_type.h"
#include "df/building_tradedepotst.h"
#include "df/buildingitemst.h"
#include "df/caravan_state.h"
#include "df/global_objects.h"
#include "df/gamest.h"
#include "df/historical_entity.h"
#include "df/item.h"
#include "df/massst.h"
#include "df/main_interface.h"
#include "df/plotinfost.h"
#include "df/talk_line_type.h"
#include "df/trade_interface_good_flag.h"
#include "df/trade_interfacest.h"
#include "df/world_site.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#  include <bcrypt.h>
#endif

namespace dfcapture {
namespace {

constexpr uintptr_t kNativeTradeRva = 0x369490;
constexpr uintptr_t kNativeTradeInitRva = 0x36b590;
constexpr uintptr_t kNativeWeightWholeRva = 0x30cb50;
constexpr uintptr_t kNativeWeightFractionRva = 0x30cb20;
constexpr char kSupportedDfSha256[] =
    "205770918fd54c96cbbcf89223ebd449e2e113c7c873ed81177c4511a3450db7";

using NativeTradeFn =
    void(__fastcall*)(df::trade_interfacest*, int32_t, int32_t, df::massst*);
using NativeTradeInitFn = void(__fastcall*)(df::trade_interfacest*, df::caravan_state*);
using NativeWeightFn = int32_t(__fastcall*)(df::item*);

NativeTradeFn g_native_trade = nullptr;
NativeTradeInitFn g_native_trade_init = nullptr;
NativeWeightFn g_native_weight_whole = nullptr;
NativeWeightFn g_native_weight_fraction = nullptr;
bool g_resolve_attempted = false;
std::string g_resolve_error;

static_assert(offsetof(df::trade_interfacest, bld) == 0xb0);
static_assert(offsetof(df::trade_interfacest, mer) == 0xb8);
static_assert(offsetof(df::trade_interfacest, civ) == 0xc0);
static_assert(offsetof(df::trade_interfacest, merchant_trader) == 0xd0);
static_assert(offsetof(df::trade_interfacest, fortress_trader) == 0xd8);
static_assert(offsetof(df::trade_interfacest, good) == 0xe0);
static_assert(offsetof(df::trade_interfacest, goodflag) == 0x110);
static_assert(offsetof(df::trade_interfacest, good_amount) == 0x140);
static_assert(offsetof(df::trade_interfacest, talkline) == 0x37a);
static_assert(offsetof(df::trade_interfacest, buildlists) == 0x37c);
static_assert(offsetof(df::trade_interfacest, counter_offer) == 0x37e);
static_assert(offsetof(df::trade_interfacest, counter_offer_item) == 0x380);

#ifdef _WIN32
bool executable_sha256(std::string* out, std::string* err) {
    wchar_t path[32768] = {};
    DWORD path_len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!path_len || path_len == std::size(path)) {
        if (err) *err = "could not resolve Dwarf Fortress.exe path";
        return false;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (err) *err = "could not open Dwarf Fortress.exe for version verification";
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0, hash_len = 0, read_len = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    std::array<unsigned char, 1 << 16> buffer{};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        goto done;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len),
                          &read_len, 0) < 0)
        goto done;
    if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len),
                          &read_len, 0) < 0)
        goto done;
    object.resize(object_len);
    digest.resize(hash_len);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_len,
                         nullptr, 0, 0) < 0)
        goto done;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
            goto done;
        if (!got)
            break;
        if (BCryptHashData(hash, buffer.data(), got, 0) < 0)
            goto done;
    }
    if (BCryptFinishHash(hash, digest.data(), hash_len, 0) < 0)
        goto done;
    {
        std::ostringstream text;
        text << std::hex << std::setfill('0');
        for (unsigned char byte : digest)
            text << std::setw(2) << static_cast<unsigned>(byte);
        *out = text.str();
    }
    ok = true;
done:
    if (!ok && err && err->empty())
        *err = "could not hash Dwarf Fortress.exe";
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

bool check_signature(uintptr_t base, uintptr_t rva, const unsigned char* bytes,
                     size_t count, const char* label, std::string* err) {
    if (std::memcmp(reinterpret_cast<const void*>(base + rva), bytes, count) == 0)
        return true;
    if (err)
        *err = std::string("DF 53.16 native barter signature mismatch at ") + label;
    return false;
}
#endif

bool resolve_native(std::string* err) {
    if (g_resolve_attempted) {
        if (!g_resolve_error.empty() && err) *err = g_resolve_error;
        return g_resolve_error.empty();
    }
    g_resolve_attempted = true;
#ifndef _WIN32
    g_resolve_error = "native barter is currently available only on Steam DF 53.16 for Windows";
#else
    std::string actual_hash;
    if (!executable_sha256(&actual_hash, &g_resolve_error))
        goto failed;
    if (actual_hash != kSupportedDfSha256) {
        g_resolve_error = "native barter requires the exact supported Steam DF 53.16 executable "
                          "(SHA-256 mismatch)";
        goto failed;
    }
    {
        HMODULE module = GetModuleHandleW(nullptr);
        if (!module) {
            g_resolve_error = "could not locate Dwarf Fortress.exe in memory";
            goto failed;
        }
        uintptr_t base = reinterpret_cast<uintptr_t>(module);
        static const unsigned char trade_sig[] = {
            0x48,0x89,0x5c,0x24,0x18,0x4c,0x89,0x4c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,
            0x55,0x41,0x56,0x41,0x57
        };
        static const unsigned char init_sig[] = {
            0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57
        };
        static const unsigned char weight_sig[] = {
            0x40,0x53,0x48,0x83,0xec,0x20,0xf7,0x41,0x10,0x00,0x00,0x00,0x20
        };
        if (!check_signature(base, kNativeTradeRva, trade_sig, sizeof(trade_sig),
                             "trade commit", &g_resolve_error) ||
            !check_signature(base, kNativeTradeInitRva, init_sig, sizeof(init_sig),
                             "trade initializer", &g_resolve_error) ||
            !check_signature(base, kNativeWeightWholeRva, weight_sig, sizeof(weight_sig),
                             "weight whole", &g_resolve_error) ||
            !check_signature(base, kNativeWeightFractionRva, weight_sig, sizeof(weight_sig),
                             "weight fraction", &g_resolve_error))
            goto failed;
        g_native_trade = reinterpret_cast<NativeTradeFn>(base + kNativeTradeRva);
        g_native_trade_init = reinterpret_cast<NativeTradeInitFn>(base + kNativeTradeInitRva);
        g_native_weight_whole = reinterpret_cast<NativeWeightFn>(base + kNativeWeightWholeRva);
        g_native_weight_fraction =
            reinterpret_cast<NativeWeightFn>(base + kNativeWeightFractionRva);
    }
#endif
failed:
    if (!g_resolve_error.empty() && err) *err = g_resolve_error;
    return g_resolve_error.empty();
}

class ScopedStackAmount {
public:
    ScopedStackAmount(df::item* item, int32_t amount) : item_(item) {
        if (!item_) return;
        original_ = item_->getStackSize();
        if (amount > 0 && amount < original_) {
            item_->setStackSize(amount);
            changed_ = true;
        }
    }
    ~ScopedStackAmount() {
        if (changed_ && item_)
            item_->setStackSize(original_);
    }
private:
    df::item* item_ = nullptr;
    int32_t original_ = 0;
    bool changed_ = false;
};

void subtract_mass(df::massst* value, int32_t whole, int32_t fraction) {
    value->whole -= whole;
    value->fraction -= fraction;
    while (value->fraction < 0) {
        --value->whole;
        value->fraction += 1000000;
    }
    while (value->fraction >= 1000000) {
        ++value->whole;
        value->fraction -= 1000000;
    }
}

bool apply_selection(df::trade_interfacest& state, int side,
                     const std::vector<NativeTradeSelection>& selected,
                     std::unordered_set<int32_t>* seen, std::string* err) {
    std::unordered_map<int32_t, int32_t> requested;
    for (const auto& selection : selected) {
        if (selection.item_id < 0 || selection.amount < 0) {
            if (err) *err = "invalid trade item selection";
            return false;
        }
        if (!requested.emplace(selection.item_id, selection.amount).second) {
            if (err) *err = "duplicate trade item selection";
            return false;
        }
    }
    for (size_t i = 0; i < state.good[side].size(); ++i) {
        auto* item = state.good[side][i];
        auto it = item ? requested.find(item->id) : requested.end();
        if (it == requested.end())
            continue;
        int32_t stack = std::max(1, item->getStackSize());
        if (it->second > stack) {
            if (err) *err = "trade amount exceeds the live stack size";
            return false;
        }
        state.goodflag[side][i].bits.selected = true;
        state.good_amount[side][i] = (it->second > 0 && it->second < stack) ? it->second : 0;
        seen->insert(item->id);
    }
    if (seen->size() != requested.size()) {
        if (err)
            *err = side == 0 ? "a selected merchant item is no longer available"
                             : "a selected fortress item is no longer at this depot";
        return false;
    }
    return true;
}

int32_t selected_value(df::trade_interfacest& state, int side) {
    int64_t total = 0;
    for (size_t i = 0; i < state.good[side].size(); ++i) {
        if (!state.goodflag[side][i].bits.selected)
            continue;
        total += native_trade_item_value(
            state.good[side][i], state.mer, state.good_amount[side][i]);
    }
    return static_cast<int32_t>(std::min<int64_t>(total, INT32_MAX));
}

df::massst remaining_capacity(df::trade_interfacest& state) {
    df::massst remaining = state.mer->total_capacity;
    // DF's own trade screen subtracts merchant goods that remain with the caravan and fortress
    // goods that the caravan will receive. Selected merchant goods leave the caravan.
    for (size_t i = 0; i < state.good[0].size(); ++i) {
        if (state.goodflag[0][i].bits.selected)
            continue;
        int32_t whole = 0, fraction = 0;
        native_trade_item_mass(state.good[0][i], state.good_amount[0][i], &whole, &fraction);
        subtract_mass(&remaining, whole, fraction);
    }
    for (size_t i = 0; i < state.good[1].size(); ++i) {
        if (!state.goodflag[1][i].bits.selected)
            continue;
        int32_t whole = 0, fraction = 0;
        native_trade_item_mass(state.good[1][i], state.good_amount[1][i], &whole, &fraction);
        subtract_mass(&remaining, whole, fraction);
    }
    return remaining;
}

} // namespace

bool native_trade_available(std::string* err) {
    return resolve_native(err);
}

df::caravan_state* native_trade_caravan(int32_t caravan_index, int32_t* resolved_index) {
    auto* plotinfo = df::global::plotinfo;
    if (!plotinfo)
        return nullptr;
    for (size_t i = 0; i < plotinfo->caravans.size(); ++i) {
        auto* caravan = plotinfo->caravans[i];
        if (!caravan || caravan->flags.bits.tribute ||
            caravan->trade_state != df::caravan_state::T_trade_state::AtDepot ||
            caravan->time_remaining <= 0)
            continue;
        if (caravan_index >= 0 && static_cast<int32_t>(i) != caravan_index)
            continue;
        if (resolved_index) *resolved_index = static_cast<int32_t>(i);
        return caravan;
    }
    return nullptr;
}

bool native_trade_prepare(df::building_tradedepotst* depot, int32_t caravan_index,
                          df::trade_interfacest& state, int32_t* resolved_index,
                          std::string* err) {
    if (!depot) {
        if (err) *err = "trade depot not found";
        return false;
    }
    if (!resolve_native(err))
        return false;
    if (df::global::game && df::global::game->main_interface.trade.open) {
        if (err) *err = "close the native Dwarf Fortress trade screen before browser barter";
        return false;
    }
    int32_t chosen = -1;
    auto* caravan = native_trade_caravan(caravan_index, &chosen);
    if (!caravan) {
        if (err) *err = "no active caravan is ready at this depot";
        return false;
    }
    auto* plotinfo = df::global::plotinfo;
    auto* site = plotinfo ? df::world_site::find(plotinfo->site_id) : nullptr;
    if (!site) {
        if (err) *err = "current fortress site is unavailable";
        return false;
    }

    state.open = false;
    state.choosing_merchant = false;
    state.st = site;
    state.bld = depot;
    g_native_trade_init(&state, caravan);
    state.open = false;
    state.buildlists = 0;
    if (!state.civ || !state.merchant_trader || !state.fortress_trader || !state.havetalker) {
        if (err) *err = "merchant and fortress traders must both be present at the depot";
        return false;
    }
    if (state.stillunloading) {
        if (err) *err = "the caravan is still unloading";
        return false;
    }

    std::unordered_set<int32_t> caravan_goods(caravan->goods.begin(), caravan->goods.end());
    for (auto* contained : depot->contained_items) {
        if (!contained || !contained->item ||
            contained->use_mode == df::building_item_role_type::PERM)
            continue;
        auto* item = contained->item;
        if (!item->flags.bits.in_building || item->flags.bits.removed ||
            item->flags.bits.garbage_collect)
            continue;
        int side = item->flags.bits.trader ? 0 : 1;
        if (side == 0 && !caravan_goods.contains(item->id))
            continue; // do not mix goods belonging to another caravan
        state.good[side].push_back(item);
    }
    for (int side = 0; side < 2; ++side) {
        std::sort(state.good[side].begin(), state.good[side].end(),
                  [](df::item* a, df::item* b) { return a->id < b->id; });
        state.goodflag[side].assign(state.good[side].size(), df::trade_interface_good_flag());
        state.good_amount[side].assign(state.good[side].size(), 0);
    }
    if (state.good[0].empty()) {
        if (err) *err = "the selected caravan has no merchant goods at this depot";
        return false;
    }
    if (resolved_index) *resolved_index = chosen;
    return true;
}

int32_t native_trade_item_value(df::item* item, df::caravan_state* caravan, int32_t amount) {
    if (!item)
        return 0;
    ScopedStackAmount stack(item, amount);
    return DFHack::Items::getValue(item, caravan);
}

void native_trade_item_mass(df::item* item, int32_t amount, int32_t* whole, int32_t* fraction) {
    if (whole) *whole = 0;
    if (fraction) *fraction = 0;
    if (!item || !resolve_native(nullptr))
        return;
    ScopedStackAmount stack(item, amount);
    if (whole) *whole = g_native_weight_whole(item);
    if (fraction) *fraction = g_native_weight_fraction(item);
}

bool native_trade_execute(df::building_tradedepotst* depot, int32_t caravan_index,
                          const std::vector<NativeTradeSelection>& merchant,
                          const std::vector<NativeTradeSelection>& fortress,
                          const std::vector<int32_t>& counter_offer_ids,
                          bool accept_counter, NativeTradeOfferResult* result,
                          std::string* err) {
    if (!result) {
        if (err) *err = "missing trade result";
        return false;
    }
    *result = NativeTradeOfferResult();
    df::trade_interfacest state;
    int32_t resolved = -1;
    if (!native_trade_prepare(depot, caravan_index, state, &resolved, err))
        return false;

    std::unordered_set<int32_t> merchant_seen, fortress_seen;
    if (!apply_selection(state, 0, merchant, &merchant_seen, err) ||
        !apply_selection(state, 1, fortress, &fortress_seen, err))
        return false;

    if (accept_counter) {
        std::unordered_set<int32_t> expected(counter_offer_ids.begin(), counter_offer_ids.end());
        for (size_t i = 0; i < state.good[1].size(); ++i) {
            auto* item = state.good[1][i];
            if (!item || !expected.contains(item->id))
                continue;
            state.goodflag[1][i].bits.selected = true;
            state.good_amount[1][i] = 0;
            state.counter_offer_item.push_back(item);
            expected.erase(item->id);
        }
        if (!expected.empty()) {
            if (err) *err = "a counteroffer item is no longer at the depot";
            return false;
        }
        state.counter_offer = true;
    }

    result->merchant_value = selected_value(state, 0);
    result->fortress_value = selected_value(state, 1);
    df::massst capacity = remaining_capacity(state);
    g_native_trade(&state, result->merchant_value, result->fortress_value, &capacity);
    result->talkline = state.talkline.value;
    result->counter_offer = state.counter_offer;
    result->committed = !state.counter_offer &&
        state.talkline.value == static_cast<int16_t>(df::talk_line_type::Trade);
    if (state.counter_offer) {
        result->counter_offer_ids.reserve(state.counter_offer_item.size());
        for (auto* item : state.counter_offer_item)
            if (item) result->counter_offer_ids.push_back(item->id);
    }
    return true;
}

} // namespace dfcapture
