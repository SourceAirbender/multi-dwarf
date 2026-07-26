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

#include <cstddef>
#include <string>

namespace dfcapture::activity_detail {

struct CandidateRank {
    int depth;
    size_t order;
};

constexpr bool candidate_wins(CandidateRank candidate, CandidateRank current) {
    return candidate.depth > current.depth ||
           (candidate.depth == current.depth && candidate.order > current.order);
}

// Name callbacks retain DF's wording while keeping source selection independently testable.
template <typename Job, typename Event, typename JobName, typename EventName>
std::string resolve_current_task(Job* job, Event* unit_event, Event* world_event,
                                 JobName job_name, EventName event_name) {
    if (job)
        return job_name(job);
    if (unit_event)
        return event_name(unit_event);
    if (world_event)
        return event_name(world_event);
    return {};
}

} // namespace dfcapture::activity_detail
