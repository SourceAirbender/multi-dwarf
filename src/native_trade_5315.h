// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace df {
struct building_tradedepotst;
struct caravan_state;
struct item;
struct trade_interfacest;
}

namespace dfcapture {

struct NativeTradeSelection {
    int32_t item_id = -1;
    int32_t amount = 0; // zero means the complete stack, matching DF's trade interface
};

struct NativeTradeOfferResult {
    bool committed = false;
    bool counter_offer = false;
    int32_t talkline = -1;
    int32_t merchant_value = 0;
    int32_t fortress_value = 0;
    std::vector<int32_t> counter_offer_ids;
};

// Resolve and validate the Steam DF 53.15 native barter ABI. This verifies the complete
// executable SHA-256, required DFHack structure offsets, and native prologues.
// It never opens or drives DF's global trade interface.
bool native_trade_5315_available(std::string* err);

// Locate an at-depot caravan by its plotinfo vector index. A negative index chooses the first.
df::caravan_state* native_trade_caravan(int32_t caravan_index, int32_t* resolved_index);

// Build a transient, closed trade_interfacest using DF's native initializer and the live depot
// contents. The returned state owns only vectors/strings; every item/caravan pointer remains DF-owned.
bool native_trade_prepare(df::building_tradedepotst* depot, int32_t caravan_index,
                          df::trade_interfacest& state, int32_t* resolved_index,
                          std::string* err);

// Apply an offer through DF 53.15's atomic native barter routine. `accept_counter` is permitted
// only by the caller after validating a server-held counteroffer token.
bool native_trade_execute(df::building_tradedepotst* depot, int32_t caravan_index,
                          const std::vector<NativeTradeSelection>& merchant,
                          const std::vector<NativeTradeSelection>& fortress,
                          const std::vector<int32_t>& counter_offer_ids,
                          bool accept_counter, NativeTradeOfferResult* result,
                          std::string* err);

int32_t native_trade_item_value(df::item* item, df::caravan_state* caravan, int32_t amount);
void native_trade_item_mass(df::item* item, int32_t amount, int32_t* whole, int32_t* fraction);

} // namespace dfcapture
