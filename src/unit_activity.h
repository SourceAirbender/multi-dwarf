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

#include "unit_activity_logic.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "modules/Job.h"
#include "modules/Units.h"

#include "df/activity_entry.h"
#include "df/activity_event.h"
#include "df/activity_event_type.h"
#include "df/job.h"
#include "df/job_type.h"
#include "df/unit.h"

namespace dfcapture {

// One world-side activity scan for one serialization pass. The per-unit resolver only performs
// an O(1) lookup in this map; it never walks world.activities itself.
class WorldActivityIndex {
public:
    WorldActivityIndex();
    df::activity_event* find(int32_t unit_id) const;

private:
    std::unordered_map<int32_t, df::activity_event*> events_by_unit_;
};

// DF separates ordinary jobs (`unit->job.current_job`, a df::job such as mine, haul, or brew) from
// ACTIVITIES (df::activity_entry + its df::activity_event subclasses -- worship, prayer, socialize,
// play, performance, conversation, sparring, drills, reading, research). Reading only the job
// vector makes every activity read as idle.
//
// Activities hang off four purpose-specific unit vectors:
//   individual_drills  (personal_activity_id)  -- military drills, service orders
//   social_activities  (shared_activity_id)    -- worship/prayer/socialize/play/perform/read/...
//   conversations      (conv_activity_id)
//   activities         (conflict_activity_id)  -- despite the generic DFHack name, this is CONFLICT
// `ignored_activities` (ignore_activity_id) is deliberately omitted: DF's own name says it is not
// current work.
//
// An entry holds a parent event plus newer subevents. The last event of the last activity is
// current. A normal job wins over activities, and the world index is only a fallback.
inline df::activity_event* last_unit_activity_event(const std::vector<int32_t>& activity_ids) {
    for (auto id = activity_ids.rbegin(); id != activity_ids.rend(); ++id) {
        auto activity = df::activity_entry::find(*id);
        if (!activity || activity->events.empty())
            continue;
        if (auto event = activity->events.back())
            return event;
    }
    return nullptr;
}

inline df::activity_event* unit_current_activity_event(df::unit* unit) {
    if (!unit)
        return nullptr;

    // Use the shared social-event helper for the common path.
    if (auto event = DFHack::Units::getMainSocialEvent(unit))
        return event;
    if (auto event = last_unit_activity_event(unit->individual_drills))
        return event;
    if (auto event = last_unit_activity_event(unit->conversations))
        return event;
    return last_unit_activity_event(unit->activities);
}

inline bool is_idle_task_placeholder(const std::string& name) {
    return name.empty() || name == "No job" || name == "No Job" ||
           name == "No activity" || name == "No Activity";
}

// The live job formatter can include material, item, reaction, and art details. Enum captions and
// generated keys provide bounded fallbacks when no formatted label is available.
inline std::string native_job_name(df::job* job) {
    if (!job)
        return {};

    std::string name = DFHack::Job::getName(job);
    if (!is_idle_task_placeholder(name))
        return name;

    const char* caption = ENUM_ATTR(job_type, caption, job->job_type);
    if (caption && !is_idle_task_placeholder(caption))
        return caption;

    return ENUM_KEY_STR(job_type, job->job_type);
}

// Native's Residents list colors the current-task label by its source, not by
// the final wording. Keep that source beside the name so callers never have to infer it from
// strings such as "Eat", "Pray to ...", or a participant-specific demonstration label.
enum class UnitTaskColorBucket : uint8_t {
    None,
    Job,
    Social,
    Need,
    Training,
};

struct UnitCurrentTask {
    std::string name;
    UnitTaskColorBucket color_bucket = UnitTaskColorBucket::None;
};

// Activity color families used by the Residents task list:
//   military practice and demonstrations -> Training
//   prayer and worship -> Need
//   conversation, recreation, teaching, and voluntary reading -> Social
//   guard duty, research, service, writing, and object work -> Job
// NONE is a sentinel rather than a live event and remains uncolored.
inline UnitTaskColorBucket activity_task_color_bucket(df::activity_event_type type) {
    switch (type) {
    case df::activity_event_type::TrainingSession:
    case df::activity_event_type::CombatTraining:
    case df::activity_event_type::SkillDemonstration:
    case df::activity_event_type::IndividualSkillDrill:
    case df::activity_event_type::Sparring:
    case df::activity_event_type::RangedPractice:
        return UnitTaskColorBucket::Training;

    case df::activity_event_type::Prayer:
    case df::activity_event_type::Worship:
        return UnitTaskColorBucket::Need;

    case df::activity_event_type::Harassment:
    case df::activity_event_type::Conversation:
    case df::activity_event_type::Conflict:
    case df::activity_event_type::Reunion:
    case df::activity_event_type::Socialize:
    case df::activity_event_type::Performance:
    case df::activity_event_type::DiscussTopic:
    case df::activity_event_type::TeachTopic:
    case df::activity_event_type::Read:
    case df::activity_event_type::Play:
    case df::activity_event_type::MakeBelieve:
    case df::activity_event_type::PlayWithToy:
    case df::activity_event_type::Encounter:
        return UnitTaskColorBucket::Social;

    case df::activity_event_type::Guard:
    case df::activity_event_type::Research:
    case df::activity_event_type::PonderTopic:
    case df::activity_event_type::FillServiceOrder:
    case df::activity_event_type::Write:
    case df::activity_event_type::CopyWrittenContent:
    case df::activity_event_type::StoreObject:
        return UnitTaskColorBucket::Job;

    case df::activity_event_type::NONE:
        return UnitTaskColorBucket::None;
    }
    return UnitTaskColorBucket::None;
}

// Return the game's current-task wording through the activity's virtual formatter. Every event
// subclass composes its own label, so this must not become a local enum-to-label table. The game
// interpolates the deity ("Pray to Armok"), the topic ("Ponder Justice"), the toy, the value, the
// per-participant role (organizer vs trainee), the travel prefixes ("Go to Sparring Match"), the
// "/Resting" suffix, and the trailing '!' -- and it does so per-unit, which is why the vmethod takes
// a unit id.
//
// DF supplies punctuation such as the `Worship!` exclamation; this code neither adds nor strips it.
inline UnitCurrentTask unit_current_task(df::unit* unit,
                                         const WorldActivityIndex* world_activities = nullptr) {
    if (!unit)
        return {};
    df::activity_event* unit_event = nullptr;
    df::activity_event* world_event = nullptr;
    if (!unit->job.current_job) {
        unit_event = unit_current_activity_event(unit);
        if (!unit_event && world_activities)
            world_event = world_activities->find(unit->id);
    }
    auto event_name = [unit](df::activity_event* event) {
        std::string name;
        event->getName(unit->id, &name);
        if (!is_idle_task_placeholder(name))
            return name;
        return ENUM_KEY_STR(activity_event_type, event->getType());
    };
    std::string name = activity_detail::resolve_current_task(
        unit->job.current_job,
        unit_event,
        world_event,
        native_job_name,
        event_name);
    if (unit->job.current_job)
        return {std::move(name), UnitTaskColorBucket::Job};
    if (auto event = unit_event ? unit_event : world_event)
        return {std::move(name), activity_task_color_bucket(event->getType())};
    return {std::move(name), UnitTaskColorBucket::None};
}

inline std::string unit_current_task_name(df::unit* unit,
                                          const WorldActivityIndex* world_activities = nullptr) {
    return unit_current_task(unit, world_activities).name;
}

} // namespace dfcapture
