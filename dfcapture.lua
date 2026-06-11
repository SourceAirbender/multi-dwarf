-- Companion Lua module for the dfcapture plugin.
--
-- The C++ side handles HTTP + premium frame capture; the more intricate game-state
-- mutations (creating stockpiles, placing buildings with materials) go through DFHack's
-- tested high-level APIs here, which is far less error-prone than replicating the
-- raws-dependent logic in C++. Called from C++ via Lua::CallLuaModuleFunction.

local _ENV = mkmodule('plugins.dfcapture')

-- Trivial round-trip used to verify the C++ -> Lua bridge in isolation.
function ping(n)
    return (n or 0) + 1
end

-- Maps the browser's preset names to DFHack's shipped stockpile library presets
-- (data/stockpiles/*.dfstock). "all" accepts everything; cat_* accept one category.
local STOCKPILE_PRESETS = {
    all = 'all', everything = 'all',
    food = 'cat_food', stone = 'cat_stone', wood = 'cat_wood',
    furniture = 'cat_furniture', finished = 'cat_finished_goods',
    bars = 'cat_bars_blocks', gems = 'cat_gems', cloth = 'cat_cloth',
    leather = 'cat_leather', ammo = 'cat_ammo', armor = 'cat_armor',
    weapons = 'cat_weapons', animals = 'cat_animals', corpses = 'cat_corpses',
    refuse = 'cat_refuse', coins = 'cat_coins', sheets = 'cat_sheets',
}

local function get_stockpile(id)
    local b = df.building.find(id)
    if b and b:getType() == df.building_type.Stockpile then return b end
    return nil
end

-- Change what a stockpile accepts: 'none' clears it; otherwise apply a category preset.
-- mode can be 'set' (replace), 'enable', or 'disable'. Returns (ok, err).
function stockpile_set_preset(id, preset, mode)
    local b = get_stockpile(id)
    if not b then return false, 'not a stockpile' end
    preset = tostring(preset or 'all'):lower()
    if preset == 'none' then
        b.settings.flags.whole = 0
        return true, ''
    end
    mode = tostring(mode or 'set'):lower()
    if mode ~= 'set' and mode ~= 'enable' and mode ~= 'disable' then
        mode = 'set'
    end
    local lib = STOCKPILE_PRESETS[preset] or preset
    local ok, err = pcall(function()
        require('plugins.stockpiles').import_settings(lib, {id = id, mode = mode})
    end)
    if not ok then return false, tostring(err) end
    return true, ''
end

-- ===== Custom stockpile item editor (DF-style: category -> sub-groups -> per-item toggles) =====
-- A category has a group flag + one or more sub-groups (DF's middle column). Each group maps to the
-- exact stockpile_settings field that native DF and DFHack's StockpileSerializer use. Some groups are
-- raw vectors (e.g. finished_goods.mats indexed by inorganic raws), while others are fixed arrays or
-- scalar bool fields (usable/unusable, dyed/undyed). Keep this table data-driven so adding another
-- native middle-column group is a local change.
local function sp_stone_allowed(inorg)
    local f = inorg.flags
    local soil = f.SOIL and not f.AQUIFER
    local mf = inorg.material.flags
    return soil or (mf.IS_STONE and not mf.NO_STONE_STOCKPILE)
end
local function sp_is_ore(inorg)
    local ok, n = pcall(function() return #inorg.metal_ore.mat_index end)
    return ok and n and n > 0
end
local function sp_is_soil(inorg) return inorg.flags.SOIL and not inorg.flags.AQUIFER end
local function sp_stone_name(m)
    local ok, s = pcall(function() return m.material.state_name.Solid end)
    if ok and s and #s > 0 then return s end
    return m.id
end
local function sp_stone_vec(b) return b.settings.stone.mats end
local function sp_inorganics() return df.global.world.raws.inorganics.all end
local function sp_any(v) return v ~= nil end
local function sp_bool(v) return v == true or v == 1 or tostring(v) == 'true' or tostring(v) == '1' end
local function sp_ensure_vec(vec, n) while #vec < n do vec:insert('#', 0) end end
local SP_QUALITIES
local function sp_title_token(s)
    s = tostring(s or '')
    s = s:gsub('_', ' '):gsub('(%l)(%u)', '%1 %2'):lower()
    return (s:gsub('^%l', string.upper))
end
local function sp_material_name(m)
    if not m then return '' end
    local ok, s = pcall(function() return m.material.state_name.Solid end)
    if ok and s and #s > 0 then return s end
    return sp_title_token(m.id or '')
end
local function sp_itemdef_name(d)
    if not d then return '' end
    return tostring((d.name_plural and #d.name_plural > 0 and d.name_plural) or
        (d.name and #d.name > 0 and d.name) or d.id or '')
end
local function sp_creature_name(c)
    if not c then return '' end
    local ok, s = pcall(function() return c.name[1] end)
    if ok and s and #s > 0 then return s end
    ok, s = pcall(function() return c.name[0] end)
    if ok and s and #s > 0 then return s end
    return sp_title_token(c.creature_id or c.id or '')
end
local function sp_color_name(c)
    if not c then return '' end
    return tostring((c.name and #c.name > 0 and c.name) or c.id or '')
end
local function sp_enum_list(entries)
    local t = {n = 0}
    for _, e in ipairs(entries) do
        local idx = tonumber(e[1])
        if idx and idx >= 0 then
            t[idx] = {name = e[2], token = e[3] or e[2]}
            if idx + 1 > t.n then t.n = idx + 1 end
        end
    end
    return t
end
local function sp_collection_count(raws)
    if type(raws) == 'table' and raws.n then return raws.n end
    return #raws
end
local function sp_collection_get(raws, idx) return raws[idx] end
local function sp_entry_name(e) return e and e.name or '' end
local function sp_bool_entries(entries)
    local list = {n = #entries}
    for i, e in ipairs(entries) do list[i - 1] = e end
    return list
end
local function sp_bool_group(key, label, entries)
    local list = sp_bool_entries(entries)
    return {
        key = key, label = label,
        raws = function() return list end,
        include = sp_any,
        name = sp_entry_name,
        get = function(b, i) return list[i].get(b) end,
        set = function(b, i, on) list[i].set(b, on) end,
    }
end
local function sp_vec_group(key, label, vec, raws, include, name, fixed)
    return {
        key = key, label = label, vec = vec, raws = raws,
        include = include or sp_any, name = name or sp_entry_name, fixed = fixed,
    }
end
local function sp_inorganic_group(key, label, vec, include)
    return sp_vec_group(key, label, vec, sp_inorganics, include, sp_material_name)
end
local function sp_quality_group(key, label, vec)
    return sp_vec_group(key, label, vec, function() return SP_QUALITIES end, sp_any, sp_entry_name, true)
end
local function sp_color_group(vec)
    return sp_vec_group('color', 'Color', vec, function() return df.global.world.raws.descriptors.colors end, sp_any, sp_color_name)
end
local function sp_organic_material_name(cat, mat_type, mat_index)
    if cat == df.organic_mat_category.Fish or
       cat == df.organic_mat_category.UnpreparedFish or
       cat == df.organic_mat_category.Eggs then
        local cr = df.global.world.raws.creatures.all[mat_type]
        local caste = cr and cr.caste and cr.caste[mat_index] or nil
        local ok, s = pcall(function() return caste.caste_name[0] end)
        if ok and s and #s > 0 then return s end
        ok, s = pcall(function() return caste.caste_name[1] end)
        if ok and s and #s > 0 then return s end
        return sp_creature_name(cr)
    end
    local ok, info = pcall(dfhack.matinfo.decode, mat_type, mat_index)
    if ok and info then
        local okn, name = pcall(function() return info.material.state_name.Solid end)
        if okn and name and #name > 0 then return name end
        okn, name = pcall(function() return info:toString() end)
        if okn and name and #name > 0 then return name end
    end
    return ('material %s:%s'):format(tostring(mat_type), tostring(mat_index))
end
local SP_ORGANIC_RAW_CACHE = {}
local function sp_organic_raws(cat)
    cat = tonumber(cat)
    if not cat then return {n = 0} end
    if SP_ORGANIC_RAW_CACHE[cat] then return SP_ORGANIC_RAW_CACHE[cat] end
    local mt = df.global.world.raws.mat_table
    local types = mt.organic_types[cat]
    local indexes = mt.organic_indexes[cat]
    local n = types and #types or 0
    local list = {n = n}
    for i = 0, n - 1 do
        list[i] = {
            name = sp_organic_material_name(cat, types[i], indexes[i]),
            mat_type = types[i],
            mat_index = indexes[i],
        }
    end
    SP_ORGANIC_RAW_CACHE[cat] = list
    return list
end
local function sp_organic_group(key, label, vec, cat)
    return sp_vec_group(key, label, vec, function() return sp_organic_raws(cat) end, sp_any, sp_entry_name)
end
local function sp_metal(m)
    return m and m.material and m.material.flags and m.material.flags.IS_METAL
end
local function sp_metal_or_stone(m)
    return m and m.material and m.material.flags and (m.material.flags.IS_METAL or m.material.flags.IS_STONE)
end
local function sp_gem(m)
    return m and m.material and m.material.flags and m.material.flags.IS_GEM
end
local function sp_finished_mat(m)
    return m and m.material and m.material.flags and
        (m.material.flags.IS_GEM or m.material.flags.IS_METAL or m.material.flags.IS_STONE)
end

SP_QUALITIES = sp_enum_list({
    {0, 'Ordinary'}, {1, 'Well-crafted'}, {2, 'Finely-crafted'}, {3, 'Superior'},
    {4, 'Exceptional'}, {5, 'Masterful'}, {6, 'Artifact'},
})
local SP_OTHER_MATS_FURNITURE = sp_enum_list({
    {0, 'Wood'}, {1, 'Plant cloth'}, {2, 'Bone'}, {3, 'Tooth'}, {4, 'Horn'},
    {5, 'Pearl'}, {6, 'Shell'}, {7, 'Leather'}, {8, 'Silk'}, {9, 'Amber'},
    {10, 'Coral'}, {11, 'Green glass'}, {12, 'Clear glass'}, {13, 'Crystal glass'}, {14, 'Yarn'},
})
local SP_OTHER_MATS_FINISHED = sp_enum_list({
    {0, 'Wood'}, {1, 'Plant cloth'}, {2, 'Bone'}, {3, 'Tooth'}, {4, 'Horn'},
    {5, 'Pearl'}, {6, 'Shell'}, {7, 'Leather'}, {8, 'Silk'}, {9, 'Amber'},
    {10, 'Coral'}, {11, 'Green glass'}, {12, 'Clear glass'}, {13, 'Crystal glass'}, {14, 'Yarn'}, {15, 'Wax'},
})
local SP_OTHER_MATS_WEAPON_ARMOR = sp_enum_list({
    {0, 'Wood'}, {1, 'Plant cloth'}, {2, 'Bone'}, {3, 'Shell'}, {4, 'Leather'},
    {5, 'Silk'}, {6, 'Green glass'}, {7, 'Clear glass'}, {8, 'Crystal glass'}, {9, 'Yarn'},
})
local SP_BAR_OTHER_MATS = sp_enum_list({
    {0, 'Coal'}, {1, 'Potash'}, {2, 'Ash'}, {3, 'Pearlash'}, {4, 'Soap'},
})
local SP_BLOCK_OTHER_MATS = sp_enum_list({
    {0, 'Green glass'}, {1, 'Clear glass'}, {2, 'Crystal glass'}, {3, 'Wood'},
})
local SP_AMMO_OTHER_MATS = sp_enum_list({{0, 'Wood'}, {1, 'Bone'}})
local SP_FINISHED_TYPES = sp_enum_list({
    {10, 'Chains'}, {11, 'Flasks'}, {12, 'Goblets'}, {13, 'Musical instruments'}, {14, 'Toys'},
    {25, 'Armor'}, {26, 'Shoes'}, {28, 'Helms'}, {29, 'Gloves'}, {36, 'Figurines'},
    {37, 'Amulets'}, {38, 'Scepters'}, {40, 'Crowns'}, {41, 'Rings'}, {42, 'Earrings'},
    {43, 'Bracelets'}, {44, 'Large gems'}, {59, 'Totems'}, {60, 'Pants'}, {61, 'Backpacks'},
    {62, 'Quivers'}, {82, 'Splints'}, {83, 'Crutches'}, {86, 'Tools'}, {89, 'Books'},
})
local SP_FURNITURE_TYPES = sp_enum_list({
    {0, 'Floodgates'}, {1, 'Hatch covers'}, {2, 'Grates'}, {3, 'Doors'}, {4, 'Catapult parts'},
    {5, 'Ballista parts'}, {6, 'Trap components'}, {7, 'Beds'}, {8, 'Traction benches'}, {9, 'Windows'},
    {10, 'Chairs'}, {11, 'Tables'}, {12, 'Coffins'}, {13, 'Statues'}, {14, 'Slabs'},
    {15, 'Querns'}, {16, 'Millstones'}, {17, 'Armor stands'}, {18, 'Weapon racks'}, {19, 'Cabinets'},
    {20, 'Anvils'}, {21, 'Buckets'}, {22, 'Bins'}, {23, 'Boxes'}, {24, 'Bags'},
    {25, 'Siege ammo'}, {26, 'Barrels'}, {27, 'Ballista arrowheads'}, {28, 'Pipe sections'},
    {29, 'Large pots'}, {30, 'Minecarts'}, {31, 'Wheelbarrows'}, {32, 'Other large tools'},
    {33, 'Sand bags'}, {34, 'Bolt thrower parts'},
})
local SP_REFUSE_TYPES = sp_enum_list({
    {23, 'Corpses'}, {46, 'Body parts'}, {47, 'Vermin remains'}, {55, 'Tanned hides'},
})

local SP_CATEGORIES = {
    ammo = {
        label = 'Ammo', flag = 'ammo',
        groups = {
            sp_vec_group('type', 'Ammo type', function(b) return b.settings.ammo.type end,
                function() return df.global.world.raws.itemdefs.ammo end, sp_any, sp_itemdef_name),
            sp_inorganic_group('mats', 'Metal', function(b) return b.settings.ammo.mats end, sp_metal),
            sp_vec_group('other', 'Other materials', function(b) return b.settings.ammo.other_mats end,
                function() return SP_AMMO_OTHER_MATS end),
            sp_quality_group('core', 'Core quality', function(b) return b.settings.ammo.quality_core end),
            sp_quality_group('total', 'Total quality', function(b) return b.settings.ammo.quality_total end),
        },
    },
    animals = {
        label = 'Animals', flag = 'animals',
        groups = {
            sp_bool_group('empty', 'Empty cages/traps', {
                {name='Empty cages', get=function(b) return b.settings.animals.empty_cages end,
                    set=function(b, on) b.settings.animals.empty_cages = sp_bool(on) end},
                {name='Empty animal traps', get=function(b) return b.settings.animals.empty_traps end,
                    set=function(b, on) b.settings.animals.empty_traps = sp_bool(on) end},
            }),
            sp_vec_group('creatures', 'Creatures', function(b) return b.settings.animals.enabled end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
        },
    },
    armor = {
        label = 'Armor', flag = 'armor',
        groups = {
            sp_vec_group('body', 'Body', function(b) return b.settings.armor.body end,
                function() return df.global.world.raws.itemdefs.armor end, sp_any, sp_itemdef_name),
            sp_vec_group('head', 'Head', function(b) return b.settings.armor.head end,
                function() return df.global.world.raws.itemdefs.helms end, sp_any, sp_itemdef_name),
            sp_vec_group('feet', 'Feet', function(b) return b.settings.armor.feet end,
                function() return df.global.world.raws.itemdefs.shoes end, sp_any, sp_itemdef_name),
            sp_vec_group('hands', 'Hands', function(b) return b.settings.armor.hands end,
                function() return df.global.world.raws.itemdefs.gloves end, sp_any, sp_itemdef_name),
            sp_vec_group('legs', 'Legs', function(b) return b.settings.armor.legs end,
                function() return df.global.world.raws.itemdefs.pants end, sp_any, sp_itemdef_name),
            sp_vec_group('shield', 'Shield', function(b) return b.settings.armor.shield end,
                function() return df.global.world.raws.itemdefs.shields end, sp_any, sp_itemdef_name),
            sp_inorganic_group('mats', 'Metal', function(b) return b.settings.armor.mats end, sp_metal),
            sp_vec_group('other', 'Other materials', function(b) return b.settings.armor.other_mats end,
                function() return SP_OTHER_MATS_WEAPON_ARMOR end),
            sp_quality_group('core', 'Core quality', function(b) return b.settings.armor.quality_core end),
            sp_quality_group('total', 'Total quality', function(b) return b.settings.armor.quality_total end),
            sp_color_group(function(b) return b.settings.armor.color end),
            sp_bool_group('use', 'Usability', {
                {name='Usable armor', get=function(b) return b.settings.armor.usable end,
                    set=function(b, on) b.settings.armor.usable = sp_bool(on) end},
                {name='Unusable armor', get=function(b) return b.settings.armor.unusable end,
                    set=function(b, on) b.settings.armor.unusable = sp_bool(on) end},
            }),
            sp_bool_group('dye', 'Dye', {
                {name='Dyed', get=function(b) return b.settings.armor.dyed end,
                    set=function(b, on) b.settings.armor.dyed = sp_bool(on) end},
                {name='Undyed', get=function(b) return b.settings.armor.undyed end,
                    set=function(b, on) b.settings.armor.undyed = sp_bool(on) end},
            }),
        },
    },
    bars = {
        label = 'Bars/blocks', flag = 'bars_blocks',
        groups = {
            sp_inorganic_group('bars_mats', 'Metal bars', function(b) return b.settings.bars_blocks.bars_mats end, sp_metal),
            sp_vec_group('bars_other', 'Other bars', function(b) return b.settings.bars_blocks.bars_other_mats end,
                function() return SP_BAR_OTHER_MATS end),
            sp_inorganic_group('blocks_mats', 'Metal/stone blocks', function(b) return b.settings.bars_blocks.blocks_mats end, sp_metal_or_stone),
            sp_vec_group('blocks_other', 'Other blocks', function(b) return b.settings.bars_blocks.blocks_other_mats end,
                function() return SP_BLOCK_OTHER_MATS end),
        },
    },
    cloth = {
        label = 'Cloth', flag = 'cloth',
        groups = {
            sp_organic_group('thread_silk', 'Silk thread', function(b) return b.settings.cloth.thread_silk end, df.organic_mat_category.Silk),
            sp_organic_group('thread_plant', 'Plant thread', function(b) return b.settings.cloth.thread_plant end, df.organic_mat_category.PlantFiber),
            sp_organic_group('thread_yarn', 'Yarn thread', function(b) return b.settings.cloth.thread_yarn end, df.organic_mat_category.Yarn),
            sp_organic_group('thread_metal', 'Metal thread', function(b) return b.settings.cloth.thread_metal end, df.organic_mat_category.MetalThread),
            sp_organic_group('cloth_silk', 'Silk cloth', function(b) return b.settings.cloth.cloth_silk end, df.organic_mat_category.Silk),
            sp_organic_group('cloth_plant', 'Plant cloth', function(b) return b.settings.cloth.cloth_plant end, df.organic_mat_category.PlantFiber),
            sp_organic_group('cloth_yarn', 'Yarn cloth', function(b) return b.settings.cloth.cloth_yarn end, df.organic_mat_category.Yarn),
            sp_organic_group('cloth_metal', 'Metal cloth', function(b) return b.settings.cloth.cloth_metal end, df.organic_mat_category.MetalThread),
            sp_color_group(function(b) return b.settings.cloth.color end),
            sp_bool_group('dye', 'Dye', {
                {name='Dyed', get=function(b) return b.settings.cloth.dyed end,
                    set=function(b, on) b.settings.cloth.dyed = sp_bool(on) end},
                {name='Undyed', get=function(b) return b.settings.cloth.undyed end,
                    set=function(b, on) b.settings.cloth.undyed = sp_bool(on) end},
            }),
        },
    },
    coins = {
        label = 'Coins', flag = 'coins',
        groups = {
            sp_inorganic_group('mats', 'Material', function(b) return b.settings.coins.mats end, sp_any),
        },
    },
    corpses = {
        label = 'Corpses', flag = 'corpses',
        groups = {
            sp_vec_group('creatures', 'Creatures', function(b) return b.settings.corpses.corpses end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
        },
    },
    finished = {
        label = 'Finished goods', flag = 'finished_goods',
        groups = {
            sp_vec_group('type', 'Type', function(b) return b.settings.finished_goods.type end,
                function() return SP_FINISHED_TYPES end),
            sp_inorganic_group('mats', 'Metal/stone/gem', function(b) return b.settings.finished_goods.mats end, sp_finished_mat),
            sp_vec_group('other', 'Other materials', function(b) return b.settings.finished_goods.other_mats end,
                function() return SP_OTHER_MATS_FINISHED end),
            sp_quality_group('core', 'Core quality', function(b) return b.settings.finished_goods.quality_core end),
            sp_quality_group('total', 'Total quality', function(b) return b.settings.finished_goods.quality_total end),
            sp_color_group(function(b) return b.settings.finished_goods.color end),
            sp_bool_group('dye', 'Dye', {
                {name='Dyed', get=function(b) return b.settings.finished_goods.dyed end,
                    set=function(b, on) b.settings.finished_goods.dyed = sp_bool(on) end},
                {name='Undyed', get=function(b) return b.settings.finished_goods.undyed end,
                    set=function(b, on) b.settings.finished_goods.undyed = sp_bool(on) end},
            }),
        },
    },
    food = {
        label = 'Food', flag = 'food',
        groups = {
            sp_bool_group('prepared', 'Prepared meals', {
                {name='Prepared meals', get=function(b) return b.settings.food.prepared_meals end,
                    set=function(b, on) b.settings.food.prepared_meals = sp_bool(on) end},
            }),
            sp_organic_group('meat', 'Meat', function(b) return b.settings.food.meat end, df.organic_mat_category.Meat),
            sp_organic_group('fish', 'Prepared fish', function(b) return b.settings.food.fish end, df.organic_mat_category.Fish),
            sp_organic_group('unprepared_fish', 'Unprepared fish', function(b) return b.settings.food.unprepared_fish end, df.organic_mat_category.UnpreparedFish),
            sp_organic_group('egg', 'Eggs', function(b) return b.settings.food.egg end, df.organic_mat_category.Eggs),
            sp_organic_group('plants', 'Plants', function(b) return b.settings.food.plants end, df.organic_mat_category.Plants),
            sp_organic_group('drink_plant', 'Plant drinks', function(b) return b.settings.food.drink_plant end, df.organic_mat_category.PlantDrink),
            sp_organic_group('drink_animal', 'Animal drinks', function(b) return b.settings.food.drink_animal end, df.organic_mat_category.CreatureDrink),
            sp_organic_group('cheese_plant', 'Plant cheese', function(b) return b.settings.food.cheese_plant end, df.organic_mat_category.PlantCheese),
            sp_organic_group('cheese_animal', 'Animal cheese', function(b) return b.settings.food.cheese_animal end, df.organic_mat_category.CreatureCheese),
            sp_organic_group('seeds', 'Seeds', function(b) return b.settings.food.seeds end, df.organic_mat_category.Seed),
            sp_organic_group('leaves', 'Leaves / growths', function(b) return b.settings.food.leaves end, df.organic_mat_category.PlantGrowth),
            sp_organic_group('powder_plant', 'Plant powder', function(b) return b.settings.food.powder_plant end, df.organic_mat_category.PlantPowder),
            sp_organic_group('powder_creature', 'Animal powder', function(b) return b.settings.food.powder_creature end, df.organic_mat_category.CreaturePowder),
            sp_organic_group('glob', 'Glob', function(b) return b.settings.food.glob end, df.organic_mat_category.Glob),
            sp_organic_group('glob_paste', 'Paste', function(b) return b.settings.food.glob_paste end, df.organic_mat_category.Paste),
            sp_organic_group('glob_pressed', 'Pressed', function(b) return b.settings.food.glob_pressed end, df.organic_mat_category.Pressed),
            sp_organic_group('liquid_plant', 'Plant liquid', function(b) return b.settings.food.liquid_plant end, df.organic_mat_category.PlantLiquid),
            sp_organic_group('liquid_animal', 'Animal liquid', function(b) return b.settings.food.liquid_animal end, df.organic_mat_category.CreatureLiquid),
            sp_organic_group('liquid_misc', 'Misc liquid', function(b) return b.settings.food.liquid_misc end, df.organic_mat_category.MiscLiquid),
        },
    },
    furniture = {
        label = 'Furniture/siege ammo', flag = 'furniture',
        groups = {
            sp_vec_group('type', 'Type', function(b) return b.settings.furniture.type end,
                function() return SP_FURNITURE_TYPES end),
            sp_inorganic_group('mats', 'Metal/stone', function(b) return b.settings.furniture.mats end, sp_metal_or_stone),
            sp_vec_group('other', 'Other materials', function(b) return b.settings.furniture.other_mats end,
                function() return SP_OTHER_MATS_FURNITURE end),
            sp_quality_group('core', 'Core quality', function(b) return b.settings.furniture.quality_core end),
            sp_quality_group('total', 'Total quality', function(b) return b.settings.furniture.quality_total end),
        },
    },
    gems = {
        label = 'Gems', flag = 'gems',
        groups = {
            sp_inorganic_group('rough_mats', 'Rough gems', function(b) return b.settings.gems.rough_mats end, sp_gem),
            sp_inorganic_group('cut_mats', 'Cut gems', function(b) return b.settings.gems.cut_mats end, sp_gem),
        },
    },
    leather = {
        label = 'Leather', flag = 'leather',
        groups = {
            sp_organic_group('mats', 'Leather', function(b) return b.settings.leather.mats end, df.organic_mat_category.Leather),
            sp_color_group(function(b) return b.settings.leather.color end),
            sp_bool_group('dye', 'Dye', {
                {name='Dyed', get=function(b) return b.settings.leather.dyed end,
                    set=function(b, on) b.settings.leather.dyed = sp_bool(on) end},
                {name='Undyed', get=function(b) return b.settings.leather.undyed end,
                    set=function(b, on) b.settings.leather.undyed = sp_bool(on) end},
            }),
        },
    },
    refuse = {
        label = 'Refuse', flag = 'refuse',
        groups = {
            sp_vec_group('type', 'Type', function(b) return b.settings.refuse.type end,
                function() return SP_REFUSE_TYPES end),
            sp_vec_group('corpses', 'Corpses', function(b) return b.settings.refuse.corpses end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('body_parts', 'Body parts', function(b) return b.settings.refuse.body_parts end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('skulls', 'Skulls', function(b) return b.settings.refuse.skulls end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('bones', 'Bones', function(b) return b.settings.refuse.bones end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('hair', 'Hair/wool', function(b) return b.settings.refuse.hair end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('shells', 'Shells', function(b) return b.settings.refuse.shells end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('teeth', 'Teeth', function(b) return b.settings.refuse.teeth end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_vec_group('horns', 'Horns', function(b) return b.settings.refuse.horns end,
                function() return df.global.world.raws.creatures.all end, sp_any, sp_creature_name),
            sp_bool_group('hide', 'Raw hides', {
                {name='Fresh raw hide', get=function(b) return b.settings.refuse.fresh_raw_hide end,
                    set=function(b, on) b.settings.refuse.fresh_raw_hide = sp_bool(on) end},
                {name='Rotten raw hide', get=function(b) return b.settings.refuse.rotten_raw_hide end,
                    set=function(b, on) b.settings.refuse.rotten_raw_hide = sp_bool(on) end},
            }),
        },
    },
    sheets = {
        label = 'Sheets', flag = 'sheet',
        groups = {
            sp_organic_group('paper', 'Paper', function(b) return b.settings.sheet.paper end, df.organic_mat_category.Paper),
            sp_organic_group('parchment', 'Parchment', function(b) return b.settings.sheet.parchment end, df.organic_mat_category.Parchment),
        },
    },
    weapons = {
        label = 'Weapons/trap comps', flag = 'weapons',
        groups = {
            sp_vec_group('weapon_type', 'Weapons', function(b) return b.settings.weapons.weapon_type end,
                function() return df.global.world.raws.itemdefs.weapons end, sp_any, sp_itemdef_name),
            sp_vec_group('trapcomp_type', 'Trap components', function(b) return b.settings.weapons.trapcomp_type end,
                function() return df.global.world.raws.itemdefs.trapcomps end, sp_any, sp_itemdef_name),
            sp_inorganic_group('mats', 'Metal/stone', function(b) return b.settings.weapons.mats end, sp_metal_or_stone),
            sp_vec_group('other', 'Other materials', function(b) return b.settings.weapons.other_mats end,
                function() return SP_OTHER_MATS_WEAPON_ARMOR end),
            sp_quality_group('core', 'Core quality', function(b) return b.settings.weapons.quality_core end),
            sp_quality_group('total', 'Total quality', function(b) return b.settings.weapons.quality_total end),
            sp_bool_group('use', 'Usability', {
                {name='Usable weapons', get=function(b) return b.settings.weapons.usable end,
                    set=function(b, on) b.settings.weapons.usable = sp_bool(on) end},
                {name='Unusable weapons', get=function(b) return b.settings.weapons.unusable end,
                    set=function(b, on) b.settings.weapons.unusable = sp_bool(on) end},
            }),
        },
    },
    wood = {
        label = 'Wood', flag = 'wood',
        groups = {
            sp_vec_group('trees', 'Trees', function(b) return b.settings.wood.mats end,
                function() return df.global.world.raws.plants.all end,
                function(p) return p.flags and p.flags.TREE end,
                function(p) return p.name end),
        },
    },
    stone = {
        label = 'Stone', flag = 'stone',
        groups = {
            { key = 'ores',  label = 'Metal ores',  vec = sp_stone_vec, raws = sp_inorganics,
              include = function(i) return sp_stone_allowed(i) and sp_is_ore(i) end,  name = sp_stone_name },
            { key = 'other', label = 'Other stone', vec = sp_stone_vec, raws = sp_inorganics,
              include = function(i) return sp_stone_allowed(i) and not sp_is_ore(i) and not sp_is_soil(i) end, name = sp_stone_name },
            { key = 'soil',  label = 'Soil / clay', vec = sp_stone_vec, raws = sp_inorganics,
              include = function(i) return sp_is_soil(i) end, name = sp_stone_name },
        },
    },
}

local function sp_group_count(g) return sp_collection_count(g.raws()) end
local function sp_group_item(g, idx) return sp_collection_get(g.raws(), idx) end
local function sp_group_get(g, b, idx)
    if g.get then return sp_bool(g.get(b, idx)) end
    local vec = g.vec(b)
    if not g.fixed then sp_ensure_vec(vec, sp_group_count(g)) end
    if g.fixed or idx < #vec then return sp_bool(vec[idx]) end
    return false
end
local function sp_group_set(g, b, idx, on)
    if g.set then g.set(b, idx, on); return end
    local vec = g.vec(b)
    if not g.fixed then sp_ensure_vec(vec, sp_group_count(g)) end
    vec[idx] = sp_bool(on) and 1 or 0
end

-- Resolve (category, group key) -> spec, group. Defaults to the first group if blank/unknown.
local function sp_find_group(cat, group)
    local spec = SP_CATEGORIES[tostring(cat or '')]
    if not spec then return nil, nil end
    for _, g in ipairs(spec.groups) do
        if g.key == tostring(group or '') then return spec, g end
    end
    return spec, spec.groups[1]
end

-- (stockpile_cat_groups + stockpile_item_list build JSON, so they're defined later after
--  json_string/json_bool are in scope.)

function stockpile_toggle_item(id, cat, group, idx, on)
    local b = get_stockpile(id)
    if not b then return false, 'not a stockpile' end
    local spec, g = sp_find_group(cat, group)
    if not g then return false, 'category not editable' end
    idx = tonumber(idx)
    if not idx or idx < 0 then return false, 'bad index' end
    local ok, err = pcall(function()
        sp_group_set(g, b, idx, on)
        if sp_bool(on) then b.settings.flags[spec.flag] = true end
    end)
    if not ok then return false, tostring(err) end
    return true, ''
end

function stockpile_toggle_all(id, cat, group, on)
    local b = get_stockpile(id)
    if not b then return false, 'not a stockpile' end
    local spec, g = sp_find_group(cat, group)
    if not g then return false, 'category not editable' end
    local want = sp_bool(on)
    local ok, err = pcall(function()
        for i = 0, sp_group_count(g) - 1 do
            local r = sp_group_item(g, i)
            if r and g.include(r, i) then sp_group_set(g, b, i, want) end
        end
        if want then b.settings.flags[spec.flag] = true end
    end)
    if not ok then return false, tostring(err) end
    return true, ''
end

-- Create a stockpile over the inclusive world-tile rectangle (x1,y1)-(x2,y2) on z and
-- apply a category preset. Returns (id, ''). On failure returns (-1, errmsg).
function create_stockpile(x1, y1, x2, y2, z, preset)
    local lx, hx = math.min(x1, x2), math.max(x1, x2)
    local ly, hy = math.min(y1, y2), math.max(y1, y2)
    local ok, bld, err = pcall(dfhack.buildings.constructBuilding, {
        type = df.building_type.Stockpile,
        abstract = true,
        pos = {x = lx, y = ly, z = z},
        width = hx - lx + 1,
        height = hy - ly + 1,
    })
    if not ok then return -1, tostring(bld) end       -- bld is the error on pcall failure
    if not bld then return -1, tostring(err or 'could not place stockpile') end

    -- Configure which items it accepts, using DFHack's tested preset import.
    local libname = STOCKPILE_PRESETS[tostring(preset or 'all'):lower()] or 'all'
    pcall(function()
        require('plugins.stockpiles').import_settings(libname, {id = bld.id, mode = 'enable'})
    end)
    return bld.id, ''
end

-- ---------------------------------------------------------------------------
-- Browser build menu + placement
-- ---------------------------------------------------------------------------

local BUILD_CATEGORIES = {
    {id='furniture', label='Furniture'},
    {id='workshops', label='Workshops'},
    {id='furnaces', label='Furnaces'},
    {id='constructions', label='Constructions'},
    {id='machines', label='Machines'},
    {id='traps', label='Traps'},
    {id='siege', label='Siege engines'},
    {id='track', label='Track'},
    {id='farming', label='Farming'},
    {id='trade', label='Trade'},
}

local function json_string(s)
    s = tostring(s or '')
    -- DF strings are CP437. A raw high byte (>=0x80, e.g. the i/o in dwarf names like "Atir"/"Onul",
    -- or special material names) is NOT valid UTF-8, and a raw control char (<0x20) is illegal in a
    -- JSON string -- either one makes the browser's JSON.parse() throw, which surfaces as
    -- "Workshop data unavailable" even though the Lua built the response fine. Convert to UTF-8 and
    -- escape every control char so the JSON is always valid regardless of item/job/worker names.
    local ok, u = pcall(dfhack.df2utf, s)
    if ok and type(u) == 'string' then s = u end
    s = s:gsub('\\', '\\\\'):gsub('"', '\\"')
         :gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
         :gsub('[%z\1-\8\11\12\14-\31\127]', function(c) return string.format('\\u%04x', c:byte()) end)
    return '"' .. s .. '"'
end

local function json_bool(v)
    return v and 'true' or 'false'
end

-- Sub-groups (DF's middle column) for a stockpile category. Defined here (not with the other
-- stockpile fns) so json_string/json_bool above are already in scope.
function stockpile_cat_groups(cat)
    local spec = SP_CATEGORIES[tostring(cat or '')]
    if not spec then return '{"ok":false,"groups":[]}\n' end
    local out = {}
    for _, g in ipairs(spec.groups) do
        out[#out + 1] = '{"key":' .. json_string(g.key) .. ',"label":' .. json_string(g.label) .. '}'
    end
    return '{"ok":true,"label":' .. json_string(spec.label) .. ',"groups":[' .. table.concat(out, ',') .. ']}\n'
end

-- The items of a category's sub-group with their on/off state.
function stockpile_item_list(id, cat, group)
    local b = get_stockpile(id)
    if not b then return '{"ok":false,"items":[]}\n' end
    local spec, g = sp_find_group(cat, group)
    if not g then return '{"ok":false,"error":"category not editable yet","items":[]}\n' end
    local ok, res = pcall(function()
        local items = {}
        for i = 0, sp_group_count(g) - 1 do
            local r = sp_group_item(g, i)
            if r and g.include(r, i) then
                local on = sp_group_get(g, b, i)
                items[#items + 1] = '{"idx":' .. i .. ',"name":' .. json_string(tostring(g.name(r))) ..
                    ',"on":' .. json_bool(on) .. '}'
            end
        end
        return '{"ok":true,"items":[' .. table.concat(items, ',') .. ']}\n'
    end)
    if ok and res then return res end
    return '{"ok":false,"items":[],"error":' .. json_string(tostring(res)) .. '}\n'
end

local function token_for(btype, subtype, custom)
    return ('%d:%d:%d'):format(tonumber(btype) or -1, tonumber(subtype) or -1, tonumber(custom) or -1)
end

local function parse_token(token)
    local t, s, c = tostring(token or ''):match('^(-?%d+):(-?%d+):(-?%d+)$')
    if not t then return nil end
    return tonumber(t), tonumber(s), tonumber(c)
end

local function direction_options(kind)
    if kind == 'axis' then
        return {
            {label='E-W', value=0},
            {label='N-S', value=1},
        }
    elseif kind == 'bridge' then
        return {
            {label='Retract', value=-1},
            {label='West', value=0},
            {label='East', value=1},
            {label='North', value=2},
            {label='South', value=3},
        }
    elseif kind == 'pump' then
        return {
            {label='From N', value=df.screw_pump_direction.FromNorth},
            {label='From E', value=df.screw_pump_direction.FromEast},
            {label='From S', value=df.screw_pump_direction.FromSouth},
            {label='From W', value=df.screw_pump_direction.FromWest},
        }
    end
    return {
        {label='N', value=0},
        {label='E', value=1},
        {label='S', value=2},
        {label='W', value=3},
    }
end

local function requirements_for(filters)
    -- NOTE: deliberately does NOT call into plugins.buildingplan. buildingplan.get_desc makes a
    -- nested CallLuaModuleFunction that misbehaves from dfcapture's render-thread context and can
    -- raise a luaL_argerror whose error-message formatting overflows the render thread's tiny
    -- stack and HARD-CRASHES the game (intermittent build-menu crash). dfcapture places buildings
    -- via constructBuilding directly, so we don't need buildingplan here. Use the filter's own name.
    local out = {}
    for _, filter in ipairs(filters or {}) do
        local desc = (filter and filter.name and #tostring(filter.name) > 0)
            and tostring(filter.name) or 'Material'
        table.insert(out, {label=desc, quantity=(filter and filter.quantity) or 1})
    end
    return out
end

local function correct_size(width, height, btype, subtype, custom, direction)
    local ok, used, adjusted_w, adjusted_h = pcall(
        dfhack.buildings.getCorrectSize,
        width or 1, height or 1, btype, subtype or -1, custom or -1, direction or 0)
    if ok and used then
        return adjusted_w or width or 1, adjusted_h or height or 1
    end
    return width or 1, height or 1
end

local function item_to_json(item)
    local parts = {
        '"token":' .. json_string(item.token),
        '"label":' .. json_string(item.label),
        '"category":' .. json_string(item.category),
        '"type":' .. tostring(item.type),
        '"subtype":' .. tostring(item.subtype or -1),
        '"custom":' .. tostring(item.custom or -1),
        '"area":' .. json_bool(item.area),
        '"direction":' .. json_bool(item.direction),
        '"directionKind":' .. json_string(item.direction_kind or ''),
        '"hollow":' .. json_bool(item.hollow),
        '"pressure":' .. json_bool(item.pressure),
        '"trackStop":' .. json_bool(item.track_stop),
        '"weaponCount":' .. json_bool(item.weapon_count),
        '"speed":' .. json_bool(item.speed),
        '"customRaw":' .. json_bool(item.custom_raw),
        '"size":{"w":' .. tostring(item.size_w or 1) .. ',"h":' .. tostring(item.size_h or 1) .. '}',
        '"limit":{"w":' .. tostring(item.limit_w or 1) .. ',"h":' .. tostring(item.limit_h or 1) .. '}',
    }

    local dir = direction_options(item.direction_kind)
    local dir_parts = {}
    if item.direction then
        for _, opt in ipairs(dir) do
            table.insert(dir_parts, '{"label":' .. json_string(opt.label) .. ',"value":' .. tostring(opt.value) .. '}')
        end
    end
    table.insert(parts, '"directions":[' .. table.concat(dir_parts, ',') .. ']')

    local req_parts = {}
    for _, req in ipairs(item.requirements or {}) do
        table.insert(req_parts, '{"label":' .. json_string(req.label) ..
            ',"quantity":' .. tostring(req.quantity or 1) .. '}')
    end
    table.insert(parts, '"requirements":[' .. table.concat(req_parts, ',') .. ']')

    return '{' .. table.concat(parts, ',') .. '}'
end

local function category_to_json(cat, count)
    return '{"id":' .. json_string(cat.id) ..
        ',"label":' .. json_string(cat.label) ..
        ',"count":' .. tostring(count or 0) .. '}'
end

local function add_build_item(items, category, label, btype, subtype, custom, opts)
    if btype == nil then return end
    subtype = subtype or -1
    custom = custom or -1
    opts = opts or {}
    if btype == df.building_type.Construction and subtype == df.construction_type.TrackNSEW then
        return
    end
    local ok_filters, filters = pcall(dfhack.buildings.getFiltersByType, {}, btype, subtype, custom)
    if not ok_filters or filters == nil then
        return
    end
    -- NOTE: do NOT call buildingplan.isPlannableBuilding here. Its nested CallLuaModuleFunction
    -- from dfcapture's render-thread context can raise an error that overflows the render thread's
    -- stack and HARD-CRASHES the game (intermittent build-menu crash). dfcapture places via
    -- constructBuilding directly, so the plannable filter isn't needed -- getFiltersByType above
    -- already establishes the building can be built.

    local size_w, size_h = correct_size(1, 1, btype, subtype, custom, opts.default_direction or 0)
    local item = {
        category=category,
        label=label,
        type=btype,
        subtype=subtype,
        custom=custom,
        token=token_for(btype, subtype, custom),
        area=opts.area or false,
        direction=opts.direction or false,
        direction_kind=opts.direction_kind or '',
        hollow=opts.hollow or false,
        pressure=opts.pressure or false,
        track_stop=opts.track_stop or false,
        weapon_count=opts.weapon_count or false,
        speed=opts.speed or false,
        custom_raw=opts.custom_raw or false,
        size_w=size_w,
        size_h=size_h,
        limit_w=opts.limit_w or size_w or 1,
        limit_h=opts.limit_h or size_h or 1,
        requirements=requirements_for(filters),
    }
    table.insert(items, item)
end

local function add_native_build_items(items)
    local bt = df.building_type
    local wt = df.workshop_type
    local ft = df.furnace_type
    local tt = df.trap_type
    local st = df.siegeengine_type
    local ct = df.construction_type

    add_build_item(items, 'furniture', 'Chair / Throne', bt.Chair)
    add_build_item(items, 'furniture', 'Bed', bt.Bed)
    add_build_item(items, 'furniture', 'Table', bt.Table)
    add_build_item(items, 'furniture', 'Coffin', bt.Coffin)
    add_build_item(items, 'furniture', 'Door', bt.Door)
    add_build_item(items, 'furniture', 'Floodgate', bt.Floodgate)
    add_build_item(items, 'furniture', 'Hatch cover', bt.Hatch)
    add_build_item(items, 'furniture', 'Wall grate', bt.GrateWall)
    add_build_item(items, 'furniture', 'Floor grate', bt.GrateFloor)
    add_build_item(items, 'furniture', 'Vertical bars', bt.BarsVertical)
    add_build_item(items, 'furniture', 'Floor bars', bt.BarsFloor)
    add_build_item(items, 'furniture', 'Cabinet', bt.Cabinet)
    add_build_item(items, 'furniture', 'Statue', bt.Statue)
    add_build_item(items, 'furniture', 'Slab', bt.Slab)
    add_build_item(items, 'furniture', 'Glass window', bt.WindowGlass)
    add_build_item(items, 'furniture', 'Gem window', bt.WindowGem)
    add_build_item(items, 'furniture', 'Box / Chest', bt.Box)
    add_build_item(items, 'furniture', 'Cage', bt.Cage)
    add_build_item(items, 'furniture', 'Chain / Rope', bt.Chain)
    add_build_item(items, 'furniture', 'Bookcase', bt.Bookcase)
    add_build_item(items, 'furniture', 'Display furniture', bt.DisplayFurniture)
    add_build_item(items, 'furniture', 'Offering place', bt.OfferingPlace)
    add_build_item(items, 'furniture', 'Stationary instrument', bt.Instrument)

    add_build_item(items, 'workshops', 'Carpenter', bt.Workshop, wt.Carpenters)
    add_build_item(items, 'workshops', 'Farmer', bt.Workshop, wt.Farmers)
    add_build_item(items, 'workshops', 'Mason / Stoneworker', bt.Workshop, wt.Masons)
    add_build_item(items, 'workshops', 'Craftsdwarf', bt.Workshop, wt.Craftsdwarfs)
    add_build_item(items, 'workshops', 'Jeweler', bt.Workshop, wt.Jewelers)
    add_build_item(items, 'workshops', 'Metalsmith forge', bt.Workshop, wt.MetalsmithsForge)
    add_build_item(items, 'workshops', 'Magma forge', bt.Workshop, wt.MagmaForge)
    add_build_item(items, 'workshops', 'Bowyer', bt.Workshop, wt.Bowyers)
    add_build_item(items, 'workshops', 'Mechanic', bt.Workshop, wt.Mechanics)
    add_build_item(items, 'workshops', 'Siege', bt.Workshop, wt.Siege)
    add_build_item(items, 'workshops', 'Butcher', bt.Workshop, wt.Butchers)
    add_build_item(items, 'workshops', 'Leather works', bt.Workshop, wt.Leatherworks)
    add_build_item(items, 'workshops', 'Tanner', bt.Workshop, wt.Tanners)
    add_build_item(items, 'workshops', 'Clothier', bt.Workshop, wt.Clothiers)
    add_build_item(items, 'workshops', 'Fishery', bt.Workshop, wt.Fishery)
    add_build_item(items, 'workshops', 'Still', bt.Workshop, wt.Still)
    add_build_item(items, 'workshops', 'Loom', bt.Workshop, wt.Loom)
    add_build_item(items, 'workshops', 'Quern', bt.Workshop, wt.Quern)
    add_build_item(items, 'workshops', 'Kennel', bt.Workshop, wt.Kennels)
    add_build_item(items, 'workshops', 'Kitchen', bt.Workshop, wt.Kitchen)
    add_build_item(items, 'workshops', 'Ashery', bt.Workshop, wt.Ashery)
    add_build_item(items, 'workshops', 'Dyer', bt.Workshop, wt.Dyers)
    add_build_item(items, 'workshops', 'Millstone', bt.Workshop, wt.Millstone)

    add_build_item(items, 'furnaces', 'Wood furnace', bt.Furnace, ft.WoodFurnace)
    add_build_item(items, 'furnaces', 'Smelter', bt.Furnace, ft.Smelter)
    add_build_item(items, 'furnaces', 'Glass furnace', bt.Furnace, ft.GlassFurnace)
    add_build_item(items, 'furnaces', 'Kiln', bt.Furnace, ft.Kiln)
    add_build_item(items, 'furnaces', 'Magma smelter', bt.Furnace, ft.MagmaSmelter)
    add_build_item(items, 'furnaces', 'Magma glass furnace', bt.Furnace, ft.MagmaGlassFurnace)
    add_build_item(items, 'furnaces', 'Magma kiln', bt.Furnace, ft.MagmaKiln)

    add_build_item(items, 'constructions', 'Wall', bt.Construction, ct.Wall, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Floor', bt.Construction, ct.Floor, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Ramp', bt.Construction, ct.Ramp, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Up stair', bt.Construction, ct.UpStair, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Down stair', bt.Construction, ct.DownStair, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Up/down stair', bt.Construction, ct.UpDownStair, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Fortification', bt.Construction, ct.Fortification, -1, {area=true, hollow=true, limit_w=31, limit_h=31})
    add_build_item(items, 'constructions', 'Reinforced wall', bt.Construction, ct.ReinforcedWall, -1, {area=true, hollow=true, limit_w=31, limit_h=31})

    add_build_item(items, 'machines', 'Screw pump', bt.ScrewPump, -1, -1, {direction=true, direction_kind='pump'})
    add_build_item(items, 'machines', 'Gear assembly', bt.GearAssembly)
    add_build_item(items, 'machines', 'Horizontal axle', bt.AxleHorizontal, -1, -1, {area=true, direction=true, direction_kind='axis', limit_w=31, limit_h=31})
    add_build_item(items, 'machines', 'Vertical axle', bt.AxleVertical)
    add_build_item(items, 'machines', 'Water wheel', bt.WaterWheel, -1, -1, {direction=true})
    add_build_item(items, 'machines', 'Windmill', bt.Windmill)
    add_build_item(items, 'machines', 'Rollers', bt.Rollers, -1, -1, {area=true, direction=true, speed=true, limit_w=31, limit_h=31})
    add_build_item(items, 'machines', 'Support', bt.Support)
    add_build_item(items, 'machines', 'Well', bt.Well)
    add_build_item(items, 'machines', 'Bridge', bt.Bridge, -1, -1, {area=true, direction=true, direction_kind='bridge', limit_w=31, limit_h=31})

    add_build_item(items, 'traps', 'Lever', bt.Trap, tt.Lever)
    add_build_item(items, 'traps', 'Pressure plate', bt.Trap, tt.PressurePlate, -1, {pressure=true})
    add_build_item(items, 'traps', 'Cage trap', bt.Trap, tt.CageTrap)
    add_build_item(items, 'traps', 'Stone-fall trap', bt.Trap, tt.StoneFallTrap)
    add_build_item(items, 'traps', 'Weapon trap', bt.Trap, tt.WeaponTrap, -1, {weapon_count=true})
    add_build_item(items, 'traps', 'Track stop', bt.Trap, tt.TrackStop, -1, {direction=true, track_stop=true})
    add_build_item(items, 'traps', 'Animal trap', bt.AnimalTrap)
    add_build_item(items, 'traps', 'Upright spear / spike', bt.Weapon, -1, -1, {weapon_count=true})

    add_build_item(items, 'siege', 'Catapult', bt.SiegeEngine, st.Catapult, -1, {direction=true})
    add_build_item(items, 'siege', 'Ballista', bt.SiegeEngine, st.Ballista, -1, {direction=true})
    add_build_item(items, 'siege', 'Bolt thrower', bt.SiegeEngine, st.BoltThrower, -1, {direction=true})

    add_build_item(items, 'farming', 'Farm plot', bt.FarmPlot, -1, -1, {area=true, limit_w=31, limit_h=31})
    add_build_item(items, 'farming', 'Nest box', bt.NestBox)
    add_build_item(items, 'farming', 'Hive', bt.Hive)
    add_build_item(items, 'farming', 'Archery target', bt.ArcheryTarget)
    add_build_item(items, 'farming', 'Armor stand', bt.Armorstand)
    add_build_item(items, 'farming', 'Weapon rack', bt.Weaponrack)
    add_build_item(items, 'farming', 'Traction bench', bt.TractionBench)

    add_build_item(items, 'trade', 'Trade depot', bt.TradeDepot)
    add_build_item(items, 'trade', 'Dirt road', bt.RoadDirt, -1, -1, {area=true, limit_w=31, limit_h=31})
    add_build_item(items, 'trade', 'Paved road', bt.RoadPaved, -1, -1, {area=true, limit_w=31, limit_h=31})

    local track_names = {
        {ct.TrackN, 'Track N'}, {ct.TrackS, 'Track S'}, {ct.TrackE, 'Track E'}, {ct.TrackW, 'Track W'},
        {ct.TrackNS, 'Track N-S'}, {ct.TrackNE, 'Track N-E'}, {ct.TrackNW, 'Track N-W'},
        {ct.TrackSE, 'Track S-E'}, {ct.TrackSW, 'Track S-W'}, {ct.TrackEW, 'Track E-W'},
        {ct.TrackNSE, 'Track N-S-E'}, {ct.TrackNSW, 'Track N-S-W'},
        {ct.TrackNEW, 'Track N-E-W'}, {ct.TrackSEW, 'Track S-E-W'},
        {ct.TrackRampN, 'Track ramp N'}, {ct.TrackRampS, 'Track ramp S'},
        {ct.TrackRampE, 'Track ramp E'}, {ct.TrackRampW, 'Track ramp W'},
        {ct.TrackRampNS, 'Track ramp N-S'}, {ct.TrackRampNE, 'Track ramp N-E'},
        {ct.TrackRampNW, 'Track ramp N-W'}, {ct.TrackRampSE, 'Track ramp S-E'},
        {ct.TrackRampSW, 'Track ramp S-W'}, {ct.TrackRampEW, 'Track ramp E-W'},
        {ct.TrackRampNSE, 'Track ramp N-S-E'}, {ct.TrackRampNSW, 'Track ramp N-S-W'},
        {ct.TrackRampNEW, 'Track ramp N-E-W'}, {ct.TrackRampSEW, 'Track ramp S-E-W'},
        {ct.TrackRampNSEW, 'Track ramp N-S-E-W'},
    }
    for _, t in ipairs(track_names) do
        add_build_item(items, 'track', t[2], bt.Construction, t[1], -1, {area=true, hollow=false, limit_w=31, limit_h=31})
    end
end

local function add_custom_build_items(items)
    local world = df.global.world
    if not world or not world.raws or not world.raws.buildings then return end
    for _, def in ipairs(world.raws.buildings.workshops or {}) do
        if def then
            add_build_item(items, 'workshops',
                (def.name and #def.name > 0) and def.name or def.code or 'Custom workshop',
                df.building_type.Workshop, df.workshop_type.Custom, def.id,
                {custom_raw=true})
        end
    end
    for _, def in ipairs(world.raws.buildings.furnaces or {}) do
        if def then
            add_build_item(items, 'furnaces',
                (def.name and #def.name > 0) and def.name or def.code or 'Custom furnace',
                df.building_type.Furnace, df.furnace_type.Custom, def.id,
                {custom_raw=true})
        end
    end
end

function building_catalog()
    -- Every step is pcall-guarded: this runs on dfcapture's RENDER THREAD, and if a Lua error
    -- escaped to DFHack's SafeCall the traceback it builds overflows the render thread's small
    -- stack and HARD-CRASHES the game (observed: intermittent crash opening the build menu). So we
    -- catch errors here and return a (possibly partial) catalog instead -- never crash.
    local items = {}
    local ok1, e1 = pcall(add_native_build_items, items)
    if not ok1 then dfhack.printerr('dfcapture building_catalog: native items failed: ' .. tostring(e1)) end
    local ok2, e2 = pcall(add_custom_build_items, items)
    if not ok2 then dfhack.printerr('dfcapture building_catalog: custom items failed: ' .. tostring(e2)) end

    pcall(table.sort, items, function(a, b)
        if a.category ~= b.category then return tostring(a.category) < tostring(b.category) end
        return tostring(a.label) < tostring(b.label)
    end)

    local counts = {}
    for _, item in ipairs(items) do
        if item and item.category then
            counts[item.category] = (counts[item.category] or 0) + 1
        end
    end

    local cat_json = {}
    for _, cat in ipairs(BUILD_CATEGORIES) do
        if (counts[cat.id] or 0) > 0 then
            local okc, j = pcall(category_to_json, cat, counts[cat.id])
            if okc and j then table.insert(cat_json, j) end
        end
    end

    local item_json = {}
    for _, item in ipairs(items) do
        local oki, j = pcall(item_to_json, item)
        if oki and j then table.insert(item_json, j) end
    end

    return '{"ok":true,"categories":[' .. table.concat(cat_json, ',') ..
        '],"items":[' .. table.concat(item_json, ',') .. ']}\n'
end

local function parse_options(raw)
    local out = {}
    for k, v in tostring(raw or ''):gmatch('([%w_]+)=([^;]*)') do
        out[k] = v
    end
    return out
end

local function opt_num(opts, key, default)
    local n = tonumber(opts[key])
    if n == nil then return default end
    return n
end

local function opt_bool(opts, key, default)
    if opts[key] == nil then return default end
    return tonumber(opts[key]) ~= 0
end

local function clamp(value, lo, hi)
    if value < lo then return lo end
    if value > hi then return hi end
    return value
end

local function map_bounds(x1, y1, x2, y2, z)
    local world = df.global.world
    if not world or not world.map then return nil, 'map unavailable' end
    local lx, hx = math.min(x1, x2), math.max(x1, x2)
    local ly, hy = math.min(y1, y2), math.max(y1, y2)
    lx = clamp(lx, 0, world.map.x_count - 1)
    hx = clamp(hx, 0, world.map.x_count - 1)
    ly = clamp(ly, 0, world.map.y_count - 1)
    hy = clamp(hy, 0, world.map.y_count - 1)
    z = clamp(z, 0, world.map.z_count - 1)
    return {x1=lx, y1=ly, x2=hx, y2=hy, z=z}
end

local function is_construction(btype)
    return btype == df.building_type.Construction
end

local function is_variable_area(btype)
    return btype == df.building_type.Bridge
        or btype == df.building_type.FarmPlot
        or btype == df.building_type.RoadPaved
        or btype == df.building_type.RoadDirt
        or btype == df.building_type.AxleHorizontal
        or btype == df.building_type.Rollers
end

local function is_pressure_plate(btype, subtype)
    return btype == df.building_type.Trap
        and subtype == df.trap_type.PressurePlate
end

local function is_weapon_trap(btype, subtype)
    return btype == df.building_type.Trap
        and subtype == df.trap_type.WeaponTrap
end

local function is_spike_trap(btype)
    return btype == df.building_type.Weapon
end

local function tile_is_construction(pos)
    local tt = dfhack.maps.getTileType(pos)
    if not tt then return false end
    if df.tiletype.attrs[tt].material ~= df.tiletype_material.CONSTRUCTION then
        return false
    end
    local construction = df.construction.find(pos)
    return construction and not construction.flags.top_of_wall
end

local ONE_BY_ONE = xy2pos(1, 1)

local function can_place_construction(reconstruct, pos)
    return dfhack.buildings.checkFreeTiles(pos, ONE_BY_ONE)
        and (reconstruct or not tile_is_construction(pos))
end

local function is_interior(bounds, x, y)
    return x ~= bounds.x1 and x ~= bounds.x2 and y ~= bounds.y1 and y ~= bounds.y2
end

-- ===== Build-material selection (DF-style "pick the specific material") =====

-- Fast flag screen for whether an item can be used as a construction material right now. DF still
-- validates reachability/stockpile at build time; this is just to build the picker list + counts.
local function item_buildable(item)
    if not item then return false end
    local f = item.flags
    if f.forbid or f.dump or f.garbage_collect or f.hostile or f.removed or f.in_job
       or f.construction or f.in_building or f.encased or f.trader or f.owned or f.artifact
       or f.spider_web or f.on_fire or f.rotten or f.murder or f.foreign then
        return false
    end
    return true
end

-- Does an item satisfy a building's job_item filter for material-grouping purposes? (item type +
-- subtype must match; mat_type < 0 means "any material", which is exactly what we want to enumerate.)
local function item_matches_filter(filter, item)
    if filter.item_type ~= nil and filter.item_type >= 0 and item:getType() ~= filter.item_type then
        return false
    end
    if filter.item_subtype ~= nil and filter.item_subtype >= 0 and item:getSubtype() ~= filter.item_subtype then
        return false
    end
    if filter.mat_type ~= nil and filter.mat_type >= 0 then
        if item:getMaterial() ~= filter.mat_type then return false end
        if filter.mat_index ~= nil and filter.mat_index >= 0 and item:getMaterialIndex() ~= filter.mat_index then
            return false
        end
    end
    return true
end

-- List the AVAILABLE materials (grouped, with on-hand counts) for each requirement of a building,
-- so the browser can offer DF-style material selection. Read-only; runs under CoreSuspender.
function build_materials(token)
    local btype, subtype, custom = parse_token(token)
    if not btype then return '{"ok":false,"error":"bad building token"}\n' end
    local ok_f, filters = pcall(dfhack.buildings.getFiltersByType, {}, btype, subtype, custom)
    if not ok_f or not filters then return '{"ok":false,"error":"no filters"}\n' end

    local items_vec = df.global.world.items.other.IN_PLAY
    local req_json = {}
    for fi, filter in ipairs(filters) do
        local pinned = (filter.mat_type ~= nil and filter.mat_type >= 0)   -- material fixed by raws
        local groups, order = {}, {}
        for ii = 0, #items_vec - 1 do
            local item = items_vec[ii]
            if item_buildable(item) and item_matches_filter(filter, item) then
                local mt, mi = item:getMaterial(), item:getMaterialIndex()
                local key = tostring(mt) .. ':' .. tostring(mi)
                local g = groups[key]
                if not g then
                    local nm = ''
                    local okm, info = pcall(dfhack.matinfo.decode, mt, mi)
                    if okm and info then
                        local oks, s = pcall(function() return info:toString() end)
                        if oks and s then nm = s end
                    end
                    g = { mat_type = mt, mat_index = mi, name = nm, count = 0 }
                    groups[key] = g
                    table.insert(order, key)
                end
                g.count = g.count + (item.stack_size or 1)
            end
        end
        table.sort(order, function(a, b) return (groups[a].name or '') < (groups[b].name or '') end)
        local mats = {}
        for _, key in ipairs(order) do
            local g = groups[key]
            table.insert(mats, '{"matType":' .. tostring(g.mat_type) ..
                ',"matIndex":' .. tostring(g.mat_index) ..
                ',"name":' .. json_string((g.name ~= '' and g.name) or ('material ' .. key)) ..
                ',"count":' .. tostring(g.count) .. '}')
        end
        table.insert(req_json, '{"index":' .. tostring(fi - 1) ..
            ',"label":' .. json_string((filter.name and #tostring(filter.name) > 0) and tostring(filter.name) or 'Material') ..
            ',"quantity":' .. tostring(filter.quantity or 1) ..
            ',"pinned":' .. json_bool(pinned) ..
            ',"materials":[' .. table.concat(mats, ',') .. ']}')
    end
    return '{"ok":true,"requirements":[' .. table.concat(req_json, ',') .. ']}\n'
end

-- Apply the browser's per-requirement material picks (opts.mat0, mat1, ... each "matType:matIndex")
-- onto the building's filters, so constructBuilding restricts each reagent to the chosen material.
local function apply_chosen_materials(filters, opts)
    for i, filter in ipairs(filters or {}) do
        local sel = opts['mat' .. (i - 1)]
        if sel then
            local mt, mi = tostring(sel):match('^(-?%d+):(-?%d+)$')
            if mt then
                filter.mat_type = tonumber(mt)
                filter.mat_index = tonumber(mi)
            end
        end
    end
end

-- DF-style "use closest material": for any requirement set to "closest", pick the matching on-hand
-- item nearest the placement (cx,cy,cz) and rewrite opts.matN to that item's specific material so
-- the normal apply_chosen_materials path uses it. Done once per placement (cheap enough).
local function resolve_closest_materials(opts, btype, subtype, custom, cx, cy, cz)
    local need = false
    for k, v in pairs(opts) do
        if v == 'closest' and tostring(k):match('^mat%d+$') then need = true; break end
    end
    if not need then return end
    local ok_f, filters = pcall(dfhack.buildings.getFiltersByType, {}, btype, subtype, custom)
    if not ok_f or not filters then return end
    local items_vec = df.global.world.items.other.IN_PLAY
    for fi, filter in ipairs(filters) do
        local key = 'mat' .. (fi - 1)
        if opts[key] == 'closest' then
            local best, bestd
            for ii = 0, #items_vec - 1 do
                local item = items_vec[ii]
                if item_buildable(item) and item_matches_filter(filter, item) then
                    local p = item.pos
                    if p then
                        local dx, dy, dz = (p.x or 0) - cx, (p.y or 0) - cy, (p.z or 0) - cz
                        local d = dx * dx + dy * dy + dz * dz * 16   -- weight z so another floor is "far"
                        if not bestd or d < bestd then bestd = d; best = item end
                    end
                end
            end
            -- resolve to a concrete material, or drop the pick (-> "any") if nothing is reachable
            opts[key] = best and (tostring(best:getMaterial()) .. ':' .. tostring(best:getMaterialIndex())) or nil
        end
    end
end

local function filters_for_building(btype, subtype, custom, opts)
    local filters = dfhack.buildings.getFiltersByType({}, btype, subtype, custom)
    if not filters then return nil end
    if is_pressure_plate(btype, subtype) and filters[1] then
        local quantity = 0
        if opt_bool(opts, 'plate_units', true) then quantity = quantity + 1 end
        if opt_bool(opts, 'plate_water', false) then quantity = quantity + 1 end
        if opt_bool(opts, 'plate_magma', false) then quantity = quantity + 1 end
        if opt_bool(opts, 'plate_track', false) then quantity = quantity + 1 end
        filters[1].quantity = math.max(1, quantity)
    elseif is_weapon_trap(btype, subtype) and filters[2] then
        filters[2].quantity = clamp(opt_num(opts, 'weapon_count', 1), 1, 10)
    elseif is_spike_trap(btype) and filters[1] then
        filters[1].quantity = clamp(opt_num(opts, 'weapon_count', 1), 1, 10)
    end
    apply_chosen_materials(filters, opts)   -- DF-style per-requirement material selection
    return filters
end

local function apply_building_options(bld, btype, subtype, direction, opts)
    if not bld then return end
    if btype == df.building_type.SiegeEngine then
        bld.facing = direction
        bld.resting_orientation = direction
    end
    for k in pairs(bld) do
        if k == 'track_stop_info' then
            bld.track_stop_info.friction = clamp(opt_num(opts, 'friction', 50000), 0, 50000)
            bld.track_stop_info.track_flags.bits.use_dump = opt_bool(opts, 'track_dump', false)
            bld.track_stop_info.dump_x_shift = clamp(opt_num(opts, 'dump_x', 0), -1, 1)
            bld.track_stop_info.dump_y_shift = clamp(opt_num(opts, 'dump_y', 0), -1, 1)
        elseif k == 'speed' then
            bld.speed = clamp(opt_num(opts, 'speed', 50000), 1000, 100000)
        elseif k == 'plate_info' then
            local p = bld.plate_info
            p.flags.bits.units = opt_bool(opts, 'plate_units', true)
            p.flags.bits.water = opt_bool(opts, 'plate_water', false)
            p.flags.bits.magma = opt_bool(opts, 'plate_magma', false)
            p.flags.bits.track = opt_bool(opts, 'plate_track', false)
            p.flags.bits.citizens = opt_bool(opts, 'plate_citizens', false)
            p.flags.bits.resets = opt_bool(opts, 'plate_resets', true)
            p.unit_min = clamp(opt_num(opts, 'unit_min', 1), 0, 1000000)
            p.unit_max = clamp(opt_num(opts, 'unit_max', 1000000), 0, 1000000)
            p.water_min = clamp(opt_num(opts, 'water_min', 1), 0, 7)
            p.water_max = clamp(opt_num(opts, 'water_max', 7), 0, 7)
            p.magma_min = clamp(opt_num(opts, 'magma_min', 1), 0, 7)
            p.magma_max = clamp(opt_num(opts, 'magma_max', 7), 0, 7)
            p.track_min = clamp(opt_num(opts, 'track_min', 1), 0, 1000000)
            p.track_max = clamp(opt_num(opts, 'track_max', 1000000), 0, 1000000)
        end
    end
end

local function plan_buildings(blds)
    local ok_bp, buildingplan = pcall(require, 'plugins.buildingplan')
    if not ok_bp then return false, 'buildingplan plugin unavailable' end
    for _, bld in ipairs(blds) do
        buildingplan.addPlannedBuilding(bld)
    end
    buildingplan.scheduleCycle()
    return true, ''
end

local function place_one(pos, btype, subtype, custom, width, height, direction, opts, full_rectangle)
    local filters = filters_for_building(btype, subtype, custom, opts)
    if not filters then return nil, 'building has no material filter' end
    local fields = {}
    if btype == df.building_type.SiegeEngine then
        fields.facing = direction
        fields.resting_orientation = direction
    end
    local bld, err = dfhack.buildings.constructBuilding{
        pos=pos, type=btype, subtype=subtype, custom=custom,
        width=width, height=height, direction=direction,
        filters=filters, fields=fields, full_rectangle=full_rectangle}
    if not bld then return nil, tostring(err or 'could not place building') end
    apply_building_options(bld, btype, subtype, direction, opts)
    return bld, ''
end

-- Temporary diagnostic: append a step marker to dfcapture.log so a hang during building
-- placement reveals the last step reached. Remove once the building-placement hang is fixed.
local function bp_dbg(msg)
    local ok, f = pcall(io.open, 'dfcapture.log', 'a')
    if ok and f then f:write('LUA build-place: ' .. tostring(msg) .. '\n'); f:close() end
end

function place_building(x1, y1, x2, y2, z, token, direction, options)
    bp_dbg('enter token=' .. tostring(token))
    local btype, subtype, custom = parse_token(token)
    if not btype then return 0, -1, 'bad building token' end
    if btype == df.building_type.Stockpile or btype == df.building_type.Civzone then
        return 0, -1, 'use the dedicated stockpile/zone tools'
    end
    if btype == df.building_type.Construction and subtype == df.construction_type.TrackNSEW then
        return 0, -1, 'that track piece cannot be planned'
    end
    direction = tonumber(direction) or 0
    if btype == df.building_type.Bridge then
        direction = clamp(direction, -1, 3)
    else
        direction = clamp(direction, 0, 3)
    end
    local opts = parse_options(options)
    local bounds, err = map_bounds(x1, y1, x2, y2, z)
    if not bounds then return 0, -1, err end
    -- Resolve any "closest material" picks against the placement center before constructing.
    pcall(resolve_closest_materials, opts, btype, subtype, custom,
        math.floor((bounds.x1 + bounds.x2) / 2), math.floor((bounds.y1 + bounds.y2) / 2), bounds.z)

    local blds = {}
    if is_construction(btype) then
        local volume = (bounds.x2 - bounds.x1 + 1) * (bounds.y2 - bounds.y1 + 1)
        local reconstruct = volume == 1
        local ok_bp, buildingplan = pcall(require, 'plugins.buildingplan')
        if ok_bp and buildingplan.getGlobalSettings then
            local ok_settings, settings = pcall(buildingplan.getGlobalSettings)
            if ok_settings and settings and settings.reconstruct then reconstruct = true end
        end
        local hollow = opt_bool(opts, 'hollow', false)
        for y = bounds.y1, bounds.y2 do
            for x = bounds.x1, bounds.x2 do
                if not (hollow and is_interior(bounds, x, y)) then
                    local pos = xyz2pos(x, y, bounds.z)
                    if can_place_construction(reconstruct, pos) then
                        local bld = place_one(pos, btype, subtype, custom, 1, 1, direction, opts, false)
                        if bld then table.insert(blds, bld) end
                    end
                end
            end
        end
    elseif is_variable_area(btype) then
        local lx, ly = bounds.x1, bounds.y1
        local width = bounds.x2 - bounds.x1 + 1
        local height = bounds.y2 - bounds.y1 + 1
        width = math.min(width, 31)
        height = math.min(height, 31)
        if btype == df.building_type.AxleHorizontal then
            if direction == 1 then width = 1 else height = 1 end
        elseif btype == df.building_type.Rollers then
            if direction == 1 or direction == 3 then height = 1 else width = 1 end
        end
        local bld, place_err = place_one(xyz2pos(lx, ly, bounds.z), btype, subtype, custom,
            width, height, direction, opts, true)
        if bld then table.insert(blds, bld) else return 0, -1, place_err end
    else
        local center_x = math.floor((bounds.x1 + bounds.x2) / 2)
        local center_y = math.floor((bounds.y1 + bounds.y2) / 2)
        local width, height = correct_size(1, 1, btype, subtype, custom, direction)
        local sx = center_x - math.floor(width / 2)
        local sy = center_y - math.floor(height / 2)
        if btype == df.building_type.ScrewPump then
            if direction == df.screw_pump_direction.FromSouth then
                sy = sy + 1
            elseif direction == df.screw_pump_direction.FromEast then
                sx = sx + 1
            end
        end
        bp_dbg('single: calling place_one at ' .. sx .. ',' .. sy)
        local bld, place_err = place_one(xyz2pos(sx, sy, bounds.z), btype, subtype, custom,
            width, height, direction, opts, false)
        bp_dbg('single: place_one returned bld=' .. tostring(bld ~= nil))
        if bld then table.insert(blds, bld) else return 0, -1, place_err end
    end

    if #blds == 0 then return 0, -1, 'no valid tiles for building placement' end
    -- Do NOT register with buildingplan here. buildingplan.addPlannedBuilding -> get_item_filters
    -- -> get_job_items makes a NESTED Lua call (get_job_item) the first time a given building type
    -- is placed (to warm its per-type cache). That nested CallLuaModuleFunction DEADLOCKS when run
    -- from our render-thread run_on_render_thread_sync context -- which is exactly why placing a
    -- not-yet-cached furniture type hung the game. constructBuilding above already created the
    -- building with its material filters, so it builds normally with on-hand materials (just not as
    -- a deferred "planned" building). See dfcapture-building-hang memory for the deferred-onupdate
    -- alternative that would restore buildingplan if we want it later.
    bp_dbg('placed ' .. #blds .. ' building(s) id=' .. tostring(blds[1].id) .. ' (buildingplan skipped)')
    return #blds, blds[1].id, ''
end

-- Browser zone names -> df.civzone_type (fortress activity zones).
local ZONE_TYPES = {
    meeting = 'MeetingHall', pen = 'Pen', pond = 'Pond', water = 'WaterSource',
    fishing = 'FishingArea', sand = 'SandCollection', clay = 'ClayCollection',
    dump = 'Dump', gather = 'PlantGathering', training = 'AnimalTraining',
    dungeon = 'Dungeon', bedroom = 'Bedroom', dining = 'DiningHall',
    office = 'Office', dormitory = 'Dormitory', barracks = 'Barracks',
    archery = 'ArcheryRange', tomb = 'Tomb',
}

-- Create an activity zone over the inclusive world-tile rectangle (x1,y1)-(x2,y2) on z.
-- Same abstract-building path as stockpiles. Returns (id, ''). On failure (-1, errmsg).
function create_zone(x1, y1, x2, y2, z, zonetype)
    local lx, hx = math.min(x1, x2), math.max(x1, x2)
    local ly, hy = math.min(y1, y2), math.max(y1, y2)
    local tname = ZONE_TYPES[tostring(zonetype or 'meeting'):lower()] or 'MeetingHall'
    local subtype = df.civzone_type[tname]
    if not subtype then return -1, 'unknown zone type' end
    local ok, bld, err = pcall(dfhack.buildings.constructBuilding, {
        type = df.building_type.Civzone,
        subtype = subtype,
        abstract = true,
        pos = {x = lx, y = ly, z = z},
        width = hx - lx + 1,
        height = hy - ly + 1,
    })
    if not ok then return -1, tostring(bld) end       -- bld is the error on pcall failure
    if not bld then return -1, tostring(err or 'could not place zone') end
    bld.spec_sub_flag.active = true
    if subtype == df.civzone_type.Pen then
        bld.zone_settings.pen.flags.check_occupants = true
    elseif subtype == df.civzone_type.Pond then
        bld.zone_settings.pond.flag.check_occupants = true
    elseif subtype == df.civzone_type.PlantGathering then
        bld.zone_settings.gather.flags.pick_trees = true
        bld.zone_settings.gather.flags.pick_shrubs = true
        bld.zone_settings.gather.flags.gather_fallen = true
    elseif subtype == df.civzone_type.ArcheryRange then
        bld.zone_settings.archery.dir_x = 1
        bld.zone_settings.archery.dir_y = 0
    elseif subtype == df.civzone_type.Tomb then
        -- DF defaults: citizens can claim tombs automatically, pets cannot unless enabled.
        bld.zone_settings.tomb.flags.no_pets = true
        bld.zone_settings.tomb.flags.no_citizens = false
    end
    return bld.id, ''
end

-- ---------------------------------------------------------------------------
-- Zone locations: taverns, temples, libraries, guildhalls, hospitals.
-- Mirrored from DFHack quickfort's location creation path so we create real
-- abstract_building records and attach the civzone through contents.building_ids.
-- ---------------------------------------------------------------------------

local LOCATION_TYPES = {
    tavern = {
        label = 'Inn/Tavern',
        klass = df.abstract_building_inn_tavernst,
        name_type = df.language_name_type.FoodStore,
        apply = function(loc)
            loc.contents.desired_goblets = 10
            loc.contents.desired_instruments = 5
            loc.contents.need_more.goblets = true
            loc.contents.need_more.instruments = true
        end,
    },
    temple = {
        label = 'Temple',
        klass = df.abstract_building_templest,
        name_type = df.language_name_type.Temple,
        apply = function(loc)
            loc.deity_type = df.religious_practice_type.RELIGION_ENID
            loc.deity_data.Religion = -1
            loc.contents.desired_instruments = 5
            loc.contents.need_more.instruments = true
        end,
    },
    library = {
        label = 'Library',
        klass = df.abstract_building_libraryst,
        name_type = df.language_name_type.Library,
        apply = function(loc)
            loc.contents.desired_paper = 10
            loc.contents.need_more.paper = true
        end,
    },
    guildhall = {
        label = 'Guildhall',
        klass = df.abstract_building_guildhallst,
        name_type = df.language_name_type.Guildhall,
        apply = function(loc)
            loc.contents.profession = df.profession.NONE
        end,
    },
    hospital = {
        label = 'Hospital',
        klass = df.abstract_building_hospitalst,
        name_type = df.language_name_type.Hospital,
        apply = function(loc)
            loc.contents.desired_splints = 5
            loc.contents.desired_thread = 75000
            loc.contents.desired_cloth = 50000
            loc.contents.desired_crutches = 5
            loc.contents.desired_powder = 750
            loc.contents.desired_buckets = 2
            loc.contents.desired_soap = 750
            loc.contents.need_more.splints = true
            loc.contents.need_more.thread = true
            loc.contents.need_more.cloth = true
            loc.contents.need_more.crutches = true
            loc.contents.need_more.powder = true
            loc.contents.need_more.buckets = true
            loc.contents.need_more.soap = true
        end,
    },
}

local LOCATION_CREATE_ORDER = {'tavern', 'temple', 'library', 'guildhall', 'hospital'}

local function zone_allows_location(zone)
    return zone and (zone.type == df.civzone_type.MeetingHall
        or zone.type == df.civzone_type.DiningHall
        or zone.type == df.civzone_type.Bedroom)
end

local function get_civzone(zone_id)
    local zone = df.building.find(tonumber(zone_id) or -1)
    if not zone or not df.building_civzonest:is_instance(zone) then return nil end
    return zone
end

local function vector_contains(vec, value)
    for _, v in ipairs(vec or {}) do
        if v == value then return true end
    end
    return false
end

local function erase_value(vec, value)
    if not vec then return end
    for i = #vec - 1, 0, -1 do
        if vec[i] == value then vec:erase(i) end
    end
end

local function find_location(site, location_id)
    if not site then return nil end
    for _, loc in ipairs(site.buildings) do
        if loc.id == location_id then return loc end
    end
    return nil
end

local function location_kind(loc)
    if df.abstract_building_inn_tavernst:is_instance(loc) then return 'tavern' end
    if df.abstract_building_templest:is_instance(loc) then return 'temple' end
    if df.abstract_building_libraryst:is_instance(loc) then return 'library' end
    if df.abstract_building_guildhallst:is_instance(loc) then return 'guildhall' end
    if df.abstract_building_hospitalst:is_instance(loc) then return 'hospital' end
    return 'other'
end

local function location_label(loc)
    local kind = location_kind(loc)
    return (LOCATION_TYPES[kind] and LOCATION_TYPES[kind].label) or tostring(df.abstract_building_type[loc:getType()] or 'Location')
end

local function location_name(loc)
    local ok, name = pcall(dfhack.translation.translateName, loc.name, true)
    if ok and name and #name > 0 then return name end
    return location_label(loc)
end

local function generated_location_name(name_type)
    local name = {
        type = name_type,
        has_name = true,
        words = {resize = false},
        parts_of_speech = {resize = false, FirstAdjective = df.part_of_speech.Adjective},
    }
    local ok, word_table = pcall(function()
        return df.global.world.raws.language.word_table[0][35]
    end)
    if ok and word_table and #word_table.words.Adjectives > 0 and #word_table.words.TheX > 0 then
        name.words.FirstAdjective = word_table.words.Adjectives[math.random(0, #word_table.words.Adjectives - 1)]
        name.words.TheX = word_table.words.TheX[math.random(0, #word_table.words.TheX - 1)]
    else
        name.first_name = 'New location'
    end
    return name
end

local function current_site()
    local ok, site = pcall(dfhack.world.getCurrentSite)
    if ok then return site end
    return nil
end

local function site_owner_id(site)
    if not site then return -1 end
    for _, entity_site_link in ipairs(site.entity_links) do
        local he = df.historical_entity.find(entity_site_link.entity_id)
        if he and he.type == df.historical_entity_type.SiteGovernment then
            return he.id
        end
    end
    return site.cur_owner_id or -1
end

local function set_location_flags(loc)
    loc.flags.VISITORS_ALLOWED = true
    loc.flags.NON_CITIZENS_ALLOWED = true
    loc.flags.MEMBERS_ONLY = false
end

local function create_location(site, kind)
    local meta = LOCATION_TYPES[kind]
    if not site or not meta then return nil, 'unknown location type' end
    local loc_id = site.next_building_id
    site.buildings:insert('#', {
        new = meta.klass,
        id = loc_id,
        site_id = site.id,
        site_owner_id = site_owner_id(site),
        pos = {x = site.pos.x, y = site.pos.y},
        name = generated_location_name(meta.name_type),
    })
    site.next_building_id = site.next_building_id + 1
    local loc = site.buildings[#site.buildings - 1]
    set_location_flags(loc)
    if meta.apply then meta.apply(loc) end
    return loc, ''
end

local function detach_zone_location(zone)
    if zone.site_id ~= -1 and zone.location_id ~= -1 then
        local old_site = df.world_site.find(zone.site_id)
        local old_loc = find_location(old_site, zone.location_id)
        local contents = old_loc and old_loc:getContents()
        if contents then erase_value(contents.building_ids, zone.id) end
    end
    zone.site_id = -1
    zone.location_id = -1
end

local function attach_zone_location(zone, site, loc)
    detach_zone_location(zone)
    zone.site_id = site.id
    zone.location_id = loc.id
    local contents = loc:getContents()
    if contents and not vector_contains(contents.building_ids, zone.id) then
        contents.building_ids:insert('#', zone.id)
    end
    zone:uncategorize()
    zone:categorize(true)
end

local function location_to_json(loc, current_id)
    local contents = loc:getContents()
    local zones = contents and contents.building_ids or {}
    return '{' ..
        '"id":' .. tostring(loc.id) ..
        ',"kind":' .. json_string(location_kind(loc)) ..
        ',"label":' .. json_string(location_label(loc)) ..
        ',"name":' .. json_string(location_name(loc)) ..
        ',"current":' .. json_bool(loc.id == current_id) ..
        ',"zoneCount":' .. tostring(#zones) ..
        '}'
end

function zone_locations_json(zone_id)
    local zone = get_civzone(zone_id)
    if not zone then return '', 'zone not found' end
    if not zone_allows_location(zone) then return '', 'zone does not accept locations' end
    local site = current_site()
    if not site then return '', 'current site unavailable' end
    local ok_name, zone_name = pcall(dfhack.buildings.getName, zone)
    if not ok_name then zone_name = '' end
    local zone_type = tostring(df.civzone_type[zone.type] or zone.type)
    local locs = {}
    for _, loc in ipairs(site.buildings) do
        local kind = location_kind(loc)
        if LOCATION_TYPES[kind] then
            table.insert(locs, location_to_json(loc, zone.location_id))
        end
    end
    local create = {}
    for _, kind in ipairs(LOCATION_CREATE_ORDER) do
        table.insert(create, '{"kind":' .. json_string(kind) ..
            ',"label":' .. json_string(LOCATION_TYPES[kind].label) .. '}')
    end
    return '{"id":' .. tostring(zone.id) ..
        ',"type":' .. json_string(zone_type) ..
        ',"name":' .. json_string(zone_name or '') ..
        ',"locationId":' .. tostring(zone.location_id or -1) ..
        ',"locations":[' .. table.concat(locs, ',') .. ']' ..
        ',"createTypes":[' .. table.concat(create, ',') .. ']}' , ''
end

function zone_location_action(zone_id, action, kind, location_id)
    local zone = get_civzone(zone_id)
    if not zone then return false, 'zone not found' end
    if not zone_allows_location(zone) then return false, 'zone does not accept locations' end
    local site = current_site()
    if not site then return false, 'current site unavailable' end
    action = tostring(action or '')
    if action == 'clear' then
        detach_zone_location(zone)
        zone:uncategorize()
        zone:categorize(true)
        return true, ''
    elseif action == 'assign' then
        local loc = find_location(site, tonumber(location_id) or -1)
        if not loc then return false, 'location not found' end
        attach_zone_location(zone, site, loc)
        return true, ''
    elseif action == 'create' then
        local loc, err = create_location(site, tostring(kind or ''))
        if not loc then return false, err end
        attach_zone_location(zone, site, loc)
        return true, ''
    end
    return false, 'unknown location action'
end

-- ---------------------------------------------------------------------------
-- Work orders (the Manager): list / create / import presets / cancel / adjust
--
-- Creation goes through DFHack's tested `workorder` script (reqscript), which
-- handles the raws-dependent defaults; preset import reuses the `orders` command.
-- We never hand-roll manager_order objects -- far less crash-prone.
-- ---------------------------------------------------------------------------

-- Curated, friendly groupings for the common hardcoded job_types + a few vanilla
-- reactions. Each entry is a df.job_type name OR a reaction code (job becomes
-- CustomReaction). order_catalog() ALSO appends every reaction in the loaded raws
-- (grouped by its workshop) so the catalog is comprehensive like DF's own
-- create-order screen. The order key is derived: 'j:'<JobName> or 'r:'<CODE>.
local ORDER_CATALOG = {
    {cat='Furniture', items={
        {label='Bed', job='ConstructBed'}, {label='Throne / Chair', job='ConstructThrone'},
        {label='Table', job='ConstructTable'}, {label='Door', job='ConstructDoor'},
        {label='Floodgate', job='ConstructFloodgate'}, {label='Cabinet', job='ConstructCabinet'},
        {label='Chest / Coffer', job='ConstructChest'}, {label='Coffin', job='ConstructCoffin'},
        {label='Statue', job='ConstructStatue'}, {label='Armor stand', job='ConstructArmorStand'},
        {label='Weapon rack', job='ConstructWeaponRack'}, {label='Hatch cover', job='ConstructHatchCover'},
        {label='Grate', job='ConstructGrate'}, {label='Slab', job='ConstructSlab'},
        {label='Quern', job='ConstructQuern'}, {label='Millstone', job='ConstructMillstone'},
        {label='Blocks', job='ConstructBlocks'}, {label='Mechanisms', job='ConstructMechanisms'},
    }},
    {cat='Containers', items={
        {label='Barrel', job='MakeBarrel'}, {label='Bin', job='ConstructBin'},
        {label='Bucket', job='MakeBucket'}, {label='Bag', job='ConstructBag'},
        {label='Cage', job='MakeCage'}, {label='Animal trap', job='MakeAnimalTrap'},
    }},
    {cat='Food & Drink', items={
        {label='Prepared meal', job='PrepareMeal'},
        {label='Brew drink (plant)', reaction='BREW_DRINK_FROM_PLANT'},
        {label='Brew drink (fruit)', reaction='BREW_DRINK_FROM_PLANT_GROWTH'},
        {label='Make mead', reaction='MAKE_MEAD'}, {label='Press honeycomb', reaction='PRESS_HONEYCOMB'},
        {label='Mill plants', job='MillPlants'}, {label='Process plants', job='ProcessPlants'},
        {label='Process plants to bag', reaction='PROCESS_PLANT_TO_BAG'},
        {label='Process plants to barrel', job='ProcessPlantsBarrel'},
        {label='Make cheese', job='MakeCheese'}, {label='Prepare raw fish', job='PrepareRawFish'},
    }},
    {cat='Textiles', items={
        {label='Spin thread', job='SpinThread'}, {label='Weave cloth', job='WeaveCloth'},
        {label='Dye cloth', job='DyeCloth'},
    }},
    {cat='Clothing & Leather', items={
        {label='Shoes', job='MakeShoes'}, {label='Gloves', job='MakeGloves'},
        {label='Trousers', job='MakePants'}, {label='Backpack', job='MakeBackpack'},
        {label='Quiver', job='MakeQuiver'}, {label='Flask / Waterskin', job='MakeFlask'},
        {label='Sew image', job='SewImage'},
    }},
    {cat='Armor & Weapons', items={
        {label='Weapon', job='MakeWeapon'}, {label='Ammo', job='MakeAmmo'},
        {label='Body armor', job='MakeArmor'}, {label='Helm', job='MakeHelm'},
        {label='Shield', job='MakeShield'}, {label='Trap component', job='MakeTrapComponent'},
        {label='Chain', job='MakeChain'},
    }},
    {cat='Smelting & Fuel', items={
        {label='Smelt ore', job='SmeltOre'}, {label='Make charcoal', job='MakeCharcoal'},
        {label='Make ash', job='MakeAsh'}, {label='Potash from ash', job='MakePotashFromAsh'},
        {label='Make lye', job='MakeLye'}, {label='Melt metal object', job='MeltMetalObject'},
        {label='Forge anvil', job='ForgeAnvil'},
    }},
    {cat='Crafts & Goods', items={
        {label='Make crafts', job='MakeCrafts'}, {label='Make totem', job='MakeTotem'},
        {label='Make toy', job='MakeToy'}, {label='Make figurine', job='MakeFigurine'},
        {label='Make amulet', job='MakeAmulet'}, {label='Make ring', job='MakeRing'},
        {label='Make bracelet', job='MakeBracelet'}, {label='Make crown', job='MakeCrown'},
        {label='Make goblet', job='MakeGoblet'}, {label='Make tool', job='MakeTool'},
        {label='Cut gems', job='CutGems'},
        {label='Make soap (tallow)', reaction='MAKE_SOAP_FROM_TALLOW'},
        {label='Make wax crafts', reaction='MAKE_WAX_CRAFTS'},
    }},
    {cat='Glass', items={
        {label='Make raw glass', job='MakeRawGlass'}, {label='Glass window', job='MakeWindow'},
    }},
}

-- Item types a stock condition can be written against (mirrors DF's condition picker).
-- condition_targets() filters out any not present in this build's df.item_type.
local CONDITION_TARGETS = {
    {label='Drinks', item='DRINK'}, {label='Prepared meals', item='FOOD'},
    {label='Plants', item='PLANT'}, {label='Seeds', item='SEEDS'},
    {label='Meat', item='MEAT'}, {label='Fish', item='FISH'}, {label='Cheese', item='CHEESE'},
    {label='Eggs', item='EGG'}, {label='Logs', item='WOOD'}, {label='Bars', item='BAR'},
    {label='Blocks', item='BLOCKS'}, {label='Stones', item='BOULDER'}, {label='Rough gems', item='ROUGH'},
    {label='Cloth', item='CLOTH'}, {label='Thread', item='THREAD'}, {label='Leather', item='SKIN_TANNED'},
    {label='Beds', item='BED'}, {label='Doors', item='DOOR'}, {label='Tables', item='TABLE'},
    {label='Chairs', item='CHAIR'}, {label='Cabinets', item='CABINET'}, {label='Chests', item='BOX'},
    {label='Coffins', item='COFFIN'}, {label='Statues', item='STATUE'}, {label='Barrels', item='BARREL'},
    {label='Bins', item='BIN'}, {label='Buckets', item='BUCKET'}, {label='Bags', item='BAG'},
    {label='Pots / tools', item='TOOL'}, {label='Crafts', item='CRAFTS'}, {label='Mechanisms', item='TRAPPARTS'},
    {label='Weapons', item='WEAPON'}, {label='Body armor', item='ARMOR'}, {label='Shields', item='SHIELD'},
    {label='Helms', item='HELM'}, {label='Gloves', item='GLOVES'}, {label='Shoes', item='SHOES'},
    {label='Trousers', item='PANTS'}, {label='Ammo', item='AMMO'}, {label='Cages', item='CAGE'},
    {label='Totems', item='TOTEM'}, {label='Goblets', item='GOBLET'}, {label='Toys', item='TOY'},
    {label='Splints', item='SPLINT'}, {label='Crutches', item='CRUTCH'}, {label='Anvils', item='ANVIL'},
    {label='Soap', item='GLOB'},
}

-- item enum name -> friendly label (for condition display)
local ITEM_LABEL = {}
for _, t in ipairs(CONDITION_TARGETS) do ITEM_LABEL[t.item] = t.label end

local function reaction_exists(code)
    local ok, found = pcall(function()
        local world = df.global.world
        local reactions = world and world.raws and world.raws.reactions and world.raws.reactions.reactions
        if not reactions then return false end
        for i = 0, #reactions - 1 do
            local rx = reactions[i]
            if rx and rx.code == code then return true end
        end
        return false
    end)
    return ok and found or false
end

local function entry_is_valid(it)
    if it.reaction then return reaction_exists(it.reaction) end
    if it.job then return df.job_type[it.job] ~= nil end
    return false
end

local function entry_key(it)
    if it.job then return 'j:' .. it.job end
    return 'r:' .. it.reaction
end

-- Friendly name for the workshop a reaction is built in (best-effort grouping).
local function reaction_workshop_label(rx)
    local b = rx.building
    if b and b.custom and #b.custom > 0 then
        local cid = b.custom[0]
        local ok, label = pcall(function()
            for _, def in ipairs(df.global.world.raws.buildings.workshops) do
                if def.id == cid then
                    return (def.name and #def.name > 0) and def.name or def.code
                end
            end
            return nil
        end)
        if ok and label then return label end
    end
    return 'Other reactions'
end

-- Full raws reaction enumeration is intentionally not run on the initial web panel
-- load. It touches a wide surface of reaction/workshop raws and can be revisited as
-- a separately tested, paged endpoint. The curated catalog below stays on the same
-- DFHack workorder creation path without making first open fragile.
local function reaction_groups_json(seen)
    seen = seen
    return {}
end

-- Returns the orderable catalog. Keep this bounded; the panel calls it on open.
function order_catalog()
    local cats = {}
    local seen = {}
    for _, group in ipairs(ORDER_CATALOG) do
        local items = {}
        for _, it in ipairs(group.items) do
            if entry_is_valid(it) then
                if it.reaction then seen[it.reaction] = true end
                table.insert(items, '{"key":' .. json_string(entry_key(it)) ..
                    ',"label":' .. json_string(it.label) .. '}')
            end
        end
        if #items > 0 then
            table.insert(cats, '{"cat":' .. json_string(group.cat) ..
                ',"items":[' .. table.concat(items, ',') .. ']}')
        end
    end
    for _, g in ipairs(reaction_groups_json(seen)) do
        table.insert(cats, g)
    end
    return '{"ok":true,"catalog":[' .. table.concat(cats, ',') .. ']}\n'
end

-- Workshops/furnaces for the DF-style "new work order" picker (grouped by station, with the
-- icon key matching the web's building_icons sheet). {buildingType, subtypeEnumName, label, icon}.
local SHOP_CATALOG_SPECS = {
    {'Workshop', 'Carpenters',      "Carpenter's Workshop",  'workshop_carpenter'},
    {'Workshop', 'Masons',          "Mason's Workshop",      'workshop_mason'},
    {'Workshop', 'Craftsdwarfs',    "Craftsdwarf's Workshop",'workshop_crafts'},
    {'Workshop', 'MetalsmithsForge',"Metalsmith's Forge",    'workshop_metalsmith'},
    {'Workshop', 'MagmaForge',      "Magma Forge",           'workshop_metalsmith'},
    {'Workshop', 'Jewelers',        "Jeweler's Workshop",    'workshop_jeweler'},
    {'Workshop', 'Bowyers',         "Bowyer's Workshop",     'workshop_bowyer'},
    {'Workshop', 'Mechanics',       "Mechanic's Workshop",   'workshop_mechanic'},
    {'Workshop', 'Siege',           "Siege Workshop",        'workshop_siege'},
    {'Workshop', 'Ashery',          "Ashery",                'workshop_ashery'},
    {'Workshop', 'Leatherworks',    "Leather Works",         'workshop_leather'},
    {'Workshop', 'Loom',            "Loom",                  'workshop_loom'},
    {'Workshop', 'Clothiers',       "Clothier's Shop",       'workshop_clothes'},
    {'Workshop', 'Dyers',           "Dyer's Shop",           'workshop_dyer'},
    {'Workshop', 'Still',           "Still",                 'workshop_still'},
    {'Workshop', 'Kitchen',         "Kitchen",               'workshop_kitchen'},
    {'Workshop', 'Butchers',        "Butcher's Shop",        'workshop_butcher'},
    {'Workshop', 'Tanners',         "Tanner's Shop",         'workshop_tanner'},
    {'Workshop', 'Fishery',         "Fishery",               'workshop_fishery'},
    {'Workshop', 'Farmers',         "Farmer's Workshop",     'workshop_farmer'},
    {'Workshop', 'Quern',           "Quern",                 'workshop_quern'},
    {'Workshop', 'Millstone',       "Millstone",             'workshop_millstone'},
    {'Workshop', 'Kennels',         "Kennels",               'workshop_kennel'},
    {'Furnace',  'Smelter',         "Smelter",               'furnace_smelter'},
    {'Furnace',  'MagmaSmelter',    "Magma Smelter",         'furnace_smelter'},
    {'Furnace',  'GlassFurnace',    "Glass Furnace",         'furnace_glass'},
    {'Furnace',  'MagmaGlassFurnace',"Magma Glass Furnace",  'furnace_glass'},
    {'Furnace',  'Kiln',            "Kiln",                  'furnace_kiln'},
    {'Furnace',  'MagmaKiln',       "Magma Kiln",            'furnace_kiln'},
    {'Furnace',  'WoodFurnace',     "Wood Furnace",          'furnace_wood'},
}

-- DF-style catalog grouped by WORKSHOP (left=stations with icons, right=their tasks). Reuses
-- dfhack.workshops.getJobs per station; each task becomes the same 'j:'/'r:' order key as the
-- curated catalog so /order-create is unchanged. Bounded; the web calls it when opening "new order".
function order_catalog_by_shop()
    local ok_wo, wo = pcall(require, 'dfhack.workshops')
    if not ok_wo or not wo then return '{"ok":false,"shops":[]}\n' end
    local groups = {}
    for _, spec in ipairs(SHOP_CATALOG_SPECS) do
        local btype = df.building_type[spec[1]]
        local subtype = (spec[1] == 'Workshop') and df.workshop_type[spec[2]] or df.furnace_type[spec[2]]
        if btype and subtype then
            local ok, jobs = pcall(wo.getJobs, btype, subtype, -1)
            if ok and jobs then
                local items = {}
                for _, def in pairs(jobs) do
                    if type(def) == 'table' then
                        local jf = def.job_fields or {}
                        local ekey
                        if jf.reaction_name and #jf.reaction_name > 0 then
                            ekey = 'r:' .. jf.reaction_name
                        elseif jf.job_type then
                            local jn = df.job_type[jf.job_type]
                            if jn then ekey = 'j:' .. jn end
                        end
                        if ekey then table.insert(items, { label = tostring(def.name or ekey), key = ekey }) end
                    end
                end
                table.sort(items, function(a, b) return a.label < b.label end)
                if #items > 0 then
                    local ij = {}
                    for _, it in ipairs(items) do
                        ij[#ij + 1] = '{"key":' .. json_string(it.key) .. ',"label":' .. json_string(it.label) .. '}'
                    end
                    groups[#groups + 1] = '{"shop":' .. json_string(spec[3]) ..
                        ',"icon":' .. json_string(spec[4]) ..
                        ',"items":[' .. table.concat(ij, ',') .. ']}'
                end
            end
        end
    end
    return '{"ok":true,"shops":[' .. table.concat(groups, ',') .. ']}\n'
end

-- The item types you can write a condition against.
function condition_targets()
    local items = {}
    for _, t in ipairs(CONDITION_TARGETS) do
        if df.item_type[t.item] ~= nil then
            table.insert(items, '{"item":' .. json_string(t.item) ..
                ',"label":' .. json_string(t.label) .. '}')
        end
    end
    return '{"ok":true,"targets":[' .. table.concat(items, ',') .. ']}\n'
end

local function pretty_enum_name(name, fallback)
    name = tostring(name or fallback or '')
    if #name == 0 then return tostring(fallback or '') end
    return (name:gsub('_', ' '):gsub('(%l)(%u)', '%1 %2'))
end

local function building_label(b)
    if not b then return '' end
    local ok, name = pcall(dfhack.buildings.getName, b)
    if ok and name and #name > 0 then return name end
    local btype = b:getType()
    if btype == df.building_type.Workshop then
        return pretty_enum_name(df.workshop_type[b.type], 'Workshop')
    elseif btype == df.building_type.Furnace then
        return pretty_enum_name(df.furnace_type[b.type], 'Furnace')
    end
    return pretty_enum_name(df.building_type[btype], 'Building')
end

-- Workshops/furnaces that can receive workshop-specific manager orders.
function order_workshops()
    local ok, result = pcall(function()
    local rows, seen = {}, {}
    local function add_vec(vec, kind)
        if not vec then return end
        for i = 0, #vec - 1 do
            local b = vec[i]
            if b and b.id and not seen[b.id] then
                seen[b.id] = true
                table.insert(rows, {
                    id = b.id,
                    label = building_label(b),
                    kind = kind,
                    x = b.centerx or b.x1 or 0,
                    y = b.centery or b.y1 or 0,
                    z = b.z or 0,
                })
            end
        end
    end
    local other = df.global.world and df.global.world.buildings and df.global.world.buildings.other
    if other then
        add_vec(other.WORKSHOP_ANY, 'Workshop')
        add_vec(other.FURNACE_ANY, 'Furnace')
    end
    table.sort(rows, function(a, b)
        if a.label == b.label then return a.id < b.id end
        return a.label < b.label
    end)
    local out = {}
    for _, b in ipairs(rows) do
        table.insert(out, '{"id":' .. tostring(b.id) ..
            ',"label":' .. json_string(b.label) ..
            ',"kind":' .. json_string(b.kind) ..
            ',"x":' .. tostring(b.x) ..
            ',"y":' .. tostring(b.y) ..
            ',"z":' .. tostring(b.z) .. '}')
    end
    return '{"ok":true,"workshops":[' .. table.concat(out, ',') .. ']}\n'
    end)
    if ok and result then return result end
    return '{"ok":false,"workshops":[],"error":' .. json_string(result) .. '}\n'
end

-- DIAG (crash hunt): flush-guaranteed file tracer. Open/write/close per line so the
-- line is durably on disk BEFORE the next operation runs -> the last line in the file
-- is unambiguously the last thing that executed before a hard crash. REMOVE once fixed.
local function wtrace(msg)
    -- ALWAYS printerr (known to flush per-line in practice) so a trace exists even if file I/O
    -- is unavailable in DFHack's sandbox. The whole file attempt is wrapped in pcall so it can
    -- NEVER raise an error onto the render thread (a raised error here would itself crash).
    dfhack.printerr('dfcap-wshop: ' .. tostring(msg))
    pcall(function()
        local f = io.open('C:/DaMain/Games/Steam/steamapps/common/Dwarf Fortress/dfcap-wshop-trace.log', 'a')
        if f and type(f) == 'userdata' then
            f:write(tostring(msg) .. '\n')
            f:close()
        end
    end)
end

-- Strip DFHack's "unknown material" placeholder so labels match DF's native UI, which simply omits
-- the material until a reagent is chosen ("Make bed", not "Make unknown material bed").
local function strip_unknown_material(name)
    if not name then return name end
    name = name:gsub('%s+of unknown material', '')   -- "X of unknown material"
    name = name:gsub('unknown material%s+', '')       -- "unknown material X"
    name = name:gsub('%s+', ' '):gsub('^%s+', ''):gsub('%s+$', '')
    return name
end

-- Friendly display name for a manager order.
local function order_label(o)
    local ok, name = pcall(dfhack.job.getManagerOrderName, o)
    if ok and name and #name > 0 then return strip_unknown_material(name) end
    if o.job_type == df.job_type.CustomReaction and o.reaction_name and #o.reaction_name > 0 then
        return o.reaction_name
    end
    local jt = df.job_type[o.job_type]
    if not jt then return 'Job #' .. tostring(o.job_type) end
    -- "ConstructBed" -> "Construct Bed"
    return (jt:gsub('(%l)(%u)', '%1 %2'))
end

local function order_material(o)
    if not o.mat_type or o.mat_type < 0 then return '' end
    wtrace('order_material: decode mat_type=' .. tostring(o.mat_type) ..
        ' mat_index=' .. tostring(o.mat_index))   -- DIAG (crash hunt): remove once localized
    local ok, mi = pcall(dfhack.matinfo.decode, o.mat_type, o.mat_index)
    if ok and mi then
        wtrace('order_material: toString')   -- DIAG
        local ok2, tok = pcall(function() return mi:toString() end)
        if ok2 and tok then return tok end
    end
    return ''
end

local function item_type_label(it)
    if it == nil or it < 0 then return 'items' end
    local name = df.item_type[it]
    if not name then return 'item#' .. it end
    return ITEM_LABEL[name] or (name:lower():gsub('_', ' '))
end

local COMPARE_LABEL = {
    [df.logic_condition_type.AtLeast] = '>=',
    [df.logic_condition_type.AtMost] = '<=',
    [df.logic_condition_type.GreaterThan] = '>',
    [df.logic_condition_type.LessThan] = '<',
    [df.logic_condition_type.Exactly] = '=',
    [df.logic_condition_type.Not] = '!=',
}

-- Curated "adjective"/property filters for a stock condition (DF's "Adj"). key -> {flags group,
-- bit, label}. Setting the bit makes the condition only count items with that property.
local CONDITION_ADJECTIVES = {
    metal        = {'flags3', 'metal',        'metal'},
    wood         = {'flags3', 'wood',         'wooden'},
    stone        = {'flags3', 'stone',        'stone'},
    hard         = {'flags3', 'hard',         'hard'},
    edged        = {'flags3', 'edged',        'edged'},
    fire_safe    = {'flags2', 'fire_safe',    'fire-safe'},
    magma_safe   = {'flags2', 'magma_safe',   'magma-safe'},
    non_economic = {'flags2', 'non_economic', 'non-economic'},
    sharpenable  = {'flags1', 'sharpenable',  'sharpenable'},
    cookable     = {'flags1', 'cookable',     'cookable'},
    millable     = {'flags1', 'millable',     'millable'},
    dyeable      = {'flags2', 'dyeable',      'dyeable'},
}

-- Friendly adjective(s) already set on a condition, for display (e.g. "fire-safe metal").
local function condition_adjective_label(c)
    local words = {}
    for _, spec in pairs(CONDITION_ADJECTIVES) do
        local ok, on = pcall(function() return c[spec[1]][spec[2]] end)
        if ok and on then table.insert(words, spec[3]) end
    end
    table.sort(words)
    return table.concat(words, ' ')
end

local function item_condition_label(c)
    local target = item_type_label(c.item_type)
    if c.mat_type and c.mat_type >= 0 then
        local ok, mi = pcall(dfhack.matinfo.decode, c.mat_type, c.mat_index)
        if ok and mi then
            local ok2, s = pcall(function() return mi:toString() end)
            if ok2 and s then target = s .. ' ' .. target end
        end
    end
    local adj = condition_adjective_label(c)
    if adj ~= '' then target = adj .. ' ' .. target end
    local cmp = COMPARE_LABEL[c.compare_type] or '?'
    return ('%s %s %d'):format(target, cmp, c.compare_val or 0)
end

local ORDER_COND_LABEL = {
    [df.workquota_order_condition_type.Activated] = 'is activated',
    [df.workquota_order_condition_type.Completed] = 'is completed',
}

local function order_condition_label(c)
    return ('after #%d %s'):format(c.order_id, ORDER_COND_LABEL[c.condition] or '?')
end

local function conditions_json(o)
    local items = {}
    local item_conditions = o.item_conditions
    if item_conditions then
        for i = 0, #item_conditions - 1 do
            local c = item_conditions[i]
            if c then
                local ok, label = pcall(item_condition_label, c)
                table.insert(items, '{"idx":' .. i ..
                    ',"label":' .. json_string(ok and label or 'condition') .. '}')
            end
        end
    end
    local ords = {}
    local order_conditions = o.order_conditions
    if order_conditions then
        for i = 0, #order_conditions - 1 do
            local c = order_conditions[i]
            if c then
                local ok, label = pcall(order_condition_label, c)
                table.insert(ords, '{"idx":' .. i ..
                    ',"label":' .. json_string(ok and label or 'dependency') .. '}')
            end
        end
    end
    return '"itemConditions":[' .. table.concat(items, ',') ..
        '],"orderConditions":[' .. table.concat(ords, ',') .. ']'
end

-- List current manager orders as JSON (with conditions + workshop limits).
-- Is a manager (MANAGE_PRODUCTION noble) assigned to the fort? DF won't coordinate work orders
-- without one. Canonical check (see DFHack gui/extended-status): the fort entity's
-- assignments_by_type.MANAGE_PRODUCTION list is non-empty.
local function has_manager()
    local ok, result = pcall(function()
        local ent = df.historical_entity.find(df.global.plotinfo.group_id)
        return ent and #ent.assignments_by_type.MANAGE_PRODUCTION > 0
    end)
    return (ok and result) and true or false
end

function list_orders()
    local mgr = has_manager()
    local ok, result = pcall(function()
    local out = {}
    local world = df.global.world
    local all = world and world.manager_orders and world.manager_orders.all
    if not all then return '{"ok":true,"hasManager":' .. json_bool(mgr) .. ',"orders":[]}\n' end
    for pos = 0, #all - 1 do
        local o = all[pos]
        if o then
        local ok_cond, cond_json = pcall(conditions_json, o)
        local parts = {
            '"id":' .. tostring(o.id),
            '"pos":' .. tostring(pos),
            '"job":' .. json_string(order_label(o)),
            '"item":' .. json_string((o.item_type and o.item_type >= 0 and df.item_type[o.item_type]) or ''),
            '"material":' .. json_string(order_material(o)),
            '"amountLeft":' .. tostring(o.amount_left),
            '"amountTotal":' .. tostring(o.amount_total),
            '"frequency":' .. json_string(df.workquota_frequency_type[o.frequency] or 'OneTime'),
            '"workshopId":' .. tostring(o.workshop_id or -1),
            '"workshopName":' .. json_string(building_label(df.building.find(o.workshop_id or -1))),
            '"maxWorkshops":' .. tostring(o.max_workshops or 0),
            '"active":' .. json_bool(o.status.active),
            '"validated":' .. json_bool(o.status.validated),
            ok_cond and cond_json or '"itemConditions":[],"orderConditions":[]',
        }
        table.insert(out, '{' .. table.concat(parts, ',') .. '}')
        end
    end
    return '{"ok":true,"hasManager":' .. json_bool(mgr) .. ',"orders":[' .. table.concat(out, ',') .. ']}\n'
    end)
    if ok and result then return result end
    return '{"ok":false,"hasManager":' .. json_bool(mgr) .. ',"orders":[],"error":' .. json_string(result) .. '}\n'
end

-- ---------------------------------------------------------------------------
-- Workshop/furnace panels
-- ---------------------------------------------------------------------------

local function get_shop(id)
    local b = df.building.find(tonumber(id) or -1)
    if not b then return nil end
    if df.building_workshopst:is_instance(b) or df.building_furnacest:is_instance(b) then
        return b
    end
    return nil
end

local function shop_kind(b)
    if df.building_workshopst:is_instance(b) then return 'Workshop' end
    if df.building_furnacest:is_instance(b) then return 'Furnace' end
    return 'Building'
end

local function shop_subtype_key(b)
    if df.building_workshopst:is_instance(b) then
        return df.workshop_type[b.type] or ''
    elseif df.building_furnacest:is_instance(b) then
        return df.furnace_type[b.type] or ''
    end
    return ''
end

local function job_label(job)
    local ok, name = pcall(dfhack.job.getName, job)
    if ok and name and #name > 0 then return strip_unknown_material(name) end
    if job.job_type == df.job_type.CustomReaction and job.reaction_name and #job.reaction_name > 0 then
        return job.reaction_name
    end
    return pretty_enum_name(df.job_type[job.job_type], 'Job')
end

local function worker_label(job)
    local ok, unit = pcall(dfhack.job.getWorker, job)
    if ok and unit then
        local ok_name, name = pcall(dfhack.units.getReadableName, unit)
        return ok_name and name or ('Unit ' .. tostring(unit.id))
    end
    return ''
end

local function shop_tasks(b)
    wtrace('shop_tasks: enter type=' .. tostring(b:getType()) .. ' sub=' .. tostring(b:getSubtype()) .. ' custom=' .. tostring(b:getCustomType()))   -- DIAG
    local tasks = {}
    local ok, jobs = pcall(function()
        return require('dfhack.workshops').getJobs(b:getType(), b:getSubtype(), b:getCustomType())
    end)
    wtrace('shop_tasks: getJobs returned ok=' .. tostring(ok))   -- DIAG
    if not ok or not jobs then return tasks end
    for key, def in pairs(jobs) do
        if type(def) == 'table' then
            local job_type = def.job_fields and def.job_fields.job_type
            local job_key = job_type and df.job_type[job_type] or ''
            local reaction = def.job_fields and def.job_fields.reaction_name or ''
            local order_key = ''
            if job_type == df.job_type.CustomReaction and reaction and #reaction > 0 then
                order_key = 'r:' .. reaction
            elseif job_key and #job_key > 0 then
                order_key = 'j:' .. job_key
            end
            table.insert(tasks, {
                key = tostring(key),
                name = tostring(def.name or job_key or key),
                job = job_key,
                reaction = tostring(reaction or ''),
                order_key = order_key,
            })
        end
    end
    table.sort(tasks, function(a, b)
        if a.name == b.name then return a.key < b.key end
        return a.name < b.name
    end)
    return tasks
end

local function shop_jobs_json(b)
    local out = {}
    for i = 0, #b.jobs - 1 do
        local job = b.jobs[i]
        if job then
            table.insert(out, '{"id":' .. tostring(job.id) ..
                ',"pos":' .. tostring(i) ..
                ',"name":' .. json_string(job_label(job)) ..
                ',"jobType":' .. json_string(df.job_type[job.job_type] or '') ..
                ',"reaction":' .. json_string(job.reaction_name or '') ..
                ',"worker":' .. json_string(worker_label(job)) ..
                ',"suspended":' .. json_bool(job.flags.suspend) ..
                ',"repeat":' .. json_bool(job.flags['repeat']) ..
                ',"doNow":' .. json_bool(job.flags.do_now) ..
                ',"working":' .. json_bool(job.flags.working or job.flags.fetching or job.flags.bringing) ..
                ',"byManager":' .. json_bool(job.flags.by_manager) .. '}')
        end
    end
    return '[' .. table.concat(out, ',') .. ']'
end

local function shop_tasks_json(tasks)
    local out = {}
    for _, task in ipairs(tasks) do
        table.insert(out, '{"key":' .. json_string(task.key) ..
            ',"name":' .. json_string(task.name) ..
            ',"job":' .. json_string(task.job) ..
            ',"reaction":' .. json_string(task.reaction) ..
            ',"orderKey":' .. json_string(task.order_key) .. '}')
    end
    return '[' .. table.concat(out, ',') .. ']'
end

local function shop_order_label(o)
    return order_label(o):gsub('%s+of unknown material', '')
end

local function shop_orders_json(id)
    local out = {}
    local all = df.global.world and df.global.world.manager_orders and df.global.world.manager_orders.all
    if not all then return '[]' end
    for pos = 0, #all - 1 do
        local o = all[pos]
        if o and tonumber(o.workshop_id or -1) == tonumber(id) then
            table.insert(out, '{"id":' .. tostring(o.id) ..
                ',"pos":' .. tostring(pos) ..
                ',"job":' .. json_string(shop_order_label(o)) ..
                ',"amountLeft":' .. tostring(o.amount_left) ..
                ',"amountTotal":' .. tostring(o.amount_total) ..
                ',"frequency":' .. json_string(df.workquota_frequency_type[o.frequency] or 'OneTime') ..
                ',"active":' .. json_bool(o.status.active) ..
                ',"validated":' .. json_bool(o.status.validated) .. '}')
        end
    end
    return '[' .. table.concat(out, ',') .. ']'
end

local function shop_items_json(b)
    local out = {}
    if not b.contained_items then return '[]' end
    for _, bi in ipairs(b.contained_items) do
        local item = bi and bi.item
        if item then
            local ok, desc = pcall(dfhack.items.getDescription, item, 0, true)
            table.insert(out, '{"id":' .. tostring(item.id) ..
                ',"name":' .. json_string(ok and desc or ('Item ' .. tostring(item.id))) ..
                ',"role":' .. json_string(df.building_item_role_type[bi.use_mode] or '') .. '}')
        end
    end
    return '[' .. table.concat(out, ',') .. ']'
end

local function shop_workers_json(b)
    local profile = b.profile
    if not profile then return '[]' end
    local permitted = {}
    for _, uid in ipairs(profile.permitted_workers) do
        permitted[uid] = true
    end
    local rows = {}
    local units = df.global.world and df.global.world.units and df.global.world.units.active
    if not units then return '[]' end
    for _, unit in ipairs(units) do
        if unit and not dfhack.units.isDead(unit) and dfhack.units.isCitizen(unit, true) then
            local ok_name, name = pcall(dfhack.units.getReadableName, unit)
            local ok_prof, prof = pcall(dfhack.units.getProfessionName, unit)
            table.insert(rows, {
                id = unit.id,
                name = ok_name and name or ('Unit ' .. tostring(unit.id)),
                profession = ok_prof and prof or '',
                assigned = permitted[unit.id] or false,
            })
        end
    end
    table.sort(rows, function(a, b)
        if a.assigned ~= b.assigned then return a.assigned end
        return a.name < b.name
    end)
    local out = {}
    for _, u in ipairs(rows) do
        table.insert(out, '{"id":' .. tostring(u.id) ..
            ',"name":' .. json_string(u.name) ..
            ',"profession":' .. json_string(u.profession) ..
            ',"assigned":' .. json_bool(u.assigned) .. '}')
    end
    return '[' .. table.concat(out, ',') .. ']'
end

-- Run one workshop_info section under pcall so a single failing section degrades to a safe
-- fallback (empty list) instead of taking down the WHOLE panel ("Workshop data unavailable").
-- Logs the exact section + error so the root cause is still pinpointed. The CoreSuspender fix
-- makes raising/catching a Lua error here safe (full stack -> no traceback overflow).
local function ws_section(label, fn, fallback)
    wtrace('workshop_info: ' .. label)
    local ok, res = pcall(fn)
    if ok and res ~= nil then return res end
    wtrace('workshop_info: ' .. label .. ' FAILED: ' .. tostring(res))
    return fallback
end

local function ws_safe_str(fn, fallback)
    local ok, v = pcall(fn)
    if ok and v ~= nil then return v end
    return fallback
end

function workshop_info(id)
    wtrace('workshop_info: ENTER id=' .. tostring(id))   -- DIAG: logged BEFORE get_shop (fishery hunt)
    local ok_gs, b = pcall(get_shop, id)
    if not ok_gs then
        wtrace('workshop_info: get_shop THREW id=' .. tostring(id) .. ': ' .. tostring(b))
        return '{"ok":false,"error":"get_shop error"}\n'
    end
    if not b then
        local okr, raw = pcall(df.building.find, tonumber(id) or -1)
        wtrace('workshop_info: get_shop NIL id=' .. tostring(id) ..
            ' rawType=' .. tostring(okr and raw and raw:getType()) ..
            ' isWkshop=' .. tostring(okr and raw and df.building_workshopst:is_instance(raw)) ..
            ' isFurnace=' .. tostring(okr and raw and df.building_furnacest:is_instance(raw)))
        return '{"ok":false,"error":"workshop not found"}\n'
    end
    wtrace('workshop_info: id=' .. tostring(id))   -- DIAG (crash hunt)
    local profile = b.profile or {}
    local tasks    = ws_section('shop_tasks',        function() return shop_tasks(b) end, {})
    local j_jobs   = ws_section('shop_jobs_json',    function() return shop_jobs_json(b) end, '[]')
    local j_tasks  = ws_section('shop_tasks_json',   function() return shop_tasks_json(tasks) end, '[]')
    local j_orders = ws_section('shop_orders_json',  function() return shop_orders_json(b.id) end, '[]')
    local j_items  = ws_section('shop_items_json',   function() return shop_items_json(b) end, '[]')
    local j_workers= ws_section('shop_workers_json', function() return shop_workers_json(b) end, '[]')
    wtrace('workshop_info: assemble')   -- DIAG
    local parts = {
        '"ok":true',
        '"id":' .. tostring(b.id),
        '"name":' .. json_string(ws_safe_str(function() return building_label(b) end, 'Workshop')),
        '"kind":' .. json_string(ws_safe_str(function() return shop_kind(b) end, 'Workshop')),
        '"subtype":' .. json_string(ws_safe_str(function() return shop_subtype_key(b) end, '')),
        '"x":' .. tostring(b.centerx or b.x1 or 0),
        '"y":' .. tostring(b.centery or b.y1 or 0),
        '"z":' .. tostring(b.z or 0),
        '"jobs":' .. j_jobs,
        '"tasks":' .. j_tasks,
        '"orders":' .. j_orders,
        '"items":' .. j_items,
        '"profile":{"maxGeneralOrders":' .. tostring(profile.max_general_orders or 0) ..
            ',"permittedCount":' .. tostring((profile.permitted_workers and #profile.permitted_workers) or 0) ..
            ',"minLevel":' .. tostring(profile.min_level or -1) ..
            ',"maxLevel":' .. tostring(profile.max_level or -1) .. '}',
        '"workers":' .. j_workers,
        '"canAddTasks":' .. json_bool(#tasks > 0),
    }
    return '{' .. table.concat(parts, ',') .. '}\n'
end

local function find_shop_job(b, job_id)
    job_id = tonumber(job_id)
    if not b or not job_id then return nil end
    for i = 0, #b.jobs - 1 do
        local job = b.jobs[i]
        if job and job.id == job_id then return job end
    end
    return nil
end

function workshop_job_action(id, job_id, action)
    local b = get_shop(id)
    if not b then return false, 'workshop not found' end
    local job = find_shop_job(b, job_id)
    if not job then return false, 'job not found in workshop' end
    action = tostring(action or '')
    if action == 'cancel' then
        local ok, err = pcall(dfhack.job.removeJob, job)
        if not ok then return false, tostring(err) end
        return true, ''
    elseif action == 'suspend' then
        job.flags.suspend = true
    elseif action == 'resume' then
        job.flags.suspend = false
    elseif action == 'repeat' then
        job.flags['repeat'] = not job.flags['repeat']
    elseif action == 'now' then
        job.flags.do_now = true
    else
        return false, 'unknown job action'
    end
    pcall(dfhack.job.checkBuildingsNow)
    return true, ''
end

local function task_material_categories(def)
    local seen, out = {}, {}
    local function add(name)
        if name and not seen[name] then
            seen[name] = true
            table.insert(out, name)
        end
    end
    for _, item_def in ipairs((def and def.items) or {}) do
        if item_def.item_type == df.item_type.WOOD or item_def.vector_id == df.job_item_vector_id.WOOD then
            add('wood')
        elseif item_def.vector_id == df.job_item_vector_id.PLANT or item_def.item_type == df.item_type.PLANT then
            add('plant')
        elseif item_def.item_type == df.item_type.THREAD then
            add('plant')
        end
    end
    return #out > 0 and out or nil
end

local function create_shop_order_from_task(b, def, amount, frequency)
    if not b then return false, 'workshop not found' end
    if not def then return false, 'task not found' end
    local job_fields = def.job_fields or {}
    local job_type = job_fields.job_type
    if not job_type or not df.job_type[job_type] then return false, 'task has no manager-order job type' end

    local order_def = {
        amount_total = clamp(tonumber(amount) or 1, 1, 9999),
        frequency = tostring(frequency or 'OneTime'),
        workshop_id = b.id,
    }
    if not df.workquota_frequency_type[order_def.frequency] then order_def.frequency = 'OneTime' end
    -- job is the STRING name (workorder's ensure_df_id accepts string or int; string matches the
    -- proven Manager create_order). material_category restored so the order shows its material.
    local job_name = (type(job_type) == 'string') and job_type or df.job_type[job_type]
    if job_name == 'CustomReaction' then
        if not job_fields.reaction_name or #job_fields.reaction_name == 0 then
            return false, 'custom reaction task has no reaction code'
        end
        order_def.job = 'CustomReaction'
        order_def.reaction = job_fields.reaction_name
    else
        order_def.job = job_name
    end

    local cats = task_material_categories(def)
    if cats then order_def.material_category = cats end

    local ok_req, wo = pcall(reqscript, 'workorder')
    if not ok_req or not wo then return false, 'workorder module unavailable' end
    wtrace('create_shop_order: job=' .. tostring(order_def.job) ..
        ' mat_cat=' .. tostring(order_def.material_category and 'set' or 'nil'))   -- DIAG (crash hunt)
    local ok, err = pcall(function()
        local orders = wo.preprocess_orders({order_def})
        wtrace('create_shop_order: preprocess ok, fillin_defaults')   -- DIAG
        wo.fillin_defaults(orders)
        wtrace('create_shop_order: fillin ok, create_orders')   -- DIAG
        wo.create_orders(orders, true)
        wtrace('create_shop_order: create_orders ok')   -- DIAG
    end)
    if not ok then return false, tostring(err) end
    return true, 'shop work order queued'
end

-- Turn a workshops.getJobs() item-filter table into a real df.job_item (the reagent
-- requirement DF gathers materials against). input_filter_defaults is exactly a job_item
-- template, so we only copy the fields that are present and leave job_item's own defaults
-- for the rest (clobbering with nil/wrong values causes "unknown material" + uncompletable jobs).
local function build_job_item(item_def)
    local ji = df.job_item:new()
    if item_def.item_type ~= nil then ji.item_type = item_def.item_type end
    if item_def.item_subtype ~= nil then ji.item_subtype = item_def.item_subtype end
    if item_def.mat_type ~= nil then ji.mat_type = item_def.mat_type end
    if item_def.mat_index ~= nil then ji.mat_index = item_def.mat_index end
    if item_def.quantity ~= nil then ji.quantity = item_def.quantity end
    if item_def.vector_id ~= nil then ji.vector_id = item_def.vector_id end
    if item_def.reaction_class ~= nil then ji.reaction_class = item_def.reaction_class end
    if item_def.has_material_reaction_product ~= nil then ji.has_material_reaction_product = item_def.has_material_reaction_product end
    if item_def.metal_ore ~= nil then ji.metal_ore = item_def.metal_ore end
    if item_def.min_dimension ~= nil then ji.min_dimension = item_def.min_dimension end
    if item_def.has_tool_use ~= nil then ji.has_tool_use = item_def.has_tool_use end
    if type(item_def.flags1) == 'table' then for k, v in pairs(item_def.flags1) do pcall(function() ji.flags1[k] = v end) end end
    if type(item_def.flags2) == 'table' then for k, v in pairs(item_def.flags2) do pcall(function() ji.flags2[k] = v end) end end
    if type(item_def.flags3) == 'table' then for k, v in pairs(item_def.flags3) do pcall(function() ji.flags3[k] = v end) end end
    if type(item_def.flags4) == 'number' then ji.flags4 = item_def.flags4 end
    if type(item_def.flags5) == 'number' then ji.flags5 = item_def.flags5 end
    return ji
end

-- DIAG (material hunt): dump every job of a given type with the exact fields DF's namer reads,
-- so a natively-queued bed (shows "Make bed") can be compared field-by-field with ours
-- (shows "Make unknown material bed"). REMOVE once the field difference is found + fixed.
local function dump_jobs_of_type(jt)
    local link = df.global.world.jobs.list.next
    while link do
        local j = link.item
        if j and j.job_type == jt then
            local okn, nm = pcall(dfhack.job.getName, j)
            wtrace(string.format('DUMP-JOB id=%s mat_type=%s mat_index=%s item_type=%s item_subtype=%s specflag=%s matcat=%s njobitems=%s name=%s',
                tostring(j.id), tostring(j.mat_type), tostring(j.mat_index),
                tostring(j.item_type), tostring(j.item_subtype),
                tostring(j.specflag.whole), tostring(j.material_category.whole),
                tostring(#j.job_items.elements), tostring(okn and nm or 'ERR')))
        end
        link = link.next
    end
end

-- Add a SINGLE direct job to the workshop building (exactly what DF's "Add new task" does):
-- not a manager work order. Direct jobs need no manager, use the building's real reagent
-- filters (so dwarves gather "any wood" etc.), and show the correct material -- which also
-- fixes the "Make unknown material X" jobs the manager-order path produced.
local function add_workshop_task(b, def)
    local job_fields = def.job_fields or {}
    local job_type = job_fields.job_type
    if not job_type or not df.job_type[job_type] then return false, 'task has no job type' end
    if job_type == df.job_type.CustomReaction and (not job_fields.reaction_name or #job_fields.reaction_name == 0) then
        return false, 'custom reaction task has no reaction code'
    end

    local job = df.job:new()
    job.job_type = job_type
    job.completion_timer = -1
    job.pos.x = b.centerx or b.x1 or 0
    job.pos.y = b.centery or b.y1 or 0
    job.pos.z = b.z or 0
    -- product material: -1 means "decided by the gathered reagent" (the normal case, e.g. a bed
    -- takes the wood it's made from); only jobs that hardcode a material set job_fields.mat_type.
    job.mat_type = job_fields.mat_type or -1
    job.mat_index = job_fields.mat_index or -1
    if job_type == df.job_type.CustomReaction then
        job.reaction_name = job_fields.reaction_name
    end

    -- link the job to the building, then append the reagent requirements.
    job.general_refs:insert('#', { new = df.general_ref_building_holderst, building_id = b.id })
    b.jobs:insert('#', job)
    wtrace('add_task: job_type=' .. tostring(df.job_type[job_type]) .. ' job.mat_type=' .. tostring(job.mat_type) ..
        ' #def.items=' .. tostring(def.items and #def.items or 0))   -- DIAG (material hunt)
    for i, item_def in ipairs(def.items or {}) do
        wtrace('add_task: item[' .. i .. '] item_type=' .. tostring(item_def.item_type) ..
            ' mat_type=' .. tostring(item_def.mat_type) .. ' vector_id=' .. tostring(item_def.vector_id) ..
            ' quantity=' .. tostring(item_def.quantity))   -- DIAG
        job.job_items.elements:insert('#', build_job_item(item_def))
    end
    wtrace('add_task: built #job_items=' .. tostring(#job.job_items.elements))   -- DIAG
    local ok_nm, nm = pcall(dfhack.job.getName, job)
    wtrace('add_task: getName=' .. tostring(ok_nm and nm or 'ERR'))   -- DIAG

    local ok, err = pcall(dfhack.job.linkIntoWorld, job, true)
    if not ok then
        -- back out the half-built job: drop the building's reference, then free the job
        -- (which owns the building-holder ref and the job_items we inserted).
        pcall(function() b.jobs:erase(#b.jobs - 1) end)
        pcall(function() job:delete() end)
        return false, 'could not link job: ' .. tostring(err)
    end
    pcall(dfhack.job.checkBuildingsNow)
    pcall(dump_jobs_of_type, job_type)   -- DIAG: dump ALL ConstructBed jobs (native + ours) to compare
    return true, 'task added'
end

function workshop_add_job(id, task_key)
    local b = get_shop(id)
    if not b then return false, 'workshop not found' end
    local ok_jobs, jobs = pcall(function()
        return require('dfhack.workshops').getJobs(b:getType(), b:getSubtype(), b:getCustomType())
    end)
    if not ok_jobs or not jobs then return false, 'could not list workshop jobs' end
    local def = jobs[tonumber(task_key)] or jobs[tostring(task_key)]
    if not def then return false, 'task not found' end
    -- Tasks tab = a single direct workshop job (NOT a manager work order). Work orders are created
    -- separately via the Work Orders tab / create_order.
    return add_workshop_task(b, def)
end

function workshop_worker_action(id, unit_id, assign)
    local b = get_shop(id)
    if not b then return false, 'workshop not found' end
    local profile = b.profile
    if not profile then return false, 'workshop has no profile' end
    unit_id = tonumber(unit_id)
    if not unit_id or not df.unit.find(unit_id) then return false, 'unit not found' end
    local vec = profile.permitted_workers
    local found = -1
    for i = 0, #vec - 1 do
        if vec[i] == unit_id then found = i; break end
    end
    if assign and found < 0 then
        vec:insert('#', unit_id)
    elseif not assign and found >= 0 then
        vec:erase(found)
    end
    return true, ''
end

function workshop_workers_clear(id)
    local b = get_shop(id)
    if not b then return false, 'workshop not found' end
    if not b.profile then return false, 'workshop has no profile' end
    b.profile.permitted_workers:resize(0)
    return true, ''
end

local function find_order(id)
    id = tonumber(id)
    if not id then return nil end
    local all = df.global.world.manager_orders.all
    for i = 0, #all - 1 do
        local o = all[i]
        if o and o.id == id then return o end
    end
    return nil
end

-- Create one manager order from a catalog key ('j:'<job> or 'r:'<reaction>).
function create_order(key, amount, frequency, workshop_id)
    key = tostring(key or '')
    local jname = key:match('^j:(.+)$')
    local rcode = key:match('^r:(.+)$')
    local def_job, def_reaction
    if jname then
        if df.job_type[jname] == nil then return false, 'unknown job: ' .. jname end
        def_job = jname
    elseif rcode then
        if not reaction_exists(rcode) then return false, 'unknown reaction: ' .. rcode end
        def_reaction = rcode
    else
        return false, 'unknown order key: ' .. key
    end
    amount = clamp(tonumber(amount) or 1, 1, 9999)
    frequency = tostring(frequency or 'OneTime')
    if not df.workquota_frequency_type[frequency] then frequency = 'OneTime' end

    local def = {amount_total = amount, frequency = frequency}
    local wid = tonumber(workshop_id)
    if wid and wid >= 0 then
        if not df.building.find(wid) then return false, 'workshop not found' end
        def.workshop_id = wid
    end
    if def_reaction then
        def.job = 'CustomReaction'
        def.reaction = def_reaction
    else
        def.job = def_job
    end

    local ok_req, wo = pcall(reqscript, 'workorder')
    if not ok_req or not wo then return false, 'workorder module unavailable' end
    local ok, err = pcall(function()
        local orders = wo.preprocess_orders({def})
        wo.fillin_defaults(orders)
        wo.create_orders(orders, true)
    end)
    if not ok then return false, tostring(err) end
    return true, 'order queued'
end

-- Add a stock condition: "amount of [adjective] [material] <item_name> <compare> <value>".
-- material = "matType:matIndex" (optional), adjective = a CONDITION_ADJECTIVES key (optional).
function add_item_condition(order_id, compare, value, item_name, material, adjective)
    local o = find_order(order_id)
    if not o then return false, 'order not found' end
    local ctype = df.logic_condition_type[tostring(compare or 'AtMost')]
    if ctype == nil then return false, 'bad comparison' end
    value = clamp(tonumber(value) or 0, 0, 999999)
    local it = df.item_type.NONE
    if item_name and item_name ~= '' then
        local resolved = df.item_type[tostring(item_name)]
        if resolved == nil then return false, 'bad item type: ' .. tostring(item_name) end
        it = resolved
    end
    local mt, mi = -1, -1
    if material and material ~= '' then
        local a, b = tostring(material):match('^(-?%d+):(-?%d+)$')
        if a then mt, mi = tonumber(a), tonumber(b) end
    end
    local adj_spec = adjective and adjective ~= '' and CONDITION_ADJECTIVES[tostring(adjective)] or nil
    local c = df.manager_order_condition_item:new()
    local ok, err = pcall(function()
        c.compare_type = ctype
        c.compare_val = value
        c.item_type = it
        c.item_subtype = -1
        c.mat_type = mt
        c.mat_index = mi
        c.min_dimension = -1
        c.reaction_id = -1
        -- CRITICAL: these have NO init-value in df-structures, so :new() leaves them at 0 -- but DF's
        -- "any" sentinel is -1. Left at 0 the condition means "metal ore #0 / dye color #0 / tool-use
        -- LIQUID_COOKING", which DF's condition checker crashes on. Set them to the proper -1/NONE.
        c.metal_ore = -1
        c.has_tool_use = -1   -- df.tool_uses.NONE
        c.dye_color = -1
        if adj_spec then c[adj_spec[1]][adj_spec[2]] = true end
        o.item_conditions:insert('#', c)
    end)
    if not ok then pcall(function() c:delete() end); return false, tostring(err) end
    return true, 'condition added'
end

-- Materials available in the fort for a given condition item type (for the condition "Mat" picker).
-- item_name is an item_type enum name (e.g. "BAR", "BOULDER"); empty = across all item types.
function condition_materials(item_name)
    local it = nil
    if item_name and item_name ~= '' then it = df.item_type[tostring(item_name)] end
    local items_vec = df.global.world.items.other.IN_PLAY
    local groups, order = {}, {}
    for ii = 0, #items_vec - 1 do
        local item = items_vec[ii]
        if item and (it == nil or item:getType() == it) then
            local mt, mi = item:getMaterial(), item:getMaterialIndex()
            if mt and mt >= 0 then
                local key = tostring(mt) .. ':' .. tostring(mi)
                local g = groups[key]
                if not g then
                    local nm = ''
                    local okm, info = pcall(dfhack.matinfo.decode, mt, mi)
                    if okm and info then
                        local oks, s = pcall(function() return info:toString() end)
                        if oks and s then nm = s end
                    end
                    g = { mat_type = mt, mat_index = mi, name = nm, count = 0 }
                    groups[key] = g
                    table.insert(order, key)
                end
                g.count = g.count + (item.stack_size or 1)
            end
        end
    end
    table.sort(order, function(a, b) return (groups[a].name or '') < (groups[b].name or '') end)
    local mats = {}
    for _, key in ipairs(order) do
        local g = groups[key]
        table.insert(mats, '{"matType":' .. tostring(g.mat_type) ..
            ',"matIndex":' .. tostring(g.mat_index) ..
            ',"name":' .. json_string((g.name ~= '' and g.name) or ('material ' .. key)) ..
            ',"count":' .. tostring(g.count) .. '}')
    end
    return '{"ok":true,"materials":[' .. table.concat(mats, ',') .. ']}\n'
end

-- DF-style suggested conditions for an order: keep the product from overflowing
-- ("amount of <product> < total") and ensure reagents are on hand ("amount of <reagent> > 0").
-- Returns clickable suggestions the web can one-tap to add via add_item_condition.
function suggested_conditions(order_id)
    local o = find_order(order_id)
    if not o then return '{"ok":false,"suggestions":[]}\n' end
    local out, seen = {}, {}
    local function add(item_name, compare, value, label)
        if not item_name or seen[item_name .. compare] then return end
        seen[item_name .. compare] = true
        table.insert(out, '{"item":' .. json_string(item_name) ..
            ',"compare":' .. json_string(compare) ..
            ',"value":' .. tostring(value) ..
            ',"label":' .. json_string(label) .. '}')
    end
    -- product: don't keep making it once we have enough
    if o.item_type and o.item_type >= 0 then
        local pname = df.item_type[o.item_type]
        if pname then
            local total = (o.amount_total and o.amount_total > 0) and o.amount_total or 10
            add(pname, 'LessThan', total, 'Amount of ' .. item_type_label(o.item_type) .. ' available is less than ' .. total)
        end
    end
    -- reagents: for a custom reaction we can read its reagent item types from the raws
    if o.reaction_name and #o.reaction_name > 0 then
        pcall(function()
            local rxs = df.global.world.raws.reactions.reactions
            for ri = 0, #rxs - 1 do
                local rx = rxs[ri]
                if rx and rx.code == o.reaction_name then
                    for gi = 0, #rx.reagents - 1 do
                        local reagent = rx.reagents[gi]
                        local rit = reagent and reagent.item_type
                        if rit and rit >= 0 then
                            local rname = df.item_type[rit]
                            if rname then
                                add(rname, 'AtLeast', 1, 'Amount of ' .. item_type_label(rit) .. ' available is at least 1')
                            end
                        end
                    end
                    break
                end
            end
        end)
    end
    return '{"ok":true,"suggestions":[' .. table.concat(out, ',') .. ']}\n'
end

-- Add a dependency: this order runs only after <other_id> is Activated/Completed.
function add_order_condition(order_id, other_id, cond_type)
    local o = find_order(order_id)
    if not o then return false, 'order not found' end
    local other = find_order(other_id)
    if not other then return false, 'target order not found' end
    if other.id == o.id then return false, 'an order cannot depend on itself' end
    local ct = df.workquota_order_condition_type[tostring(cond_type or 'Completed')]
    if ct == nil then return false, 'bad condition type' end
    local c = df.manager_order_condition_order:new()
    local ok, err = pcall(function()
        c.order_id = other.id
        c.condition = ct
        o.order_conditions:insert('#', c)
    end)
    if not ok then pcall(function() c:delete() end); return false, tostring(err) end
    return true, 'dependency added'
end

-- Remove a condition by index. kind = 'item' or 'order'. Erases the pointer (no
-- delete) the same way cancel_order does -- safe, tiny leak.
function remove_condition(order_id, kind, idx)
    local o = find_order(order_id)
    if not o then return false, 'order not found' end
    idx = tonumber(idx)
    local vec = (tostring(kind) == 'order') and o.order_conditions or o.item_conditions
    if not idx or idx < 0 or idx >= #vec then return false, 'bad condition index' end
    vec:erase(idx)
    return true, 'condition removed'
end

-- Limit how many workshops fill this order at once (0 = unlimited).
function set_order_max_workshops(order_id, max)
    local o = find_order(order_id)
    if not o then return false, 'order not found' end
    o.max_workshops = clamp(tonumber(max) or 0, 0, 30)
    return true, 'updated'
end

-- Assign an order to one workshop/furnace. workshop_id < 0 clears the assignment.
function set_order_workshop(order_id, workshop_id)
    local o = find_order(order_id)
    if not o then return false, 'order not found' end
    local wid = tonumber(workshop_id) or -1
    if wid >= 0 and not df.building.find(wid) then return false, 'workshop not found' end
    o.workshop_id = wid
    return true, 'updated'
end

-- Move an order up (dir<0) or down (dir>0) in the manager queue (= priority).
function reorder_order(order_id, dir)
    local all = df.global.world.manager_orders.all
    order_id = tonumber(order_id)
    local idx = nil
    for i = 0, #all - 1 do
        if all[i].id == order_id then idx = i; break end
    end
    if idx == nil then return false, 'order not found' end
    local j = idx + ((tonumber(dir) or 0) < 0 and -1 or 1)
    if j < 0 or j >= #all then return false, 'cannot move further' end
    local moved = all[idx]
    all:erase(idx)
    all:insert(j, moved)
    return true, 'reordered'
end

-- Import a shipped/saved order preset by name (e.g. "library/basic"). Returns (ok, msg).
function import_order_preset(name)
    name = tostring(name or '')
    if #name == 0 then return false, 'no preset name' end
    local before = #df.global.world.manager_orders.all
    local ok, err = pcall(dfhack.run_command, 'orders', 'import', name)
    if not ok then return false, tostring(err) end
    local added = #df.global.world.manager_orders.all - before
    return true, ('imported %d order(s) from %s'):format(added, name)
end

-- List shipped presets without invoking another DFHack command during panel load.
function order_presets()
    local out = {
        json_string('library/basic'),
        json_string('library/furnace'),
        json_string('library/glassstock'),
        json_string('library/military'),
        json_string('library/rockstock'),
        json_string('library/smelting'),
    }
    return '{"ok":true,"presets":[' .. table.concat(out, ',') .. ']}\n'
end

-- Cancel (remove) a manager order by id. Mirrors workorder.lua's own erase path.
function cancel_order(id)
    id = tonumber(id)
    if not id then return false, 'bad id' end
    local all = df.global.world.manager_orders.all
    for i = #all - 1, 0, -1 do
        if all[i].id == id then
            all:erase(i)
            return true, ''
        end
    end
    return false, 'order not found'
end

-- Change an order's target amount and/or frequency. Returns (ok, msg).
function adjust_order(id, amount, frequency)
    id = tonumber(id)
    if not id then return false, 'bad id' end
    local all = df.global.world.manager_orders.all
    for i = 0, #all - 1 do
        local o = all[i]
        if o and o.id == id then
            local a = tonumber(amount)
            if a and a >= 0 then
                o.amount_total = clamp(a, 0, 9999)
                o.amount_left = o.amount_total
            end
            if frequency ~= nil and frequency ~= '' then
                local f = df.workquota_frequency_type[tostring(frequency)]
                if f then o.frequency = f end
            end
            return true, ''
        end
    end
    return false, 'order not found'
end

local function safe_json(fn)
    return function(...)
        local ok, result = pcall(fn, ...)
        if ok then return result end
        return '{"ok":false,"error":' .. json_string(result) .. '}\n'
    end
end

order_catalog = safe_json(order_catalog)
condition_targets = safe_json(condition_targets)
order_workshops = safe_json(order_workshops)
list_orders = safe_json(list_orders)
order_presets = safe_json(order_presets)
workshop_info = safe_json(workshop_info)

return _ENV

