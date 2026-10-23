// Menu.cpp — the ImGui menu: ESP, CONFIG and DEBUG.
//
// Layout rules, applied everywhere below:
//  * the section heading carries the context, so a row label is the shortest honest one ("Arrows"
//    under "Off-screen markers", not "arrows for what is behind you");
//  * a unit lives in the value, never in the label ("Corner 22%", "Thickness 1.00 px");
//  * one row per element, with its colour on the same line, so a colour is never a separate
//    "… colour" row;
//  * one hint per section at most, and it is a fact, not a paragraph.
//
// The menu never reads game state and never touches the renderer. It edits Config, the host reacts
// to the change hooks, and live numbers arrive through MenuStats.
#include "aion/Menu.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "aion/EntityCache.hpp"
#include "aion/ImGuiLayer.hpp"
#include "aion/Strings.hpp"

namespace aion {
namespace {

// One row of the category grid.
struct Category {
    const char* label;
    EntityType type;
    bool Config::*toggle;
    std::string Config::*patterns;  // null where the client's own classes decide the category
    int Config::*color;
};

const Category kCategories[] = {
    {"Players", EntityType::Player, &Config::cat_player, &Config::list_player, &Config::color_player},
    {"NPCs", EntityType::Npc, &Config::cat_npc, &Config::list_npc, &Config::color_npc},
    {"Pets", EntityType::Pet, &Config::cat_pet, &Config::list_pet, &Config::color_pet},
    {"Monsters", EntityType::Monster, &Config::cat_monster, &Config::list_monster,
     &Config::color_monster},
    {"Items", EntityType::Item, &Config::cat_item, &Config::list_item, &Config::color_item},
    {"Gathering", EntityType::Gather, &Config::cat_gather, &Config::list_gather,
     &Config::color_gather},
    {"Interactables", EntityType::Interact, &Config::cat_interact, &Config::list_interact,
     &Config::color_interact},
    {"Cubes", EntityType::Cube, &Config::cat_cube, &Config::list_cube, &Config::color_cube},
    {"Chests", EntityType::Chest, &Config::cat_chest, nullptr, &Config::color_chest},
    {"Portals", EntityType::Portal, &Config::cat_portal, nullptr, &Config::color_portal},
    {"World objects", EntityType::Object, &Config::cat_object, nullptr, &Config::color_object},
    {"Other", EntityType::Other, &Config::cat_other, nullptr, &Config::color_other},
};
constexpr int kCategoryCount = (int)(sizeof(kCategories) / sizeof(kCategories[0]));

const char* const kSnaplineNames[] = {"off", "bottom", "character", "top"};

// The lists the picker can add to, in menu order. Nothing else writes list_* but the text boxes.
const char* const kDestNames[] = {"Bosses", "Hunting", "Never drawn"};

// The panes, in the left rail.
const char* const kTabs[] = {"ESP", "CONFIG", "DEBUG"};
constexpr int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));

// The ini holds two flags for the box (draw one, and make it brackets rather than an outline)
// because that is what earlier versions wrote. To the user it is one choice, so it is one control
// and the pair is derived from it.
enum class BoxMode { Off, Outline, Corners };
const char* const kBoxModeNames[] = {"off", "outline", "corners"};

BoxMode BoxModeOf(const Config& c) {
    if (!c.esp_box) return BoxMode::Off;
    return c.esp_box_corners ? BoxMode::Corners : BoxMode::Outline;
}

void SetBoxMode(Config& c, BoxMode m) {
    c.esp_box = m != BoxMode::Off;
    c.esp_box_corners = m == BoxMode::Corners;
}

// The config stores colours as 0xRRGGBB so aion.ini stays readable; ImGui wants floats.
ImVec4 ColorFromInt(int v) {
    return ImVec4(((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, 1.f);
}

int IntFromColor(const ImVec4& c) {
    const auto byte = [](float f) {
        int i = (int)(f * 255.f + 0.5f);
        return i < 0 ? 0 : (i > 255 ? 255 : i);
    };
    return (byte(c.x) << 16) | (byte(c.y) << 8) | byte(c.z);
}

void Section(const char* title) { ImGui::SeparatorText(title); }

// Dimmed text that wraps, so a hint cannot be half drawn in a narrow window.
void Hint(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// A labelled slider over one of Config's doubles. ImGui's sliders are floats; the precision lost is
// below anything these settings can show.
bool SliderRow(const char* label, double* v, float lo, float hi, const char* fmt) {
    float f = (float)*v;
    ImGui::SetNextItemWidth(190.f);
    if (!ImGui::SliderFloat(label, &f, lo, hi, fmt)) return false;
    *v = f;
    return true;
}

// The same, for a share the config stores as 0.22 and the user reads as 22%.
bool PercentRow(const char* label, double* v, float lo_pct, float hi_pct) {
    float pct = (float)(*v * 100.0);
    ImGui::SetNextItemWidth(190.f);
    if (!ImGui::SliderFloat(label, &pct, lo_pct, hi_pct, "%.0f%%")) return false;
    *v = pct / 100.0;
    return true;
}

bool IntSlider(const char* label, int* v, int lo, int hi, const char* fmt = "%d") {
    ImGui::SetNextItemWidth(190.f);
    return ImGui::SliderInt(label, v, lo, hi, fmt);
}

bool Combo(const char* label, int* v, const char* const items[], int count) {
    ImGui::SetNextItemWidth(190.f);
    return ImGui::Combo(label, v, items, count);
}

// Picker plus the hex the file will hold. Each colour needs its own widget id: several rows share a
// window, and two pickers on one address collide.
bool ColorRow(const char* label, int* value) {
    ImGui::PushID(value);
    ImVec4 c = ColorFromInt(*value);
    const bool changed = ImGui::ColorEdit3(label, (float*)&c, ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine();
    ImGui::TextDisabled("#%06X", (unsigned)*value);
    if (changed) *value = IntFromColor(c);
    ImGui::PopID();
    return changed;
}

// Just the swatch: the element rows and the category grid put the colour beside its own label.
bool ColorSwatch(const char* id, int* value) {
    ImGui::PushID(value);
    ImVec4 c = ColorFromInt(*value);
    const bool changed = ImGui::ColorEdit3(
        id, (float*)&c, ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoInputs);
    if (changed) *value = IntFromColor(c);
    ImGui::PopID();
    return changed;
}

// ImGui edits a fixed buffer, so a string is copied in and written back only when it changed.
bool MultiLine(const char* id, std::string& s, float height) {
    char buf[8192];
    const std::size_t n = s.size() < sizeof(buf) - 1 ? s.size() : sizeof(buf) - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    ImGui::PushID(id);
    const bool changed = ImGui::InputTextMultiline("##text", buf, sizeof(buf), ImVec2(-1, height));
    if (changed) s.assign(buf);
    ImGui::PopID();
    return changed;
}

// Two columns of label/value rows. The debug numbers change every frame, so they are aligned
// instead of written as sentences. The label column is sized to the longest label and the value
// column takes what is left of the row: it was a fixed 124px in the session column, which cut
// "149.3 MB, verified" off at the cell edge with nothing to say a value had been truncated.
bool BeginStats(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// A value too long for its cell is ellipsized instead of silently cut, and the full text is on
// hover. Only values need this: a label that does not fit is the author's own text.
void ValueText(const char* text) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (ImGui::CalcTextSize(text).x <= avail) {
        ImGui::TextUnformatted(text);
        return;
    }
    std::string cut(text);
    while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > avail) cut.pop_back();
    ImGui::TextUnformatted((cut + "...").c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void KVva(const ImVec4& color, const char* label, const char* fmt, va_list args) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    char buf[256];
    std::vsnprintf(buf, sizeof buf, fmt, args);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ValueText(buf);
    ImGui::PopStyleColor();
}

void KV(const char* label, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    KVva(ImGui::GetStyleColorVec4(ImGuiCol_Text), label, fmt, args);
    va_end(args);
}

// The palette the session and status panels read in. Green/amber/red are STATE, never decoration:
// every coloured value below answers "good, watch, or broken", so the colour is a reading of the
// number rather than a mood, and a value that has no verdict attached stays in the theme's own
// colour. Three of these are also palette the ESP boxes and the HUD's group headings use, so the
// same green means the same thing in all three places.
const ImVec4 kGood = ImVec4(0.31f, 0.90f, 0.47f, 1.f);
const ImVec4 kWarn = ImVec4(1.00f, 0.75f, 0.24f, 1.f);
const ImVec4 kBad = ImVec4(1.00f, 0.32f, 0.32f, 1.f);
const ImVec4 kCalm = ImVec4(0.55f, 0.55f, 0.58f, 1.f);
const ImVec4 kAccent = ImVec4(0.59f, 0.78f, 1.00f, 1.f);

// A value that carries a verdict. The colour is the first argument on purpose: on these panels it
// is not styling, it is half the reading - "attached yes" and "camera waiting" are not the same news.
void KVc(const ImVec4& color, const char* label, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    KVva(color, label, fmt, args);
    va_end(args);
}

// A heading in the colour its group owns, so a section is findable by colour and the panes do not
// read as one long list of rows.
void Section(const char* title, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
}

// Where the inline swatches of a column of rows start, so the colours line up.
constexpr float kSwatchX = 210.f;

}  // namespace

Menu::Menu(Config* cfg) : cfg_(cfg) {}

void Menu::BuildImGui(ImGuiLayer& layer) {
    if (!open_) return;
    request_close = false;
    layer_ = &layer;

    // Sized to the display it opens on, so the window cannot start off the screen. The constraints
    // keep it there while it is dragged or resized.
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(
        ImVec2(std::min(1120.f, io.DisplaySize.x - 80.f), std::min(640.f, io.DisplaySize.y - 110.f)),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40.f, 40.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(780.f, 400.f), io.DisplaySize);

    bool keep_open = true;
    if (!ImGui::Begin("AION2 ESP", &keep_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (!keep_open) request_close = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) request_close = true;

    // The footer is a band of two rows - the status line and the actions - which the tab body
    // reserves room for.
    const float footer_h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 3.f;

    // Three columns: what you switched to, the pane itself, and the session under both. The rail is
    // vertical so it reads as a list of panes rather than a strip of buttons, and the session column
    // is on screen under every pane - "what am I attached to" should never need a tab of its own.
    // Wide enough for a whole word, and for the two writes that sit under the panes below. The
    // session column is sized by its longest value - "149.3 MB, verified" beside its label - so it
    // grows with the font instead of cropping the value.
    const float rail_w = 150.f;
    const float session_w = 272.f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;

    ImGui::BeginChild("rail", ImVec2(rail_w, -footer_h), ImGuiChildFlags_Borders);
    for (int i = 0; i < kTabCount; ++i) {
        ImGui::PushID(i);
        const bool selected = (i == tab_);
        if (selected) {
            // The marker gets a gutter of its own. Drawn at the label's own x - which is where this
            // used to be - the 3px bar was painted through the first letter of the selected tab.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float pad_y = ImGui::GetStyle().FramePadding.y;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(p.x, p.y - pad_y + 1.f),
                ImVec2(p.x + 3.f, p.y - pad_y + ImGui::GetFrameHeight() - 1.f),
                ImGui::GetColorU32(ImGuiCol_CheckMark));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 9.f);
        }
        if (ImGui::Selectable(kTabs[i], selected)) tab_ = i;
        ImGui::PopID();
    }
    // Save and Reload act on the file the whole menu writes, so they sit under the panes rather
    // than beside the status line at the bottom of the window, where they read as pane-local
    // actions. The status line stays in the footer, where its result is reported.
    ImGui::Separator();
    const float rail_button_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Save", ImVec2(rail_button_w, 0.f)) && on_save_ini) on_save_ini();
    if (ImGui::Button("Reload", ImVec2(rail_button_w, 0.f)) && on_reload_ini) on_reload_ini();
    Hint("Save writes aion.ini; Reload re-reads it.");
    ImGui::EndChild();

    ImGui::SameLine();
    // The body is one scrolling region: a long pane cannot push the footer off the window.
    ImGui::BeginChild("body", ImVec2(-(session_w + gap), -footer_h), ImGuiChildFlags_None);
    switch (tab_) {
        case 0: PaneEsp(); break;
        case 1: PaneConfig(); break;
        default: PaneDebug(); break;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("session", ImVec2(session_w, -footer_h), ImGuiChildFlags_Borders);
    SessionPanel();
    ImGui::EndChild();

    // The picker's snapshot is taken here, not inside the section that asks for it: the host
    // replaces the row list, and replacing a container while the table drawing it is being built
    // would leave every reference the loop holds dangling.
    if (refresh_requested_) {
        refresh_requested_ = false;
        if (on_refresh_actors) on_refresh_actors();
    }

    ImGui::Separator();
    // The status line owns its own clipped row, ending where the buttons begin: a result as long as
    // the class dump (it ends with the file path) used to run under them. Hovering shows it in full.
    // Wide enough for the label plus its frame padding: at the 19px menu font "Dump classes" is
    // wider than the 92px frame it had, so the text drew out through the button's own border.
    const float dump_w = 118.f;
    const float status_w =
        ImGui::GetContentRegionAvail().x - dump_w - ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::BeginChild("status", ImVec2(status_w, ImGui::GetFrameHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted(status_.empty() ? "ready" : status_.c_str());
    const bool status_hovered = ImGui::IsWindowHovered() && !status_.empty();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (status_hovered) ImGui::SetTooltip("%s", status_.c_str());

    ImGui::SameLine(ImGui::GetWindowWidth() - dump_w - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button("Dump classes", ImVec2(dump_w, 0.f)) && on_dump_classes) on_dump_classes();

    ImGui::End();
}

// ------------------------------------------------------------------------ ESP ----

void Menu::PaneEsp() {
    ImGui::Checkbox("Enabled", &cfg_->esp_enabled);
    Hint("The switch for everything below.");

    Section("Categories");
    SectionCategories();

    Section("Elements");
    SectionElements();

    Section("Lines");
    int mode = cfg_->snapline;
    if (Combo("From", &mode, kSnaplineNames, 4)) cfg_->snapline = mode;
    ImGui::Checkbox("Marker lines", &cfg_->esp_offscreen_snapline);

    Section("Off-screen markers");
    ImGui::Checkbox("Arrows", &cfg_->esp_offscreen);
    SliderRow("Range", &cfg_->offscreen_range_m, 5.0, 300.0, "%.0f m");

    Section("Limits");
    SectionLimits();
}

void Menu::SectionCategories() {
    // A grid, so a table: draw, colour, name, and the live count, which is the only number in the
    // menu worth aligning. No stripes - a dimmed name already reads as "off".
    if (ImGui::BeginTable("categories", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("draw", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("colour", ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("category", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kCategoryCount; ++i) {
            const Category& c = kCategories[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##on", &(cfg_->*(c.toggle)));
            ImGui::TableSetColumnIndex(1);
            ColorSwatch("##colour", &(cfg_->*(c.color)));
            ImGui::TableSetColumnIndex(2);
            const bool on = cfg_->*(c.toggle);
            if (!on)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(c.label);
            if (!on) ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%d", stats_.per_category[(int)c.type]);
            ImGui::PopID();
        }
        // The hunting list as a row of the same grid: it is a category the user switches on and
        // colours, and its count is the number of listed actors being drawn. Listed actors are drawn
        // even when their own category is off, which is what makes "Monsters off, Target on" show
        // the target instead of nothing.
        ImGui::PushID("target");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("##on", &cfg_->esp_target);
        ImGui::TableSetColumnIndex(1);
        ColorSwatch("##colour", &cfg_->color_target);
        ImGui::TableSetColumnIndex(2);
        if (!cfg_->esp_target)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("Target (my hunting list)");
        if (!cfg_->esp_target) ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3);
        ImGui::TextDisabled("%d", stats_.targets);
        ImGui::PopID();
        ImGui::EndTable();
    }

    if (ImGui::Button("All on")) {
        for (int i = 0; i < kCategoryCount; ++i) (cfg_->*(kCategories[i].toggle)) = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("All off")) {
        for (int i = 0; i < kCategoryCount; ++i) (cfg_->*(kCategories[i].toggle)) = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Invert")) {
        for (int i = 0; i < kCategoryCount; ++i)
            (cfg_->*(kCategories[i].toggle)) = !(cfg_->*(kCategories[i].toggle));
    }
    Hint("Target is the hunting list from CONFIG as a category of its own: a listed actor is drawn "
         "in its colour even when its own category is off.");
}

void Menu::SectionElements() {
    int box = (int)BoxModeOf(*cfg_);
    if (Combo("Box", &box, kBoxModeNames, 3)) SetBoxMode(*cfg_, (BoxMode)box);
    PercentRow("Corner", &cfg_->esp_corner_frac, 5.f, 50.f);
    // Floor at 1 px: the stroke is feathered, so anything below 1 renders that fraction of the
    // colour - a slider that reaches 0.05 draws the whole ESP at 5% alpha, which reads as "the ESP
    // is broken". A deliberately faint overlay is still possible by editing esp_thickness in
    // aion.ini, but it can no longer be reached by dragging.
    SliderRow("Thickness", &cfg_->esp_thickness, 1.f, 4.f, "%.2f px");

    ImGui::Checkbox("Name", &cfg_->esp_name);
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##name", &cfg_->color_label);
    ImGui::Checkbox("Distance", &cfg_->esp_distance);
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##distance", &cfg_->color_distance);
    ImGui::Checkbox("Class", &cfg_->esp_class);
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##class", &cfg_->color_class);

    SliderRow("Label size", &cfg_->esp_text_scale, 0.5f, 3.f, "%.2f x");
    ImGui::Checkbox("Text shadow", &cfg_->text_shadow);
}

void Menu::SectionLimits() {
    SliderRow("Max distance", &cfg_->max_distance_m, 10.0, 500.0, "%.0f m");
    IntSlider("Draw limit", &cfg_->max_entities_drawn, 16, 2048, "%d actors");

    ImGui::Checkbox("Hide dead", &cfg_->hide_dead);
    ImGui::SameLine(kSwatchX);
    ImGui::Checkbox("Hide my own character", &cfg_->filter_local_player);

    ImGui::Checkbox("Show every actor", &cfg_->esp_show_all);
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##class_all", &cfg_->color_class_all);
    if (cfg_->esp_show_all)
        Hint("Diagnostic: scenery and volumes included, labelled with the real class in the colour "
             "beside it. The limits above keep the clutter down.");

    ImGui::Checkbox("Draw not-yet-shown", &cfg_->esp_hidden);
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##hidden", &cfg_->color_hidden);
    if (cfg_->esp_hidden) {
        SliderRow("Not-yet-shown range", &cfg_->hidden_range_m, 50.0, 2000.0, "%.0f m");
        Hint("Actors the game is not rendering - monsters it has not shown you yet, and everything "
             "else outside its own view. Drawn in the colour beside the row. The game itself shows "
             "nothing beyond roughly 25 m, so this is what reaches past it.");
    } else {
        Hint("The game flags every actor it is not rendering (bHidden). Off: those are not drawn at "
             "all, which is what stops boxes appearing on monsters that have not appeared yet.");
    }
}

// --------------------------------------------------------------------- CONFIG ----

void Menu::PaneConfig() {
    Section("Lists");
    SectionLists();

    Section("Discover");
    SectionDiscover();

    Section("Names");
    SectionNames();

    if (ImGui::CollapsingHeader("Class patterns")) SectionClassPatterns();

    Section("Display");
    SectionDisplay();
}

void Menu::SectionLists() {
    Hint("One pattern per line, matched against the model, object name, class and the learned name. "
         "A comma separates entries too, which is the form the ini itself uses. Bosses and Hunting "
         "match a whole name exactly; the category lists are substrings.");
    bool changed = false;

    ImGui::TextUnformatted("Bosses");
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##boss", &cfg_->color_boss);
    changed |= MultiLine("boss", cfg_->list_boss, 110.f);

    ImGui::TextUnformatted("Hunting");
    ImGui::SameLine(kSwatchX);
    ColorSwatch("##hunting", &cfg_->color_target);
    changed |= MultiLine("hunting", cfg_->list_target, 110.f);

    ImGui::TextUnformatted("Never drawn");
    Hint("Hidden in every mode. Matched against the class or the mesh asset - the mesh is the only "
         "key for scenery wearing a name that reads as collectable (a chest prop, a decoration).");
    changed |= MultiLine("never", cfg_->never_show, 90.f);

    ImGui::TextUnformatted("Ignored classes");
    Hint("Never categorised, so never drawn.");
    changed |= MultiLine("ignore", cfg_->list_ignore, 90.f);

    if (changed && on_rules_changed) on_rules_changed();
}

void Menu::set_actor_list(std::vector<ActorSighting> rows) {
    std::sort(rows.begin(), rows.end(), [](const ActorSighting& a, const ActorSighting& b) {
        if (a.category != b.category) return a.category < b.category;
        if (a.label != b.label) return a.label < b.label;
        return a.model < b.model;
    });
    actors_ = std::move(rows);
    actor_sel_.assign(actors_.size(), 0);
}

// What the overlay has actually seen, so a list entry can be picked instead of guessed. The class
// and the model are shown next to the name for exactly the case that made this necessary: a name the
// user recognises and a class they have never heard of.
void Menu::SectionDiscover() {
    Hint("Tick what the overlay has seen and add it to a list. Refresh takes a snapshot of the "
         "actors in range; the overlay adds the model, which is the stable key.");
    ImGui::SetNextItemWidth(210.f);
    ImGui::InputTextWithHint("##actor_search", "search", actor_search_, sizeof actor_search_);
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh_requested_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.f);
    ImGui::Combo("##dest", &actor_dest_, kDestNames, 3);
    ImGui::SameLine();
    if (ImGui::Button("Add selected")) AddSelectedActors();

    if (actors_.empty()) {
        Hint("Nothing yet: press Refresh while the actors you want are in range.");
        return;
    }

    if (ImGui::BeginTable("actors", 6,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg,
                          ImVec2(0.f, 240.f))) {
        ImGui::TableSetupColumn("add", ImGuiTableColumnFlags_WidthFixed, 34.f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthFixed, 165.f);
        ImGui::TableSetupColumn("class", ImGuiTableColumnFlags_WidthFixed, 165.f);
        ImGui::TableSetupColumn("category", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < actors_.size(); ++i) {
            const ActorSighting& a = actors_[i];
            if (actor_search_[0] && !HasI(a.label, actor_search_) && !HasI(a.model, actor_search_) &&
                !HasI(a.cls, actor_search_) && !HasI(a.category, actor_search_))
                continue;
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool sel = i < actor_sel_.size() && actor_sel_[i] != 0;
            if (ImGui::Checkbox("##sel", &sel) && i < actor_sel_.size())
                actor_sel_[i] = sel ? 1 : 0;
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(a.label.empty() ? a.model.c_str() : a.label.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", a.model.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", a.cls.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextDisabled("%s", a.category.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextDisabled("%d", a.live);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void Menu::AddSelectedActors() {
    std::string Config::*dest[] = {&Config::list_boss, &Config::list_target, &Config::never_show};
    const int which = (actor_dest_ >= 0 && actor_dest_ < 3) ? actor_dest_ : 0;
    std::string& list = cfg_->*(dest[which]);

    int added = 0;
    for (std::size_t i = 0; i < actors_.size() && i < actor_sel_.size(); ++i) {
        if (!actor_sel_[i]) continue;
        // The model is the stable key; the label is the fallback for an actor whose mesh is unknown.
        // Matching covers both, so either one works, but the model survives a rename.
        const std::string& key = actors_[i].model.empty() ? actors_[i].label : actors_[i].model;
        if (key.empty()) continue;
        // Whole-name duplicate test, the same rule the matcher uses: a line "Contaminated Swarm"
        // must not stop "Contaminated Swarm Egg" from being added.
        {
            std::size_t pos = 0;
            bool already = false;
            while (pos <= list.size()) {
                const std::size_t nl = list.find('\n', pos);
                const std::string line =
                    list.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (!line.empty() && EqualsI(line, key)) { already = true; break; }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            if (already) continue;
        }
        if (!list.empty() && list.back() != '\n') list += '\n';
        list += key;
        actor_sel_[i] = 0;
        ++added;
    }
    if (added && on_rules_changed) on_rules_changed();

    char msg[128];
    std::snprintf(msg, sizeof msg, "%d entr%s added to %s", added, added == 1 ? "y" : "ies",
                  kDestNames[which]);
    SetStatus(msg);
}

void Menu::SectionNames() {
    Hint("%d learned, %d forced, %d hidden. Names come from the client's nameplates.",
         stats_.names_learned, stats_.names_forced, stats_.names_hidden);

    ImGui::SetNextItemWidth(240.f);
    ImGui::InputTextWithHint("##search", "search", search_, sizeof search_);
    ImGui::SameLine();
    if (ImGui::Button("Add row")) {
        Config::NameRule fresh;
        fresh.mode = Config::NameRule::Mode::Forced;
        cfg_->name_rules.push_back(fresh);
        if (on_rules_changed) on_rules_changed();
    }

    bool changed = false;
    int erase_at = -1;
    if (ImGui::BeginTable("rules", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("pattern", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed, 92.f);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.f);
        ImGui::TableHeadersRow();
        static const char* kModes = "Auto\0Forced\0Hidden\0";
        for (std::size_t i = 0; i < cfg_->name_rules.size(); ++i) {
            Config::NameRule& r = cfg_->name_rules[i];
            // The search box hides rows rather than deleting them.
            if (search_[0] && !HasI(r.pattern, search_) && !HasI(r.label, search_)) continue;
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char pattern[128];
            std::snprintf(pattern, sizeof pattern, "%s", r.pattern.c_str());
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::InputText("##pattern", pattern, sizeof pattern)) {
                r.pattern = pattern;
                changed = true;
            }
            ImGui::TableSetColumnIndex(1);
            int mode = (int)r.mode;
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::Combo("##mode", &mode, kModes)) {
                r.mode = (Config::NameRule::Mode)mode;
                changed = true;
            }
            ImGui::TableSetColumnIndex(2);
            char label[160];
            std::snprintf(label, sizeof label, "%s", r.label.c_str());
            ImGui::SetNextItemWidth(-1.f);
            const bool forced = r.mode == Config::NameRule::Mode::Forced;
            ImGui::BeginDisabled(!forced);
            if (ImGui::InputText("##label", label, sizeof label)) {
                r.label = label;
                changed = true;
            }
            ImGui::EndDisabled();
            if (!forced && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", r.mode == Config::NameRule::Mode::Auto
                                            ? "Auto: the client's live name wins"
                                            : "Hidden: no name label for this model");
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("x")) erase_at = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (erase_at >= 0) {
        // Erasing the row is the whole delete: the key is rewritten from this vector on save.
        cfg_->name_rules.erase(cfg_->name_rules.begin() + erase_at);
        changed = true;
    }

    if (ImGui::CollapsingHeader("Paste rows")) {
        Hint("One per line: pattern=Label to force it, #pattern to keep the client's name.");
        ImGui::InputTextMultiline("##paste", paste_, sizeof paste_, ImVec2(-1.f, 70.f));
        if (ImGui::Button("Add them")) {
            // The old aion_names.txt syntax is accepted on purpose: a block copied out of that file
            // pastes straight in.
            const std::string text = paste_;
            std::size_t pos = 0;
            while (pos < text.size()) {
                std::size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                std::string line = text.substr(pos, nl - pos);
                pos = nl + 1;
                const std::size_t a = line.find_first_not_of(" \t\r");
                if (a == std::string::npos) continue;
                const std::size_t b = line.find_last_not_of(" \t\r");
                line = line.substr(a, b - a + 1);
                if (line.empty() || line[0] == ';') continue;
                Config::NameRule r;
                if (line[0] == '#') {
                    r.mode = Config::NameRule::Mode::Auto;
                    line.erase(0, 1);
                } else {
                    const std::size_t eq = line.find('=');
                    if (eq == std::string::npos) {
                        r.mode = Config::NameRule::Mode::Auto;
                    } else {
                        r.mode = Config::NameRule::Mode::Forced;
                        r.label = line.substr(eq + 1);
                        line.erase(eq);
                    }
                }
                r.pattern = line;
                if (!r.pattern.empty()) cfg_->name_rules.push_back(r);
            }
            paste_[0] = '\0';
            changed = true;
        }
        ImGui::SameLine();
        Hint("a trailing * matches a family");
    }

    if (changed && on_rules_changed) on_rules_changed();
}

void Menu::SectionClassPatterns() {
    Hint("Extra substrings per category, for classes the built-in rules miss.");
    bool changed = false;
    for (int i = 0; i < kCategoryCount; ++i) {
        const Category& c = kCategories[i];
        if (!c.patterns) continue;
        ImGui::TextUnformatted(c.label);
        changed |= MultiLine(c.label, cfg_->*(c.patterns), 46.f);
    }
    if (changed && on_rules_changed) on_rules_changed();
}

void Menu::SectionDisplay() {
    IntSlider("Menu text", &cfg_->menu_font_px, 10, 28, "%d px");
    if (ImGui::IsItemDeactivatedAfterEdit() && layer_)
        layer_->RebuildFonts(cfg_->font_face, cfg_->menu_font_px);
    IntSlider("Label text", &cfg_->esp_font_px, 8, 32, "%d px");
    if (ImGui::IsItemDeactivatedAfterEdit() && on_font_changed) on_font_changed();
    ImGui::Checkbox("Scale label text with resolution", &cfg_->esp_font_auto);
    Hint("On, the size above is the size at 1080p and a taller frame rasterizes it larger (1440p "
         "x1.33, 4K x2), never smaller. Off pins every client to the size above.");
    IntSlider("Update rate", &cfg_->present_fps, -1, 240, "%d");
    Hint("-1 presents every frame, 0 follows the display's refresh rate, a positive number is a cap.");

    ImGui::Checkbox("VSync", &cfg_->overlay_vsync);
    ImGui::Checkbox("GPU composition", &cfg_->overlay_dcomp);
    Hint("GPU composition needs a restart to take effect.");
    ImGui::Checkbox("Log to file", &cfg_->log_to_file);
    ImGui::Checkbox("Idle when the game is not focused", &cfg_->skip_when_unfocused);
    Hint("The menu stays awake while it is open, so a name can be typed with the game unfocused.");
}

// ------------------------------------------------------------------- SESSION ----

// Identity and health, under every pane. Slow-changing on purpose: the per-frame numbers are the
// DEBUG pane's job, so this column answers "what am I attached to, and is it healthy" at a glance
// without repeating the tables.
void Menu::SessionPanel() {
    const MenuStats& s = stats_;

    Section("Session", kAccent);
    if (BeginStats("session")) {
        KV("game", "%s", session_.game.c_str());
        KV("process", "%s", session_.process.c_str());
        KV("pid", "%u", session_.pid);
        KV("base", "%s", session_.module.c_str());
        // The image size is the patch identity, and "verified" is the whole point of the row: it says
        // the offsets were checked against exactly this build. Anything else is the case the user has
        // to know about before trusting a single box on screen.
        const bool verified = session_.image.find("verified") != std::string::npos;
        KVc(verified ? kGood : kWarn, "image", "%s", session_.image.c_str());
        KV("engine", "UE5");
        KVc(s.dcomp ? kGood : kWarn, "render", "%s",
            s.dcomp ? "GPU composition" : "layered (CPU)");
        KV("display", "%d x %d, %d Hz", s.width, s.height, s.display_hz);
        ImGui::EndTable();
    }

    Section("Status", kAccent);
    if (BeginStats("status")) {
        // One verdict for the column, then the readings it came from - the same three conditions the
        // HUD's badge covers and in the same order, so the two panels cannot tell different stories.
        // A missing camera is fatal to the reading (nothing on screen is projected), a strip is
        // interference the user should see, and not being elevated is worth flagging because the
        // client refuses reads to a lesser-integrity process.
        const char* verdict = "OK";
        ImVec4 vcol = kGood;
        if (!session_.attached) {
            verdict = "NOT ATTACHED";
            vcol = kBad;
        } else if (!s.cam_valid) {
            verdict = "NO CAMERA";
            vcol = kBad;
        } else if (s.stripped) {
            verdict = "STRIPPED";
            vcol = kWarn;
        } else if (!session_.elevated) {
            verdict = "LIMITED";
            vcol = kWarn;
        }
        KVc(vcol, "verdict", "%s", verdict);
        KVc(session_.attached ? kGood : kBad, "attach", "%s", session_.attached ? "yes" : "no");
        KVc(session_.elevated ? kGood : kWarn, "elevated", "%s", session_.elevated ? "yes" : "no");
        KVc(s.cam_valid ? kGood : kWarn, "camera", "%s", s.cam_valid ? "tracking" : "waiting");
        // The FPS figure is only bad news when the display is not being kept up with, so it is
        // coloured against the display's own rate rather than against an absolute number.
        ImVec4 fcol = kCalm;
        if (s.display_hz > 0)
            fcol = s.fps >= s.display_hz * 0.9
                       ? kGood
                       : (s.fps >= s.display_hz * 0.5 ? kWarn : kBad);
        KVc(fcol, "fps", "%.0f", s.fps);
        KV("kept / drawn", "%d / %d", s.kept, s.drawn);
        // Both are 0 on a healthy session; the first open is acquisition, not a re-open.
        KVc(s.handle_reopens ? kWarn : kGood, "handle reopen", "%u", s.handle_reopens);
        KVc(s.stripped ? kWarn : kGood, "read strips", "%u", s.stripped);
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------- DEBUG ----

void Menu::PaneDebug() {
    const MenuStats& s = stats_;
    ImGui::Checkbox("Debug HUD", &cfg_->esp_debug);
    Hint("Coloured timings and a one-second log line for the ESP's own health.");

    Section("Frame");
    if (BeginStats("frame")) {
        KV("fps", "%.1f", s.fps);
        KV("update", "%.2f ms", s.update_ms);
        KV("draw", "%.2f ms", s.draw_ms);
        KV("flush", "%.2f ms", s.flush_ms);
        KV("present", "%.2f ms (%s)", s.present_ms, s.dcomp ? "GPU composition" : "layered, CPU");
        if (s.present_wait_ms > 0.05)
            KV("wait", "%.2f ms (display taking frames)", s.present_wait_ms);
        KV("resolution", "%d x %d, display %d Hz", s.width, s.height, s.display_hz);
        KV("vertices", "%d", s.verts);
        ImGui::EndTable();
    }

    Section("Entities");
    if (BeginStats("entities")) {
        KV("kept", "%d", s.kept);
        KV("listed", "%d", s.listed);
        KV("drawn", "%d", s.drawn);
        KV("culled", "%d", s.culled);
        KV("garbage", "%d", s.garbage);
        KV("validated", "%d", s.validated);
        KVc(s.handle_reopens ? kWarn : kGood, "handle reopen", "%u", s.handle_reopens);
        KVc(s.stripped ? kWarn : kGood, "read strips", "%u", s.stripped);
        ImGui::EndTable();
    }

    Section("Camera");
    if (s.cam_valid && BeginStats("camera")) {
        KV("position", "%.0f, %.0f, %.0f", s.cam_x, s.cam_y, s.cam_z);
        KV("field of view", "%.1f", s.fov);
        ImGui::EndTable();
    } else if (!s.cam_valid) {
        Hint("waiting for the local player");
    }

    Section("Names");
    if (BeginStats("names")) {
        KV("learned", "%d", s.names_learned);
        KV("forced", "%d", s.names_forced);
        KV("hidden", "%d", s.names_hidden);
        ImGui::EndTable();
    }

    Section("Per category");
    if (ImGui::BeginTable("cats", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("category", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, 48.f);
        ImGui::TableSetupColumn("on screen", ImGuiTableColumnFlags_WidthFixed, 68.f);
        ImGui::TableHeadersRow();
        const ImVec4 off_col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        for (int i = 0; i < kCategoryCount; ++i) {
            const Category& c = kCategories[i];
            const ImVec4 col = ColorFromInt(cfg_->*c.color);
            const bool on = cfg_->*c.toggle;
            const int live = s.per_category[(int)c.type];
            const int onscreen = s.per_category_onscreen[(int)c.type];
            ImGui::TableNextRow();
            // The label carries the colour its boxes are drawn in, so a row here can be matched to
            // what is on screen without knowing the palette by heart, and a category that is
            // switched off is dimmed - its counts are not being drawn whatever they say.
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? col : off_col);
            ImGui::TextUnformatted(c.label);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, live ? kCalm : off_col);
            ImGui::Text("%d", live);
            ImGui::PopStyleColor();
            // The on-screen count is the one that says what the client is actually showing you, so
            // it lights up in its own colour the moment there is anything to see.
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, onscreen > 0 ? col : kCalm);
            ImGui::Text("%d", onscreen);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

}  // namespace aion
