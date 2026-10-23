// RealNames.hpp — the learned name tables.
//
// A localized name is not stored on the actor, not in the name pool and not in the loaded
// data tables: the client resolves it into a widget and renders that as the entity's
// nameplate. So the name is learned from nameplates, but it is remembered per *model*:
// one sighting of MOB_GraveSoul_01 names every instance of that model, at any distance and
// in every later session, because the table is persisted as text (aion_names.txt).
//
// The same sightings also answer where the client keeps the name on the actor itself, if it
// keeps it there at all: an offset that reproduces the name of two different actors is
// adopted, persisted and then read directly — which names even the entities whose nameplate
// the client never builds. A stale offset (game update) cannot produce a wrong name, because
// a learned offset may never contradict a name already known for that model.
//
// What the user decides about a model's label (force it, take the client's own live name, or
// hide the label) is not learned and is not kept here: those rules are edited in the menu and
// persisted in aion.ini's name_rules key, and are fed to the table with SetRules().
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Config.hpp"
#include "Memory.hpp"
#include "Offsets.hpp"
#include "Sdk.hpp"
#include "Strings.hpp"

namespace aion {

// How the learned per-actor field stores the name.
enum class NameFieldKind : u8 { None = 0, FString = 1, FText = 2 };

inline const char* NameFieldKindName(NameFieldKind k) {
    switch (k) {
        case NameFieldKind::FString: return "fstring";
        case NameFieldKind::FText: return "ftext";
        default: return "none";
    }
}

// An entity name is short, printable and mostly letters/digits — not a hit-point bar, not a
// "Lv." label, not punctuation soup. Used to reject fields that merely look like strings.
inline bool PlausibleName(const std::string& s) {
    if (s.size() < 2 || s.size() > 64) return false;
    int alnum = 0;
    for (unsigned char c : s) {
        if (c < 0x20) return false;
        if (std::isalnum(c)) ++alnum;
    }
    return alnum * 4 >= static_cast<int>(s.size());
}

// Reads the name stored at `addr` the way the client keeps it: an FString header, or an FText
// whose shared data holds the string. `kind` reports which form matched.
inline std::string ReadNameAt(IMemory* m, addr_t addr, NameFieldKind* kind, std::size_t cap) {
    if (kind) *kind = NameFieldKind::None;
    std::string s = m->read_fstring(addr, cap, true);
    if (!s.empty() && PlausibleName(s)) {
        if (kind) *kind = NameFieldKind::FString;
        return s;
    }
    s = m->read_ftext(addr, cap, true);
    if (!s.empty() && PlausibleName(s)) {
        if (kind) *kind = NameFieldKind::FText;
        return s;
    }
    return {};
}

// Offsets inside `actor` where `name` is kept as an FString or an FText. Bounded and
// read-only: a position is only test-read when the pointer stored there already leads to the
// literal, so a field that holds a different string costs one probe read, not a decode.
inline std::vector<u32> NameFieldCandidates(IMemory* m, addr_t actor, const std::string& name,
                                            u32 span = 0x3000, std::size_t max_out = 8) {
    std::vector<u32> out;
    if (!m || actor == 0 || !PlausibleName(name)) return out;
    // Non-ASCII names would need the game's own encoding here; the literal test is skipped
    // for them rather than guessing one (the name still lands in the model table).
    for (unsigned char c : name)
        if (c >= 0x80) return out;
    std::vector<char16_t> lit(name.begin(), name.end());
    std::vector<char> probe(0x200);
    for (u32 off = 0; off + 8 <= span && out.size() < max_out; off += 8) {
        const addr_t p = m->ptr_at(actor + off);
        if (!m->valid_ptr(p)) continue;
        if (!m->read(p, probe.data(), probe.size())) continue;
        bool hit = false;
        for (std::size_t i = 0; i + lit.size() * 2 <= probe.size(); i += 2)
            if (std::memcmp(probe.data() + i, lit.data(), lit.size() * 2) == 0) {
                hit = true;
                break;
            }
        if (!hit) continue;
        NameFieldKind k = NameFieldKind::None;
        if (ReadNameAt(m, actor + off, &k, 128) == name && k != NameFieldKind::None)
            out.push_back(off);
    }
    return out;
}

// Collects (actor, name) pairs produced by the nameplates and decides where the client keeps
// the name on the actor. An offset is adopted only when it reproduces every sighting's own
// name, so a constant (a class tag, a shared label) cannot pass.
class NameFieldLearner {
public:
    void Add(addr_t actor, const std::string& name) {
        if (actor == 0 || !PlausibleName(name)) return;
        for (auto& s : sightings_)
            if (s.actor == actor) {
                s.name = name;
                return;
            }
        if (sightings_.size() >= 8) return;  // enough evidence, keep the cost bounded
        sightings_.push_back({actor, name});
    }

    std::size_t size() const { return sightings_.size(); }

    // The first offset that yields each sighting its own name, on every sighting.
    bool Learn(IMemory* m, u32* offset, NameFieldKind* kind) {
        if (sightings_.size() < 2) return false;
        std::vector<u32> common =
            NameFieldCandidates(m, sightings_[0].actor, sightings_[0].name);
        for (std::size_t i = 1; i < sightings_.size() && !common.empty(); ++i) {
            const std::vector<u32> c =
                NameFieldCandidates(m, sightings_[i].actor, sightings_[i].name);
            std::vector<u32> keep;
            for (u32 o : common)
                if (std::find(c.begin(), c.end(), o) != c.end()) keep.push_back(o);
            common.swap(keep);
        }
        for (u32 o : common) {
            bool all = true;
            NameFieldKind k0 = NameFieldKind::None;
            for (const auto& s : sightings_) {
                NameFieldKind k = NameFieldKind::None;
                if (ReadNameAt(m, s.actor + o, &k, 128) != s.name) {
                    all = false;
                    break;
                }
                k0 = k;
            }
            if (all) {
                if (offset) *offset = o;
                if (kind) *kind = k0;
                return true;
            }
        }
        return false;
    }

    void Clear() { sightings_.clear(); }

private:
    struct Sighting {
        addr_t actor = 0;
        std::string name;
    };
    std::vector<Sighting> sightings_;
};

// model -> localized name, learned from the client's nameplates and persisted as text.
class NameTable {
public:
    void Load(const std::string& path) {
        path_ = path;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return;
        char line[2048];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            if (s[0] == ';') {
                comments_.push_back(s);
                continue;
            }
            // A '#' line is where aion_names.txt used to keep the user's rules ('#key' and
            // '#prefix*': never label those models from storage, use the client's live name). Those
            // rules live in aion.ini's name_rules now and were imported from these lines once, so
            // the line is only kept verbatim - a file the user edited is never rewritten behind the
            // user's back - and decides nothing any more.
            if (s[0] == '#') {
                legacy_lines_.push_back(s);
                continue;
            }
            const std::size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Lower(Trim(s.substr(0, eq)));
            const std::string val = Trim(s.substr(eq + 1));
            if (key.empty() || val.empty()) continue;
            if (key == "direct_name") {
                ParseDirect(val);
                continue;
            }
            if (key == "ambiguous") {
                for (std::size_t b = 0; b < val.size();) {
                    const std::size_t e = val.find(',', b);
                    const std::string k2 = Lower(Trim(val.substr(b, e == std::string::npos
                                                                        ? std::string::npos
                                                                        : e - b)));
                    if (!k2.empty()) ambiguous_.insert(k2);
                    if (e == std::string::npos) break;
                    b = e + 1;
                }
                continue;
            }
            map_[key] = val;
        }
        std::fclose(f);
        for (const auto& a : ambiguous_) map_.erase(a);
        // A model under an Auto rule must not keep a label this file holds: the point of the rule is
        // that what shows is the name the client reports now, not a stored one. Normally the rules
        // arrive after the file (SetRules), but dropping here too means loading a file onto a table
        // that already has rules cannot resurrect a label either.
        DropAutoLabels();
    }

    // The user's rules, from aion.ini's name_rules. They replace the '#' lines this file used to
    // carry, and they are re-applied whenever the ini is reloaded, so the ini stays the one place
    // they are edited.
    void SetRules(const std::vector<Config::NameRule>& rules) {
        forced_.clear();
        hidden_.clear();
        suppressed_.clear();
        suppressed_prefixes_.clear();
        for (const auto& r : rules) {
            const std::string p = Lower(Trim(r.pattern));
            if (p.empty()) continue;
            // A trailing '*' is the family form: the rule covers every model whose name starts
            // with what is left, which is what a portal needs (its model changes between runs).
            const bool family = p[p.size() - 1] == '*';
            const std::string key = family ? Trim(p.substr(0, p.size() - 1)) : p;
            if (key.empty()) continue;
            if (r.mode == Config::NameRule::Mode::Forced)
                forced_.push_back({key, family, r.label});
            else if (r.mode == Config::NameRule::Mode::Hidden)
                hidden_.push_back({key, family, {}});
            else if (family)
                suppressed_prefixes_.push_back(key);
            else
                suppressed_.insert(key);
        }
        DropAutoLabels();
    }

    // The rows aion_names.txt used to hold as the user's rules, for an aion.ini that has no
    // name_rules key yet: 'key=Name' was a forced label, '#key' / '#prefix*' took the client's own
    // name. The learned rows themselves stay in the file, which is read as the cache it now is -
    // only the rules move. Read once, by the caller, and only while the key is missing: after that
    // the ini is the authority.
    static std::vector<Config::NameRule> ImportRules(const std::string& path) {
        std::vector<Config::NameRule> out;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return out;
        char line[2048];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == ';') continue;
            Config::NameRule r;
            if (s[0] == '#') {
                r.mode = Config::NameRule::Mode::Auto;
                r.pattern = Lower(Trim(s.substr(1)));  // a trailing '*' is the family form
            } else {
                const std::size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = Lower(Trim(s.substr(0, eq)));
                const std::string v = Trim(s.substr(eq + 1));
                // The file's own bookkeeping is not a rule.
                if (k.empty() || v.empty() || k == "direct_name" || k == "ambiguous") continue;
                r.mode = Config::NameRule::Mode::Forced;
                r.pattern = k;
                r.label = v;
            }
            if (!r.pattern.empty()) out.push_back(std::move(r));
        }
        std::fclose(f);
        return out;
    }

    // Records a sighting. True when the table changed (so it can be saved). A model seen with
    // two different names is not a name key at all - a shared player mesh, a pet whose owner
    // chose a name - so it is dropped and remembered as ambiguous rather than handing one
    // entity another's name.
    bool Add(const std::string& model, const std::string& name) {
        if (model.empty() || !PlausibleName(name)) return false;
        if (HasI(name, "lv.") || HasI(name, "lv ")) return false;  // level labels share the plate
        const std::string key = Lower(model);
        // A Forced rule owns that model's label: the client's own sighting can neither replace it
        // nor start the two-names-seen seesaw that drops a key as ambiguous. This is what makes a
        // forced label stick, which is why the mode exists.
        const Rule* forced = Match(forced_, key);
        if (forced && !forced->label.empty()) return false;
        // A model under an Auto rule keeps taking the client's own name, however often the client
        // reports a different one, and is never marked dirty, so it is never written back as a
        // label: read it live, every session.
        if (IsAuto(key)) {
            map_[key] = name;
            return false;
        }
        if (ambiguous_.count(key)) return false;
        auto it = map_.find(key);
        if (it != map_.end()) {
            if (it->second == name) return false;
            map_.erase(it);
            ambiguous_.insert(key);
            dirty_ = true;
            return true;
        }
        map_[key] = name;
        dirty_ = true;
        return true;
    }

    // Exact model first, then the longest known key contained in the model, so a variant
    // (MOB_ForestSpirit_01_V01) inherits its base entry (MOB_ForestSpirit_01).
    const std::string* Find(const std::string& model) const {
        if (model.empty()) return nullptr;
        const std::string key = Lower(model);
        // A Forced rule outranks everything learned: it is the user's own label, and every
        // nameplate in the world must not be able to edit it. A 'force:pattern' entry whose label
        // has not been typed yet carries nothing, so it pins nothing either.
        const Rule* forced = Match(forced_, key);
        if (forced && !forced->label.empty()) return &forced->label;
        if (map_.empty()) return nullptr;
        auto it = map_.find(key);
        if (it != map_.end()) return &it->second;
        const std::string* best = nullptr;
        std::size_t best_len = 0;
        for (const auto& kv : map_) {
            if (kv.first.size() < 6 || kv.first.size() >= key.size()) continue;
            if (key.find(kv.first) == std::string::npos) continue;
            if (kv.first.size() > best_len) {
                best_len = kv.first.size();
                best = &kv.second;
            }
        }
        return best;
    }

    // A Hidden rule covers this model: the entity still draws, its name label never does. The
    // caller has to know, because the label it would otherwise fall back to is not a rule's - the
    // model name, the class name, the live nameplate - and all of those are still names.
    bool IsNameHidden(const std::string& model) const {
        // Called for every entity on every frame, so a table with no Hidden rules costs a branch
        // rather than a lowercased copy of every model name.
        if (model.empty() || hidden_.empty()) return false;
        return Match(hidden_, Lower(model)) != nullptr;
    }

    // An Auto rule: never label this model from storage - the client's own name is used instead, in
    // this session and in later ones. Two forms: 'model' for one model, and 'prefix*' for a whole
    // family, which is what portals need because their model changes between runs.
    bool IsAuto(const std::string& key) const {
        if (suppressed_.count(key)) return true;
        for (const auto& p : suppressed_prefixes_)
            if (key.size() >= p.size() && key.compare(0, p.size(), p) == 0) return true;
        return false;
    }

    std::size_t Size() const { return map_.size(); }
    // Rule counts for the startup log: a run that silently ignored its rules must not look like one
    // that never read them.
    std::size_t AutoRules() const { return suppressed_.size() + suppressed_prefixes_.size(); }
    std::size_t ForcedRules() const { return forced_.size(); }
    std::size_t HiddenRules() const { return hidden_.size(); }

    u32 direct_offset() const { return direct_offset_; }
    NameFieldKind direct_kind() const { return direct_kind_; }

    bool SetDirect(u32 offset, NameFieldKind kind) {
        if (offset == direct_offset_ && kind == direct_kind_) return false;
        direct_offset_ = offset;
        direct_kind_ = kind;
        dirty_ = true;
        return true;
    }

    // The name read from the learned per-actor field, or {} when there is nothing valid there.
    // A value that contradicts the name already known for the model is NOT refused any more: a
    // "model" is a mesh, and two monsters can share one (a "Baby" variant is the same mesh,
    // scaled), so the per-actor field is the only source that can tell such a pair apart and it
    // has to be allowed to disagree with the table. A stale offset is caught by the shape of the
    // value instead - the learner only adopts an offset that reproduced every live sighting's own
    // name - and by the label path, which now prefers this value over the table.
    std::string DirectName(IMemory* m, addr_t actor, const std::string& model) const {
        (void)model;  // kept in the signature: the caller's model is no longer a veto
        if (!m || actor == 0 || !direct_offset_ || direct_kind_ == NameFieldKind::None)
            return {};
        NameFieldKind k = NameFieldKind::None;
        const std::string s = ReadNameAt(m, actor + direct_offset_, &k, 128);
        if (s.empty() || k != direct_kind_ || !PlausibleName(s)) return {};
        // The same two shapes Add() rejects: a nameplate also carries the level label.
        if (HasI(s, "lv.") || HasI(s, "lv ")) return {};
        return s;
    }

    void Save() {
        if (!dirty_ || path_.empty()) return;
        // Auto-rule keys are never written out: their whole job is to be read live.
        std::vector<std::pair<std::string, std::string>> rows;
        rows.reserve(map_.size());
        for (const auto& kv : map_)
            if (!IsAuto(kv.first)) rows.push_back(kv);
        std::sort(rows.begin(), rows.end());
        std::FILE* f = std::fopen(path_.c_str(), "wb");
        if (!f) return;
        std::fprintf(f,
                     "; aion_names.txt - model -> name, learned from the client's own nameplates.\n"
                     "; This file is the learned cache. The rules you edit in the menu (force a\n"
                     "; label / take the client's own name / hide it) live in aion.ini's name_rules.\n"
                     ";   key=Name   the name learned for that model\n"
                     ";   Delete a line to let it be re-learned.\n"
                     "; ';' and '#' lines below are kept exactly as they were found, for a file\n"
                     "; that predates name_rules; they decide nothing.\n");
        if (direct_offset_)
            std::fprintf(f, "; direct_name=0x%X:%s  (per-actor name field, validated on load)\n",
                         static_cast<unsigned>(direct_offset_),
                         NameFieldKindName(direct_kind_));
        // The user's own lines are written back untouched: a save that silently dropped them is
        // what made them useless, and a file whose rules were imported into aion.ini is still the
        // file the user wrote - nothing here is deleted on the user's behalf.
        for (const auto& c : comments_)
            std::fprintf(f, "%s\n", c.c_str());
        for (const auto& c : legacy_lines_)
            std::fprintf(f, "%s\n", c.c_str());
        for (const auto& kv : rows)
            std::fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
        if (!ambiguous_.empty()) {
            std::vector<std::string> amb(ambiguous_.begin(), ambiguous_.end());
            std::sort(amb.begin(), amb.end());
            std::fprintf(f, "; these models show different names on different entities, so they\n"
                            "; are not used as name keys (edit by hand to remove):\n");
            std::fprintf(f, "ambiguous=");
            for (std::size_t i = 0; i < amb.size(); ++i)
                std::fprintf(f, "%s%s", i ? "," : "", amb[i].c_str());
            std::fprintf(f, "\n");
        }
        std::fclose(f);
        dirty_ = false;
    }

private:
    // One rule as it is matched against a lowercased model: the pattern with its trailing '*'
    // removed, and whether it was a family ('prefix*') rule.
    struct Rule {
        std::string key;
        bool family = false;
        std::string label;  // Forced rules only
    };

    // The rule covering `key`: an exact pattern first, then the longest family prefix, so a
    // specific rule wins over the broader one that also covers the model.
    static const Rule* Match(const std::vector<Rule>& rules, const std::string& key) {
        const Rule* best = nullptr;
        for (const auto& r : rules) {
            if (!r.family) {
                if (r.key == key) return &r;
            } else if (key.size() >= r.key.size() && key.compare(0, r.key.size(), r.key) == 0) {
                if (!best || r.key.size() > best->key.size()) best = &r;
            }
        }
        return best;
    }

    // Drops from the table every model an Auto rule covers. What the client reports live is what
    // should be shown for those, so a label the file or an earlier session left there must not win -
    // and one learned later is shown but never written back (Add).
    void DropAutoLabels() {
        if (suppressed_.empty() && suppressed_prefixes_.empty()) return;
        for (auto it = map_.begin(); it != map_.end();) {
            if (IsAuto(it->first)) it = map_.erase(it);
            else ++it;
        }
    }

    static std::string Lower(const std::string& s) {
        std::string o = s;
        for (char& c : o)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return o;
    }
    static std::string Trim(const std::string& s) {
        const char* ws = " \t\r\n";
        const std::size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return {};
        const std::size_t b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    }
    void ParseDirect(const std::string& val) {
        const std::size_t colon = val.find(':');
        const std::string offs = colon == std::string::npos ? val : val.substr(0, colon);
        const std::string kind = colon == std::string::npos ? std::string() : val.substr(colon + 1);
        const u32 o = static_cast<u32>(std::strtoul(offs.c_str(), nullptr, 0));
        if (!o || o > 0x10000) return;
        NameFieldKind k = NameFieldKind::None;
        if (HasI(kind, "ftext")) k = NameFieldKind::FText;
        else if (kind.empty() || HasI(kind, "fstring")) k = NameFieldKind::FString;
        if (k == NameFieldKind::None) return;
        direct_offset_ = o;
        direct_kind_ = k;
    }

    std::unordered_map<std::string, std::string> map_;  // lowercased model -> display name
    std::unordered_set<std::string> ambiguous_;  // models whose sightings disagree
    std::vector<Rule> forced_;  // 'force:' rules: the label pinned for those models
    std::vector<Rule> hidden_;  // 'hide:' rules: those models are drawn without a name
    std::unordered_set<std::string> suppressed_;  // 'auto:' for one model: named live, not from storage
    std::vector<std::string> suppressed_prefixes_;  // 'auto:' for a family
    std::vector<std::string> comments_;           // ';' lines, written back verbatim
    std::vector<std::string> legacy_lines_;       // '#' lines, kept but no longer rules
    std::string path_;
    u32 direct_offset_ = 0;
    NameFieldKind direct_kind_ = NameFieldKind::None;
    bool dirty_ = false;
};

}  // namespace aion
