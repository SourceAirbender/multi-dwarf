// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
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

#pragma once

#include "modules/Items.h"
#include "modules/Job.h"
#include "modules/Maps.h"
#include "modules/Units.h"

#include "df/building.h"
#include "df/building_cagest.h"
#include "df/building_type.h"
#include "df/global_objects.h"
#include "df/item.h"
#include "df/job.h"
#include "df/job_type.h"
#include "df/specific_ref.h"
#include "df/specific_ref_type.h"
#include "df/tile_designation.h"
#include "df/unit.h"
#include "df/world.h"

#include <algorithm>

namespace dfcapture {

// Ownership is shared; eligibility is not. Stocks includes personally owned, forbidden,
// dump-designated, foreign/imported, artifact, web, encased, and burning items. Job/material
// pickers apply a stricter availability layer. Trade and Kitchen keep their native surface-
// specific filters after this ownership gate. Naming each purpose makes those differences
// reviewable instead of hiding one-off flag copies at the call sites.
enum class FortItemPurpose {
    Stocks,
    Available,
    Kitchen,
    Presence,
    ConditionMaterial,
    TradeDepot,
};

namespace fort_stock_detail {

inline bool holder_is_caged(df::unit* holder) {
    if (!holder)
        return false;
    if (DFHack::Units::getContainer(holder))
        return true;
    auto world = df::global::world;
    if (!world)
        return false;
    // An installed cage can name its occupants in
    // assigned_units even when Units::getContainer() has no cage-item ref yet.
    for (auto building : world->buildings.all) {
        if (!building || building->getType() != df::building_type::Cage)
            continue;
        auto cage = static_cast<df::building_cagest*>(building);
        if (std::find(cage->assigned_units.begin(), cage->assigned_units.end(), holder->id) !=
                cage->assigned_units.end())
            return true;
    }
    return false;
}

inline bool base_rejected(df::item* item) {
    if (!item)
        return true;
    const auto& flags = item->flags;
    return flags.bits.hostile || flags.bits.trader || flags.bits.garbage_collect ||
           flags.bits.removed;
}

// These are Stocks-list exclusions, not generic job-availability rules. Foreign, owned,
// forbidden, dumped, webbed, encased, artifact, and burning items remain visible.
inline bool stocks_rejected(df::item* item) {
    if (base_rejected(item))
        return true;
    const auto& flags = item->flags;
    return flags.bits.in_building || flags.bits.dead_dwarf || flags.bits.murder ||
           flags.bits.construction;
}

inline bool available_rejected(df::item* item) {
    if (base_rejected(item))
        return true;
    const auto& flags = item->flags;
    return flags.bits.in_job || flags.bits.in_building || flags.bits.rotten ||
           flags.bits.spider_web || flags.bits.construction || flags.bits.encased ||
           flags.bits.owned || flags.bits.forbid || flags.bits.dump || flags.bits.on_fire ||
           flags.bits.artifact || flags.bits.murder;
}

inline bool rejects_for_purpose(df::item* item, FortItemPurpose purpose) {
    if (purpose == FortItemPurpose::Stocks)
        return stocks_rejected(item);
    if (purpose == FortItemPurpose::Available)
        return available_rejected(item);
    return base_rejected(item);
}

inline bool stocks_position_is_visible(df::item* outer) {
    if (!outer)
        return false;
    df::coord pos;
    if (outer->flags.bits.in_inventory) {
        if (outer->flags.bits.in_job) {
            auto ref = DFHack::Items::getSpecificRef(outer, df::specific_ref_type::JOB);
            if (!ref || !ref->data.job || ref->data.job->job_type == df::job_type::Eat ||
                    ref->data.job->job_type == df::job_type::Drink)
                return false;
            auto worker = DFHack::Job::getWorker(ref->data.job);
            if (!worker)
                return false;
            pos = worker->pos;
        } else {
            auto holder = DFHack::Items::getHolderUnit(outer);
            if (!holder)
                return false;
            pos = holder->pos;
        }
    } else {
        pos = outer->pos;
    }
    if (!pos.isValid())
        return false;
    auto designation = DFHack::Maps::getTileDesignation(pos);
    return designation && !designation->bits.hidden;
}

} // namespace fort_stock_detail

// Shared fort-property predicate. The holder rule is load-bearing:
// Admit citizen inventory and reject a non-citizen holder, except
// for its explicit captive-in-a-cage path. Therefore squad-issued citizen gear counts, while a
// visitor, long-term resident, diplomat, mercenary, caravan guard, or invader's gear does not.
inline bool is_fort_stock_item(df::item* item, FortItemPurpose purpose) {
    if (!item || !item->isActual())
        return false;

    df::item* outer = item;
    for (int depth = 0; outer && depth < 32; ++depth) {
        if (fort_stock_detail::rejects_for_purpose(outer, purpose))
            return false;
        df::item* container = DFHack::Items::getContainer(outer);
        if (!container || container == outer)
            break;
        outer = container;
    }
    if (!outer)
        return false;

    if (outer->flags.bits.in_inventory && !outer->flags.bits.in_job) {
        df::unit* holder = DFHack::Items::getHolderUnit(outer);
        if (!holder)
            return false;
        if (!DFHack::Units::isCitizen(holder) &&
                !(purpose == FortItemPurpose::Stocks && fort_stock_detail::holder_is_caged(holder)))
            return false;
        if (purpose == FortItemPurpose::Available)
            return false;
    }

    if (purpose == FortItemPurpose::Stocks &&
            !fort_stock_detail::stocks_position_is_visible(outer))
        return false;
    return true;
}

} // namespace dfcapture
