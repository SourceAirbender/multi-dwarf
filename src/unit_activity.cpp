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

#include "unit_activity.h"

#include "DataDefs.h"

#include "df/activity_entry.h"
#include "df/activity_event.h"
#include "df/activity_event_conversationst.h"
#include "df/activity_event_copy_written_contentst.h"
#include "df/activity_event_conflictst.h"
#include "df/activity_event_encounterst.h"
#include "df/activity_event_fill_service_orderst.h"
#include "df/activity_event_guardst.h"
#include "df/activity_event_harassmentst.h"
#include "df/activity_event_participants.h"
#include "df/activity_event_reunionst.h"
#include "df/conflict_sidest.h"
#include "df/conversation_participantst.h"
#include "df/encounter_unitst.h"
#include "df/global_objects.h"
#include "df/harassment_target_profilest.h"
#include "df/historical_figure.h"
#include "df/world.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dfcapture {
namespace {

void append_historical_figure_unit(std::vector<int32_t>& units, int32_t hfid) {
    if (auto figure = df::historical_figure::find(hfid)) {
        if (figure->unit_id >= 0)
            units.push_back(figure->unit_id);
    }
}

// Participant linkage in DFHack 53.15-r1's df.activity.xml:
//
// * lines 7-17 define activity_event_participants.units; lines 185-240, 670-702, 734-792 and
//   847-911 embed that common list in training, prayer/social, research, reading/writing, play and
//   performance events. getParticipantInfo (lines 130-132) is DF's vmethod for that exact list.
// * Conversation deliberately does NOT use it: lines 345-348 define conversation_participantst's
//   unit_id and lines 594-596 define the pointer vector.
// * Other unit-bearing shapes are FillServiceOrder.unit_id (211-216),
//   Encounter.unit_target[].unit (291-302, 319-328), Reunion.reunion_unit (335-342),
//   Conflict.sides[].unit_ids (651-662), and CopyWrittenContent.unit_id (805-816).
// * Guard and Harassment only store historical-figure IDs (271-279, 640-643); those are resolved
//   through historical_figure.unit_id (df.history_figure.xml:1062). StoreObject (917-923) has no
//   unit or historical-figure participant linkage and therefore cannot be indexed honestly.
std::vector<int32_t> participant_units(df::activity_event* event) {
    std::vector<int32_t> units;
    if (!event)
        return units;

    switch (event->getType()) {
    case df::activity_event_type::TrainingSession:
    case df::activity_event_type::CombatTraining:
    case df::activity_event_type::SkillDemonstration:
    case df::activity_event_type::IndividualSkillDrill:
    case df::activity_event_type::Sparring:
    case df::activity_event_type::RangedPractice:
    case df::activity_event_type::Prayer:
    case df::activity_event_type::Socialize:
    case df::activity_event_type::Worship:
    case df::activity_event_type::Performance:
    case df::activity_event_type::Research:
    case df::activity_event_type::PonderTopic:
    case df::activity_event_type::DiscussTopic:
    case df::activity_event_type::Read:
    case df::activity_event_type::Write:
    case df::activity_event_type::TeachTopic:
    case df::activity_event_type::Play:
    case df::activity_event_type::MakeBelieve:
    case df::activity_event_type::PlayWithToy:
        if (auto participants = event->getParticipantInfo())
            units = participants->units;
        break;

    case df::activity_event_type::Conversation:
        if (auto conversation = DFHack::virtual_cast<df::activity_event_conversationst>(event)) {
            for (auto participant : conversation->participants) {
                if (participant)
                    units.push_back(participant->unit_id);
            }
        }
        break;

    case df::activity_event_type::FillServiceOrder:
        if (auto service = DFHack::virtual_cast<df::activity_event_fill_service_orderst>(event))
            units.push_back(service->unit_id);
        break;

    case df::activity_event_type::CopyWrittenContent:
        if (auto copy = DFHack::virtual_cast<df::activity_event_copy_written_contentst>(event))
            units.push_back(copy->unit_id);
        break;

    case df::activity_event_type::Reunion:
        if (auto reunion = DFHack::virtual_cast<df::activity_event_reunionst>(event))
            units.insert(units.end(), reunion->reunion_unit.begin(), reunion->reunion_unit.end());
        break;

    case df::activity_event_type::Conflict:
        if (auto conflict = DFHack::virtual_cast<df::activity_event_conflictst>(event)) {
            for (auto side : conflict->sides) {
                if (side)
                    units.insert(units.end(), side->unit_ids.begin(), side->unit_ids.end());
            }
        }
        break;

    case df::activity_event_type::Encounter:
        if (auto encounter = DFHack::virtual_cast<df::activity_event_encounterst>(event)) {
            for (auto target : encounter->unit_target) {
                if (target)
                    units.push_back(target->unit);
            }
            for (auto hfid : encounter->encounter_hf)
                append_historical_figure_unit(units, hfid);
        }
        break;

    case df::activity_event_type::Guard:
        if (auto guard = DFHack::virtual_cast<df::activity_event_guardst>(event)) {
            for (auto hfid : guard->guard_hfid)
                append_historical_figure_unit(units, hfid);
        }
        break;

    case df::activity_event_type::Harassment:
        if (auto harassment = DFHack::virtual_cast<df::activity_event_harassmentst>(event)) {
            for (auto hfid : harassment->harasser_hf)
                append_historical_figure_unit(units, hfid);
            for (auto target : harassment->target_profile) {
                if (target)
                    append_historical_figure_unit(units, target->hfid);
            }
            append_historical_figure_unit(units, harassment->talker_hfid);
        }
        break;

    // StoreObject has no participant linkage in df.activity.xml:917-923. NONE is not a live
    // subclass. Leaving both unindexed is safer than attaching an unrelated unit.
    case df::activity_event_type::StoreObject:
    case df::activity_event_type::NONE:
        break;
    }
    return units;
}

int event_depth(df::activity_event* event,
                const std::unordered_map<int32_t, df::activity_event*>& events_by_id,
                std::unordered_map<int32_t, int>& memo,
                std::unordered_set<int32_t>& visiting) {
    if (!event)
        return 0;
    if (auto found = memo.find(event->event_id); found != memo.end())
        return found->second;
    if (!visiting.insert(event->event_id).second)
        return 0;

    int depth = 0;
    if (auto parent = events_by_id.find(event->parent_event_id); parent != events_by_id.end())
        depth = 1 + event_depth(parent->second, events_by_id, memo, visiting);
    visiting.erase(event->event_id);
    memo[event->event_id] = depth;
    return depth;
}

} // namespace

WorldActivityIndex::WorldActivityIndex() {
    auto world = df::global::world;
    if (!world)
        return;

    // `world.activities.all` is the authoritative world-side activity vector
    // (df.activity.xml:944-958). Build one local winner map per activity so a later activity can
    // replace an older one, matching the unit-side convention of taking the last activity ID.
    for (auto activity : world->activities.all) {
        if (!activity)
            continue;

        std::unordered_map<int32_t, df::activity_event*> events_by_id;
        for (auto event : activity->events) {
            if (event)
                events_by_id[event->event_id] = event;
        }

        std::unordered_map<int32_t, int> depths;
        std::unordered_set<int32_t> visiting;
        struct Candidate {
            df::activity_event* event;
            int depth;
            size_t order;
        };
        std::unordered_map<int32_t, Candidate> local;

        for (size_t order = 0; order < activity->events.size(); ++order) {
            auto event = activity->events[order];
            if (!event || event->flags.bits.dismissed)
                continue;
            int depth = event_depth(event, events_by_id, depths, visiting);
            for (auto unit_id : participant_units(event)) {
                if (unit_id < 0)
                    continue;
                auto found = local.find(unit_id);
                if (found == local.end() || activity_detail::candidate_wins(
                        {depth, order}, {found->second.depth, found->second.order})) {
                    local[unit_id] = {event, depth, order};
                }
            }
        }

        for (const auto& [unit_id, candidate] : local)
            events_by_unit_[unit_id] = candidate.event;
    }
}

df::activity_event* WorldActivityIndex::find(int32_t unit_id) const {
    auto found = events_by_unit_.find(unit_id);
    return found == events_by_unit_.end() ? nullptr : found->second;
}

} // namespace dfcapture
