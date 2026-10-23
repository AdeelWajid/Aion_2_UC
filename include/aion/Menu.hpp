// Menu.hpp — the ImGui menu: ESP, CONFIG and DEBUG.
//
// Where a setting lives is a rule, not a decoration:
//   ESP     it changes the picture: the master switch, the category grid, the elements, the limits
//   CONFIG  it is written to aion.ini, or set once per session: lists, names, patterns, display
//   DEBUG   it is a read-only number
// Nothing appears in two tabs, and nothing gets a tab of its own: a setting that is only
// occasionally used goes behind a CollapsingHeader inside the tab that owns it. That rule is what
// keeps the tab bar from growing a pane per feature, which is how the previous menu reached five.
//
// The menu never reads game state and never touches the renderer. It edits Config, the host reacts
// to the change hooks below, and live numbers arrive through MenuStats, which the host fills once
// per frame. Save and Reload sit at the foot of the rail, with the panes whose settings they write;
// the class dump, which is a diagnostic rather than a setting, stays in the footer.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Config.hpp"

namespace aion {

class ImGuiLayer;

// One frame's worth of numbers for the DEBUG tab, copied by value so the tab cannot cost reads.
struct MenuStats {
    double fps = 0.0, update_ms = 0.0, reproj_ms = 0.0, draw_ms = 0.0;
    double flush_ms = 0.0, present_ms = 0.0;
    // The composition swapchain's Present() wait: the display taking frames, not this program's cost.
    double present_wait_ms = 0.0;
    int verts = 0, listed = 0, kept = 0, culled = 0, validated = 0, drawn = 0;
    // Actors the client has marked as pending destruction. They stay in the level's actor array
    // until the GC runs, and are dropped from the ESP for exactly that reason.
    int garbage = 0;
    unsigned handle_reopens = 0;
    // Reads that hit a handle the client had already closed. Both counters are 0 on a healthy
    // session; they disagree only when a re-open itself failed.
    unsigned stripped = 0;
    bool cam_valid = false, dcomp = false;
    int width = 0, height = 0, display_hz = 0;
    float fov = 0.f, cam_x = 0.f, cam_y = 0.f, cam_z = 0.f;
    // Live entity counts per EntityType, and how many of them were on screen this frame.
    int per_category[16] = {0};
    int per_category_onscreen[16] = {0};
    // Actors the hunting list matches this frame: the Target row's live count.
    int targets = 0;
    // Name table: how many labels are known, and how many rules are in force.
    int names_learned = 0, names_forced = 0, names_hidden = 0;
    unsigned auto_dodge_fires = 0;
    int auto_dodge_ms_since = -1;
    const char* auto_dodge_last = nullptr;
    int auto_dodge_proj = 0;
    int auto_dodge_ui = 0;
    int auto_dodge_anim = 0;
    const char* auto_dodge_input = nullptr;
};

// One actor the overlay has seen, for CONFIG's picker: the label the user recognises, the model
// that is the stable key, the class and category behind the label, and how many actors matched it in
// the snapshot. Filled by the host on request.
struct ActorSighting {
    std::string label, model, cls, category;
    int live = 0;
};

// The session panel's facts: what this overlay is attached to and what it is running as. Set once
// the client is attached - none of it changes while the overlay runs - so the column never copies
// strings per frame. The numbers that do move come from MenuStats.
struct MenuSession {
    std::string game = "not attached";  // AION2
    std::string process;                // AION2.exe
    unsigned pid = 0;
    std::string module;                 // base address of the client image
    std::string image;                  // its verified size, which is the patch identity
    bool elevated = false;
    bool attached = false;
};

class Menu {
public:
    explicit Menu(Config* cfg);

    bool Open() const { return open_; }
    void Toggle() { open_ = !open_; }
    void Close() { open_ = false; }

    // One-line result of the last action, drawn in the menu's own footer.
    void SetStatus(std::string s) { status_ = std::move(s); }
    const std::string& status() const { return status_; }

    void set_stats(const MenuStats& s) { stats_ = s; }
    void set_session(const MenuSession& s) { session_ = s; }

    // The picker's data: what the cache held when the user last asked, not a live view - a list that
    // changes under the mouse cannot be ticked. Sorted by category then name, so rows stay put.
    void set_actor_list(std::vector<ActorSighting> rows);
    // Fired when the user asks for a fresh snapshot; the host fills it with set_actor_list.
    std::function<void()> on_refresh_actors;

    // Draws the menu window. Call inside an ImGui frame, and only while Open().
    void BuildImGui(ImGuiLayer& layer);

    // Font size changed: the overlay has to re-rasterize its ESP atlas.
    std::function<void()> on_font_changed;
    // The three verbs the menu exposes.
    std::function<void()> on_save_ini;
    std::function<void()> on_reload_ini;
    std::function<void()> on_dump_classes;
    std::function<void()> on_auto_dodge_test;
    // A list, a class pattern, a colour or a name rule changed: the classifier has to be rebuilt
    // from Config.
    std::function<void()> on_rules_changed;

    // Set by the menu when the user closes it from inside (Esc or the window's X). The host turns
    // this into Close() + focus hand-back, so the two ways out behave identically.
    bool request_close = false;

private:
    void PaneEsp();
    void PaneConfig();
    void PaneDebug();
    // The right column: identity and health, on screen under every pane.
    void SessionPanel();
    // Sections, so each tab body stays a list of headings with one call per group.
    void SectionCategories();
    void SectionElements();
    void SectionLimits();
    void SectionLists();
    void SectionDiscover();
    void SectionNames();
    void SectionClassPatterns();
    void SectionDisplay();
    void SectionAutoDodge();
    // Writes the ticked rows into the chosen list, as patterns.
    void AddSelectedActors();

    Config* cfg_;
    MenuStats stats_;
    MenuSession session_;
    int tab_ = 0;
    // Set for the duration of BuildImGui: the menu's own text size row needs the layer to
    // re-rasterize its atlas the moment the size changes.
    ImGuiLayer* layer_ = nullptr;
    bool open_ = false;
    char search_[64] = {};
    // Bulk paste for the name rules: one rule per line.
    char paste_[8192] = {};
    // The picker: the last snapshot, one tick per row, its own search box, and which list the
    // ticks are added to (Bosses / Hunting / Never drawn).
    std::vector<ActorSighting> actors_;
    std::vector<char> actor_sel_;
    char actor_search_[64] = {};
    int actor_dest_ = 0;
    // Set by the Refresh button, honoured after the tab body: see BuildImGui.
    bool refresh_requested_ = false;
    std::string status_;
};

}  // namespace aion
