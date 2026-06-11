#include "unit_sheet.h"

#include "json_util.h"

#include "MiscUtils.h"
#include "modules/Buildings.h"
#include "modules/DFSDL.h"
#include "modules/Items.h"
#include "modules/Job.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/building.h"
#include "df/building_civzonest.h"
#include "df/caste_raw.h"
#include "df/creature_raw.h"
#include "df/emotion_type.h"
#include "df/entity_position.h"
#include "df/entity_position_assignment.h"
#include "df/goal_type.h"
#include "df/global_objects.h"
#include "df/item.h"
#include "df/job_skill.h"
#include "df/mental_attribute_type.h"
#include "df/need_type.h"
#include "df/personality_goalst.h"
#include "df/personality_facet_type.h"
#include "df/personality_memory_handlerst.h"
#include "df/personality_moodst.h"
#include "df/personality_needst.h"
#include "df/personality_preferencest.h"
#include "df/personality_preference_type.h"
#include "df/personality_valuest.h"
#include "df/physical_attribute_type.h"
#include "df/plotinfost.h"
#include "df/skill_rating.h"
#include "df/squad.h"
#include "df/training_assignment.h"
#include "df/unit.h"
#include "df/unit_emotion_memory.h"
#include "df/unit_inventory_item.h"
#include "df/unit_labor.h"
#include "df/unit_relationship_type.h"
#include "df/unit_skill.h"
#include "df/unit_soul.h"
#include "df/unit_thought_type.h"
#include "df/value_type.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_unit_sheet_mutex;

void append_lines(std::ostringstream& body, const std::vector<std::string>& lines) {
    append_json_string_array(body, lines);
}

void append_unit_json(std::ostringstream& body, const UnitSheet& unit) {
    body << "{"
         << "\"id\":" << unit.id << ","
         << "\"portraitTexpos\":" << unit.portrait_texpos << ","
         << "\"sheetIconTexpos\":" << unit.sheet_icon_texpos << ","
         << "\"name\":" << json_string(unit.name) << ","
         << "\"race\":" << json_string(unit.race) << ","
         << "\"profession\":" << json_string(unit.profession) << ","
         << "\"currentJob\":" << json_string(unit.current_job) << ","
         << "\"age\":" << json_string(unit.age) << ","
         << "\"sex\":" << json_string(unit.sex) << ","
         << "\"status\":" << json_string(unit.status) << ","
         << "\"training\":" << json_string(unit.training) << ","
         << "\"bodySummary\":" << json_string(unit.body_summary) << ","
         << "\"flags\":";
    append_lines(body, unit.flags);
    body << ",\"overviewRelationLines\":";
    append_lines(body, unit.overview_relation_lines);
    body << ",\"overviewTraitLines\":";
    append_lines(body, unit.overview_trait_lines);
    body << ",\"overviewPositionLines\":";
    append_lines(body, unit.overview_position_lines);
    body << ",\"overviewSquadLines\":";
    append_lines(body, unit.overview_squad_lines);
    body << ",\"overviewSkillLines\":";
    append_lines(body, unit.overview_skill_lines);
    body << ",\"overviewNeedLines\":";
    append_lines(body, unit.overview_need_lines);
    body << ",\"overviewMemoryLines\":";
    append_lines(body, unit.overview_memory_lines);
    body << ",\"statusLines\":";
    append_lines(body, unit.status_lines);
    body << ",\"inventoryLines\":";
    append_lines(body, unit.inventory_lines);
    body << ",\"healthLines\":";
    append_lines(body, unit.health_lines);
    body << ",\"healthStatusLines\":";
    append_lines(body, unit.health_status_lines);
    body << ",\"healthWoundLines\":";
    append_lines(body, unit.health_wound_lines);
    body << ",\"healthTreatmentLines\":";
    append_lines(body, unit.health_treatment_lines);
    body << ",\"healthHistoryLines\":";
    append_lines(body, unit.health_history_lines);
    body << ",\"healthDescriptionLines\":";
    append_lines(body, unit.health_description_lines);
    body << ",\"skillLines\":";
    append_lines(body, unit.skill_lines);
    body << ",\"roomLines\":";
    append_lines(body, unit.room_lines);
    body << ",\"roomAssignmentLines\":";
    append_lines(body, unit.room_assignment_lines);
    body << ",\"laborLines\":";
    append_lines(body, unit.labor_lines);
    body << ",\"laborWorkDetailLines\":";
    append_lines(body, unit.labor_work_detail_lines);
    body << ",\"laborWorkshopLines\":";
    append_lines(body, unit.labor_workshop_lines);
    body << ",\"laborLocationLines\":";
    append_lines(body, unit.labor_location_lines);
    body << ",\"laborWorkAnimalLines\":";
    append_lines(body, unit.labor_work_animal_lines);
    body << ",\"relationLines\":";
    append_lines(body, unit.relation_lines);
    body << ",\"groupLines\":";
    append_lines(body, unit.group_lines);
    body << ",\"militaryLines\":";
    append_lines(body, unit.military_lines);
    body << ",\"militarySquadLines\":";
    append_lines(body, unit.military_squad_lines);
    body << ",\"militaryUniformLines\":";
    append_lines(body, unit.military_uniform_lines);
    body << ",\"militaryKillLines\":";
    append_lines(body, unit.military_kill_lines);
    body << ",\"thoughtLines\":";
    append_lines(body, unit.thought_lines);
    body << ",\"personalityLines\":";
    append_lines(body, unit.personality_lines);
    body << ",\"personalityTraitLines\":";
    append_lines(body, unit.personality_trait_lines);
    body << ",\"personalityValueLines\":";
    append_lines(body, unit.personality_value_lines);
    body << ",\"personalityPreferenceLines\":";
    append_lines(body, unit.personality_preference_lines);
    body << ",\"personalityNeedLines\":";
    append_lines(body, unit.personality_need_lines);
    body << ",\"actions\":[";
    for (size_t i = 0; i < unit.actions.size(); ++i) {
        if (i)
            body << ",";
        const auto& action = unit.actions[i];
        body << "{"
             << "\"hotkey\":" << json_string(action.hotkey) << ","
             << "\"label\":" << json_string(action.label) << ","
             << "\"value\":" << json_string(action.value) << ","
             << "\"available\":" << (action.available ? "true" : "false")
             << "}";
    }
    body << "]}";
}

struct RenderThreadUnitRequest {
    int32_t unit_id = -1;
    UnitSheet unit;
    Camera tile;
    std::string err;
    std::promise<bool> done;
};

} // namespace

void append_unit_sheet_json(std::ostringstream& body, const UnitSheet& unit) {
    append_unit_json(body, unit);
}

const char* yes_no(bool value) {
    return value ? "Yes" : "No";
}

std::string unit_age_label(df::unit* unit) {
    double age = Units::getAge(unit);
    if (age < 0)
        return "Age unknown";
    int years = static_cast<int>(age);
    return std::to_string(years) + (years == 1 ? " Year Old" : " Years Old");
}

std::string unit_sex_label(df::unit* unit) {
    if (Units::isFemale(unit))
        return "female";
    if (Units::isMale(unit))
        return "male";
    return "unknown";
}

df::training_assignment* find_training_assignment(df::unit* unit) {
    auto plotinfo = df::global::plotinfo;
    if (!unit || !plotinfo)
        return nullptr;
    return binsearch_in_vector(plotinfo->training.training_assignments,
                               &df::training_assignment::animal_id, unit->id);
}

std::string unit_training_label(df::unit* unit) {
    if (Units::isWar(unit))
        return "War trained";
    if (Units::isHunter(unit))
        return "Hunting trained";
    if (Units::isTrained(unit))
        return "Trained";
    if (Units::isMarkedForWarTraining(unit))
        return "Marked for war training";
    if (Units::isMarkedForHuntTraining(unit))
        return "Marked for hunting training";
    if (Units::isMarkedForTaming(unit))
        return "Marked for taming";
    if (Units::isTame(unit))
        return "Tame";
    if (Units::isTamable(unit))
        return "Tamable";
    return "";
}

void push_if(std::vector<std::string>& lines, bool condition, const std::string& line) {
    if (condition)
        lines.push_back(line);
}

std::string capitalize_first(std::string text) {
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

std::string pretty_key(std::string key) {
    for (char& ch : key) {
        if (ch == '_')
            ch = ' ';
        else
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return capitalize_first(key);
}

std::string pronoun_subject(df::unit* unit) {
    if (Units::isFemale(unit))
        return "She";
    if (Units::isMale(unit))
        return "He";
    return "It";
}

std::string pronoun_object(df::unit* unit) {
    if (Units::isFemale(unit))
        return "her";
    if (Units::isMale(unit))
        return "him";
    return "it";
}

std::string pronoun_possessive(df::unit* unit) {
    if (Units::isFemale(unit))
        return "her";
    if (Units::isMale(unit))
        return "his";
    return "its";
}

std::string join_phrases(const std::vector<std::string>& phrases) {
    if (phrases.empty())
        return "";
    if (phrases.size() == 1)
        return phrases[0];
    std::string out;
    for (size_t i = 0; i < phrases.size(); ++i) {
        if (i)
            out += (i + 1 == phrases.size()) ? " and " : ", ";
        out += phrases[i];
    }
    return out;
}

int physical_attr_value(df::unit* unit, df::physical_attribute_type attr) {
    return Units::getPhysicalAttrValue(unit, attr);
}

int physical_attr_low_threshold(df::unit* unit, df::physical_attribute_type attr) {
    if (auto caste = Units::getCasteRaw(unit))
        return caste->attributes.phys_att_range[attr][1];
    return 700;
}

int physical_attr_high_threshold(df::unit* unit, df::physical_attribute_type attr) {
    if (auto caste = Units::getCasteRaw(unit))
        return caste->attributes.phys_att_range[attr][5];
    return 1300;
}

int physical_attr_very_low_threshold(df::unit* unit, df::physical_attribute_type attr) {
    if (auto caste = Units::getCasteRaw(unit))
        return caste->attributes.phys_att_range[attr][0];
    return 450;
}

int mental_attr_value(df::unit* unit, df::mental_attribute_type attr) {
    return Units::getMentalAttrValue(unit, attr);
}

int mental_attr_low_threshold(df::unit* unit, df::mental_attribute_type attr) {
    if (auto caste = Units::getCasteRaw(unit))
        return caste->attributes.ment_att_range[attr][1];
    return 700;
}

int mental_attr_high_threshold(df::unit* unit, df::mental_attribute_type attr) {
    if (auto caste = Units::getCasteRaw(unit))
        return caste->attributes.ment_att_range[attr][5];
    return 1300;
}

bool trait_high(df::unit* unit, df::personality_facet_type facet) {
    auto soul = unit->status.current_soul;
    return soul && soul->personality.traits[facet] >= 76;
}

bool trait_low(df::unit* unit, df::personality_facet_type facet) {
    auto soul = unit->status.current_soul;
    return soul && soul->personality.traits[facet] <= 24;
}

std::string physical_description_sentence(df::unit* unit) {
    using df::enums::physical_attribute_type::AGILITY;
    using df::enums::physical_attribute_type::DISEASE_RESISTANCE;
    using df::enums::physical_attribute_type::ENDURANCE;
    using df::enums::physical_attribute_type::RECUPERATION;
    using df::enums::physical_attribute_type::STRENGTH;
    using df::enums::physical_attribute_type::TOUGHNESS;

    std::vector<std::string> positives;
    std::vector<std::string> negatives;
    auto add_physical = [&](df::physical_attribute_type attr, const char* high, const char* low, const char* very_low = nullptr) {
        int value = physical_attr_value(unit, attr);
        if (value >= physical_attr_high_threshold(unit, attr)) {
            positives.push_back(high);
        } else if (value < physical_attr_low_threshold(unit, attr)) {
            negatives.push_back(value <= physical_attr_very_low_threshold(unit, attr) && very_low ? very_low : low);
        }
    };

    add_physical(AGILITY, "agile", "clumsy");
    add_physical(STRENGTH, "strong", "weak", "very weak");
    add_physical(TOUGHNESS, "tough", "fragile");
    add_physical(ENDURANCE, "slow to tire", "quick to tire", "very quick to tire");
    add_physical(RECUPERATION, "quick to heal", "slow to heal");
    add_physical(DISEASE_RESISTANCE, "resistant to disease", "susceptible to disease");

    if (positives.empty() && negatives.empty())
        return "";

    std::string sentence = pronoun_subject(unit) + " is ";
    if (!positives.empty())
        sentence += join_phrases(positives);
    if (!positives.empty() && !negatives.empty())
        sentence += ", but " + pronoun_subject(unit);
    if (!negatives.empty()) {
        if (!positives.empty())
            sentence += " is ";
        sentence += join_phrases(negatives);
    }
    sentence += ".";
    return sentence;
}

std::vector<std::string> unit_condition_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (!Units::isAlive(unit))
        lines.push_back("Dead");
    if (!Units::isSane(unit))
        lines.push_back("Not sane");
    push_if(lines, unit->counters.unconscious > 0, "Unconscious");
    push_if(lines, unit->counters.stunned > 0, "Stunned");
    push_if(lines, unit->counters.winded > 0, "Winded");
    push_if(lines, unit->counters.pain > 0, "In pain");
    push_if(lines, unit->counters.nausea > 0, "Nauseated");
    push_if(lines, unit->counters.dizziness > 0, "Dizzy");
    push_if(lines, unit->counters2.paralysis > 0, "Paralyzed");
    push_if(lines, unit->counters2.numbness > 0, "Numb");
    push_if(lines, unit->counters2.fever > 0, "Fevered");
    push_if(lines, unit->counters2.exhaustion > 0, "Exhausted");

    auto strength = df::enums::physical_attribute_type::STRENGTH;
    auto agility = df::enums::physical_attribute_type::AGILITY;
    auto toughness = df::enums::physical_attribute_type::TOUGHNESS;
    auto endurance = df::enums::physical_attribute_type::ENDURANCE;
    auto recuperation = df::enums::physical_attribute_type::RECUPERATION;
    auto disease_resistance = df::enums::physical_attribute_type::DISEASE_RESISTANCE;
    push_if(lines, physical_attr_value(unit, endurance) < physical_attr_low_threshold(unit, endurance),
            "Low stamina");
    push_if(lines, physical_attr_value(unit, strength) < physical_attr_low_threshold(unit, strength),
            "Weak");
    push_if(lines, physical_attr_value(unit, agility) < physical_attr_low_threshold(unit, agility),
            "Clumsy");
    push_if(lines, physical_attr_value(unit, toughness) < physical_attr_low_threshold(unit, toughness),
            "Fragile");
    push_if(lines, physical_attr_value(unit, recuperation) < physical_attr_low_threshold(unit, recuperation),
            "Recovers slowly");
    push_if(lines, physical_attr_value(unit, disease_resistance) < physical_attr_low_threshold(unit, disease_resistance),
            "Disease-prone");
    if (!unit->body.wounds.empty())
        lines.push_back("Injured");
    if (unit->effective_rate > 100)
        lines.push_back("Recovers quickly");
    if (lines.empty())
        lines.push_back("Healthy");
    return lines;
}

std::string related_unit_label(int32_t id) {
    if (id == -1)
        return "None";
    if (auto unit = df::unit::find(id)) {
        auto name = Units::getReadableName(unit);
        return name.empty() ? ("Unit " + std::to_string(id)) : name;
    }
    return "Unit " + std::to_string(id);
}

std::vector<std::string> unit_inventory_lines(df::unit* unit) {
    std::vector<std::string> lines;
    for (auto inv : unit->inventory) {
        if (!inv || !inv->item)
            continue;
        std::string desc = Items::getReadableDescription(inv->item);
        if (desc.empty())
            desc = Items::getDescription(inv->item, 0, true);
        if (!desc.empty())
            lines.push_back(desc);
        if (lines.size() >= 12)
            break;
    }
    if (lines.empty())
        lines.push_back("No inventory items.");
    return lines;
}

std::vector<std::string> unit_health_lines(df::unit* unit, const std::vector<std::string>& conditions) {
    std::vector<std::string> lines;
    lines.push_back(unit->body.wounds.empty() ? "Healthy" :
        std::to_string(unit->body.wounds.size()) + (unit->body.wounds.size() == 1 ? " wound" : " wounds"));
    for (const auto& condition : conditions) {
        if (condition != "Healthy" && condition != "Injured")
            lines.push_back(condition);
    }
    if (unit->body.wounds.empty())
        lines.push_back("No health problems");
    else
        lines.push_back("Detailed wound breakdown is not wired yet.");
    return lines;
}

std::vector<std::string> unit_health_description_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (auto caste = Units::getCasteRaw(unit)) {
        if (!caste->description.empty())
            lines.push_back(caste->description);
    }
    auto attrs = physical_description_sentence(unit);
    if (!attrs.empty())
        lines.push_back(attrs);
    if (lines.empty())
        lines.push_back("No description available.");
    return lines;
}

std::vector<std::string> unit_health_wound_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (unit->body.wounds.empty()) {
        lines.push_back("No wounds.");
    } else {
        lines.push_back(std::to_string(unit->body.wounds.size()) +
                        (unit->body.wounds.size() == 1 ? " wound recorded." : " wounds recorded."));
        lines.push_back("Detailed wound part descriptions are not independently decoded yet.");
    }
    return lines;
}

std::vector<std::string> unit_health_treatment_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (unit->body.wounds.empty())
        lines.push_back("No treatment scheduled.");
    else
        lines.push_back("Treatment details are not independently decoded yet.");
    return lines;
}

std::vector<std::string> unit_health_history_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (unit->body.wounds.empty())
        lines.push_back("No health history.");
    else
        lines.push_back("Health history is present but not independently decoded yet.");
    return lines;
}

std::vector<std::string> unit_skill_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (soul) {
        std::vector<std::pair<int, std::string>> ranked;
        for (auto skill : soul->skills) {
            if (!skill || skill->rating <= df::skill_rating::Dabbling)
                continue;
            auto skill_name = df::enum_traits<df::job_skill>::attrs(skill->id).caption;
            auto rating_name = df::enum_traits<df::skill_rating>::attrs(skill->rating).caption;
            if (skill_name && rating_name)
                ranked.emplace_back(static_cast<int>(skill->rating),
                                    std::string(rating_name) + " " + skill_name);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& entry : ranked) {
            lines.push_back(entry.second);
            if (lines.size() >= 12)
                break;
        }
    }
    if (lines.empty())
        lines.push_back("No notable skills.");
    return lines;
}

std::vector<std::string> unit_room_lines(df::unit* unit) {
    std::vector<std::string> lines;
    for (auto building : unit->owned_buildings) {
        if (!building)
            continue;
        std::string name = Buildings::getName(static_cast<df::building*>(building));
        if (name.empty())
            name = "Building " + std::to_string(building->id);
        lines.push_back(name);
        if (lines.size() >= 12)
            break;
    }
    if (lines.empty()) {
        lines.push_back("No Study");
        lines.push_back("No Quarters");
        lines.push_back("No Dining Room");
        lines.push_back("No Tomb");
    }
    return lines;
}

std::vector<std::string> unit_labor_work_detail_lines(df::unit* unit) {
    if (!Units::isCitizen(unit))
        return {"Cannot assign work details"};
    return {"No work details assigned."};
}

std::vector<std::string> unit_labor_workshop_lines(df::unit*) {
    return {"No workshops assigned."};
}

std::vector<std::string> unit_labor_location_lines(df::unit*) {
    return {"No locations assigned."};
}

std::vector<std::string> unit_labor_work_animal_lines(df::unit* unit) {
    if (Units::isTame(unit) && !Units::isCitizen(unit))
        return {"No work animal assignment."};
    return {"No work animals assigned."};
}

std::vector<std::string> unit_labor_lines(df::unit* unit) {
    std::vector<std::string> lines;
    for (int i = 0; i <= df::enum_traits<df::unit_labor>::last_item_value; ++i) {
        auto labor = static_cast<df::unit_labor>(i);
        if (!df::enum_traits<df::unit_labor>::is_valid(i) || !unit->status.labors[i])
            continue;
        auto caption = df::enum_traits<df::unit_labor>::attrs(labor).caption;
        if (caption)
            lines.push_back(std::string(caption));
        if (lines.size() >= 16)
            break;
    }
    if (lines.empty())
        lines.push_back("No labors enabled.");
    return lines;
}

std::vector<std::string> unit_relation_lines(df::unit* unit) {
    std::vector<std::string> lines;
    lines.push_back("Pet owner: " + related_unit_label(unit->relationship_ids[df::enums::unit_relationship_type::PetOwner]));
    lines.push_back("Mother: " + related_unit_label(unit->relationship_ids[df::enums::unit_relationship_type::Mother]));
    lines.push_back("Father: " + related_unit_label(unit->relationship_ids[df::enums::unit_relationship_type::Father]));
    lines.push_back("Spouse: " + related_unit_label(unit->relationship_ids[df::enums::unit_relationship_type::Spouse]));
    return lines;
}

std::vector<std::string> unit_group_lines(df::unit* unit) {
    std::vector<std::string> lines;
    lines.push_back("Civilization id: " + std::to_string(unit->civ_id));
    lines.push_back("Population id: " + std::to_string(unit->population_id));
    lines.push_back(std::string("Own group: ") + yes_no(Units::isOwnGroup(unit)));
    lines.push_back(std::string("Fort controlled: ") + yes_no(Units::isFortControlled(unit)));
    lines.push_back(std::string("Citizen: ") + yes_no(Units::isCitizen(unit)));
    return lines;
}

std::vector<std::string> unit_military_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (unit->military.squad_id == -1)
        lines.push_back("No squad assigned");
    else {
        lines.push_back("Squad id: " + std::to_string(unit->military.squad_id));
        lines.push_back("Squad position: " + std::to_string(unit->military.squad_position));
        lines.push_back("Patrol timer: " + std::to_string(unit->military.patrol_timer));
    }
    return lines;
}

std::vector<std::string> unit_military_uniform_lines(df::unit* unit) {
    if (unit->military.squad_id == -1)
        return {"No uniform assigned"};
    return {"Uniform details are not independently decoded yet."};
}

std::vector<std::string> unit_military_kill_lines(df::unit*) {
    return {"No kills recorded"};
}

std::string unit_current_job_label(df::unit* unit) {
    if (!unit || !unit->job.current_job)
        return "No job";
    auto name = Job::getName(unit->job.current_job);
    return name.empty() ? "Unknown job" : name;
}

std::vector<std::string> unit_overview_relation_lines(df::unit* unit) {
    std::vector<std::string> lines;
    int32_t spouse_id = unit->relationship_ids[df::enums::unit_relationship_type::Spouse];
    if (spouse_id != -1)
        lines.push_back("Spouse: " + related_unit_label(spouse_id));
    int32_t lover_id = unit->relationship_ids[df::enums::unit_relationship_type::Lover];
    if (lover_id != -1 && lover_id != spouse_id)
        lines.push_back("Lover: " + related_unit_label(lover_id));
    int32_t owner_id = unit->relationship_ids[df::enums::unit_relationship_type::PetOwner];
    if (owner_id != -1)
        lines.push_back("Owner: " + related_unit_label(owner_id));
    return lines;
}

std::vector<std::string> unit_personality_trait_lines(df::unit* unit);

std::vector<std::string> unit_overview_trait_lines(df::unit* unit,
                                                   const std::vector<std::string>& status_lines) {
    std::vector<std::string> lines;
    for (const auto& line : status_lines) {
        if (line != "Healthy")
            lines.push_back(line);
        if (lines.size() >= 6)
            return lines;
    }

    auto short_trait = [&](df::personality_facet_type facet, const char* high, const char* low) {
        if (trait_high(unit, facet))
            lines.push_back(high);
        else if (trait_low(unit, facet))
            lines.push_back(low);
    };

    using namespace df::enums::personality_facet_type;
    short_trait(CHEER_PROPENSITY, "Cheerful", "Often sad");
    short_trait(PERSEVERANCE, "High willpower", "Poor focus");
    short_trait(TOLERANT, "Tolerant", "Disdains harmony");
    short_trait(ALTRUISM, "Merciful", "Self-interested");
    short_trait(BRAVERY, "Brave", "Quick to give up");
    using namespace df::enums::mental_attribute_type;
    if (mental_attr_value(unit, KINESTHETIC_SENSE) >= mental_attr_high_threshold(unit, KINESTHETIC_SENSE))
        lines.push_back("High kinesthetic sense");
    else if (mental_attr_value(unit, KINESTHETIC_SENSE) < mental_attr_low_threshold(unit, KINESTHETIC_SENSE))
        lines.push_back("Poor kinesthetic sense");
    if (mental_attr_value(unit, SOCIAL_AWARENESS) >= mental_attr_high_threshold(unit, SOCIAL_AWARENESS))
        lines.push_back("Good social ability");
    else if (mental_attr_value(unit, SOCIAL_AWARENESS) < mental_attr_low_threshold(unit, SOCIAL_AWARENESS))
        lines.push_back("Low social ability");

    if (lines.empty()) {
        for (const auto& line : unit_personality_trait_lines(unit)) {
            if (line == "No notable personality traits.")
                continue;
            lines.push_back(line);
            if (lines.size() >= 6)
                break;
        }
    }
    return lines;
}

std::string noble_position_label(const Units::NoblePosition& pos, df::unit* unit) {
    if (!pos.position)
        return "";
    int plural_idx = 0;
    std::string name;
    if (Units::isFemale(unit))
        name = pos.position->name_female[plural_idx];
    else if (Units::isMale(unit))
        name = pos.position->name_male[plural_idx];
    if (name.empty())
        name = pos.position->name[plural_idx];
    return name;
}

std::vector<std::string> unit_overview_position_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (Units::isCitizen(unit))
        lines.push_back("Citizen");
    else if (Units::isOwnGroup(unit))
        lines.push_back("Fort controlled");
    else if (Units::isTame(unit))
        lines.push_back("Tame");

    std::vector<Units::NoblePosition> positions;
    if (Units::getNoblePositions(&positions, unit)) {
        for (const auto& pos : positions) {
            auto label = noble_position_label(pos, unit);
            if (!label.empty())
                lines.push_back(label);
            if (lines.size() >= 6)
                break;
        }
    }
    if (lines.size() == (Units::isCitizen(unit) || Units::isOwnGroup(unit) || Units::isTame(unit) ? 1u : 0u))
        lines.push_back("No official position");
    return lines;
}

std::vector<std::string> unit_overview_squad_lines(df::unit* unit) {
    std::vector<std::string> lines;
    if (unit->military.squad_id == -1) {
        lines.push_back("Squad: None");
        return lines;
    }
    auto squad = df::squad::find(unit->military.squad_id);
    if (!squad) {
        lines.push_back("Squad id: " + std::to_string(unit->military.squad_id));
        return lines;
    }
    std::string name = squad->alias;
    if (name.empty())
        name = Translation::translateName(&squad->name, true);
    if (name.empty())
        name = "Squad " + std::to_string(squad->id);
    lines.push_back("Squad: " + name);
    if (unit->military.squad_position >= 0)
        lines.push_back("Position: " + std::to_string(unit->military.squad_position + 1));
    return lines;
}

std::vector<std::string> unit_overview_skill_lines(const std::vector<std::string>& skill_lines) {
    std::vector<std::string> lines;
    for (const auto& line : skill_lines) {
        if (line == "No notable skills.")
            continue;
        lines.push_back(line);
        if (lines.size() >= 6)
            break;
    }
    return lines;
}

std::vector<std::string> unit_overview_need_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (!soul)
        return lines;
    std::vector<std::pair<int32_t, std::string>> ranked;
    for (auto need : soul->personality.needs) {
        if (!need || need->focus_level >= 0)
            continue;
        auto key = pretty_key(DFHack::enum_item_key(need->id));
        ranked.emplace_back(need->focus_level, "Unmet need: " + key);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : ranked) {
        lines.push_back(entry.second);
        if (lines.size() >= 7)
            break;
    }
    return lines;
}

std::string relation_for_subthought(int32_t subthought) {
    switch (subthought) {
    case 0: return "pet";
    case 1: return "spouse";
    case 2: return "mother";
    case 3: return "father";
    case 9: return "lover";
    case 11: return "sibling";
    case 12: return "child";
    case 13: return "friend";
    case 14: return "acquaintance";
    case 18: return "animal training partner";
    default: return "acquaintance";
    }
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string emotion_name(df::emotion_type emotion) {
    if (!df::enum_traits<df::emotion_type>::is_valid(static_cast<int32_t>(emotion)) ||
        emotion == df::emotion_type::ANYTHING)
        return "emotion";
    return pretty_key(DFHack::enum_item_key(emotion));
}

std::string thought_phrase(df::unit* unit, df::unit_thought_type thought, int32_t subthought) {
    std::string text;
    if (df::enum_traits<df::unit_thought_type>::is_valid(static_cast<int32_t>(thought))) {
        const char* caption = df::enum_traits<df::unit_thought_type>::attrs(thought).caption;
        if (caption)
            text = caption;
    }
    if (text.empty() || text == "[multiple]")
        text = pretty_key(DFHack::enum_item_key(thought));

    replace_all(text, "[relation]", relation_for_subthought(subthought));
    replace_all(text, "[somebody]", "somebody");
    replace_all(text, "[a baby]", "a baby");
    replace_all(text, "[animal]", "an animal");
    replace_all(text, "[vermin]", "vermin");
    replace_all(text, "[deity]", "a deity");
    replace_all(text, "[skill]", "a skill");
    replace_all(text, "[quality]", "quality");
    replace_all(text, "[building]", "building");
    replace_all(text, "[relative]", "relative");
    replace_all(text, "[research]", "research");
    replace_all(text, "[topic]", "a topic");
    replace_all(text, "[book]", "a book");
    replace_all(text, "[his]", pronoun_possessive(unit));
    replace_all(text, "[he]", pronoun_subject(unit));
    return text;
}

std::string emotion_memory_line(df::unit* unit, df::emotion_type emotion,
                                df::unit_thought_type thought, int32_t subthought,
                                bool current) {
    auto emotion_text = emotion_name(emotion);
    std::transform(emotion_text.begin(), emotion_text.end(), emotion_text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    auto phrase = thought_phrase(unit, thought, subthought);
    std::string out = pronoun_subject(unit) + (current ? " feels " : " felt ") + emotion_text;
    if (!phrase.empty())
        out += " " + phrase;
    out += ".";
    return out;
}

bool valid_memory(const df::unit_emotion_memory& memory) {
    return memory.type != df::emotion_type::ANYTHING &&
           memory.thought != df::unit_thought_type::None &&
           df::enum_traits<df::emotion_type>::is_valid(static_cast<int32_t>(memory.type)) &&
           df::enum_traits<df::unit_thought_type>::is_valid(static_cast<int32_t>(memory.thought));
}

std::vector<std::string> unit_recent_feeling_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (!soul)
        return lines;

    for (auto mood : soul->personality.emotions) {
        if (!mood || mood->type == df::emotion_type::ANYTHING ||
            mood->thought == df::unit_thought_type::None)
            continue;
        lines.push_back(emotion_memory_line(unit, mood->type, mood->thought, mood->subthought, true));
        if (lines.size() >= 6)
            break;
    }

    if (auto memories = soul->personality.memories) {
        for (const auto& memory : memories->shortterm) {
            if (!valid_memory(memory))
                continue;
            lines.push_back(emotion_memory_line(unit, memory.type, memory.thought, memory.subthought, false));
            if (lines.size() >= 16)
                break;
        }
        for (const auto& memory : memories->longterm) {
            if (lines.size() >= 16 || !valid_memory(memory))
                continue;
            lines.push_back(emotion_memory_line(unit, memory.type, memory.thought, memory.subthought, false));
        }
    }
    return lines;
}

std::vector<std::string> unit_thought_lines(df::unit* unit) {
    std::vector<std::string> lines;
    lines = unit_recent_feeling_lines(unit);
    if (!lines.empty())
        return lines;

    auto soul = unit->status.current_soul;
    if (soul) {
        lines.push_back("Stress: " + std::to_string(soul->personality.stress));
        lines.push_back("Long-term stress: " + std::to_string(soul->personality.longterm_stress));
        lines.push_back("Current focus: " + std::to_string(soul->personality.current_focus));
        if (soul->personality.dreams.empty()) {
            lines.push_back("No dreams recorded.");
        } else {
            for (auto dream : soul->personality.dreams) {
                if (!dream)
                    continue;
                auto name = pretty_key(DFHack::enum_item_key(dream->type));
                lines.push_back(std::string("Dream: ") + name);
                if (lines.size() >= 12)
                    break;
            }
        }
    }
    if (lines.empty())
        lines.push_back("No thought data for this unit.");
    return lines;
}

void add_mental_attribute_line(std::vector<std::string>& lines, df::unit* unit,
                               df::mental_attribute_type attr,
                               const char* high_text, const char* low_text) {
    int value = mental_attr_value(unit, attr);
    if (value >= mental_attr_high_threshold(unit, attr))
        lines.push_back(pronoun_subject(unit) + " has " + high_text + ".");
    else if (value < mental_attr_low_threshold(unit, attr))
        lines.push_back(pronoun_subject(unit) + " has " + low_text + ".");
}

void add_trait_line(std::vector<std::string>& lines, df::unit* unit,
                    df::personality_facet_type facet,
                    const char* high_text, const char* low_text) {
    if (trait_high(unit, facet))
        lines.push_back(pronoun_subject(unit) + " " + high_text + ".");
    else if (trait_low(unit, facet))
        lines.push_back(pronoun_subject(unit) + " " + low_text + ".");
}

std::vector<std::string> unit_personality_trait_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (soul) {
        using namespace df::enums::mental_attribute_type;
        add_mental_attribute_line(lines, unit, EMPATHY, "a great sense of empathy", "a poor sense of empathy");
        add_mental_attribute_line(lines, unit, KINESTHETIC_SENSE, "a good kinesthetic sense", "a poor kinesthetic sense");
        add_mental_attribute_line(lines, unit, LINGUISTIC_ABILITY, "a way with words", "little facility with words");
        add_mental_attribute_line(lines, unit, SOCIAL_AWARENESS, "a good sense for social relationships", "a meager ability with social relationships");
        add_mental_attribute_line(lines, unit, MEMORY, "a good memory", "a poor memory");
        add_mental_attribute_line(lines, unit, MUSICALITY, "natural musical ability", "no natural musical ability");

        using namespace df::enums::personality_facet_type;
        add_trait_line(lines, unit, DEPRESSION_PROPENSITY, "often feels discouraged", "rarely feels discouraged");
        add_trait_line(lines, unit, ANGER_PROPENSITY, "is quick to anger", "is slow to anger");
        add_trait_line(lines, unit, ANXIETY_PROPENSITY, "tends to worry", "is calm under pressure");
        add_trait_line(lines, unit, ORDERLINESS, "likes to keep things orderly", "does not mind a little clutter");
        add_trait_line(lines, unit, FRIENDLINESS, "is a friendly individual", "keeps others at arm's length");
        add_trait_line(lines, unit, GREGARIOUSNESS, "enjoys forming deep emotional bonds", "tends to keep away from others");
        add_trait_line(lines, unit, CURIOUS, "is curious and eager to learn", "is not very curious");
        add_trait_line(lines, unit, ART_INCLINED, "is moved by art and natural beauty", "does not care about art or natural beauty");
        add_trait_line(lines, unit, IMAGINATION, "has a vivid imagination", "has little imagination");
        add_trait_line(lines, unit, PERSEVERANCE, "has strong willpower", "gives up easily");
    }
    if (lines.empty())
        lines.push_back("No notable personality traits.");
    return lines;
}

std::vector<std::string> unit_personality_value_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (soul) {
        for (auto value : soul->personality.values) {
            if (!value)
                continue;
            lines.push_back(pretty_key(DFHack::enum_item_key(value->type)) +
                            ": " + std::to_string(value->strength));
            if (lines.size() >= 16)
                break;
        }
    }
    if (lines.empty())
        lines.push_back("No values recorded.");
    return lines;
}

std::vector<std::string> unit_personality_preference_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (soul) {
        for (auto pref : soul->personality.preferences) {
            if (!pref)
                continue;
            std::string type = pretty_key(DFHack::enum_item_key(pref->type));
            lines.push_back(type + " preference: " + std::to_string(pref->var1) + ", " +
                            std::to_string(pref->var2) + ", " + std::to_string(pref->var3) + ", " +
                            std::to_string(pref->var4));
            if (lines.size() >= 16)
                break;
        }
    }
    if (lines.empty())
        lines.push_back("No preferences recorded.");
    return lines;
}

std::string need_level_label(int32_t level) {
    if (level <= 1)
        return "Slight";
    if (level < 5)
        return "Moderate";
    if (level < 10)
        return "Strong";
    return "Intense";
}

std::string need_focus_label(int32_t focus) {
    if (focus >= 300)
        return "satisfied";
    if (focus >= 100)
        return "content";
    if (focus >= 0)
        return "distracting";
    return "unfulfilled";
}

std::vector<std::string> unit_personality_need_lines(df::unit* unit) {
    std::vector<std::string> lines;
    auto soul = unit->status.current_soul;
    if (soul) {
        for (auto need : soul->personality.needs) {
            if (!need)
                continue;
            lines.push_back(need_level_label(need->need_level) + " need to " +
                            pretty_key(DFHack::enum_item_key(need->id)) +
                            " (" + need_focus_label(need->focus_level) +
                            ", focus " + std::to_string(need->focus_level) + ")");
            if (lines.size() >= 16)
                break;
        }
    }
    if (lines.empty())
        lines.push_back("No needs recorded.");
    return lines;
}

UnitSheet build_unit_sheet(df::unit* unit) {
    UnitSheet sheet;
    if (!unit)
        return sheet;

    sheet.present = true;
    sheet.id = unit->id;
    sheet.portrait_texpos = unit->portrait_texpos;
    sheet.sheet_icon_texpos = unit->sheet_icon_texpos;
    sheet.name = Units::getReadableName(unit);
    if (sheet.name.empty())
        sheet.name = Units::getRaceReadableName(unit);
    sheet.race = Units::getRaceReadableName(unit);
    sheet.profession = Units::getProfessionName(unit);
    sheet.current_job = unit_current_job_label(unit);
    sheet.age = unit_age_label(unit);
    sheet.sex = unit_sex_label(unit);
    sheet.status_lines = unit_condition_lines(unit);
    sheet.status = sheet.status_lines.empty() ? "Healthy" : sheet.status_lines.front();
    sheet.training = unit_training_label(unit);
    sheet.body_summary = unit->body.wounds.empty() ? "No health problems" :
        std::to_string(unit->body.wounds.size()) + (unit->body.wounds.size() == 1 ? " wound" : " wounds");
    sheet.inventory_lines = unit_inventory_lines(unit);
    sheet.health_lines = unit_health_lines(unit, sheet.status_lines);
    sheet.health_status_lines = sheet.health_lines;
    sheet.health_wound_lines = unit_health_wound_lines(unit);
    sheet.health_treatment_lines = unit_health_treatment_lines(unit);
    sheet.health_history_lines = unit_health_history_lines(unit);
    sheet.health_description_lines = unit_health_description_lines(unit);
    sheet.skill_lines = unit_skill_lines(unit);
    sheet.room_lines = unit_room_lines(unit);
    sheet.room_assignment_lines = sheet.room_lines;
    sheet.labor_lines = unit_labor_lines(unit);
    sheet.labor_work_detail_lines = unit_labor_work_detail_lines(unit);
    sheet.labor_workshop_lines = unit_labor_workshop_lines(unit);
    sheet.labor_location_lines = unit_labor_location_lines(unit);
    sheet.labor_work_animal_lines = unit_labor_work_animal_lines(unit);
    sheet.relation_lines = unit_relation_lines(unit);
    sheet.group_lines = unit_group_lines(unit);
    sheet.military_lines = unit_military_lines(unit);
    sheet.military_squad_lines = sheet.military_lines;
    sheet.military_uniform_lines = unit_military_uniform_lines(unit);
    sheet.military_kill_lines = unit_military_kill_lines(unit);
    sheet.thought_lines = unit_thought_lines(unit);
    sheet.personality_trait_lines = unit_personality_trait_lines(unit);
    sheet.personality_value_lines = unit_personality_value_lines(unit);
    sheet.personality_preference_lines = unit_personality_preference_lines(unit);
    sheet.personality_need_lines = unit_personality_need_lines(unit);
    sheet.personality_lines = sheet.personality_trait_lines;
    sheet.overview_relation_lines = unit_overview_relation_lines(unit);
    sheet.overview_trait_lines = unit_overview_trait_lines(unit, sheet.status_lines);
    sheet.overview_position_lines = unit_overview_position_lines(unit);
    sheet.overview_squad_lines = unit_overview_squad_lines(unit);
    sheet.overview_skill_lines = unit_overview_skill_lines(sheet.skill_lines);
    sheet.overview_need_lines = unit_overview_need_lines(unit);
    sheet.overview_memory_lines = sheet.thought_lines;

    if (Units::isCitizen(unit))
        sheet.flags.push_back("Citizen");
    if (Units::isTame(unit))
        sheet.flags.push_back("Tame");
    if (Units::isPet(unit))
        sheet.flags.push_back("Pet");
    if (Units::isAvailableForAdoption(unit))
        sheet.flags.push_back("Available for adoption");
    if (Units::isMarkedForSlaughter(unit))
        sheet.flags.push_back("Marked for slaughter");
    if (Units::isMarkedForGelding(unit))
        sheet.flags.push_back("Marked for gelding");

    auto training = find_training_assignment(unit);
    bool has_trainer = training && training->trainer_id != -1;
    sheet.actions.push_back({"Ctrl+b", "Butcher", yes_no(Units::isMarkedForSlaughter(unit)), true});
    sheet.actions.push_back({"Ctrl+g", "Geld", yes_no(Units::isMarkedForGelding(unit)), Units::isGeldable(unit)});
    sheet.actions.push_back({"Ctrl+a", "Adopt", yes_no(Units::isAvailableForAdoption(unit)), Units::isTame(unit)});
    sheet.actions.push_back({"Ctrl+t", "Has Trainer", yes_no(has_trainer), Units::isMarkedForTraining(unit)});
    return sheet;
}

std::string unit_sheet_json(const std::string& player,
                            const UnitSheet& unit,
                            const Camera& tile) {
    std::ostringstream body;
    body << "{"
         << "\"player\":" << json_string(player) << ","
         << "\"kind\":\"unit\","
         << "\"title\":" << json_string(unit.name) << ","
         << "\"tile\":{\"x\":" << tile.x << ",\"y\":" << tile.y << ",\"z\":" << tile.z << "},"
         << "\"unit\":";
    append_unit_sheet_json(body, unit);
    body << "}\n";
    return body.str();
}

bool unit_sheet_on_render_thread(int32_t unit_id,
                                 UnitSheet& unit,
                                 Camera& tile,
                                 std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_unit_sheet_mutex);

    auto request = std::make_shared<RenderThreadUnitRequest>();
    request->unit_id = unit_id;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        try {
            auto found = df::unit::find(request->unit_id);
            if (!found) {
                request->err = "unit not found";
                request->done.set_value(false);
                return;
            }
            request->unit = build_unit_sheet(found);
            request->tile = Camera{found->pos.x, found->pos.y, found->pos.z};
            request->done.set_value(request->unit.present);
        } catch (const std::exception& ex) {
            request->err = ex.what();
            request->done.set_value(false);
        } catch (...) {
            request->err = "unknown unit sheet error";
            request->done.set_value(false);
        }
    });

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err.empty() ? "unit sheet failed" : request->err;
        return false;
    }
    unit = std::move(request->unit);
    tile = request->tile;
    return true;
}

} // namespace dfcapture
