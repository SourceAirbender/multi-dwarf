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

#include "httplib.h"

namespace dfcapture {

// Hospital and health-management domain. A hospital is not a civzone_type; it is an
// abstract_building_hospitalst LOCATION (abstract_building_type::HOSPITAL) attached to a
// MeetingHall/DiningHall/Bedroom civzone via zone.location_id, exactly like tavern/temple/library.
// Hospital zone validation is exposed through the route implementation.
// Its supply maxima live on location->getContents() (abstract_building_contents): desired_* /
// count_* / need_more for splints/thread/cloth/crutches/powder(plaster)/buckets/soap. This module
// exposes the READ surface (supplies, hospital furniture counts, patient list from unit->health,
// doctors by medical labor, chief-medical-dwarf noble, active medical-job queue) and the safe
// MUTATION (supply-maxima config -- the exact desired_*/need_more write DF's Locations screen +
// quickfort perform). Per-dwarf medical-labor toggles reuse the existing /labor* routes; chief
// medical dwarf assignment reuses the existing /noble-assign + /noble-candidates routes. Additive
// JSON only; no binary wire changes.
void register_hospital_routes(httplib::Server& server);

} // namespace dfcapture
