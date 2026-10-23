// EntityCache.hpp — actor collection independent from rendering.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Actors.hpp"
#include "Camera.hpp"
#include "Config.hpp"
#include "Nameplate.hpp"
#include "Projection.hpp"
#include "RealNames.hpp"
#include "Reflection.hpp"
#include "Strings.hpp"
#include "World.hpp"

namespace aion {

enum class EntityType : std::uint8_t {
    Other = 0,
    Player,
    Npc,
    Pet,
    Monster,
    Item,
    Gather,
    Interact,  // quest/story interaction objects (traces, levers, hidden objects)
    Object,  // any other world actor (env objects, props)
    Noise,   // engine/world scaffolding - never drawn
    // Appended (not inserted) so the existing values keep their indices, which arrays and
    // the category counters rely on. Portals/gates: mesh Portal_Space_01 is the "Lost Ruin
    // Entrance" on BP_EnvObj_Common_C.
    Portal,
    // Reward/hidden cubes. Their own category because the client's interaction classes put
    // them in Interact and the Gather "Cube" pattern would otherwise take them.
    Cube,
    // Containers the client still reports as unopened. The family used to be dropped outright by
    // AlwaysHidden; the state byte that separates a live chest from a spent one is
    // AionEnvObjAnimInstance::MainState, so the ones worth walking to have their own category.
    Chest,
};

inline const char* EntityTypeName(EntityType t) {
    switch (t) {
        case EntityType::Player:  return "Player";
        case EntityType::Npc:     return "NPC";
        case EntityType::Pet:     return "Pet";
        case EntityType::Monster: return "Monster";
        case EntityType::Item:    return "Item";
        case EntityType::Gather:  return "Gather";
        case EntityType::Interact: return "Interact";
        case EntityType::Portal: return "Portal";
        case EntityType::Cube: return "Cube";
        case EntityType::Chest: return "Chest";
        case EntityType::Object:  return "Object";
        case EntityType::Noise:   return "Noise";
        default:                  return "Other";
    }
}

// Case-insensitive substring test — see Strings.hpp.

// Optional user-supplied per-category class lists (comma separated substrings,
// from aion.ini). Checked BEFORE the built-in patterns so users can pin exact
// classes without recompiling. There is no chest list: containers are never drawn
// (see kContainers in AlwaysHidden), and never_show= is the list that hides more.
struct ClassRules {
    std::vector<std::string> player, npc, pet, monster, item, gather, cube, interact,
        ignore;
    // Classes never drawn, not even in the esp_show_all diagnostic (substrings).
    std::vector<std::string> never_show;
    // Bosses and mini-bosses (substrings). Nothing the client exposes says which monster will
    // attack unprovoked - the aggro decision belongs to the server (AggroInfoData /
    // NpcFirstAggro_NT) and no rank survives in the object names or dumps - so the rank is
    // declared here and painted differently. See IsBossActor.
    std::vector<std::string> boss;
    // The monsters the user is hunting (same matching as boss). Painted in the drop colour, so a
    // short deliberate list reads as "this is what you came for" - see IsTargetActor.
    std::vector<std::string> target;
    // pattern -> display name. Matched (case-insensitive substring) against the
    // mesh/model name first, then the object name and the class name. This is how
    // "MOB_Specter_01" becomes "Lost Specter".
    std::vector<std::pair<std::string, std::string>> name_map;
};

// "pattern=Display Name;other=Name" -> { {"pattern","Display Name"}, ... }
inline std::vector<std::pair<std::string, std::string>> ParseNameMap(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        std::string entry = s.substr(i, j - i);
        const std::size_t eq = entry.find('=');
        if (eq != std::string::npos) {
            auto trim = [](std::string v) {
                const char* ws = " \t\r\n";
                const auto a = v.find_first_not_of(ws);
                if (a == std::string::npos) return std::string{};
                const auto b = v.find_last_not_of(ws);
                return v.substr(a, b - a + 1);
            };
            std::string pat = trim(entry.substr(0, eq));
            std::string name = trim(entry.substr(eq + 1));
            if (!pat.empty() && !name.empty()) out.emplace_back(std::move(pat), std::move(name));
        }
        i = j + 1;
    }
    return out;
}

// name_map -> display name, or {} when nothing matches.
inline std::string MapDisplayName(const std::vector<std::pair<std::string, std::string>>& map,
                                  const std::string& model, const std::string& cls,
                                  const std::string& object_name) {
    for (const auto& kv : map) {
        if (HasI(model, kv.first.c_str()) || HasI(object_name, kv.first.c_str()) ||
            HasI(cls, kv.first.c_str()))
            return kv.second;
    }
    return {};
}

// InList lives in Strings.hpp.

// "a, b ,c" -> {"a","b","c"}. A line break separates too: the menu's list boxes are text, and one
// name per line is how a list is actually built while playing. The ini value is still one line (the
// file is split into lines before it is parsed), so Pairs() writes the breaks back out as commas.
inline std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    const auto sep = [](char c) { return c == ',' || c == '\n' || c == '\r'; };
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || sep(s[i]))) ++i;
        std::size_t j = i;
        while (j < s.size() && !sep(s[j])) ++j;
        std::size_t e = j;
        while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
        if (e > i) out.push_back(s.substr(i, e - i));
        i = j + 1;
    }
    return out;
}

// The list_* keys of aion.ini as the classifier wants them, in one place: the startup path and
// the menu's reload both go through it, so the two cannot drift apart. (list_cube used to be
// documented in aion.ini and read by the classifier, but was never filled in from the config.)
// boss_extra / target_extra append entries from a list file to the ini's lists: the union the
// retired aion_boss.txt / aion_target.txt used to reach the classifier through. Nothing passes them
// any more - those files are folded into list_boss / list_target once, at startup, so that the ini
// is the one home for the lists - and only overlay_test's CheckListFiles still covers the union.
inline ClassRules RulesFromConfig(const Config& c,
                                  const std::vector<std::string>& boss_extra = {},
                                  const std::vector<std::string>& target_extra = {}) {
    ClassRules r;
    r.player = SplitList(c.list_player);
    r.npc = SplitList(c.list_npc);
    r.pet = SplitList(c.list_pet);
    r.monster = SplitList(c.list_monster);
    r.item = SplitList(c.list_item);
    r.gather = SplitList(c.list_gather);
    r.cube = SplitList(c.list_cube);
    r.interact = SplitList(c.list_interact);
    r.ignore = SplitList(c.list_ignore);
    r.never_show = SplitList(c.never_show);
    r.boss = SplitList(c.list_boss);
    r.boss.insert(r.boss.end(), boss_extra.begin(), boss_extra.end());
    r.target = SplitList(c.list_target);
    r.target.insert(r.target.end(), target_extra.begin(), target_extra.end());
    r.name_map = ParseNameMap(c.name_map);
    return r;
}


// Whether a class belongs to a pawn the client drives as a character at all. This is the guard the
// MOB_ asset rule needs: MOB_ is Aion's "mobile placeable" family, not a hostiles-only one - a pet
// (BP_Pet_C + MOB_DranaVar_03) and an invisible marker (BPCh_NpcBase_C + MOB_Invisible) both carry
// it - so an asset from that family can only mean "monster" on a class that is a creature.
inline bool IsCreatureClass(const std::string& cls) {
    static const char* kCreature[] = {"BPCh_",        "NpcBase",  "AionNpc", "AionSimpleCharacter",
                                      "AionEffectCharacter", "AionPet", "PetBase", "Character",
                                      "Pawn"};
    for (const char* p : kCreature)
        if (HasI(cls, p)) return true;
    return false;
}

// The container family: chests, lootboxes, bags and the rest. Matched against the class AND the
// mesh, because the class is shared with everything else the client places (BP_EnvObj_Common_C) and
// for most of these the mesh name is the only key - this build's class list has no Chest class at
// all. Deliberately no bare "Box" or "Cube": the reward cubes are their own category and are drawn
// on purpose. The client's own act containers are excluded - they are mechanism actors whose class
// name would otherwise match "Container", and they carry no loot.
inline bool IsContainer(const std::string& cls, const std::string& model) {
    static const char* kContainers[] = {
        "Chest",     "Treasure", "RewardBox", "LootBox",  "Lockbox", "Strongbox",
        "Bag",       "Pouch",    "Satchel",   "Backpack", "Sack",    "Crate",
        "Barrel",    "Container",
    };
    if (HasI(cls, "ActContainer")) return false;
    for (const char* p : kContainers)
        if (HasI(cls, p) || HasI(model, p)) return true;
    return false;
}

// Classify an actor from its class, object name and (optionally) its mesh/model
// asset name. Order matters: the first matching rule wins, so specific classes are
// tested before generic ones.
inline EntityType ClassifyEntity(const std::string& cls, const std::string& name,
                                 const ClassRules* rules = nullptr,
                                 const std::string& model = {}) {
    if (rules) {
        if (InList(rules->ignore, cls)) return EntityType::Noise;
        if (InList(rules->gather, cls)) return EntityType::Gather;
        if (InList(rules->cube, cls)) return EntityType::Cube;
        if (InList(rules->interact, cls)) return EntityType::Interact;
        if (InList(rules->item, cls)) return EntityType::Item;
        if (InList(rules->player, cls)) return EntityType::Player;
        if (InList(rules->npc, cls)) return EntityType::Npc;
        if (InList(rules->pet, cls)) return EntityType::Pet;
        if (InList(rules->monster, cls)) return EntityType::Monster;
    }
    // --- engine / world scaffolding: never useful on an ESP -------------------
    static const char* kNoise[] = {
        "Volume", "NavPath", "NavMesh", "RecastNav", "Spline", "Audio", "Sound",
        "Nameplate", "Controller", "WorldSettings", "GameMode", "GameSession",
        "GameState", "GameInstance", "PlayerStart", "Landscape", "Fog", "Sky",
        "PostProcess", "Decal", "Light", "Foliage", "Grid", "Manager", "System",
        "Widget", "HUD", "Trigger", "Environment", "Weather", "Occlusion",
        "WorldDataLayers", "Brush", "PathPoint", "ActPlay", "Footstep", "BGM",
        "Flux", "Mask", "Domain", "Painter", "Ocean", "Wave", "Build", "Interior",
        "PlayerCamera", "Projectile", "Replay", "NavData", "Replicated", "CameraActor",
        // Seen live and always useless: path splines, bare engine actors.
        "WindPath", "Road", "Rope", "Cloth", "Destructible",
        // Engine-placed decoration. The world is mostly these (hundreds of stone floors,
        // rocks and HLOD proxies), so they must not be kept and read every frame now that
        // streaming levels are enumerated too.
        "StaticMeshActor", "SkeletalMeshActor", "HLOD", "InstancedFoliageActor",
    };
    for (const char* pat : kNoise)
        if (HasI(cls, pat)) return EntityType::Noise;
    // A plain "Actor"/"Object" has no useful identity at all.
    if (cls == "Actor" || cls == "Object" || cls == "DefaultPawn") return EntityType::Noise;

    // A container is a category, not a dropped family: whether it has been opened is something the
    // client states (AionEnvObjAnimInstance::MainState), so the ones still holding loot are drawn.
    // The spent check in EntityCache::Update drops the rest. Before the character rules, so no
    // pattern below can claim a chest.
    if (IsContainer(cls, model)) return EntityType::Chest;

    // --- characters -----------------------------------------------------------
    // NOTE: "PcBase" is deliberately NOT matched as a substring - it also appears
    // inside "NpcBase", which previously made every NPC show up as a player.
    static const char* kPlayer[] = {"BPCh_PcBase", "AionPlayer", "PlayerCharacter"};
    for (const char* p : kPlayer)
        if (HasI(cls, p)) return EntityType::Player;

    static const char* kPet[] = {"BP_Pet", "PetBase", "AionPet", "Summoned"};
    for (const char* p : kPet)
        if (HasI(cls, p)) return EntityType::Pet;

    // The invisible-placeable marker. The actor has no mesh at all, so it can never be the creature
    // it was spawned for: the client uses it for a mechanism whose visuals come from another actor -
    // the abyss portal, trap and laser emitters, skill shards. The MOB_ rule below called all 62 of
    // them in one zone monsters, which is exactly the screen full of boxes they put under the
    // Monsters toggle. A Noise actor is still readable in the esp_show_all diagnostic.
    if (HasI(model, "MOB_Invisible")) return EntityType::Noise;

    // Aion spawns creatures from one generic character class ("AionSimpleCharacter",
    // "NpcBase"), so the class cannot separate a monster from an NPC - and those generic
    // patterns are matched just below, which made every MOB_* actor an NPC. The asset
    // name is the reliable signal ("MOB_WindEle_01", "Mob_OdSpider_02_V02"), so it is
    // tested before the NPC class patterns. Case insensitive: one pattern covers
    // MOB_/Mob_/mob_. It only counts on a creature class: the same asset family carries props
    // (a portal or a shard is a BP_EnvObj_Common_C with a MOB_* mesh), and those must fall through
    // to the portal/gather/interact rules, which read the mesh that identifies them.
    if (IsCreatureClass(cls) && (HasI(name, "mob_") || HasI(model, "mob_") || HasI(cls, "mob_")))
        return EntityType::Monster;

    static const char* kNpc[] = {"BPCh_NpcBase", "NpcBase", "AionNpc", "AionSimpleCharacter",
                                 "AionEffectCharacter"};
    for (const char* p : kNpc)
        if (HasI(cls, p)) return EntityType::Npc;

    static const char* kMonster[] = {"Monster", "Mob", "Boss", "Enemy"};
    for (const char* p : kMonster)
        if (HasI(cls, p)) return EntityType::Monster;

    // Portals and gates. The mesh is what identifies them: "Portal_Space_01" is the
    // "Lost Ruin Entrance", carried by the shared BP_EnvObj_Common_C class.
    static const char* kPortal[] = {"Portal_Space", "Portal", "Gate_"};
    for (const char* p : kPortal)
        if (HasI(cls, p) || HasI(name, p) || HasI(model, p)) return EntityType::Portal;

    // Reward/hidden cubes. Two other rules would otherwise claim them: the Gather "Cube"
    // pattern below (any mesh with Cube in the name) and the Interact pattern after it (the
    // client's own interaction classes). They are containers you open, so they get their own
    // category and colour. Not "MonsterCube" - the Monster class pattern runs earlier and
    // would win. Extend with list_cube= in aion.ini, which is matched against the class name.
    static const char* kCube[] = {"HiddenCube", "Hidden_Cube", "EnergyCube", "OdEnergyCube"};
    for (const char* p : kCube)
        if (HasI(cls, p) || HasI(name, p) || HasI(model, p)) return EntityType::Cube;

    static const char* kGather[] = {"Gather", "Herb", "Ore", "Cube", "AbsorbObj",
                                    "Resource", "Mineral", "Plant", "Gatherable", "Mushroom",
                                    "Flower", "Fiber",
                                    // "Cotton_Lv1" is a fibre node, reported live as GATHER and
                                    // missing from the overlay. A bare material word, like the
                                    // Ore/Plant/Herb entries above, so Cotton_Lv2 and numbered
                                    // variants are covered too.
                                    "Cotton",
                                    // The "Od*" family are the small resource nodes; a live
                                    // BP_EnvObj_Common_C carrying the mesh OdSmall was the
                                    // unidentified one (8 instances in the class dump).
                                    "OdSmall",
                                    // Food_* nodes are gatherables too ("Food_Lv1_Vegetable_A"
                                    // is the "Morfa" vegetable node). Kept to the Food_Lv1_
                                    // family so a dropped food item's mesh cannot match.
                                    "Food_Lv1",
                                    // "VegetableMela" is the vegetable node, carried by the same
                                    // shared BP_EnvObj_Common_C class as Morfa/OdSmall/Protect_01,
                                    // so the mesh is again the only key. Pinned to the asset family
                                    // rather than the bare "Vegetable", so a food item's mesh cannot
                                    // match and land in Gather.
                                    "VegetableMela",
                                    // "TreeBetua" is a tree gatherable on the same shared
                                    // BP_EnvObj_Common_C class. Pinned to the asset family
                                    // rather than a bare "Tree", so an unrelated tree asset
                                    // cannot match and land in Gather.
                                    "TreeBetua",
                                    // "ShellSherry" is a shell gatherable, reported live as a
                                    // gather node whose class name is the only stable key: one
                                    // placement is displayed as "Conide", and the display name
                                    // varies per placement. Without this the pair matched no rule
                                    // and fell through to Object, which is off by default - so
                                    // the node was not drawn at all.
                                    "ShellSherry"};
    for (const char* p : kGather)
        if (HasI(cls, p) || HasI(name, p) || HasI(model, p)) return EntityType::Gather;

    // Quest/story interaction objects. "Protect_01" is the "Empryan Trace: Altgard"
    // style object - interacting with it is not collecting a resource, so it must not
    // share the Gather category. Its display name varies per placement, so only the mesh
    // is pinned here; extend via list_interact for more.
    static const char* kInteract[] = {"Protect_", "Interact"};
    for (const char* p : kInteract)
        if (HasI(cls, p) || HasI(name, p) || HasI(model, p)) return EntityType::Interact;

    static const char* kItem[] = {"DroppedObj", "HomingDrop", "DropObj", "Item", "Pickup",
                                  "Loot"};
    for (const char* p : kItem)
        if (HasI(cls, p) || HasI(name, p) || HasI(model, p)) return EntityType::Item;

    // Note: the MOB_* asset check that identifies models like "MOB_WindEle_01" runs
    // earlier, before the generic NPC class patterns could claim them.

    // Anything else that is still a real world actor.
    return EntityType::Object;
}

// Actors that must never be drawn, in any mode - not even in the esp_show_all diagnostic.
// Three kinds qualify: sub-objects of another actor (a controller sits on the pawn it
// drives, projectiles and footstep actors are spawned by one), scenery that carries no
// gameplay meaning ("Honeycomb"), and containers - which are claimable exactly once and are
// pure clutter on an ESP afterwards. never_show= in aion.ini extends the list; it is matched
// against the class name and, when it has been resolved, the mesh name too - most of these
// objects share one class, so the mesh is often the only thing that identifies them.
// The bare engine classes are exact matches: "Actor" must not match every "...Actor".
inline bool AlwaysHidden(const std::string& cls, const std::string& model,
                         const ClassRules* rules = nullptr) {
    if (cls == "Actor" || cls == "Object" || cls == "DefaultPawn") return true;
    static const char* kAlwaysHidden[] = {"Controller", "Projectile", "Footstep"};
    for (const char* p : kAlwaysHidden)
        if (HasI(cls, p)) return true;
    // Containers are deliberately NOT in this list: they are a category of their own
    // (EntityType::Chest), and since the client states whether one has been opened, the ones still
    // holding loot are drawn rather than dropped with the scenery. See IsContainer.
    // Built-in mesh patterns, matched against the resolved mesh name because these objects
    // share one blueprint class: scenery with no gameplay meaning. The MOB_*/Mob_* family is
    // NOT listed here - the spiders in it are monsters and are classified as such by the
    // asset check above.
    struct NeverMesh {
        const char* pattern;
        const char* why;
    };
    static const NeverMesh kNeverMeshes[] = {
        {"Honeycomb", "environment"},
        {"DeadVillager", "environment"},
        // This exact variant is environment scenery, while the rest of the family (and
        // Mob_OdSpider_02 without the version suffix) are real monster spiders and must
        // keep showing. The pattern is the full mesh name so no sibling is caught.
        {"Mob_OdSpider_02_V02", "environment sprite"},
        // The training dummy at the starting camp. In the class list too, since only one of
        // the two is known to carry it.
        {"TrainingScarecrow", "training dummy"},
        // The elemental spirits (MOB_FireEle/MOB_EarthEle/MOB_WaterEle/MOB_WindEle - the client's
        // "Whirlwind Spirit", "Soft Breeze Spirit") used to be listed here as ambient, on the
        // theory that a MOB_ asset on the generic AionSimpleCharacter class could not be told from
        // scenery. They are monsters: they are named by the client's own nameplates, they are
        // visible and attackable. Listing them here dropped exactly what the ESP exists for, so the
        // four entries were removed 2026-10-11 - they now classify as Monster through the MOB_
        // asset rule like every other mob.
        // "MOB_TraDummy_01" is the training dummy - same problem, a MOB_ asset on the generic
        // BPCh_NpcBase_C class, so the classifier calls it Monster.
        {"MOB_TraDummy", "training dummy, not a monster"},
        // Shugo body variants are scenery wearing a name that reads like a gatherable, and
        // they were landing in Gather. Substring, so every ShugoBodyDiscolored_* is covered.
        {"ShugoBodyDiscolored", "Shugo body variant, not a gatherable"},
    };
    // Built-in class patterns, for objects whose class is enough to identify them.
    static const NeverMesh kNeverClasses[] = {
        {"AbsorbObj_C", "gathering node"},  // every resource node in the world
        {"AionWorldActPlayActor", "world act-play prop"},
        {"AionEffectCharacter", "effect stand-in actor"},
        {"AionTargetDecalActor", "target decal"},
        // Classified as Noise already (the kNoise patterns above cover the Light/Volume/Sound
        // families), which hides them normally - but the esp_show_all diagnostic draws Noise
        // too, so they kept turning up in the world inventory. These are the ones that carry no
        // identity whatsoever, so they are hidden in every mode.
        {"StaticMeshActor", "engine static mesh"},
        {"InstancedFoliageActor", "engine foliage"},
        {"PointLight", "light"},
        {"AionBGMVolume", "BGM volume"},
        {"AkAmbientSound", "ambient sound"},
        {"TrainingScarecrow", "training dummy"},
    };
    for (const NeverMesh& m : kNeverClasses)
        if (HasI(cls, m.pattern)) return true;
    for (const NeverMesh& m : kNeverMeshes)
        if (!model.empty() && HasI(model, m.pattern)) return true;
    if (rules) {
        if (InList(rules->never_show, cls)) return true;
        if (!model.empty() && InList(rules->never_show, model)) return true;
    }
    return false;
}

struct Entity {
    addr_t Addr = 0;
    std::string Name;        // display name (name_map > model > object name)
    std::string Model;       // mesh/visual asset name: the real identity
    std::string ClassName;
    FVector Location;
    FRotator Rotation;
    double Distance = 0.0;
    FVector2D Screen;          // screen position of origin
    FVector2D BoundsMin, BoundsMax;
    bool OnScreen = false;
    addr_t RootComponent = 0;
    bool LocalPlayer = false;
    bool Dead = false;         // AionCharAnimInstance::bDead == 1, from a successful read only
    // AionCharAnimInstance::bHpDangerAlert == 1: the client is flagging this actor as low on health.
    // Read for the local player while the self panel is on, so the steady state pays no read for it.
    // Only ever true from a successful read.
    bool LowHealth = false;
    // Whether LowHealth was actually READ this frame. A bit that reads false and a bit that could not
    // be read are different answers, and the self panel must not present the second as "ok".
    bool LowHealthKnown = false;
    // AActor::bHidden == 1: the client is not rendering this actor, so it is not on screen even
    // though it exists at a real position. Only ever true from a successful read.
    bool ClientHidden = false;
    bool Boss = false;         // in the user's boss list: painted as a boss, not a trash mob
    bool Target = false;       // in the user's target list: painted in the drop colour
    EntityType Type = EntityType::Other;
};

// A user list (bosses, targets) against an actor. Matched in the same order as the name map: the
// mesh/model first (the actor's real identity - "MOB_DranaVar_03"), then the object name, then the
// name the user sees on screen (the learned localized name - "Drana Predator", see
// aion_names.txt), then the class name. Without the display name the user would have to know the
// model: the only name in front of them is the one the ESP already paints. Empty list = nothing
// matches, which is the only honest default: the client is never told a monster's rank, only that
// it has aggroed once it already has.
inline bool MatchesList(const std::vector<std::string>& list, const std::string& cls,
                        const std::string& name, const std::string& model,
                        const std::string& display = {}, bool exact = false) {
    if (list.empty()) return false;
    const auto Hit = [&](const std::string& s) {
        return exact ? InListExact(list, s) : InList(list, s);
    };
    if (!model.empty() && Hit(model)) return true;
    if (!name.empty() && Hit(name)) return true;
    if (!display.empty() && display != model && Hit(display)) return true;
    return !cls.empty() && Hit(cls);
}

// Bosses and mini-bosses: painted solid red, the colour every monster used to get. Matched EXACTLY
// (case-insensitive whole string): a substring rule made "Contaminated Swarm" also claim
// "Contaminated Swarm Egg" - a different actor, with a different name.
inline bool IsBossActor(const ClassRules& r, const std::string& cls, const std::string& name,
                        const std::string& model, const std::string& display = {}) {
    return MatchesList(r.boss, cls, name, model, display, /*exact=*/true);
}

// The user's own hunt list: painted in the drop colour (see EspRenderer::EntityColor). Exact for the
// same reason as the boss list.
inline bool IsTargetActor(const ClassRules& r, const std::string& cls, const std::string& name,
                          const std::string& model, const std::string& display = {}) {
    return MatchesList(r.target, cls, name, model, display, /*exact=*/true);
}

// Whether the ESP is drawing this category at all. Used both when drawing and when
// collecting: a category that is off costs nothing, so its transform is never read.
inline bool CategoryVisible(const Config& c, EntityType t) {
    // Diagnostic mode shows everything, including Noise (scenery, volumes, splines).
    if (c.esp_show_all) return true;
    switch (t) {
        case EntityType::Player:  return c.cat_player;
        case EntityType::Npc:     return c.cat_npc;
        case EntityType::Pet:     return c.cat_pet;
        case EntityType::Monster: return c.cat_monster;
        case EntityType::Item:    return c.cat_item;
        case EntityType::Gather:  return c.cat_gather;
        case EntityType::Interact: return c.cat_interact;
        case EntityType::Portal:  return c.cat_portal;
        case EntityType::Cube:    return c.cat_cube;
        case EntityType::Chest:   return c.cat_chest;
        case EntityType::Object:  return c.cat_object;
        case EntityType::Noise:   return false;
        default:                  return c.cat_other;
    }
}

// Origin every ESP range is measured from. The third-person camera sits behind and above
// the pawn and can be zoomed out tens of metres, so a camera-based range overstates every
// distance by the camera offset and disagrees with the range the client itself shows. The
// local pawn's own entity is in the list and carries the pawn's root origin, so that is the
// origin; the camera is the fallback for the frames before the pawn resolves, and for the
// census tools that never learn the pawn address.
inline FVector DistanceOrigin(const std::vector<Entity>& es, const FVector& cam_loc) {
    for (const auto& e : es)
        if (e.LocalPlayer) return e.Location;
    return cam_loc;
}

inline void ComputeDistances(std::vector<Entity>& es, const FVector& origin) {
    for (auto& e : es) e.Distance = (e.Location - origin).Length();
}

// Categories that are ACharacter-derived and therefore have a movement component.
inline bool IsCharacterType(EntityType t) {
    return t == EntityType::Player || t == EntityType::Npc || t == EntityType::Pet ||
           t == EntityType::Monster;
}

// World objects the client builds on one shared class (BP_EnvObj_Common_C) - gathering nodes,
// interactables, and the containers that are still unopened. Their per-instance state (spent or not)
// hangs off the mesh component's anim instance, not off the actor, so they need their own path to it.
inline bool IsNodeType(EntityType t) {
    return t == EntityType::Gather || t == EntityType::Interact || t == EntityType::Chest;
}

// Types whose name a nameplate can carry: characters always, and the world props the client
// gives a nameplate to (gathering nodes, dropped items, interactables - each gated by
// its own option, see AionWidget_Setting_GatherNamePlate).
inline bool NameplateNamed(EntityType t) {
    return IsCharacterType(t) || t == EntityType::Gather ||
           t == EntityType::Item || t == EntityType::Interact || t == EntityType::Portal ||
           t == EntityType::Cube;
}

// A model may key the name table when it identifies the entity. Players must not - every
// player of a gender renders one mesh (GF_RootMesh), which is how "Alexa" once became the
// name of every female player - and neither must pets, whose name is the owner's choice on a
// shared monster mesh. Their names come from PlayerState and the nameplate. A collision is
// caught anyway: a model seen with two different names is dropped and marked ambiguous.
inline bool ModelNamesEntity(EntityType t) {
    return t != EntityType::Player && t != EntityType::Pet && t != EntityType::Noise;
}

// A player's name is on its PlayerState (NPCs have none, which is why they need the widget
// path at all). Readable at any distance, and it is the character name rather than whatever
// the nameplate is showing.
inline std::string PlayerNameOf(Sdk* sdk, addr_t actor) {
    const addr_t ps = sdk->mem->ptr_at(actor + off::apawn::PlayerState);
    if (!sdk->mem->valid_ptr(ps)) return {};
    const std::string n =
        sdk->mem->read_fstring(ps + off::aplayer_state::PlayerNamePrivate, 128, true);
    return PlausibleName(n) ? n : std::string{};
}

class EntityCache {
public:
    // Extent used to synthesise a bounding box when no mesh bounds are available. The box
    // width is derived from the height (half of it), so there is only one number to tune.
    double default_half_height = 90.0;

    // Address of the local pawn; matching entities are flagged LocalPlayer.
    void set_local_pawn(addr_t p) { local_pawn_ = p; }
    // Drop every cached identity, so the next frames re-read it: a changed rule (a new boss name, a
    // new never_show entry) then applies as if the overlay had been restarted, instead of waiting
    // for each actor to come up for revalidation on its own schedule.
    void invalidate_metadata() { meta_.clear(); }

    // Optional user class lists (aion.ini) consulted before built-in patterns.
    void set_rules(const ClassRules& r) { rules_ = r; }
    // The rules the cache is classifying with. The census reads them from here rather than keeping
    // a second copy, so a reload can never leave the two out of step.
    const ClassRules& rules() const { return rules_; }

    // Learned model -> name table (RealNames.hpp). When set, it is consulted before the
    // per-actor nameplate walk and every nameplate hit teaches it a new entry.
    void set_name_table(NameTable* t) { names_ = t; }

    // Actors of the persistent level plus every streaming level. Aion loads the world in
    // sublevels, and gathering nodes and quest objects are placed in them - reading
    // only the persistent level omitted them entirely, which is why an early node census kept
    // coming out as zero. Levels() already contains the persistent level, so it is skipped
    // here to avoid listing everything twice.
    //
    // Reading every level's actor array costs ~8 ms, so it cannot happen in one frame:
    // the persistent level (which holds the characters, 1-2 syscalls) is re-read every
    // frame, while the streamed levels are swept a few per frame and only published when
    // the whole sweep is complete, so entity lists never flicker while it is in progress.
    const std::vector<addr_t>& CollectActors(World& world, Level& level) {
        frame_actors_.clear();
        const addr_t world_addr = world.Get().Addr();
        const addr_t persistent = world.PersistentLevel().Addr();
        // A new world (zone/map change), or a teardown (the world pointer goes null
        // mid-transition), invalidates every address cached from the old one: the level list, the
        // sweep in progress, the published streamed set and the per-actor metadata. Without this
        // reset the previous zone's freed actors were appended to the new level's list and drawn
        // until the next full sweep completed (up to levels/8 frames), and an address the new
        // world reused inherited the old actor's identity straight out of meta_.
        if (world_addr != world_ || persistent != persistent_level_) {
            world_ = world_addr;
            persistent_level_ = persistent;
            levels_.clear();
            pending_.clear();
            streamed_.clear();
            cursor_ = 0;
            meta_.clear();
        }
        if (!persistent) return frame_actors_;
        // One actor, one entity. The persistent level and the streaming levels are separate actor
        // arrays and nothing guarantees they are disjoint: a level can appear twice in
        // UWorld::Levels, and an actor can be named by both a level and the sweep that is in flight
        // when it moves. The list is drawn as-is, so a second entry is a second box for one actor -
        // the "one entity, two boxes" a report of doubled lines looks like. Keyed by address: the
        // same address cannot be two actors inside one frame, and a *recycled* address is already
        // told apart from the old actor by the FUObjectItem serial in StaleIdentity.
        seen_.clear();
        const std::vector<addr_t>& named = level.Actors(persistent);
        frame_actors_.reserve(named.size() + streamed_.size());
        for (addr_t a : named)
            if (seen_.insert(a).second) frame_actors_.push_back(a);
        if (cursor_ == 0) {
            levels_ = world.Levels();
            pending_.clear();
        }
        constexpr u32 kLevelsPerFrame = 8;
        for (u32 i = 0; i < kLevelsPerFrame && cursor_ < levels_.size(); ++i, ++cursor_) {
            const addr_t l = levels_[cursor_];
            if (!l || l == persistent) continue;
            for (addr_t a : level.Actors(l)) pending_.push_back(a);
        }
        if (cursor_ >= levels_.size()) {
            cursor_ = 0;
            streamed_.swap(pending_);
        }
        for (addr_t a : streamed_)
            if (seen_.insert(a).second) frame_actors_.push_back(a);
        return frame_actors_;
    }

    void Update(Sdk* sdk, const Camera& cam, Projection& proj,
                const std::string& local_pawn_addr_filter = {}) {
        entities_.clear();
        World world(sdk);
        Level level(sdk);
        if (!world.PersistentLevel().Addr()) return;
        const auto& actors = CollectActors(world, level);
        entities_.reserve(actors.size());
        for (addr_t a : actors) {
            if (!sdk->mem->valid_ptr(a)) continue;
            Actor actor(sdk, a);
            Entity e;
            e.Addr = a;
            e.LocalPlayer = (a == local_pawn_);
            e.Name = actor.Name();
            e.ClassName = actor.ClassName();
            if (e.Name.empty()) continue;
            e.Type = ClassifyEntity(e.ClassName, e.Name, &rules_);
            // The census path has no cached model yet, so the class and object name carry the
            // match here; the per-actor cache uses the model first.
            e.Boss = IsBossActor(rules_, e.ClassName, e.Name, {});
            e.Target = IsTargetActor(rules_, e.ClassName, e.Name, {});
            // A controller, projectile or footstep actor belongs to another actor and is
            // never an entity of its own.
            if (AlwaysHidden(e.ClassName, {}, &rules_)) continue;
            // esp_show_all keeps scenery so every actor can be drawn and identified.
            if (e.Type == EntityType::Noise && !Config::Instance().esp_show_all) continue;
            // The client's own flags, recorded for callers that want them. This path is the census
            // one (recon), whose lists stay complete by design, so nothing is culled here.
            e.ClientHidden = actor.ClientHidden();
            e.Location = actor.Location();
            e.Rotation = actor.Rotation();
            e.RootComponent = actor.RootComponent();
            ProjectEntity(e, cam, proj);
            entities_.push_back(std::move(e));
        }
        ComputeDistances(entities_, DistanceOrigin(entities_, cam.Location));
        std::sort(entities_.begin(), entities_.end(),
                  [](const Entity& a, const Entity& b) { return a.Distance < b.Distance; });
    }

    const std::vector<Entity>& Entities() const { return entities_; }

    // World-to-screen for one entity. Split out of the update loops so a frame can be
    // projected twice: once as the entities are collected, and again after the transform
    // pass onto a camera read at the end of it. The camera read at the top of the frame is
    // one syscall per actor older than the positions, and that skew is what shows up as the
    // boxes trailing the world when the camera turns.
    void ProjectEntity(Entity& e, const Camera& cam, Projection& proj) const {
        e.OnScreen = proj.WorldToScreen(e.Location, cam.Rotation, cam.Location, cam.FOV, e.Screen);
        if (!e.OnScreen) {
            // Clear the box: a stale one would still be drawn if this entity came back
            // off-screen after having been projected on-screen.
            e.BoundsMin = {0.f, 0.f};
            e.BoundsMax = {0.f, 0.f};
            return;
        }
        FVector up = e.Location, dn = e.Location;
        up.Z += default_half_height;
        dn.Z -= default_half_height;
        FVector2D t, b;
        if (proj.WorldToScreen(up, cam.Rotation, cam.Location, cam.FOV, t) &&
            proj.WorldToScreen(dn, cam.Rotation, cam.Location, cam.FOV, b)) {
            const float h = std::abs(b.Y - t.Y);
            const float w = h * 0.5f;
            e.BoundsMin = {e.Screen.X - w * 0.5f, t.Y};
            e.BoundsMax = {e.Screen.X + w * 0.5f, b.Y};
        } else {
            // One extent failed to project (a corner behind the camera plane). This runs a second
            // time per frame from Reproject(), on an entity that already carries the previous
            // camera's box, so without clearing it a STALE box would be drawn instead of the
            // degenerate-bounds marker the renderer falls back to.
            e.BoundsMin = {0.f, 0.f};
            e.BoundsMax = {0.f, 0.f};
        }
    }

    // Re-project the entities already collected onto a newer camera. No memory reads: the
    // positions are untouched, so this only redoes the arithmetic and costs microseconds.
    void Reproject(Projection& proj, const Camera& cam) {
        if (!cam.valid) return;
        for (auto& e : entities_) ProjectEntity(e, cam, proj);
    }

    // Mesh/visual asset name of one actor - the identity used by the class dump so
    // users can build a name_map. Safe to call from any thread that owns the Sdk.
    std::string ModelOf(Sdk* sdk, addr_t actor, const std::string& cls) {
        return MeshAssetFor(sdk, actor, IsCharacterType(ClassifyEntity(cls, {}, &rules_)));
    }

    // Category of an actor under the same rules the ESP uses, so a tool that writes to the
    // name table cannot mistake a player's shared mesh for a model that names entities.
    EntityType ClassifyWithRules(const std::string& cls, const std::string& name,
                                 const std::string& model) const {
        return ClassifyEntity(cls, name, &rules_, model);
    }

    // Counters for the last UpdateBulk call (why actors were dropped).
    struct Diag {
        std::size_t listed = 0, validated = 0, read_fail = 0, name_fail = 0, root_fail = 0;
        std::size_t noise = 0, kept = 0, cached = 0, culled = 0, garbage = 0;
    };
    const Diag& diag() const { return diag_; }

    // Whether the renderer would mark this entity while the camera is not showing it. Measured
    // from the camera rather than from the pawn, because the entity's pawn-relative Distance is
    // computed after this pass; the slack covers the camera's own offset behind the pawn, and the
    // marker's own gate is the renderer's - this only decides whether the dead/alive read is worth
    // a syscall.
    bool NearEnoughToMark(const Entity& e, const Camera& cam) const {
        const Config& c = Config::Instance();
        if (!c.esp_offscreen) return false;
        const double r = c.offscreen_range_m * 100.0 + 2000.0;
        const double dx = e.Location.X - cam.Location.X;
        const double dy = e.Location.Y - cam.Location.Y;
        const double dz = e.Location.Z - cam.Location.Z;
        return dx * dx + dy * dy + dz * dz <= r * r;
    }

    // Fast out-of-process path with a persistent per-actor metadata cache. Names,
    // classes, categories and root components barely change, so they are read once
    // and only re-validated every kValidateFrames; the entry is keyed by the actor's
    // FUObjectItem serial, so a recycled address is re-read rather than inherited.
    // Per frame the cost falls to one transform read per actor - plus one bDead byte
    // for visible characters - instead of ~7 RPM calls per actor.
    // cull_hidden skips even that transform read for categories the ESP is not drawing.
    // The census paths (capture mode, recon) pass false so their lists stay complete.
    void UpdateBulk(Sdk* sdk, const Camera& cam, Projection& proj, bool need_dead = true,
                    bool cull_hidden = true) {
        entities_.clear();
        ++tick_;
        diag_ = Diag{};  // cleared first, so a world-transition frame reports zeros, not the last frame
        World world(sdk);
        Level level(sdk);
        if (!world.PersistentLevel().Addr()) return;
        const auto& actors = CollectActors(world, level);
        entities_.reserve(actors.size());
        diag_.listed = actors.size();

        constexpr std::size_t kBlock = 0x1D0;
        // Validation is budgeted so a full sweep never hitches a frame. ~1000 actors
        // revalidate every 64 frames, i.e. ~16/frame are needed; 48 leaves headroom
        // while capping the burst at ~4 ms.
        constexpr u32 kMaxValidatePerFrame = 48;
        // Extra per-frame allowance for actors that have no cache entry at all, so a spawn is not
        // queued behind the revalidation sweep (see the budget note in the loop).
        constexpr u32 kMaxNewPerFrame = 16;
        u32 validated = 0;
        std::vector<u8> block(kBlock);
        for (addr_t a : actors) {
            if (!sdk->mem->valid_ptr(a)) continue;

            Meta* m = nullptr;
            auto it = meta_.find(a);
            const bool have_entry = (it != meta_.end());
            // Budget priority. A never-validated actor (a fresh spawn, or a streaming level's actor
            // met for the first time) is invisible until the sweep's pointer happens to reach it -
            // and with 1419 levels named by this client that is up to levels/8 frames away, with the
            // revalidation sweep competing for the same budget. New entries therefore get their own
            // small allowance on top of the 48/frame revalidation budget.
            const bool new_entry = !have_entry;
            if (have_entry && (tick_ - it->second.tick) < kValidateFrames) {
                // The identity check (address vs FUObjectItem serial) deliberately does NOT run here.
                // It costs one read per entry, and this loop sees every slot the level names -
                // measured live, listed=8195 with kept=19 took update=8.9 ms, i.e. ~8000 reads a frame
                // to protect 19 actors. It runs further down, for the actors that actually reach the
                // list, which is where a recycled address could mislabel something the user sees.
                // The culls below read cached metadata either way, and that cache is on its own
                // 64-frame validation cadence, so nothing is made staler than it already was.
                m = &it->second;
            } else if (validated < kMaxValidatePerFrame + (new_entry ? kMaxNewPerFrame : 0u)) {
                ++validated;
                ++diag_.validated;
                // Failures are cached too: otherwise the same head-of-list actors
                // (managers/infos carry no RootComponent) would eat the budget every
                // single frame and the rest of the level would never be reached.
                Meta& slot = meta_[a];
                slot.tick = tick_;
                slot.usable = false;
                slot.type = EntityType::Noise;
                if (!sdk->mem->read(a, block.data(), kBlock)) {
                    ++diag_.read_fail;
                    continue;
                }
                // Stronger liveness than the range test at the loop head: the same test
                // UObject::IsValid makes - the vtable points inside the image and the
                // InternalIndex is within the object array - computed from the block already
                // fetched, so it costs no extra read. A freed-and-reused address fails it.
                const addr_t vt =
                    *reinterpret_cast<const addr_t*>(block.data() + off::uobject::VTable);
                const u32 idx =
                    *reinterpret_cast<const u32*>(block.data() + off::uobject::InternalIndex);
                if (!sdk->mem->in_module(vt) ||
                    idx >= (u32)(std::max)(sdk->objects.Num(), 1) * 4u) {
                    slot.usable = false;
                    continue;
                }
                // Identity of the object-array slot behind this actor, so a reused address can be
                // told apart (see StaleIdentity). FUObjectItem::SerialNumber @0x10; when the item
                // cannot be resolved the serial is left unknown and the address is the key.
                slot.item = sdk->objects.ItemAt((i32)idx);
                const bool prev_serial_ok = slot.serial_ok;
                const u32 prev_serial = slot.serial;
                slot.serial_ok = slot.item != 0;
                slot.serial =
                    slot.serial_ok
                        ? sdk->mem->u32_at(slot.item + off::uobject_item::SerialNumber)
                        : 0;
                // The corpse latch belongs to the object, not to the address: a recycled address
                // must not inherit "this was a corpse", or a fresh actor would come up hidden. The
                // same object keeps its serial across revalidations, so this clears the latch only
                // when the object behind the address really changed - and when the serial cannot be
                // resolved at all, which is the fail-open direction (a shown corpse, never a hidden
                // live actor).
                if (slot.dead_latch &&
                    !(prev_serial_ok && slot.serial_ok && prev_serial == slot.serial))
                    slot.dead_latch = false;
                // Read out of the block the identity pass already fetched, so the steady state pays
                // nothing for it. Refreshed on every validation frame, so a node the client consumed
                // is gone within kValidateFrames at the latest.
                slot.garbage =
                    (*reinterpret_cast<const u32*>(block.data() + off::uobject::Flags) &
                     off::uobject::RF_MirroredGarbage) != 0;
                const u32 name_idx = *reinterpret_cast<const u32*>(block.data() + off::uobject::Name);
                const addr_t cls = *reinterpret_cast<const addr_t*>(block.data() + off::uobject::Class);
                const addr_t root =
                    *reinterpret_cast<const addr_t*>(block.data() + off::aactor::RootComponent);
                std::string nm = sdk->Name(name_idx);
                if (nm.empty()) ++diag_.name_fail;
                if (!sdk->mem->valid_ptr(root)) ++diag_.root_fail;
                if (nm.empty() || !sdk->mem->valid_ptr(root)) continue;
                slot.name = std::move(nm);
                slot.cls = sdk->mem->valid_ptr(cls)
                               ? sdk->Name(sdk->mem->u32_at(cls + off::uobject::Name))
                               : std::string{};
                slot.model = MeshAssetFor(sdk, a, IsCharacterType(ClassifyEntity(
                                                    slot.cls, slot.name, &rules_)));
                // After the mesh lookup: the never-draw rules cover meshes as well
                // ("Honeycomb" is environment on the shared BP_EnvObj_Common_C class).
                if (AlwaysHidden(slot.cls, slot.model, &rules_)) {
                    ++diag_.noise;
                    continue;
                }
                slot.type = ClassifyEntity(slot.cls, slot.name, &rules_, slot.model);
                // The per-entity name, when it can be had without a nameplate: a player's
                // comes from its PlayerState, and an NPC's or monster's from the learned
                // per-actor field, if the client keeps one.
                if (slot.type == EntityType::Player) {
                    slot.real = PlayerNameOf(sdk, a);
                } else if (names_ && ModelNamesEntity(slot.type)) {
                    const std::string dn = names_->DirectName(sdk->mem, a, slot.model);
                    if (!dn.empty()) {
                        slot.real = dn;
                        names_->Add(slot.model, dn);
                    }
                }
                // A learned name is the client's own and belongs to the model, so it takes
                // precedence over the user's name_map.
                std::string disp = MapDisplayName(rules_.name_map, slot.model, slot.cls, slot.name);
                if (ModelNamesEntity(slot.type)) {
                    if (const std::string* learned = names_ ? names_->Find(slot.model) : nullptr)
                        disp = *learned;
                }
                slot.display = !disp.empty() ? disp
                              : !slot.model.empty() ? slot.model
                                                    : slot.name;
                // Resolved here, not per frame: it depends only on cached identity, and the
                // draw path must not pay a string scan for every actor every frame. After the
                // display name, so a list entry can name the actor the user sees on screen.
                slot.boss = IsBossActor(rules_, slot.cls, slot.name, slot.model, slot.display);
                slot.target =
                    IsTargetActor(rules_, slot.cls, slot.name, slot.model, slot.display);
                slot.root = root;
                // The client's own dead flag, reached by name:
                // Character.Mesh (0x3C0) -> SkeletalMeshComponent::AnimScriptInstance ->
                // AionCharAnimInstance::bDead. Both hops are reflection lookups, cached per class;
                // if either is absent the entity simply has no dead signal and is never treated
                // as dead.
                slot.anim = 0;
                slot.dead_off = -1;
                slot.danger_off = -1;
                slot.inactive_off = -1;
                slot.main_state_off = -1;
                slot.movement = 0;
                slot.move_off = -1;
                // dead_latch is deliberately NOT reset here: this block re-runs every 64 frames for
                // the same actor, and the body a latched corpse leaves is exactly what must survive
                // it. It is cleared where the actor's identity changes (see the serial check below).
                if (IsCharacterType(slot.type) || IsNodeType(slot.type)) {
                    // Both hops are reflection lookups cached per class, so no gameplay offset is
                    // hardcoded for either. The mesh slot differs with the base class:
                    // ACharacter::Mesh for characters, AAionEnvObj::SkeletalMeshComponent for the
                    // world objects, which are not characters at all.
                    const bool is_char = IsCharacterType(slot.type);
                    if (is_char) {
                        // The corpse latch's release signal (see Meta::dead_latch):
                        // ACharacter::CharacterMovement -> MovementMode, by property name on the
                        // component's own class. It is declared on UCharacterMovementComponent and
                        // the class here is the derived AionCharMovementComponent, so the lookup
                        // walks up the supers.
                        slot.movement = sdk->mem->ptr_at(a + off::acharacter::CharacterMovement);
                        if (sdk->mem->valid_ptr(slot.movement))
                            slot.move_off = MovementModeOffset(
                                sdk, sdk->mem->ptr_at(slot.movement + off::uobject::Class));
                    }
                    const addr_t comp = sdk->mem->ptr_at(
                        a + (is_char ? off::acharacter::Mesh
                                     : off::aionenvobj::SkeletalMeshComponent));
                    if (sdk->mem->valid_ptr(comp)) {
                        slot.mesh = comp;
                        const addr_t comp_class = sdk->mem->ptr_at(comp + off::uobject::Class);
                        const BoolGate drawn_gate = RecentlyRenderedGate(sdk, comp_class);
                        // Only a gate with a real mask counts: a property whose mask could not be
                        // read must not turn into "the renderer did not draw it".
                        slot.drawn_off = drawn_gate.valid() ? drawn_gate.off : -1;
                        slot.drawn_byte = drawn_gate.byte;
                        slot.drawn_mask = drawn_gate.mask;
                        const i32 ao = AnimInstanceOffset(sdk, comp_class);
                        if (ao >= 0) {
                            const addr_t anim = sdk->mem->ptr_at(comp + (addr_t)ao);
                            if (sdk->mem->valid_ptr(anim)) {
                                const addr_t anim_class =
                                    sdk->mem->ptr_at(anim + off::uobject::Class);
                                slot.anim = anim;
                                if (is_char) {
                                    slot.dead_off = DeadOffset(sdk, anim_class);
                                    slot.danger_off = DangerOffset(sdk, anim_class);
                                } else if (slot.type == EntityType::Chest)
                                    slot.main_state_off = MainStateOffset(sdk, anim_class);
                                else
                                    slot.inactive_off = InactiveOffset(sdk, anim_class);
                            }
                        }
                    }
                }
                slot.usable = true;
                m = &slot;
            } else if (it != meta_.end()) {
                m = &it->second;  // budget spent; identity is still valid, revalidate later
            } else {
                continue;         // brand new actor and no validation budget left
            }
            // A node the client has consumed is marked pending destruction but is still in the
            // level's actor array, so it would be drawn until the GC runs - which is why the ESP
            // kept showing nodes that could no longer be gathered. The census paths keep them
            // (their lists stay complete) and so does SHOW ALL, whose job is to show everything.
            // The record at actor+0x348 ("still yields something" byte, and the tier of the pickup
            // on offer at +0x349) was tried as a second signal and reverted: the tier rotates by
            // itself on untouched gathering nodes - measured, four nodes each lost a tier while
            // nothing was being harvested, and the first version of the check latched them as
            // harvested for good. A byte that changes on its own is not evidence of anything.
            if (cull_hidden && m->garbage && !Config::Instance().esp_show_all) {
                ++diag_.garbage;
                continue;
            }
            // Noise is dropped before any per-actor byte is read, so scenery costs nothing at all:
            // the diagnostic mode keeps it so every actor can be inspected.
            if (!m->usable ||
                (m->type == EntityType::Noise && !Config::Instance().esp_show_all)) {
                ++diag_.noise;
                continue;
            }
            // A category the ESP is not drawing needs no per-actor read either: skipping it here is
            // what keeps a town full of disabled players/NPCs/drops from costing a syscall each, and
            // from eating the draw limit that the wanted actors need. The local pawn is exempt: its
            // position is the origin every range is measured from (see DistanceOrigin).
            //
            // The hunting list is a category of its own: a listed actor is kept even when its own
            // category is off, or "Monsters off, Target on" would draw nothing at all. The flag is
            // resolved with the rest of the identity above, so the exemption costs no extra work -
            // and the draw path applies the same rule, or the actor would be collected and not
            // drawn.
            const bool listed = m->target && Config::Instance().esp_target;
            if (cull_hidden && !CategoryVisible(Config::Instance(), m->type) && !listed &&
                a != local_pawn_) {
                ++diag_.culled;
                continue;
            }
            // The client's own visibility byte, read HERE - after the two culls that cost nothing, so
            // an actor the user is not drawing at all is not even read - and not with the identity
            // block: this is relevancy, not identity. It changes as the player walks, so a copy cached
            // for the 64-frame validation cadence lagged ~1 s behind - the actor you just walked up to
            // stayed hidden, and the one you just walked away from kept its box. One byte, and a
            // failed read is UNKNOWN, which leaves the actor drawn.
            bool client_hidden = false;
            if (const auto hb = sdk->mem->try_read<u8>(a + off::aactor::HiddenFlags))
                client_hidden = (*hb & off::aactor::HiddenBit) != 0;
            // ... but bHidden does not mean "the client is not rendering this". Measured with
            // --dump-gates: the client sets it on actors it IS drawing (an NPC at 34 m read
            // bHidden=1 while SkinnedMeshComponent::bRecentlyRendered read 1), while characters past
            // 128 m read bHidden=0 and nothing was drawn past 45 m. So the byte only counts when the
            // renderer agrees it did not draw the actor - that keeps the on-screen actors the old
            // rule dropped. One byte, and only for actors whose bHidden is already set.
            // Only a *resolvable* gate may suppress. With no mesh component, or a class whose
            // bRecentlyRendered could not be reflected, the byte alone is not evidence that the
            // client is hiding this actor, so it stays drawn - the same fail-open rule the failed
            // read below follows. Without this, an unresolvable gate silently restored the old
            // bHidden-alone rule, which hides every actor past ~25 m: the "client is not rendering
            // this" band became "the ESP does not draw this".
            const bool drawn_gate_known = m->mesh && m->drawn_off >= 0;
            if (client_hidden && !drawn_gate_known) {
                client_hidden = false;
            } else if (client_hidden) {
                const auto rb = sdk->mem->try_read<u8>(m->mesh + (addr_t)m->drawn_off +
                                                       m->drawn_byte);
                // Drawn, or the drawn byte unreadable: neither is evidence that the client is hiding
                // this actor, and a failed read must never suppress a live entity - the same contract
                // bDead and the spent-node checks follow.
                if (!rb || ((*rb & m->drawn_mask) != 0))
                    client_hidden = false;
            }
            // The local pawn is exempt whatever the byte says: it is the origin every distance is
            // measured from, and losing it silently re-bases the whole frame on the camera.
            if (a == local_pawn_) client_hidden = false;
            // The client is not rendering this actor: it is outside its relevancy set, which is
            // where monsters that have not appeared yet live - a real actor with a real mesh at a
            // real position, and nothing on screen. Off by default, so those actors are simply not
            // drawn; with esp_hidden they are kept for the renderer, which draws them in
            // color_hidden out to hidden_range_m rather than pretending they are visible.
            if (cull_hidden && client_hidden && !Config::Instance().esp_hidden &&
                !Config::Instance().esp_show_all) {
                continue;
            }
            ++diag_.kept;

            // A world node the client reports as spent. The client leaves the actor in the
            // level's actor array (it hides it through its own animation state), so without this it
            // is drawn forever after being gathered. Read only for the node types, and after the
            // culls above so a node the user has switched off costs nothing - only a confirmed 1
            // suppresses, and a failed read leaves the node drawable.
            // A container: the client's own record of whether it has been opened. MainState is an
            // EAnimInteractionState - Available (0) until the object is used, Finish (1) after it -
            // so anything but Available is a spent container, which the ESP does not draw. A failed
            // read is not evidence of anything, so it leaves the container drawn.
            if (cull_hidden && m->anim && m->main_state_off >= 0) {
                const auto ms = sdk->mem->try_read<u8>(m->anim + (addr_t)m->main_state_off);
                m->collected = ms && (*ms != off::aionenvobj_anim::MainStateAvailable);
            }
            if (cull_hidden && m->anim && m->inactive_off >= 0) {
                const auto inactive = sdk->mem->try_read<u8>(m->anim + (addr_t)m->inactive_off);
                m->collected = inactive && (*inactive != 0);
            }
            if (cull_hidden && m->collected && !Config::Instance().esp_show_all) {
                continue;
            }

            // The entry's identity is (address, serial): if this address now backs a different
            // object, drop the entry so the new actor cannot inherit the old one's name, class or
            // type, and re-read it on a later frame. This is the only place it runs (see the note at
            // the cache lookup), and it covers everything that is published - nothing the user sees
            // gets a recycled actor's identity. Skipped on the frame the entry was validated: that
            // copy was written from a fresh read.
            if (m->tick != tick_ && StaleIdentity(sdk, *m, a)) {
                meta_.erase(a);
                continue;
            }

            Entity e;
            e.Addr = a;
            e.LocalPlayer = (a == local_pawn_);
            e.Name = m->display;
            e.Model = m->model;
            e.ClassName = m->cls;
            e.Type = m->type;
            e.Boss = m->boss;
            e.Target = m->target;
            e.ClientHidden = client_hidden;
            e.RootComponent = m->root;
            // A failed transform read is UNKNOWN, not the world origin: reading through a helper
            // that lets failure yield (0,0,0) put the actor at the map origin, where the range cull
            // then dropped it - a read failure suppressing a live entity. Keep the last position
            // that did read instead.
            FVector loc{};
            if (sdk->mem->read(m->root + off::uscene_component::RelativeLocation, &loc,
                               sizeof(loc))) {
                m->last_loc = loc;
                m->have_loc = true;
            } else if (!m->have_loc) {
                // Position never read and this frame's read failed: it is UNKNOWN. The zero default
                // would place the actor at the world origin, where the range cull then drops it - a
                // read failure suppressing a live entity - and for the local pawn it would re-base
                // every distance in the frame on the origin. Skip it until a read succeeds.
                ++diag_.root_fail;
                continue;
            }
            e.Location = m->last_loc;
            ProjectEntity(e, cam, proj);

            // Only characters the ESP will actually draw need the movement read (a separate
            // object, so it costs its own syscall): the ones on screen, plus - with off-screen
            // markers on - the ones near enough to be marked, so a corpse behind the camera is
            // not reported as a live enemy. Everything further off stays unread.
            if (need_dead && IsCharacterType(e.Type) &&
                (e.OnScreen || NearEnoughToMark(e, cam))) {
                // AionCharAnimInstance::bDead, read through the reflection-resolved instance and
                // offset. A failed read is UNKNOWN, not dead: try_read yields nullopt and Dead
                // keeps its false default, and hide_dead only hides on a CONFIRMED bDead == 1.
                if (m->anim && m->dead_off >= 0) {
                    if (const auto dead = sdk->mem->try_read<u8>(m->anim + (addr_t)m->dead_off)) {
                        if (*dead) {
                            // The client's own death state. Latched, because the client clears it
                            // again long before the body is gone (Meta::dead_latch has the
                            // measurement that this exists for).
                            e.Dead = true;
                            m->dead_latch = true;
                        } else if (m->dead_latch) {
                            // bDead is 0 but this character was confirmed dead: the death animation
                            // has ended and the body is still lying in the level. Only a movement
                            // mode that read successfully and is not MOVE_None puts it back on its
                            // feet, so a corpse keeps its latch - which is the point, since the
                            // renderer keeps drawing it (bRecentlyRendered stays 1 on a component
                            // the client stopped drawing).
                            bool up_again = true;  // no release signal: the latch is never held alone
                            if (m->movement && m->move_off >= 0) {
                                if (const auto mm =
                                        sdk->mem->try_read<u8>(m->movement + (addr_t)m->move_off))
                                    up_again = (*mm != 0);
                            }
                            if (up_again) m->dead_latch = false;
                            else e.Dead = true;
                        }
                    } else {
                        // UNKNOWN is not dead, and it does not stay latched either: the same contract
                        // as above, so a byte that stops reading can never hold an actor down.
                        m->dead_latch = false;
                    }
                }
            }
            // The local player's own low-health bit, for the self panel: the client's own warning
            // byte, read here for the one actor the user is. Only while that panel is on, so the
            // steady state pays nothing for it, and only a successful read marks it known - a bit
            // that reads false and a bit that could not be read are different answers.
            if (e.LocalPlayer && Config::Instance().self_vitals && m->anim && m->danger_off >= 0) {
                if (const auto d = sdk->mem->try_read<u8>(m->anim + (addr_t)m->danger_off)) {
                    e.LowHealth = (*d != 0);
                    e.LowHealthKnown = true;
                }
            }
            entities_.push_back(std::move(e));
        }
        ComputeDistances(entities_, DistanceOrigin(entities_, cam.Location));
        std::sort(entities_.begin(), entities_.end(),
                  [](const Entity& a, const Entity& b) { return a.Distance < b.Distance; });
        // Names read from a nameplate are applied after the list is built, so one that
        // arrives is visible on the frame it was read.
        ResolveRealNames(sdk, 1);
        for (auto& e : entities_) {
            // The client's own name for THIS actor wins where it exists. A name is resolved per
            // actor, but the learned table is keyed by model (mesh) - and two monsters can share
            // one mesh, which is how a "Baby" variant is built (the same mesh, scaled), so the
            // table cannot tell such a pair apart. The table stays the fallback for the actors whose
            // nameplate the client has not built, which is the reach it exists for.
            auto it = meta_.find(e.Addr);
            if (it != meta_.end() && !it->second.real.empty()) {
                e.Name = it->second.real;
            } else if (ModelNamesEntity(e.Type)) {
                if (const std::string* learned = names_ ? names_->Find(e.Model) : nullptr)
                    e.Name = *learned;
            }
            // A Hidden rule is the last word on that label - the entity draws, the name does not -
            // so it is applied after every source above (the display name, the learned table, the
            // nameplate, PlayerState) has had its say, and none of them can put a name back. Unlike
            // the learned table it is not limited to models that identify an entity: hiding a player
            // mesh or a pet mesh by pattern is a reasonable thing to ask for, and a rule that
            // silently does nothing is worse than one that hides too much.
            if (names_ && names_->IsNameHidden(e.Model)) e.Name.clear();
        }
        // Drop entries for actors that have been gone for a while (destroyed, or a
        // different streaming level). Only runs when the cache has clearly grown.
        if (meta_.size() > actors.size() * 2 + 64) {
            for (auto i2 = meta_.begin(); i2 != meta_.end();) {
                if (tick_ - i2->second.tick > 600) i2 = meta_.erase(i2);
                else ++i2;
            }
        }
    }

private:
    // Per-actor data that rarely changes. tick records the frame it was last
    // validated on, so a recycled actor address cannot keep a stale identity.
    struct Meta {
        std::string name, cls;
        std::string model;    // mesh/visual asset: the actor's real identity
        std::string display;  // what the ESP shows
        std::string real;     // name read from the game's nameplate widget, once resolved
        u32 name_tick = 0;    // frame the nameplate was last read (re-read periodically)
        EntityType type = EntityType::Other;
        bool boss = false;    // resolved once with the identity, like the category
        bool target = false;  // the user's hunt list; painted in the drop colour
        addr_t root = 0;
        bool usable = false;  // false: no valid name/root - cached so it is not re-read
        bool garbage = false; // pending destruction: the client has consumed this node
        // Last transform that READ successfully. A failed transform read is UNKNOWN, not the world
        // origin: letting it yield (0,0,0) put the actor at the map origin, where the range cull
        // then dropped it (a read failure suppressing a live entity), and for the local pawn it
        // re-based every distance in the frame on the map origin.
        FVector last_loc{};
        bool have_loc = false;
        // A node the client reports as spent. Only ever true from a successful read of
        // bInactiveCondition, so a failed read leaves the node drawable.
        bool collected = false;
        // Anim-instance class -> offset of its bInactiveCondition byte (world objects only).
        i32 inactive_off = -1;
        // The same, for the containers: offset of AionEnvObjAnimInstance::MainState, the byte that
        // says whether a chest has been opened yet.
        i32 main_state_off = -1;
        // The dead signal, resolved by reflection at validation: the anim instance and the
        // offset of its bDead byte. anim == 0 or dead_off < 0 means the client exposes no dead
        // signal here, and the entity is "not known dead" rather than dead.
        addr_t anim = 0;
        i32 dead_off = -1;
        // The self panel's low-health signal: offset of AionCharAnimInstance::bHpDangerAlert,
        // resolved by reflection alongside bDead. -1 means the client exposes no low-health flag here.
        i32 danger_off = -1;
        // The corpse latch's release signal, and why bDead alone cannot carry the state: measured
        // live (2026-10-12, MOB_Pino/OwlSoldier/Mag/SparkyOd), bDead reads 1 from the killing blow
        // until the death animation ends - about 10-20 s - and the client then clears it while the
        // body is still lying in the level. bDead 0 after that is NOT "alive": the corpse reads
        // AActor::bHidden 1, but the renderer's own bRecentlyRendered still reads 1 (a flag that
        // never clears for a component the client stopped drawing), so the "bHidden AND not drawn"
        // gate keeps the body and the box came back on a dead monster - which is the bug this
        // closes. UCharacterMovementComponent::MovementMode is the field that does separate the two:
        // it reads MOVE_None from the killing blow for as long as the body lies there, and returns
        // to MOVE_Walking when the client puts the character back on its feet (respawn, revive).
        //
        // So a confirmed death is LATCHED, and released only by a successful read of a movement mode
        // that is not MOVE_None. The latch is what makes MOVE_None usable at all: on its own it is
        // not a death signal (a spawning or paused pawn reads it too - see README "Hiding dead
        // characters"), and an actor that never confirmed bDead == 1 is never latched, so
        // uncertainty still hides nothing.
        addr_t movement = 0;  // ACharacter::CharacterMovement
        i32 move_off = -1;    // its MovementMode byte, resolved by name; -1 = no release signal here
        bool dead_latch = false;
        // The mesh component and the renderer's own "I drew this" gate. AActor::bHidden turned out
        // not to mean "not rendering" (see the frame loop), so an actor whose bHidden byte is set is
        // only treated as hidden when the renderer did not draw it either.
        addr_t mesh = 0;
        i32 drawn_off = -1;  // SkinnedMeshComponent::bRecentlyRendered, resolved by reflection
        u8 drawn_byte = 0, drawn_mask = 0;
        // Identity of the FUObjectItem backing this actor (SerialNumber @0x10). A recycled
        // address is detected by an item that no longer points at the actor, or a moved serial.
        addr_t item = 0;
        u32 serial = 0;
        bool serial_ok = false;
        u32 tick = 0;
    };
    static constexpr u32 kValidateFrames = 64;
    static constexpr u32 kNameRetryFrames = 60;  // re-read a nameplate about once a second

    Sdk* nameplate_sdk_ = nullptr;
    std::unique_ptr<Reflection> reflection_;
    std::unique_ptr<NameplateReader> nameplate_;
    NameTable* names_ = nullptr;      // learned names (may outlive one cache)
    NameFieldLearner name_learner_;   // nameplate sightings that identify the direct field
    u32 learn_tick_ = 0;
    int learn_tries_ = 0;

    // Reads display names from the game's own nameplate widgets (Nameplate.hpp). A nameplate
    // only exists while the client shows one for that entity - which is the set the ESP
    // cares about - and walking its widget tree costs a few hundred reads, so one entity is
    // resolved per call and the answer is cached per actor. Unresolved entities are retried
    // periodically, since a nameplate appears and disappears as entities come into range.
    void ResolveRealNames(Sdk* sdk, int budget) {
        if (budget <= 0) return;
        if (!nameplate_ || nameplate_sdk_ != sdk) {
            nameplate_sdk_ = sdk;
            reflection_ = std::make_unique<Reflection>(sdk);
            nameplate_ = std::make_unique<NameplateReader>(sdk, reflection_.get());
        }
        int done = 0;
        // The budget counts nameplates FOUND, so an entry with no nameplate costs one probe and no
        // budget - and meta_ holds every actor ever seen, so without this second bound the loop
        // would walk the whole map every frame. Most actors (props, drops, departed ones) have no
        // nameplate at all.
        int probed = 0;
        for (auto& kv : meta_) {
            if (done >= budget || probed >= 96) break;
            Meta& m = kv.second;
            if (!m.usable || !m.real.empty() || !NameplateNamed(m.type)) continue;
            // No shortcut for a model the table already names: the table is keyed by mesh, and two
            // monsters can share one (a "Baby" variant is the same mesh, scaled), so a model name
            // is not this actor's name. The walk is what reads the client's own name for the actor
            // - which is the only thing that tells such a pair apart - and the table is left to the
            // label path as the fallback for actors with no nameplate. Entities whose nameplate the
            // client has not built are skipped below for one read, so the budget still lands on the
            // ones that have one.
            if (m.name_tick && tick_ - m.name_tick < kNameRetryFrames) continue;
            m.name_tick = tick_;
            ++probed;                                     // the probe is what costs a read
            if (!nameplate_->Widget(kv.first)) continue;  // no nameplate shown right now
            ++done;
            const std::string n = nameplate_->Name(kv.first);
            if (!n.empty()) {
                m.real = n;
                if (ModelNamesEntity(m.type)) {
                    if (names_) names_->Add(m.model, n);
                    name_learner_.Add(kv.first, n);
                }
            }
        }
        // Two sightings are enough to identify the per-actor name field, if the client keeps
        // one: it is persisted and from then on read directly for every character. Five
        // attempts is all this gets - a name that only ever exists in a widget will never be
        // found on the actor, and the search is not free.
        if (names_ && !names_->direct_offset() && name_learner_.size() >= 2 &&
            learn_tries_ < 2 && tick_ - learn_tick_ >= 300) {
            learn_tick_ = tick_;
            ++learn_tries_;
            u32 off = 0;
            NameFieldKind kind = NameFieldKind::None;
            if (name_learner_.Learn(sdk->mem, &off, &kind)) {
                names_->SetDirect(off, kind);
                names_->Save();
                AION_INFO("name field learned: actor+0x%X (%s)", off, NameFieldKindName(kind));
            }
            // No else: learning is retried periodically, and this build simply does not keep the
            // name on the actor, so the notice repeated every attempt. That is the normal case -
            // names come from the client's nameplates and the learned table (aion_names.txt) - and
            // the startup log already says how many names were loaded.
        }
    }

    // Offset of a named reflected field on a class (supers included), or -1. Reflection only;
    // no gameplay offset is hardcoded for it.
    i32 NamedPropOffset(Sdk* sdk, addr_t cls, const char* name) {
        if (!sdk->mem->valid_ptr(cls)) return -1;
        Reflection refl(sdk);
        return refl.GetPropertyOffset(cls, name);
    }

    // Mesh-component class -> offset of its "AnimScriptInstance" property, resolved by name and
    // cached per class (the layout is shared by every instance of a blueprint).
    i32 AnimInstanceOffset(Sdk* sdk, addr_t comp_class) {
        auto it = anim_off_cache_.find(comp_class);
        if (it != anim_off_cache_.end()) return it->second;
        if (anim_off_cache_.size() > 4096) anim_off_cache_.clear();
        return anim_off_cache_
            .emplace(comp_class, NamedPropOffset(sdk, comp_class, "AnimScriptInstance"))
            .first->second;
    }

    // A bool property's location: the byte holding it and the mask inside that byte. FBoolProperty's
    // ByteOffset/ByteMask are the one pair reflection does not expose by name, so they come from
    // Offsets.hpp - and --dump-gates re-checks them every run against bHidden, whose byte and mask are
    // already validated, so a patch that moves them prints MISMATCH instead of a wrong reading.
    struct BoolGate {
        i32 off = -1;
        u8 byte = 0;
        u8 mask = 0;
        bool valid() const { return off >= 0 && mask != 0; }
    };

    // Mesh-component class -> SkinnedMeshComponent::bRecentlyRendered: the renderer's own answer to
    // "did I draw this actor this frame". This is the flag that tracks the screen; AActor::bHidden
    // does not - measured live, the client sets bHidden on actors it is drawing, so bHidden alone
    // dropped on-screen NPCs.
    BoolGate RecentlyRenderedGate(Sdk* sdk, addr_t comp_class) {
        auto it = drawn_gate_cache_.find(comp_class);
        if (it != drawn_gate_cache_.end()) return it->second;
        if (drawn_gate_cache_.size() > 4096) drawn_gate_cache_.clear();
        BoolGate g;
        if (sdk->mem->valid_ptr(comp_class)) {
            Reflection refl(sdk);
            const PropertyInfo pi = refl.GetProperty(comp_class, "bRecentlyRendered");
            g.off = pi.Offset;
            if (pi.Offset >= 0 && pi.Addr) {
                g.byte = sdk->rd<u8>(pi.Addr + off::fbool_property::ByteOffset);
                g.mask = sdk->rd<u8>(pi.Addr + off::fbool_property::ByteMask);
            }
        }
        return drawn_gate_cache_.emplace(comp_class, g).first->second;
    }

    // Movement-component class -> offset of its "MovementMode" property, resolved by name and
    // cached. It is an EMovementMode: 0 is MOVE_None, 1 MOVE_Walking, and so on. -1 means the class
    // (or an unreadable chain) exposes none, and the actor then has no corpse-latch release signal -
    // the same shape as DeadOffset, and for the same reason: no gameplay offset is hardcoded.
    i32 MovementModeOffset(Sdk* sdk, addr_t move_class) {
        auto it = move_off_cache_.find(move_class);
        if (it != move_off_cache_.end()) return it->second;
        if (move_off_cache_.size() > 4096) move_off_cache_.clear();
        return move_off_cache_.emplace(move_class, NamedPropOffset(sdk, move_class, "MovementMode"))
            .first->second;
    }

    // Anim-instance class -> offset of its "bDead" property, resolved by name and cached.
    i32 DeadOffset(Sdk* sdk, addr_t anim_class) {
        auto it = dead_off_cache_.find(anim_class);
        if (it != dead_off_cache_.end()) return it->second;
        if (dead_off_cache_.size() > 4096) dead_off_cache_.clear();
        return dead_off_cache_.emplace(anim_class, NamedPropOffset(sdk, anim_class, "bDead"))
            .first->second;
    }

    // Anim-instance class -> offset of its "bHpDangerAlert" property, resolved by name and cached. A
    // class without it, or an unreadable chain, yields -1: the actor then has no low-health signal
    // and is never "weak" - the same shape as DeadOffset, and for the same reason (no gameplay offset
    // is hardcoded).
    i32 DangerOffset(Sdk* sdk, addr_t anim_class) {
        auto it = danger_off_cache_.find(anim_class);
        if (it != danger_off_cache_.end()) return it->second;
        if (danger_off_cache_.size() > 4096) danger_off_cache_.clear();
        return danger_off_cache_
            .emplace(anim_class, NamedPropOffset(sdk, anim_class, "bHpDangerAlert"))
            .first->second;
    }

    // Env-object anim-instance class -> offset of its "MainState" property, resolved by name and
    // cached per class - no gameplay offset is hardcoded. It is an EAnimInteractionState: Available
    // until the object is used, Finish after. Measured live, 15 untouched world objects read
    // Available and the 2 already-consumed ones Finish, on actors the client leaves in the level.
    i32 MainStateOffset(Sdk* sdk, addr_t anim_class) {
        auto it = main_state_off_cache_.find(anim_class);
        if (it != main_state_off_cache_.end()) return it->second;
        if (main_state_off_cache_.size() > 4096) main_state_off_cache_.clear();
        return main_state_off_cache_
            .emplace(anim_class, NamedPropOffset(sdk, anim_class, "MainState"))
            .first->second;
    }

    // Env-object anim-instance class -> offset of its "bInactiveCondition" byte, resolved by name
    // and cached per class. The client sets it when a node is spent; measured live, it flips 0 -> 1
    // at the moment of gathering and persists, on an actor the client leaves in the level.
    i32 InactiveOffset(Sdk* sdk, addr_t anim_class) {
        auto it = inactive_off_cache_.find(anim_class);
        if (it != inactive_off_cache_.end()) return it->second;
        if (inactive_off_cache_.size() > 4096) inactive_off_cache_.clear();
        return inactive_off_cache_
            .emplace(anim_class, NamedPropOffset(sdk, anim_class, "bInactiveCondition"))
            .first->second;
    }

    // True when a cached identity no longer belongs to the object now at `a`. The FUObjectItem
    // is read in one go: a recycled address backs a different object, so the item either points
    // elsewhere or its SerialNumber (@0x10) has moved on. An item that cannot be read is not
    // treated as stale, so a transient read failure cannot drop a live actor - the validation
    // cadence re-reads it. With no serial (the flagged fallback) the address is the whole key,
    // and only the cadence can catch a reused address.
    bool StaleIdentity(Sdk* sdk, const Meta& m, addr_t a) const {
        if (!m.serial_ok || !m.item) return false;
        u8 item[off::uobject_item::Size];
        if (!sdk->mem->read(m.item, item, sizeof(item))) return false;
        addr_t obj = 0;
        u32 serial = 0;
        std::memcpy(&obj, item + off::uobject_item::Object, sizeof(obj));
        std::memcpy(&serial, item + off::uobject_item::SerialNumber, sizeof(serial));
        return obj != a || serial != m.serial;
    }

    // Class name of a UClass, cached (component scans ask repeatedly).
    const std::string& ClassNameOf(Sdk* sdk, addr_t cls) {
        static const std::string empty;
        if (!sdk->mem->valid_ptr(cls)) return empty;
        auto it = class_cache_.find(cls);
        if (it != class_cache_.end()) return it->second;
        if (class_cache_.size() > 4096) class_cache_.clear();
        return class_cache_.emplace(cls, sdk->Name(sdk->mem->u32_at(cls + off::uobject::Name)))
            .first->second;
    }

    // Offsets of the "*MeshComponent" properties declared by a class (supers included).
    // Blueprint actors expose their mesh components as actor properties -
    // BP_EnvObj_Common_C carries SkeletalMeshComponent/StaticMeshComponent - and that is
    // the reliable way in: the component arrays do not always list them, and a scan that
    // misses them leaves scenery and gathering nodes with no identity at all. Cached per
    // class; every instance of a blueprint shares the layout.
    const std::vector<i32>& MeshComponentOffsets(Sdk* sdk, addr_t cls) {
        static const std::vector<i32> empty;
        if (!sdk->mem->valid_ptr(cls)) return empty;
        auto it = mesh_prop_offsets_.find(cls);
        if (it != mesh_prop_offsets_.end()) return it->second;
        if (mesh_prop_offsets_.size() > 4096) mesh_prop_offsets_.clear();
        std::vector<i32> out;
        Reflection refl(sdk);
        addr_t c = cls;
        for (int d = 0; d < 64 && c; ++d) {
            for (const auto& pi : refl.EnumerateProperties(c))
                // "Mesh" rather than "MeshComponent": some blueprints name the property
                // just "Mesh". The pointed object's class is checked afterwards, so a
                // wider name filter cannot produce a wrong component.
                if (pi.Offset >= 0 && pi.Name.find("Mesh") != std::string::npos)
                    out.push_back(pi.Offset);
            c = sdk->mem->ptr_at(c + off::ustruct::SuperStruct);
        }
        return mesh_prop_offsets_.emplace(cls, std::move(out)).first->second;
    }

    // Name of the mesh/visual asset behind an actor. Characters expose it through
    // ACharacter::Mesh; anything else is found through a *MeshComponent and reading its
    // mesh property. The resolved object is type-checked (StaticMesh / SkeletalMesh) so a
    // wrong offset can never yield a bogus name.
    std::string MeshAssetFor(Sdk* sdk, addr_t actor, bool is_character) {
        addr_t comp = is_character ? sdk->mem->ptr_at(actor + off::acharacter::Mesh) : 0;
        if (!sdk->mem->valid_ptr(comp)) {
            comp = 0;
            // 1. The actor's own reflected mesh-component properties (authoritative).
            const addr_t acls = sdk->mem->ptr_at(actor + off::uobject::Class);
            for (i32 poff : MeshComponentOffsets(sdk, acls)) {
                const addr_t c = sdk->mem->ptr_at(actor + (addr_t)poff);
                if (!sdk->mem->valid_ptr(c)) continue;
                const addr_t ccls = sdk->mem->ptr_at(c + off::uobject::Class);
                if (sdk->mem->valid_ptr(ccls) &&
                    ClassNameOf(sdk, ccls).find("MeshComponent") != std::string::npos) {
                    comp = c;
                    break;
                }
            }
        }
        if (!sdk->mem->valid_ptr(comp)) {
            // 2. Fallback: the blueprint-created / instance component arrays.
            comp = 0;
            const u32 arrays[2] = {off::aactor::BlueprintCreatedComponents,
                                   off::aactor::InstanceComponents};
            for (u32 arr : arrays) {
                for (addr_t c : sdk->mem->read_tarray<addr_t>(actor + arr, 64)) {
                    if (!sdk->mem->valid_ptr(c)) continue;
                    const addr_t ccls = sdk->mem->ptr_at(c + off::uobject::Class);
                    if (sdk->mem->valid_ptr(ccls) &&
                        ClassNameOf(sdk, ccls).find("MeshComponent") != std::string::npos) {
                        comp = c;
                        break;
                    }
                }
                if (comp) break;
            }
        }
        if (!sdk->mem->valid_ptr(comp)) return {};
        const u32 mesh_offs[2] = {off::uskeletal_mesh_component::SkeletalMesh,
                                  off::ustatic_mesh_component::StaticMesh};
        for (u32 mo : mesh_offs) {
            const addr_t mesh = sdk->mem->ptr_at(comp + mo);
            if (!sdk->mem->valid_ptr(mesh)) continue;
            const std::string mc = ClassNameOf(sdk, sdk->mem->ptr_at(mesh + off::uobject::Class));
            if (mc == "StaticMesh" || mc == "SkeletalMesh" || mc == "NiagaraSystem")
                return sdk->Name(sdk->mem->u32_at(mesh + off::uobject::Name));
        }
        return {};
    }

    std::vector<Entity> entities_;
    std::unordered_map<addr_t, Meta> meta_;
    std::unordered_map<addr_t, std::string> class_cache_;
    std::unordered_map<addr_t, std::vector<i32>> mesh_prop_offsets_;
    // Cached world actor list (persistent + streaming levels); see CollectActors.
    std::vector<addr_t> frame_actors_;   // persistent level + published streamed actors, deduplicated
    std::vector<addr_t> streamed_;       // last completed streamed-level sweep
    std::vector<addr_t> pending_;        // sweep under construction
    std::vector<addr_t> levels_;         // UWorld::Levels snapshot for the current sweep
    // CollectActors' dedup scratch. A member, not a local: it holds one entry per actor in the
    // level every frame, and clear() keeps its buckets, so the pass costs no allocation.
    std::unordered_set<addr_t> seen_;
    u32 cursor_ = 0;                     // next streamed level to read
    addr_t world_ = 0;                   // UWorld the cached list belongs to; a change resets it
    addr_t persistent_level_ = 0;        // that world's persistent level, tracked alongside
    std::unordered_map<addr_t, i32> anim_off_cache_;  // mesh-comp class -> AnimScriptInstance off
    std::unordered_map<addr_t, BoolGate> drawn_gate_cache_;  // mesh-comp class -> bRecentlyRendered
    std::unordered_map<addr_t, i32> dead_off_cache_;  // anim-instance class -> bDead offset
    std::unordered_map<addr_t, i32> move_off_cache_;  // movement-component class -> MovementMode offset
    std::unordered_map<addr_t, i32> danger_off_cache_;  // anim-instance class -> bHpDangerAlert off
    std::unordered_map<addr_t, i32> inactive_off_cache_;  // env-obj anim class -> bInactiveCondition
    std::unordered_map<addr_t, i32> main_state_off_cache_;  // env-obj anim class -> MainState
    Diag diag_;
    u32 tick_ = 0;
    addr_t local_pawn_ = 0;
    ClassRules rules_;
};

}  // namespace aion
