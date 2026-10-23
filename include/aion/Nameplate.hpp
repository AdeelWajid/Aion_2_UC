// Nameplate.hpp — the display name of an actor, read from the game's own nameplate widget.
//
// An actor never stores its name: the client resolves it from its data and puts the result
// in a widget. A character that is currently showing a nameplate has one in
// NameplateWidgetComponent (a WC_NamePlate_C user widget), whose WidgetTree holds the
// AionTextBlock widgets. The tree is walked through reflection (UPanelWidget::Slots ->
// UPanelSlot::Content) because the inherited members of these widget classes are not all
// listed on the derived class.
#pragma once

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Memory.hpp"
#include "Reflection.hpp"
#include "Sdk.hpp"
#include "Strings.hpp"

namespace aion {

class NameplateReader {
public:
    NameplateReader(Sdk* sdk, Reflection* refl) : sdk_(sdk), refl_(refl) {}

    // The nameplate holder of the actor, or 0 when it has none (a nameplate only exists
    // while the game shows one: nearby entities, not every actor in the level).
    //
    // Characters use the validated AionCharacter::NameplateWidgetComponent. World props
    // (gathering nodes, dropped items) declare their own under a different name and offset -
    // BP_EnvObj_Common_C::NamePlateComponent at +0x4F8, null until the client shows it - and
    // the client only shows theirs when the matching option is on
    // (AionWidget_Setting_GatherNamePlate / _MonsterNamePlate / _PlayerNamePlate). So the
    // class's own properties are searched for a nameplate member, and the answer is cached
    // per class; a class that declares none yields 0 rather than a fixed offset landing on
    // unrelated data.
    addr_t Widget(addr_t actor) const {
        const addr_t off_nameplate = NameplateOffset(sdk_->mem->ptr_at(actor + off::uobject::Class));
        if (!off_nameplate) return 0;
        const addr_t wc = sdk_->mem->ptr_at(actor + off_nameplate);
        return sdk_->mem->valid_ptr(wc) ? wc : 0;
    }

    addr_t Root(addr_t actor) const {
        const addr_t wc = Widget(actor);
        if (!wc) return 0;
        // Two hops: the widget on the actor is a pooled helper whose per-entity plate widget
        // is a member, and the plate is the one that owns a built WidgetTree.
        const addr_t plate = Plate(wc);
        const addr_t tree = Tree(plate ? plate : wc);
        if (!tree) return 0;
        return sdk_->mem->ptr_at(tree + off::uwidgettree::RootWidget);
    }

    // Every widget in the nameplate tree, parents before children. `visit` gets the widget
    // and its class name; the tree is walked through reflection because the inherited
    // members of these widget classes are not all listed on the derived class.
    template <class F>
    void Walk(addr_t actor, F&& visit, int max_widgets = 400) const {
        WalkWidget(Root(actor), std::forward<F>(visit), max_widgets);
    }

    // The same walk from a widget instead of an actor: the actor's pooled helper holds the
    // last built nameplate, which is readable after the per-entity plate is detached.
    template <class F>
    void WalkWidget(addr_t root, F&& visit, int max_widgets = 400) const {
        IMemory* m = sdk_->mem;
        std::vector<addr_t> stack{root};
        std::unordered_set<addr_t> seen;
        while (!stack.empty() && static_cast<int>(seen.size()) < max_widgets) {
            const addr_t w = stack.back();
            stack.pop_back();
            if (!m->valid_ptr(w) || !seen.insert(w).second) continue;
            const addr_t cls = m->ptr_at(w + off::uobject::Class);
            if (!m->valid_ptr(cls)) continue;
            const std::string cn = sdk_->Name(m->u32_at(cls + off::uobject::Name));
            visit(w, cn);
            // Children: UPanelWidget::Slots holds UPanelSlot objects, each with a Content.
            const addr_t slots = PropOffset(cls, "Slots");
            if (slots)
                for (addr_t slot : m->read_tarray<addr_t>(w + slots, 256)) {
                    if (!m->valid_ptr(slot)) continue;
                    const addr_t content =
                        PropOffset(m->ptr_at(slot + off::uobject::Class), "Content");
                    if (content) stack.push_back(m->ptr_at(slot + content));
                }
            // Single-child widgets carry their content directly.
            const addr_t content = PropOffset(cls, "Content");
            if (content) stack.push_back(m->ptr_at(w + content));
        }
    }

    // The displayed string of a text widget. Which form holds it is not worth depending on,
    // so both the FText and the FString layout are accepted at the same offset.
    std::string TextOf(addr_t widget) const {
        std::string t = sdk_->mem->read_ftext(widget + off::utextblock::Text, 256, true);
        if (t.empty()) t = sdk_->mem->read_fstring(widget + off::utextblock::Text, 256, true);
        return t;
    }

    // Every non-empty text in the nameplate tree, parents before children. When the plate
    // itself yields nothing, the pooled helper the actor keeps is walked instead: it still
    // holds the last nameplate the client built for that entity, which is the reach this
    // reads from while the plate is detached (entity out of range).
    std::vector<std::string> Texts(addr_t actor, int max_widgets = 400) const {
        std::vector<std::string> out;
        Collect(Root(actor), out, max_widgets);
        if (out.empty()) Collect(Widget(actor), out, max_widgets);
        return out;
    }

    // Where a text widget's string is stored, for the dumps: the FString form, or the FText
    // shared object with the inner offset the string was found at. The bytes next to a
    // string-table entry (its table id and key) are what identify the client's own source.
    std::string TextStorage(addr_t widget) const {
        IMemory* m = sdk_->mem;
        const addr_t hdr = widget + off::utextblock::Text;
        const addr_t d = m->ptr_at(hdr);
        char buf[192];
        if (!m->valid_ptr(d)) {
            std::snprintf(buf, sizeof(buf), "none (hdr=0x%llX)", (unsigned long long)hdr);
            return buf;
        }
        // An FText's shared data begins with flags/history words; the string follows. Report
        // the offset that decodes, so a string-table entry (TableId FName + Key) can be told
        // from a literal by looking at the words after the string.
        for (addr_t o = 0x8; o <= 0x60; o += 8) {
            const std::string s = m->read_fstring(d + o, 128, true);
            if (s.empty()) continue;
            std::snprintf(buf, sizeof(buf), "ftext@0x%llX+0x%llX words=%08X %08X %08X",
                          (unsigned long long)d, (unsigned long long)o, m->u32_at(d),
                          m->u32_at(d + 4), m->u32_at(d + 8));
            return buf;
        }
        if (!m->read_fstring(hdr, 128, true).empty())
            return "fstring";
        std::snprintf(buf, sizeof(buf), "unknown (hdr=0x%llX data=0x%llX)",
                      (unsigned long long)hdr, (unsigned long long)d);
        return buf;
    }

    // The name: the first text that is not a number, a level, or a localization placeholder.
    // A nameplate also carries hit point bars, levels and titles, which is what the filter
    // removes; a title that precedes the name cannot be told apart and is accepted.
    std::string Name(addr_t actor) const {
        for (const auto& t : Texts(actor)) {
            bool numeric = true;
            for (unsigned char c : t)
                if (!std::isdigit(c) && c != '.' && c != '%' && c != ' ' && c != '/') {
                    numeric = false;
                    break;
                }
            if (numeric || HasI(t, "lv.") || HasI(t, "lv ")) continue;
            if (t[0] == '<' || t[0] == '%' || t[0] == '[') continue;  // <Desc>, %STR_..., [guild]
            return t;
        }
        return {};
    }

private:
    // Offset of the actor's nameplate holder. The exact character property first, then any
    // property whose name mentions a nameplate (world props name theirs differently), else 0.
    addr_t NameplateOffset(addr_t cls) const {
        if (!sdk_->mem->valid_ptr(cls)) return 0;
        auto it = np_offset_.find(cls);
        if (it != np_offset_.end()) return it->second;
        // A class that declares a nameplate at all also declares the character one, so the
        // validated offset is only a fallback and never applies to a class without one.
        addr_t any = 0;
        for (addr_t c = cls; c; c = sdk_->mem->ptr_at(c + off::ustruct::SuperStruct)) {
            for (const auto& pi : refl_->EnumerateProperties(c)) {
                if (pi.Offset <= 0) continue;
                if (pi.Name == "NameplateWidgetComponent") {
                    np_offset_[cls] = static_cast<addr_t>(pi.Offset);
                    return np_offset_[cls];
                }
                if (!any && HasI(pi.Name, "nameplate")) any = static_cast<addr_t>(pi.Offset);
            }
            if (c == sdk_->mem->ptr_at(c + off::ustruct::SuperStruct)) break;  // malformed chain
        }
        np_offset_[cls] = any;
        return any;
    }

    // Text blocks under `root`, in walk order.
    void Collect(addr_t root, std::vector<std::string>& out, int max_widgets) const {
        if (!root) return;
        WalkWidget(
            root,
            [&](addr_t w, const std::string& cn) {
                if (cn.find("TextBlock") == std::string::npos) return;
                const std::string t = TextOf(w);
                if (!t.empty()) out.push_back(t);
            },
            max_widgets);
    }

    // True when `o` is a live object whose class is named `cls`. The class object itself has
    // to be a class, so an arbitrary readable pointer cannot pass this test.
    bool IsClass(addr_t o, const char* cls) const {
        IMemory* m = sdk_->mem;
        if (!m->valid_ptr(o)) return false;
        const addr_t c = m->ptr_at(o + off::uobject::Class);
        if (!m->valid_ptr(c)) return false;
        const addr_t cc = m->ptr_at(c + off::uobject::Class);
        if (!m->valid_ptr(cc)) return false;
        if (!HasI(sdk_->Name(m->u32_at(cc + off::uobject::Name)), "Class")) return false;
        return sdk_->Name(m->u32_at(c + off::uobject::Name)) == cls;
    }

    // First member of `obj` that is a `cls`, used when a supplied offset does not fit.
    addr_t FindObject(addr_t obj, const char* cls) const {
        for (addr_t off = 0x40; off <= 0x800; off += 8)
            if (IsClass(sdk_->mem->ptr_at(obj + off), cls))
                return sdk_->mem->ptr_at(obj + off);
        return 0;
    }

    // The per-entity plate widget of a nameplate widget (members of a blueprint widget are
    // not all reflected, so it is found by class).
    addr_t Plate(addr_t wc) const {
        const addr_t plate = FindObject(wc, "NamePlate_C");
        if (plate) return plate;
        const addr_t owner = FindObject(wc, "AionNameplateActor");
        return owner ? FindObject(owner, "NamePlate_C") : 0;
    }

    // The WidgetTree of a user widget: the supplied offset, verified by class, else a search.
    addr_t Tree(addr_t widget) const {
        addr_t t = sdk_->mem->ptr_at(widget + off::uuserwidget::WidgetTree);
        if (!IsClass(t, "WidgetTree")) t = FindObject(widget, "WidgetTree");
        return t;
    }

    // Offset of a named property anywhere in the class chain, or 0. Cached per (class, name),
    // because the widget walk asks the same two questions ("Slots", "Content") of every widget in
    // the tree and a widget tree has few distinct classes; and bounded, because
    // Reflection::EnumerateProperties rebuilds a property list on every call, and a malformed
    // (self-referential) SuperStruct chain would otherwise spin here forever - NameplateOffset
    // guards its own chain walk for exactly that reason.
    addr_t PropOffset(addr_t cls, const char* name) const {
        if (!sdk_->mem->valid_ptr(cls)) return 0;
        const std::string key = std::to_string((unsigned long long)cls) + ":" + name;
        auto cached = prop_off_.find(key);
        if (cached != prop_off_.end()) return cached->second;
        addr_t found = 0;
        addr_t c = cls;
        for (int depth = 0, seen = 0; depth < 64 && seen < 4096 && c; ++depth) {
            const addr_t next = sdk_->mem->ptr_at(c + off::ustruct::SuperStruct);
            for (const auto& pi : refl_->EnumerateProperties(c)) {
                ++seen;
                if (pi.Offset >= 0 && pi.Name == name) {
                    found = static_cast<addr_t>(pi.Offset);
                    break;
                }
            }
            if (found || next == c) break;
            c = next;
        }
        if (prop_off_.size() > 4096) prop_off_.clear();
        return prop_off_.emplace(key, found).first->second;
    }

    Sdk* sdk_;
    Reflection* refl_;
    mutable std::unordered_map<addr_t, addr_t> np_offset_;  // class -> nameplate member offset
    mutable std::unordered_map<std::string, addr_t> prop_off_;  // "class:name" -> property offset
};

}  // namespace aion
