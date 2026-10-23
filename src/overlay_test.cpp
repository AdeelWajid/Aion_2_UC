// overlay_test.cpp — standalone verification of the D3D11 overlay + projection + ESP
// rendering path, using synthetic camera/entities (no game process involved).
//
// Two kinds of check, because the ESP fails in two kinds of way:
//  - geometry and units are asserted against a recording IOverlay, which is what catches a
//    wrong bracket length, a distance measured from the wrong origin, or an off-by-one
//    filter - none of which a screenshot shows clearly;
//  - the same frame is rasterized to overlay_test.bmp, because "is the stroke smooth" and
//    "is the panel legible" can only be looked at.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "aion/Camera.hpp"
#include "aion/DataFile.hpp"
#include "aion/DefaultIni.hpp"
#include "aion/EntityCache.hpp"
#include "aion/Hud.hpp"
#include "aion/ImGuiLayer.hpp"
#include "aion/Menu.hpp"
#include "aion/Overlay.hpp"
#include "aion/Projection.hpp"
#include "aion/Renderer.hpp"
#include "aion/SelfBars.hpp"
#include "app/Window.hpp"   // CreateOverlayWindow / OverlayInstanceRunning (the one-instance rule)

using namespace aion;

static int g_fails = 0;

static void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// Records the primitives a frame emits, so geometry can be checked numerically.
struct Recorder final : IOverlay {
    struct Seg { float x0, y0, x1, y1; };
    std::vector<Seg> lines;
    std::vector<Seg> rects;
    // Fills are kept separately from outlines: every panel background is a fill, and a test that
    // wants to know whether a panel is tall enough needs them without disturbing the box-geometry
    // assertions that read `rects`.
    std::vector<Seg> fills;
    std::vector<Color> line_colors;
    // Stroke widths, so a test can tell a solid stroke from a sub-pixel hairline.
    std::vector<float> line_widths;
    struct TextItem { float x, y; std::string s; };
    std::vector<TextItem> texts_v;  // positions and strings, so label layout can be asserted
    int texts = 0;
    bool Ready() const override { return true; }
    int Width() const override { return 1920; }
    int Height() const override { return h_px; }
    // The backend reports the font's cell height, so a test can hand the UI the size a 1440p client
    // would get - which is exactly where a fixed-pixel layout comes apart.
    float TextHeight(FontSize fs) const override {
        return fs == FontSize::Small ? small_h : 20.f;
    }
    int h_px = 1080;
    float small_h = 12.f;
    void BeginFrame() override {}
    void Line(float x0, float y0, float x1, float y1, Color c, float th) override {
        lines.push_back({x0, y0, x1, y1});
        line_colors.push_back(c);
        line_widths.push_back(th);
    }
    void Rect(float x0, float y0, float x1, float y1, Color, float) override {
        rects.push_back({x0, y0, x1, y1});
    }
    void FillRect(float x0, float y0, float x1, float y1, Color) override {
        fills.push_back({x0, y0, x1, y1});
    }
    void Text(float x, float y, const std::string& s, Color, float) override {
        ++texts;
        texts_v.push_back({x, y, s});
    }
    void EndFrame() override {}
};

static Entity MakeEntity(const char* name, const char* cls, FVector loc, const Camera& cam,
                         Projection& proj, double hh = 90.0) {
    Entity e;
    e.Name = name;
    e.ClassName = cls;
    e.Type = EntityType::Monster;
    e.Location = loc;
    e.Distance = (loc - cam.Location).Length();
    FVector2D s;
    e.OnScreen = proj.WorldToScreen(loc, cam.Rotation, cam.Location, cam.FOV, s);
    e.Screen = s;
    if (e.OnScreen) {
        FVector up = loc, dn = loc;
        up.Z += hh;
        dn.Z -= hh;
        FVector2D t, b;
        proj.WorldToScreen(up, cam.Rotation, cam.Location, cam.FOV, t);
        proj.WorldToScreen(dn, cam.Rotation, cam.Location, cam.FOV, b);
        float h = std::abs(b.Y - t.Y);
        float w = h * 0.5f;
        e.BoundsMin = {s.X - w * 0.5f, t.Y};
        e.BoundsMax = {s.X + w * 0.5f, b.Y};
    }
    return e;
}

// Box geometry, range units and distance origin - the parts a capture cannot pin down.
static void CheckGeometry() {
    printf("geometry:\n");
    Config cfg;
    cfg.snapline = SnaplineOff;
    cfg.esp_name = false;
    cfg.esp_class = false;
    cfg.esp_distance = false;

    Entity e;
    e.Name = "probe";
    e.ClassName = "BPCh_MonsterBase_C";
    e.Type = EntityType::Monster;
    e.OnScreen = true;
    e.Location = {1000, 0, 0};
    e.Distance = 500.0;
    e.BoundsMin = {100.f, 200.f};
    e.BoundsMax = {200.f, 400.f};
    const std::vector<Entity> one{e};
    Camera cam;
    cam.valid = true;
    Projection proj;

    Recorder rec;
    EspRenderer().RenderEntities(rec, cam, one, cfg);
    Check(rec.lines.size() == 8 && rec.rects.empty(),
          "corner brackets: 8 strokes, no closed rectangle");
    // Short side is 100px, so the arm is 0.22 * 100 = 22px along the top edge.
    const bool arm_ok = !rec.lines.empty() && std::abs(rec.lines[0].x0 - 100.f) < 0.01f &&
                        std::abs(rec.lines[0].x1 - 122.f) < 0.01f &&
                        std::abs(rec.lines[0].y0 - 200.f) < 0.01f;
    Check(arm_ok, "top-left bracket arm runs 100,200 -> 122,200");

    cfg.esp_box_corners = false;
    Recorder full;
    EspRenderer().RenderEntities(full, cam, one, cfg);
    Check(full.lines.empty() && full.rects.size() == 1,
          "esp_box_corners=0: one closed rectangle, no strokes");
    cfg.esp_box_corners = true;

    // max_distance_m is metres while Entity::Distance is world units (cm).
    cfg.max_distance_m = 5.0;
    Recorder at_edge;
    EspRenderer().RenderEntities(at_edge, cam, one, cfg);
    Entity past = e;
    past.Distance = 600.0;
    Recorder over_edge;
    EspRenderer().RenderEntities(over_edge, cam, std::vector<Entity>{past}, cfg);
    Check(!at_edge.lines.empty() && over_edge.lines.empty(),
          "max_distance_m=5 keeps a 5.0m target and drops a 6.0m one");

    // Boss colour split: the flag comes only from the declared list, and it changes the colour
    // without changing the category.
    {
        Config boss_cfg;
        boss_cfg.snapline = SnaplineOff;
        boss_cfg.esp_name = false;
        boss_cfg.esp_class = false;
        boss_cfg.esp_distance = false;
        Entity mob;
        mob.Name = "Congealed Drana";
        mob.ClassName = "BPCh_NpcBase_C";
        mob.Model = "MOB_DranaVar_06";
        mob.Type = EntityType::Monster;
        mob.OnScreen = true;
        mob.Distance = 500.0;
        mob.BoundsMin = {100.f, 200.f};
        mob.BoundsMax = {200.f, 400.f};
        Camera cam;
        cam.valid = true;

        Recorder ordinary;
        EspRenderer().RenderEntities(ordinary, cam, std::vector<Entity>{mob}, boss_cfg);
        const Color pale = ordinary.line_colors.empty() ? Color{} : ordinary.line_colors[0];

        ClassRules family;
        // The whole model, exactly as the menu's picker writes it: the lists match a whole name.
        family.boss = SplitList("MOB_DranaVar_03");
        Check(!IsBossActor(family, mob.ClassName, mob.Name, mob.Model),
              "a monster not on the boss list is not a boss");
        Check(IsBossActor(family, mob.ClassName, mob.Name, "MOB_DranaVar_03"),
              "the boss list matches the model");
        Check(!IsBossActor(family, mob.ClassName, mob.Name, "MOB_DranaVar_03_V02"),
              "the boss list is a whole-name match: a longer model is a different actor");
        Check(!IsBossActor(family, mob.ClassName, "Drana Predator", "MOB_DranaVar_06"),
              "a shared family prefix alone does not promote a monster");
        Check(IsBossActor(family, mob.ClassName, "MOB_DranaVar_03", "MOB_Other"),
              "the list also matches the object name");
        Check(!IsBossActor(family, mob.ClassName, "DranaVar_03 the Awakened", "MOB_Other"),
              "a longer object name that merely contains the entry is a different actor");
        // The name the user reads off the ESP label must be usable: it is the only name in
        // front of them, and it is not the model and not the engine object name.
        ClassRules by_name;
        by_name.boss = SplitList("Drana Predator");
        Check(!IsBossActor(by_name, mob.ClassName, mob.Name, mob.Model, mob.Name),
              "a display name that is not listed stays ordinary");
        Check(IsBossActor(by_name, mob.ClassName, mob.Name, mob.Model, "Drana Predator"),
              "the list matches the learned on-screen name");
        Check(!IsBossActor(by_name, mob.ClassName, mob.Name, "MOB_DranaVar_06", "Congealed Drana"),
              "the other monster of the same family stays ordinary");

        // The report that produced the whole-name rule: the lists used to match substrings, so
        // "Contaminated Swarm" also claimed "Contaminated Swarm Egg" - a different actor.
        ClassRules hunt;
        hunt.target = SplitList("Contaminated Swarm");
        Check(IsTargetActor(hunt, "BPCh_NpcBase_C", "Contaminated Swarm", "MOB_Swarm_01",
                            "Contaminated Swarm"),
              "the hunt list matches the name that is listed");
        Check(!IsTargetActor(hunt, "BPCh_NpcBase_C", "Contaminated Swarm Egg", "MOB_SwarmEgg_01",
                             "Contaminated Swarm Egg"),
              "\"Contaminated Swarm\" does not claim \"Contaminated Swarm Egg\"");

        // The documented format: comma separated or one name per line, each entry a case-insensitive
        // whole name. The shipped comment used to show a semicolon, which SplitList treats as an
        // ordinary character - that entry then matches nothing and the setting looks broken.
        ClassRules many;
        many.boss = SplitList("MOB_DranaVar_03, Drana Predator");
        Check(many.boss.size() == 2, "the boss list splits on commas and trims the spaces");
        Check(IsBossActor(many, mob.ClassName, mob.Name, "MOB_DranaVar_03"),
              "a comma separated list matches the model");
        Check(IsBossActor(many, mob.ClassName, mob.Name, mob.Model, "Drana Predator"),
              "and the on-screen name in the same list");
        Check(SplitList("MOB_A;Drana Predator").size() == 1,
              "a semicolon is not a separator, so no example may use one");

        many.boss = SplitList("Coradon\nGorilla");
        Check(many.boss.size() == 2 && many.boss[1] == "Gorilla",
              "a line break separates entries, so a list can be typed one name per line");
        Check(IsBossActor(many, "BPCh_NpcBase_C", "Coradon", ""),
              "the entry below the break matches");

        // The ini value stays one line: a value spanning lines would be cut in two when the file is
        // read back, so a break is written as a comma.
        Config breaks;
        breaks.list_boss = "Coradon\nGorilla";
        bool stored_one_line = false;
        for (const auto& kv : breaks.Pairs()) {
            if (kv.first != "list_boss") continue;
            stored_one_line = kv.second.find('\n') == std::string::npos &&
                              kv.second.find("Coradon,Gorilla") != std::string::npos;
        }
        Check(stored_one_line, "a list with line breaks is stored on one line, with commas");
        Check(RulesFromConfig(breaks).boss.size() == 2, "and it loads back as two entries");

        Entity boss = mob;
        boss.Model = "MOB_DranaVar_03";
        boss.Boss = IsBossActor(family, boss.ClassName, boss.Name, boss.Model);
        Check(boss.Boss && boss.Type == EntityType::Monster,
              "the boss keeps its category: only the colour changes");
        Recorder deep;
        EspRenderer().RenderEntities(deep, cam, std::vector<Entity>{boss}, boss_cfg);
        const Color red = deep.line_colors.empty() ? Color{} : deep.line_colors[0];
        const bool split = std::abs(pale.R - 1.f) < 0.01f && std::abs(pale.G - 150.f / 255.f) < 0.01f &&
                           std::abs(red.R - 1.f) < 0.01f && std::abs(red.G - 60.f / 255.f) < 0.01f;
        Check(split, "ordinary monsters are pale red (255,150,150) and bosses solid red (255,60,60)");
        Check(!deep.lines.empty() && deep.lines.size() == ordinary.lines.size(),
              "the boss is drawn with the same geometry, only recoloured");
    }

    // Distance origin: the pawn, not the third-person camera behind it.
    std::vector<Entity> probe;
    Entity me;
    me.LocalPlayer = true;
    me.Location = {1000, 0, 0};
    Entity foe;
    foe.Location = {2000, 0, 0};
    probe.push_back(me);
    probe.push_back(foe);
    const FVector cam_behind{-500, 0, 900};
    ComputeDistances(probe, DistanceOrigin(probe, cam_behind));
    const double from_pawn = probe[1].Distance;
    const double from_camera = (foe.Location - cam_behind).Length();
    printf("  pawn->foe %.1fm, camera->foe %.1fm\n", from_pawn / 100.0, from_camera / 100.0);
    Check(std::abs(from_pawn - 1000.0) < 0.5, "range is measured from the local pawn");
    Check(from_camera > from_pawn + 1000.0, "the camera origin would have read far larger");
}

// aion.ini handling: loading means "the file is now the truth", every list_* key reaches the
// classifier through one mapping, and a data file is looked for next to the exe before the working
// directory (the shipped bug: launched from Explorer, the overlay ran on defaults and the boss list
// looked broken).
static void CheckConfig() {
    printf("config:\n");
    const std::string tmp = ResolveDataFile("aion_test_cfg.tmp");
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f) {
        std::fprintf(f, "esp_box=0\nmax_distance_m=333\nlist_boss=MOB_A,MOB_B\nlist_cube=BP_CubeX\n");
        std::fclose(f);
    }
    Config c;
    c.esp_box = true;
    c.max_distance_m = 999.0;
    c.list_gather = "STALE";  // a value only the previous file could have left behind
    c.Load(tmp.c_str());
    Check(f != nullptr, "the test config was written");
    Check(!c.esp_box, "a key in the file is applied");
    Check(std::abs(c.max_distance_m - 333.0) < 0.001, "a numeric key is applied");
    Check(c.list_gather.empty(), "loading resets keys the file no longer has");
    const ClassRules rr = RulesFromConfig(c);
    Check(rr.boss.size() == 2, "list_boss reaches the classifier");
    Check(rr.cube.size() == 1, "list_cube reaches the classifier (it used to be parsed and dropped)");
    Check(IsBossActor(rr, "BPCh_NpcBase_C", "NPC_1", "MOB_B"), "the parsed boss entry matches");
    std::remove(tmp.c_str());

    Check(ResolveDataFile("C:\\nowhere\\aion.ini") == "C:\\nowhere\\aion.ini",
          "an absolute data path is used as given, never rewritten");
    const std::string name = "aion_probe_zz.tmp";
    const std::string miss = ResolveDataFile(name);  // no such file: the exe-folder fallback
    Check(miss != name && miss.find(name) == miss.size() - name.size(),
          "an unknown data file resolves to a full path ending in its name");
    const std::string exe_dir = miss.substr(0, miss.find_last_of("\\/"));
    const std::string in_parent = exe_dir + "\\..\\" + name;
    std::FILE* a = std::fopen(in_parent.c_str(), "wb");
    if (a) std::fclose(a);
    const std::string from_parent = ResolveDataFile(name);
    Check(from_parent != miss, "the exe's parent folder is searched too (build\\ -> project root)");
    std::FILE* b = std::fopen(miss.c_str(), "wb");
    if (b) std::fclose(b);
    Check(ResolveDataFile(name) == miss, "a file next to the exe wins over the parent folder");
    std::remove(miss.c_str());
    std::remove(in_parent.c_str());
}

// The aion.ini a fresh copy starts from: every persisted key exactly once, at its shipped default,
// and loading the file back has to reproduce those defaults. This is what catches the two ways the
// generated file can lie - a default changed in Config but not in the template, and a key that is
// written (Pairs) but never read back (Load), which is how max_entities_drawn, esp_debug and
// log_to_file silently reset on restart before this existed.
static void CheckDefaultIni() {
    const std::string path = ResolveDataFile("aion_test_default.ini");
    std::remove(path.c_str());
    const Config def;
    const auto pairs = def.Pairs();
    Check(WriteDefaultIni(path), "a default aion.ini is written when there is none");
    Check(!WriteDefaultIni(path), "an existing aion.ini is never overwritten");

    std::string text;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    Check(!text.empty(), "the generated file has content");

    std::size_t missing = 0, total = 0;
    for (const auto& kv : pairs) {
        const std::string key = kv.first + "=";
        std::size_t at = 0, count = 0;
        while ((at = text.find("\n" + key, at)) != std::string::npos) { ++count; at += key.size() + 1; }
        if (!count) ++missing;
        total += count;
    }
    Check(missing == 0, "every persisted key is in the generated file");
    Check(total == pairs.size(), "...exactly once each");

    Config back;
    back.Load(path);
    const auto again = back.Pairs();
    std::size_t differs = 0;
    for (std::size_t i = 0; i < pairs.size() && i < again.size(); ++i) {
        if (pairs[i].first == again[i].first && pairs[i].second == again[i].second) continue;
        ++differs;
        if (differs <= 5)
            printf("       %s: wrote '%s', read back '%s'\n", pairs[i].first.c_str(),
                   pairs[i].second.c_str(), again[i].second.c_str());
    }
    Check(differs == 0, "loading it back reproduces the shipped defaults");
    std::remove(path.c_str());
}

// The declared lists that grow while playing: one pattern per line, comments and blank lines
// ignored, and the entries union with the ini's lists without the Config being rewritten - which is
// what keeps "Save settings" from writing a file's contents back into aion.ini.
static void CheckListFiles() {
    const std::string path = ResolveDataFile("aion_test_boss.txt");
    std::remove(path.c_str());
    Check(ReadPatternFile(path).empty(), "a missing list file is an empty list, not an error");
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "# declared bosses\n  MOB_DranaVar_03  \n\nMOB_WindEle_01\n");
        std::fclose(f);
    }
    const std::vector<std::string> pats = ReadPatternFile(path);
    Check(pats.size() == 2, "a pattern file reads its non-comment lines only");
    Check(pats.size() == 2 && pats[0] == "MOB_DranaVar_03" && pats[1] == "MOB_WindEle_01",
          "...trimmed of the whitespace around them");

    Config cfg;  // shipped defaults: list_boss is empty
    ClassRules r = RulesFromConfig(cfg, pats, {});
    Check(r.boss.size() == pats.size(), "the file's entries reach the classifier");
    Check(IsBossActor(r, "BPCh_NpcBase_C", "MOB_DranaVar_03", "", ""),
          "an actor named by the file is classified as a boss");
    Check(cfg.list_boss.empty(), "the Config the ini owns is left untouched");
    cfg.list_boss = "Drana,WindEle";
    r = RulesFromConfig(cfg, pats, {"MOB_Fossa_01"});
    Check(r.boss.size() == 4 && r.target.size() == 1, "ini and file entries union");
    Check(IsTargetActor(r, "", "", "MOB_Fossa_01", ""), "a target from the file matches too");
    std::remove(path.c_str());
}

// Name, class and distance hang off the box's centre, not its left edge: left-aligned, a long
// name runs away from the box and a short one looks attached to nothing. The measured width comes
// from the same call the renderer used, so this pins the pivot - and the left-edge assertion is
// the actual regression, independent of the measurement.
static void CheckLabels() {
    printf("labels:\n");
    Config cfg;
    cfg.esp_enabled = true;
    cfg.esp_box = true;
    cfg.snapline = SnaplineOff;
    cfg.esp_name = cfg.esp_class = cfg.esp_distance = true;
    cfg.esp_text_scale = 1.f;

    Entity e;
    e.Name = "Odious Spider";
    e.ClassName = "BPCh_MonsterBase_C";
    e.Type = EntityType::Monster;
    e.OnScreen = true;
    e.Distance = 1234.0;
    e.BoundsMin = {100.f, 200.f};
    e.BoundsMax = {300.f, 400.f};

    Recorder rec;
    Camera cam;
    EspRenderer().RenderEntities(rec, cam, std::vector<Entity>{e}, cfg);
    const float box_cx = (e.BoundsMin.X + e.BoundsMax.X) * 0.5f;
    Check(rec.texts == 3, "name, class and distance all drawn");
    for (const Recorder::TextItem& t : rec.texts_v) {
        const float w = rec.TextWidth(t.s, (float)cfg.esp_text_scale, IOverlay::FontSize::Small);
        const float label_cx = t.x + w * 0.5f;
        const bool centred = std::fabs(label_cx - box_cx) <= 0.75f;
        printf("    \"%s\" x=%.1f w=%.1f label_centre=%.1f box_centre=%.1f%s\n", t.s.c_str(), t.x,
               w, label_cx, box_cx, centred ? "" : "   <- off centre");
        Check(centred, "label centred on the box");
        Check(t.x > e.BoundsMin.X, "label not hung off the box's left edge");
    }
}

// The label size follows the client when esp_font_auto is on (the default). esp_font_px is defined
// at 1080p, so a taller frame rasterizes it larger and a label keeps its fraction of the frame; a
// shorter or DPI-scaled client keeps the configured size rather than shrinking with it.
static void CheckFontScale() {
    printf("font scale:\n");
    Config c;
    c.esp_font_px = 14;
    c.esp_font_auto = true;
    struct Case { int h; int want; const char* what; };
    const Case cases[] = {
        {768, 14, "a 768-line client keeps the configured size"},
        {864, 14, "a 1080p client at 125% scaling keeps it (1536x864)"},
        {1080, 14, "1080p is the reference size"},
        {1200, 16, "1920x1200 rasterizes 10/9 of it"},
        {1440, 19, "1440p rasterizes 4/3 of it"},
        {1600, 21, "2560x1600 rasterizes 40/27 of it"},
        {1800, 23, "2880x1800 rasterizes 5/3 of it"},
        {2160, 28, "4K doubles it"},
    };
    for (const Case& k : cases) {
        const int got = EspFontPx(c, k.h);
        printf("    %5d -> %2d px%s\n", k.h, got, got == k.want ? "" : "   <- wrong");
        Check(got == k.want, k.what);
    }
    c.esp_font_auto = false;
    Check(EspFontPx(c, 2160) == 14, "off pins the configured size on every client");
    c.esp_font_auto = true;
    c.esp_font_px = 32;
    Check(EspFontPx(c, 4320) == 96, "the rasterized size is capped at the atlas budget");
    c.esp_font_px = 6;
    Check(EspFontPx(c, 240) == 6, "below 1080 lines the configured size is used as-is");
}

// One entry per client size the tool has to look right on: the two it is actually run at, the
// budget and scaled laptop panels, the 16:10 sizes, 4K, and a portrait window for the FOV axis.
// Three properties per size, all numeric and none of them a restatement of the projection:
//  - the view-axis point lands on the exact centre: a half-pixel viewport error, or an aspect
//    applied on top of a viewport that already carries it, shows up here;
//  - the same tangent offset moves the same number of pixels horizontally and vertically, which is
//    the "no stretch" property: it fails the moment half_x and half_y are not the pair the aspect
//    demands (this is what a 16:10 client caught);
//  - the FOV names the wider axis (UE's rule): a 45 degree ray along the horizontal axis is the
//    right edge on a landscape client, along the vertical axis on a portrait one, and the other
//    axis is provably narrower.
static void CheckResolutions() {
    printf("resolutions:\n");
    struct Size { int w, h; const char* name; };
    const Size sizes[] = {
        {1366, 768, "1366x768"},    {1536, 864, "1536x864 (1080p at 125%)"},
        {1600, 900, "1600x900"},    {1920, 1080, "1920x1080"},
        {1920, 1200, "1920x1200"},  {2560, 1440, "2560x1440"},
        {2560, 1600, "2560x1600"},  {2880, 1800, "2880x1800"},
        {3440, 1440, "3440x1440 UW"}, {3840, 2160, "3840x2160"},
        {5120, 1440, "5120x1440 super UW"}, {1080, 1920, "1080x1920 portrait"},
    };
    Camera cam;
    cam.Location = {0, 0, 0};
    cam.Rotation = {0, 0, 0};  // forward = +X, right = +Y, up = +Z
    cam.FOV = 90.f;            // half_tan = tan(45) = 1, so the 45 degree rays below are exact
    Projection proj;
    for (const Size& s : sizes) {
        proj.SetViewport(s.w, s.h);
        const double cx = s.w * 0.5, cy = s.h * 0.5;
        const double t = 0.2;  // a 0.2 (11 degree) tangent offset, well inside the frame
        FVector2D centre{}, right{}, up{}, edge_x{}, edge_y{};
        const bool ok =
            proj.WorldToScreen({100, 0, 0}, cam.Rotation, cam.Location, cam.FOV, centre) &&
            proj.WorldToScreen({100, 100 * t, 0}, cam.Rotation, cam.Location, cam.FOV, right) &&
            proj.WorldToScreen({100, 0, 100 * t}, cam.Rotation, cam.Location, cam.FOV, up) &&
            proj.WorldToScreen({100, 100, 0}, cam.Rotation, cam.Location, cam.FOV, edge_x) &&
            proj.WorldToScreen({100, 0, 100}, cam.Rotation, cam.Location, cam.FOV, edge_y);
        printf("    %-22s centre=(%.1f,%.1f) dx=%.2f dy=%.2f edge=(%.1f,%.1f)\n", s.name, centre.X,
               centre.Y, right.X - cx, cy - up.Y, edge_x.X, edge_y.Y);
        Check(ok, "every corner of the frame projects");
        Check(std::fabs(centre.X - cx) < 0.01 && std::fabs(centre.Y - cy) < 0.01,
              "the view axis is the exact centre of the frame");
        // Both offsets are the same tangent (0.2) at 90 degrees FOV, so both must be the same
        // number of pixels: dx = (w/2) * t/half_x, dy = (h/2) * t/half_y.
        const double dx = right.X - cx, dy = cy - up.Y;
        Check(std::fabs(dx - dy) < 0.02, "the frame is not stretched (equal pixel scale both ways)");
        if (s.w >= s.h) {
            Check(std::fabs(edge_x.X - s.w) < 0.01, "45 degrees across is the frame's right edge");
            Check(edge_y.Y < -1.0, "the vertical axis is the narrower one on a landscape frame");
        } else {
            Check(std::fabs(edge_y.Y) < 0.01, "45 degrees up is the frame's top edge");
            Check(edge_x.X > s.w + 1.0, "the horizontal axis is the narrower one on a portrait frame");
        }
    }
}

// --- synthetic UE graph, to exercise EntityCache against injected faults -------------------
//
// EntityCache reads a live process, so a fix to its lifetime/read-failure behaviour cannot be
// checked against the game in this session. This builds the smallest memory the code path needs -
// an FNamePool, a chunked FUObjectArray, a UWorld/ULevel with an actor array, and one character with
// a mesh component, an anim instance and a bDead byte - behind a fake IMemory that can be told to
// fail a read. That is enough to force the three faults the fixes are about: a failed bDead read, a
// world switch, and an address the object array recycles.
namespace {

struct FakeMemory final : IMemory {
    addr_t base = 0x100000;
    std::map<addr_t, std::vector<u8>> pages;          // sparse 0x1000 pages
    std::vector<std::pair<addr_t, addr_t>> deny;      // [lo,hi) reads that fail, to force a fault
    // Every syscall the code path would make on the real client, so a cost regression can be asserted
    // rather than argued about.
    mutable std::size_t reads = 0;

    addr_t module_base() const override { return base; }
    std::size_t module_size() const override { return 0x20000000; }
    bool writable() const override { return true; }

    void poke(addr_t a, const void* src, std::size_t n) {
        const u8* s = static_cast<const u8*>(src);
        for (std::size_t i = 0; i < n; ++i) {
            auto& pg = pages[(a + i) & ~(addr_t)0xFFF];
            if (pg.empty()) pg.assign(0x1000, 0);
            pg[(a + i) & 0xFFF] = s[i];
        }
    }
    void zero(addr_t a, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            auto& pg = pages[(a + i) & ~(addr_t)0xFFF];
            if (pg.empty()) pg.assign(0x1000, 0);
            pg[(a + i) & 0xFFF] = 0;
        }
    }
    template <class T> void put(addr_t a, T v) { poke(a, &v, sizeof(T)); }
    void putstr(addr_t a, const char* s) { poke(a, s, std::strlen(s)); }

    bool read(addr_t a, void* out, std::size_t n) const override {
        for (const auto& d : deny)
            if (a < d.second && a + n > d.first) return false;
        ++reads;
        u8* o = static_cast<u8*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = pages.find((a + i) & ~(addr_t)0xFFF);
            if (it == pages.end()) return false;      // unmapped = the read fails, like the real one
            o[i] = it->second[(a + i) & 0xFFF];
        }
        return true;
    }
    bool write(addr_t a, const void* in, std::size_t n) override { poke(a, in, n); return true; }
};

// The minimal engine graph EntityCache::UpdateBulk walks.
struct FakeGame {
    FakeMemory mem;
    Sdk sdk;
    addr_t bump = 0;
    addr_t pool = 0, name_block = 0, gobjects = 0, chunk_table = 0, gworld_cell = 0;
    u32 name_cursor = 2;                          // byte offset into block 0; id = offset / 2
    std::vector<addr_t> objects;

    addr_t alloc(std::size_t n) {
        const addr_t a = bump;
        const std::size_t sz = (n + 0xF) & ~std::size_t(0xF);
        bump += sz;
        mem.zero(a, sz);                              // mapped and zero, so a block read succeeds
        return a;
    }

    void begin() {
        bump = mem.base + 0x10000;
        pool        = alloc(0x40);
        name_block  = alloc(0x20000);
        gobjects    = alloc(0x40);
        chunk_table = alloc(0x40);
        gworld_cell = alloc(8);
        mem.put<addr_t>(pool + off::name_pool::Blocks, name_block);
        mem.put<u32>(pool + off::name_pool::CurrentBlock, 1);
        mem.put<addr_t>(gobjects + off::uobject_array::Objects, chunk_table);
    }

    // Intern a name into the pool (block 0, narrow entries). An FNameEntryId is the entry's
    // byte offset / 2, and entries are variable length, so the id is not an ordinal - it is
    // computed from where the entry lands.
    u32 intern(const std::string& s) {
        const u32 entry_off = name_cursor;
        const u32 id = entry_off / off::name_pool::EntryStride;
        const addr_t e = name_block + entry_off;
        mem.put<u16>(e, (u16)(s.size() << 6));        // { bIsWide:0, Len:10 }
        mem.putstr(e + 2, s.c_str());
        name_cursor = entry_off + 2 + (u32)s.size();
        if (name_cursor & 1) ++name_cursor;           // entries stay 2-byte aligned
        mem.put<u32>(pool + off::name_pool::CurrentByteCursor, name_cursor);
        return id;
    }

    // A UObject, registered in the array with its own index as InternalIndex.
    addr_t obj(u32 name_id, addr_t cls, std::size_t size) {
        const addr_t a = alloc(size);
        mem.put<addr_t>(a + off::uobject::VTable, mem.base + 0x800);   // inside the image
        mem.put<addr_t>(a + off::uobject::Class, cls);
        mem.put<u32>(a + off::uobject::Name, name_id);
        mem.put<u32>(a + off::uobject::InternalIndex, (u32)objects.size());
        objects.push_back(a);
        return a;
    }

    // An FProperty (FField) in a class's ChildProperties chain.
    addr_t prop(u32 name_id, i32 offset, addr_t next = 0) {
        // 0x80, not 0x50: FBoolProperty's ByteOffset/ByteMask sit past FProperty at +0x71/+0x72, and
        // a fake bool property has to be readable there for the bRecentlyRendered gate.
        const addr_t f = alloc(0x80);
        mem.put<addr_t>(f + off::ffield::Next, next);
        mem.put<u32>(f + off::ffield::NamePrivate, name_id);
        mem.put<i32>(f + off::fproperty::Offset_Internal, offset);
        return f;
    }

    struct CharRig { addr_t actor = 0, comp = 0, anim = 0, mov = 0; };

    // One character: an actor with a mesh component that carries an AnimScriptInstance, an anim
    // instance carrying bDead, and a movement component. Pass move_cls to give the movement
    // component a class that reflects a MovementMode property: that is the signal a latched death
    // is released by (EntityCache::Meta::dead_latch), and without it a character has none.
    CharRig make_char(addr_t actor_cls, u32 actor_name, addr_t comp_cls, addr_t anim_cls, double x,
                      addr_t move_cls = 0) {
        const u32 n_misc = intern("ComponentObj");
        CharRig r;
        r.actor = obj(actor_name, actor_cls, 0x400);
        r.comp  = obj(n_misc, comp_cls, 0xA00);
        r.anim  = obj(n_misc, anim_cls, 0x900);
        r.mov   = obj(n_misc, move_cls, 0x300);
        const addr_t root = obj(n_misc, 0, 0x1C0);
        mem.put<double>(root + off::uscene_component::RelativeLocation, x);
        mem.put<addr_t>(r.actor + off::aactor::RootComponent, root);
        mem.put<addr_t>(r.actor + off::acharacter::Mesh, r.comp);
        mem.put<addr_t>(r.actor + off::acharacter::CharacterMovement, r.mov);
        mem.put<addr_t>(r.comp + off::aactor::RootComponent, 0);
        mem.put<i32>(r.mov + off::ucharacter_movement::MovementMode, 0);
        return r;
    }

    // A UWorld with a persistent level and, optionally, streamed sub-levels. More than
    // kLevelsPerFrame (8) sub-levels make the sweep span several frames, which is what exposes a
    // world change that leaves the previous zone's streamed set published.
    void make_world(const std::vector<addr_t>& persistent,
                    const std::vector<std::vector<addr_t>>& subs) {
        const addr_t w = alloc(0x200);
        const addr_t l = alloc(0x100);
        fill_level(l, persistent);
        mem.put<addr_t>(w + off::uworld::PersistentLevel, l);
        if (!subs.empty()) {
            const addr_t lv = alloc(subs.size() * 8);
            for (std::size_t i = 0; i < subs.size(); ++i) {
                const addr_t sl = alloc(0x100);
                fill_level(sl, subs[i]);
                mem.put<addr_t>(lv + i * 8, sl);
            }
            mem.put<addr_t>(w + off::uworld::Levels, lv);
            mem.put<i32>(w + off::uworld::Levels + 8, (i32)subs.size());
            mem.put<i32>(w + off::uworld::Levels + 12, (i32)subs.size());
        }
        mem.put<addr_t>(gworld_cell, w);
    }

    void fill_level(addr_t l, const std::vector<addr_t>& actors) {
        const addr_t arr = alloc(actors.size() * 8 + 8);
        for (std::size_t i = 0; i < actors.size(); ++i) mem.put<addr_t>(arr + i * 8, actors[i]);
        mem.put<addr_t>(l + off::ulevel::Actors, arr);
        mem.put<i32>(l + off::ulevel::Actors + 8, (i32)actors.size());
        mem.put<i32>(l + off::ulevel::Actors + 12, (i32)actors.size());
    }

    // Publish the object array over everything created so far, then wire the Sdk to it.
    void finish() {
        const u32 n = (u32)objects.size();
        const addr_t c0 = alloc((std::size_t)n * off::uobject_item::Size);
        mem.put<addr_t>(chunk_table, c0);
        mem.put<u32>(gobjects + off::uobject_array::MaxElements, 0x10000);
        mem.put<u32>(gobjects + off::uobject_array::NumElements, n);
        mem.put<u32>(gobjects + off::uobject_array::NumChunks, 1);
        for (u32 i = 0; i < n; ++i) {
            mem.put<addr_t>(c0 + i * off::uobject_item::Size + off::uobject_item::Object, objects[i]);
            mem.put<u32>(c0 + i * off::uobject_item::Size + off::uobject_item::SerialNumber, 1);
        }
        sdk.mem = &mem;
        sdk.base = mem.base;
        sdk.gnames = pool;
        sdk.gobjects = gobjects;
        sdk.gworld = gworld_cell;
        sdk.pool.Init(&mem, pool, false);
        sdk.objects.Init(&mem, gobjects, false);
        sdk.ready = true;
    }
};

// The acceptance tests for the four audit bugs. Each forces the fault the fix is about.
static void CheckEntityCache() {
    printf("-- entity cache: dead signal, world reset, recycled address --\n");
    Config& cfg = Config::Instance();
    const bool save_npc = cfg.cat_npc, save_all = cfg.esp_show_all, save_dead = cfg.hide_dead;
    cfg.cat_npc = true;
    cfg.esp_show_all = false;
    cfg.hide_dead = true;                       // the default the bug fired under

    constexpr i32 kAnimOff = 0x988;             // fake "AnimScriptInstance" property offset
    constexpr i32 kDeadOff = 0x80C;             // fake "bDead" property offset

    // ---- BUG-1/2: the dead signal ------------------------------------------------------------
    {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_npc   = g.intern("BPCh_NpcBase_C");
        const u32 n_comp  = g.intern("SkeletalMeshComponent");
        const u32 n_anim  = g.intern("AionCharAnimInstance");
        const u32 n_move  = g.intern("AionCharMovementComponent");
        const u32 n_npcobj = g.intern("NPC_Test_1");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_npc  = g.obj(n_npc, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        const addr_t cls_move = g.obj(n_move, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), kAnimOff));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bDead"), kDeadOff));
        g.mem.put<addr_t>(cls_move + off::ustruct::ChildProperties,
                          g.prop(g.intern("MovementMode"),
                                 off::ucharacter_movement::MovementMode));
        const FakeGame::CharRig r =
            g.make_char(cls_npc, n_npcobj, cls_comp, cls_anim, 1000.0, cls_move);
        g.mem.put<addr_t>(r.comp + kAnimOff, r.anim);       // mesh -> AnimScriptInstance
        g.mem.put<u8>(r.anim + kDeadOff, 0);
        g.make_world({r.actor}, {});
        g.finish();

        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        const auto run = [&] { cache.UpdateBulk(&g.sdk, cam, proj, true, true); };

        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Name == "NPC_Test_1",
              "the character is collected and named through the synthetic graph");
        Check(!cache.Entities()[0].Dead, "bDead==0 -> not dead");

        // The signal the old code used, now resolved by name like bDead, and set to MOVE_None: on
        // its own it must still decide nothing. That is what the latch exists for - a live
        // character with no movement mode yet (a spawning or paused pawn) is not a corpse.
        g.mem.put<u8>(r.mov + off::ucharacter_movement::MovementMode, 0);   // MOVE_None
        run();
        Check(cache.Entities().size() == 1 && !cache.Entities()[0].Dead,
              "MovementMode==MOVE_None with bDead==0 no longer marks a live character dead");

        // Genuinely dead: confirmed bDead==1 hides it through the real renderer.
        g.mem.put<u8>(r.anim + kDeadOff, 1);
        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Dead, "bDead==1 -> Dead");
        {
            Recorder rec;
            EspRenderer().RenderEntities(rec, cam, cache.Entities(), cfg);
            Check(rec.lines.empty() && rec.rects.empty() && rec.texts == 0,
                  "hide_dead hides the CONFIRMED-dead actor");
        }

        // Forced read failure on the bDead byte: the value is UNKNOWN, so never hidden. The old
        // signal is deliberately left saying "dead" (MovementMode == MOVE_None, and bDead == 1 in
        // memory), so this fails under the old code and only the read-failure contract can pass it.
        g.mem.put<u8>(r.anim + kDeadOff, 1);
        g.mem.put<u8>(r.mov + off::ucharacter_movement::MovementMode, 0);
        g.mem.deny.push_back({r.anim + kDeadOff, r.anim + kDeadOff + 1});
        run();
        Check(cache.Entities().size() == 1 && !cache.Entities()[0].Dead,
              "a FAILED bDead read is unknown, not dead: the actor is not hidden");
        {
            Recorder rec;
            EspRenderer().RenderEntities(rec, cam, cache.Entities(), cfg);
            Check(!rec.lines.empty(), "...and it still draws");
        }
        g.mem.deny.clear();

        // The corpse, as measured live (2026-10-12): the client clears bDead when the death
        // animation ends, with the body still lying in the level - bDead 0, AActor::bHidden 1,
        // bRecentlyRendered still reading 1, MovementMode MOVE_None. Before the latch the box came
        // back on a dead monster right here, because nothing else on the actor says "corpse".
        g.mem.put<u8>(r.mov + off::ucharacter_movement::MovementMode, 0);   // MOVE_None
        g.mem.put<u8>(r.anim + kDeadOff, 1);
        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Dead,
              "a confirmed bDead==1 latches the death");
        g.mem.put<u8>(r.anim + kDeadOff, 0);            // the death animation has ended
        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Dead,
              "bDead cleared + MovementMode MOVE_None -> still Dead (the corpse keeps its latch)");
        {
            Recorder rec;
            EspRenderer().RenderEntities(rec, cam, cache.Entities(), cfg);
            Check(rec.lines.empty() && rec.rects.empty() && rec.texts == 0,
                  "...and the box stays gone after the client cleared bDead");
        }
        // Respawn or revive: the client puts the character back on its feet, which must release the
        // latch - or a respawned monster would never show again.
        g.mem.put<u8>(r.mov + off::ucharacter_movement::MovementMode, 1);   // MOVE_Walking
        run();
        Check(cache.Entities().size() == 1 && !cache.Entities()[0].Dead,
              "MovementMode MOVE_Walking releases the latch: the character is alive again");
        {
            Recorder rec;
            EspRenderer().RenderEntities(rec, cam, cache.Entities(), cfg);
            Check(!rec.lines.empty(), "...and it is drawn again");
        }
        // A character with no MovementMode to release it by is never latched: the fail-open
        // direction, so an unresolvable release signal can only ever show a corpse, never hide a
        // live actor. (The component here is the same object, left with no class to reflect.)
        g.mem.put<addr_t>(r.mov + off::uobject::Class, 0);
        g.mem.put<u8>(r.anim + kDeadOff, 1);
        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Dead,
              "bDead==1 is still dead on the frame it is read");
        g.mem.put<u8>(r.anim + kDeadOff, 0);
        run();
        Check(cache.Entities().size() == 1 && !cache.Entities()[0].Dead,
              "with no MovementMode resolvable the death is not held: nothing is hidden on the latch");
    }

    // ---- BUG-3: a world change must not draw the previous zone's actors ----------------------
    {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_npc   = g.intern("BPCh_NpcBase_C");
        const u32 n_comp  = g.intern("SkeletalMeshComponent");
        const u32 n_anim  = g.intern("AionCharAnimInstance");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_npc  = g.obj(n_npc, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), kAnimOff));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bDead"), kDeadOff));

        // Two zones, four distinct actors, all characters.
        const auto mk = [&](const char* nm, double x) {
            const FakeGame::CharRig r =
                g.make_char(cls_npc, g.intern(nm), cls_comp, cls_anim, x);
            g.mem.put<addr_t>(r.comp + kAnimOff, r.anim);
            return r;
        };
        const FakeGame::CharRig a1 = mk("ZoneA_Npc", 1000.0);
        const FakeGame::CharRig a2 = mk("ZoneA_Stream", 1200.0);
        const FakeGame::CharRig b1 = mk("ZoneB_Npc", 2000.0);
        const FakeGame::CharRig b2 = mk("ZoneB_Stream", 2200.0);
        // Nine sub-levels, the actor in the first: the sweep takes two frames, so the streamed
        // set stays published across a frame boundary - which is where a world change leaks it.
        const auto nine = [](const std::vector<addr_t>& first) {
            std::vector<std::vector<addr_t>> v(9);
            v[0] = first;
            return v;
        };
        g.make_world({a1.actor}, nine({a2.actor}));
        g.finish();

        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        const auto run = [&] { cache.UpdateBulk(&g.sdk, cam, proj, true, true); };
        const auto has = [&](const char* nm) {
            for (const auto& e : cache.Entities()) if (e.Name == nm) return true;
            return false;
        };

        run();
        run();   // the streamed sweep over nine levels completes on the second frame
        Check(has("ZoneA_Npc") && has("ZoneA_Stream"),
              "zone A: persistent + streamed actors are both listed once the sweep completes");

        // The world pointer moves to zone B. The very next frame must not contain zone A's actors.
        g.make_world({b1.actor}, nine({b2.actor}));
        run();
        Check(has("ZoneB_Npc"), "zone B: the new persistent actor is listed after the switch");
        Check(!has("ZoneA_Stream") && !has("ZoneA_Npc"),
              "zone A's actors are gone on the first frame after the world change (no ghosts)");
        run();
        Check(has("ZoneB_Stream"), "zone B's streamed actor is picked up");
    }

    // ---- BUG-4: a recycled address must not inherit the previous actor's metadata -------------
    {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_npc   = g.intern("BPCh_NpcBase_C");
        const u32 n_comp  = g.intern("SkeletalMeshComponent");
        const u32 n_anim  = g.intern("AionCharAnimInstance");
        const u32 n_old   = g.intern("OldName_Npc");
        const u32 n_new   = g.intern("NewName_Npc");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_npc  = g.obj(n_npc, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), kAnimOff));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bDead"), kDeadOff));
        const FakeGame::CharRig r = g.make_char(cls_npc, n_old, cls_comp, cls_anim, 1000.0);
        g.mem.put<addr_t>(r.comp + kAnimOff, r.anim);
        g.make_world({r.actor}, {});
        g.finish();

        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        const auto run = [&] { cache.UpdateBulk(&g.sdk, cam, proj, true, true); };

        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Name == "OldName_Npc",
              "the actor is named before recycling");

        // Recycle: the same address now backs a different object (a new name) whose array slot
        // carries a new serial. The cached identity must not survive.
        const i32 idx = (i32)g.mem.u32_at(r.actor + off::uobject::InternalIndex);
        const addr_t item = g.sdk.objects.ItemAt(idx);
        g.mem.put<u32>(r.actor + off::uobject::Name, n_new);
        g.mem.put<u32>(item + off::uobject_item::SerialNumber, 7);
        run();
        Check(cache.Entities().empty() || cache.Entities()[0].Name != "OldName_Npc",
              "the recycled address does not inherit the old name");
        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Name == "NewName_Npc",
              "the new object at the recycled address is re-read");
    }

    cfg.cat_npc = save_npc;
    cfg.esp_show_all = save_all;
    cfg.hide_dead = save_dead;
}

// A third-person camera on a spring arm: the pivot is the character plus (0,0,pivot_z), and the
// camera sits `boom` back along the view direction - so pitching the camera up drops it below and
// behind the character, which is the geometry the off-screen marker has to survive.
static Camera ThirdPersonCam(FVector chr, double pitch, double yaw, double boom, double pivot_z) {
    Camera c;
    const double d2r = 3.14159265358979323846 / 180.0;
    const double p = pitch * d2r, y = yaw * d2r;
    const double fwd[3] = {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    c.Location = {chr.X - fwd[0] * boom, chr.Y - fwd[1] * boom, chr.Z + pivot_z - fwd[2] * boom};
    c.Rotation = {pitch, yaw, 0.0};
    c.FOV = 90.f;
    c.valid = true;
    return c;
}

// Renders one projected entity with nothing but the off-screen marker switched on, so every segment
// that comes back is the marker and its tip can be asserted directly. `box` is the difference
// between "the marker drew" and "nothing drew".
static Recorder MarkerFor(const FVector& node, const Camera& cam, Config& cfg, bool box = false) {
    Projection proj;
    proj.SetViewport(1920, 1080);
    const Entity e = MakeEntity("Node", "AionSimpleCharacter", node, cam, proj);
    cfg.esp_box = box;
    cfg.esp_box_corners = false;
    cfg.snapline = SnaplineOff;
    cfg.esp_offscreen = true;
    cfg.esp_offscreen_snapline = false;
    cfg.esp_name = cfg.esp_class = cfg.esp_distance = cfg.esp_show_all = false;
    Recorder rec;
    EspRenderer().RenderEntities(rec, cam, std::vector<Entity>{e}, cfg);
    return rec;
}

static void CheckProjection() {
    printf("-- esp projection: off-screen marker direction, third person --\n");
    const float H = 1080.f;
    const float margin = 20.f;
    // The arrow's tip is the far end of the shaft, which is the first segment drawn.
    const auto tip_y = [](const Recorder& r) { return r.lines.empty() ? -1.f : r.lines[0].y1; };
    const auto points_down = [](const Recorder& r) {
        return !r.lines.empty() && r.lines[0].y0 < r.lines[0].y1;
    };

    {   // The reported case: a node below the player, the camera pitched up at the sky. The node
        // projects far below the frame, so the marker has to be on the bottom edge and point down.
        Config cfg;
        const Camera cam = ThirdPersonCam({0, 0, 0}, 50.0, 0.0, 400.0, 100.0);
        const Recorder rec = MarkerFor({0, 0, -300}, cam, cfg);
        Check(rec.lines.size() == 3, "a node below a skyward camera draws one arrow, not a box");
        Check(std::abs(tip_y(rec) - (H - margin)) < 1.f,
              "the arrow is on the BOTTOM edge, where the node actually projects");
        Check(points_down(rec), "and it points DOWN, toward the node");
    }
    {   // Same family, a steeper pitch: the camera has dropped lower, the node is still entirely
        // below the frame, and the answer must not move - bottom edge, pointing down.
        Config cfg;
        const Camera cam = ThirdPersonCam({0, 0, 0}, 60.0, 0.0, 400.0, 100.0);
        const Recorder rec = MarkerFor({0, 0, -250}, cam, cfg);
        Check(std::abs(tip_y(rec) - (H - margin)) < 1.f,
              "a steeper pitch keeps the arrow on the bottom edge");
        Check(points_down(rec), "and still pointing down");
    }
    {   // Control: the mirror image - a node above the frustum, camera pitched down.
        Config cfg;
        const Camera cam = ThirdPersonCam({0, 0, 0}, -20.0, 0.0, 400.0, 100.0);
        const Recorder rec = MarkerFor({0, 0, 1000}, cam, cfg);
        Check(std::abs(tip_y(rec) - margin) < 1.f, "a node above the view is marked on the TOP edge");
        Check(!rec.lines.empty() && rec.lines[0].y0 > rec.lines[0].y1, "pointing up, toward it");
    }
    {   // A node behind the camera has no projection at all; the yaw-plane bearing keeps that
        // case exactly as it was - bottom edge, pointing down.
        Config cfg;
        const Camera cam = ThirdPersonCam({0, 0, 0}, 0.0, 0.0, 400.0, 100.0);
        const Recorder rec = MarkerFor({-800, 0, 0}, cam, cfg);
        Check(std::abs(tip_y(rec) - (H - margin)) < 1.f,
              "a node behind the camera still marks the bottom edge");
        Check(points_down(rec), "and still points down, with no sign flip");
    }
    {   // A node off the frame sideways stays on the side it is on.
        Config cfg;
        const Camera cam = ThirdPersonCam({0, 0, 0}, 0.0, 0.0, 400.0, 100.0);
        const Recorder rec = MarkerFor({1000, -1500, 0}, cam, cfg);
        Check(!rec.lines.empty() && rec.lines[0].x1 < 1000.f,
              "a node off to the left is marked on the left");
    }
    {   // On screen: the box draws exactly as before, and no marker is added to it.
        Config cfg;
        const Camera cam = ThirdPersonCam({0, 0, 0}, 0.0, 0.0, 400.0, 100.0);
        const Recorder rec = MarkerFor({1000, 0, 0}, cam, cfg, /*box=*/true);
        Check(rec.rects.size() == 1 && rec.lines.empty(),
              "an on-screen node draws its box and no marker");
    }
}

// The client's own visibility flag, the client's own "this node is spent" flag, and the per-actor
// name when two monsters share one mesh. All three are consumer-visible - a box on a monster that
// has not appeared, a box on a node already gathered, two monsters wearing one name - so each one
// gets a test that fails under the behaviour it replaced.
static void CheckClientVisibility() {
    printf("-- client visibility (AActor::bHidden), spent nodes, shared-mesh names --\n");
    Config& cfg = Config::Instance();
    const bool save_npc = cfg.cat_npc, save_gather = cfg.cat_gather;
    const bool save_interact = cfg.cat_interact;
    const bool save_all = cfg.esp_show_all, save_hidden = cfg.esp_hidden;
    cfg.cat_npc = true;
    cfg.cat_gather = true;
    cfg.cat_interact = true;
    cfg.esp_show_all = false;
    cfg.esp_hidden = false;

    constexpr i32 kAnimOff = 0x988;      // fake "AnimScriptInstance" property offset
    constexpr i32 kInactiveOff = 0x629;  // fake "bInactiveCondition" property offset

    // ---- an actor the client has flagged as not rendered -------------------------------------
    {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_npc   = g.intern("BPCh_NpcBase_C");
        const u32 n_comp  = g.intern("SkeletalMeshComponent");
        const u32 n_anim  = g.intern("AionCharAnimInstance");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_npc  = g.obj(n_npc, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        // The mesh component carries bRecentlyRendered as well - the renderer's own flag, and the one
        // the hidden gate has to consult, because measured live bHidden is set on actors the client
        // is drawing.
        constexpr i32 kDrawnOff = 0x844;
        const addr_t drawn_prop = g.prop(g.intern("bRecentlyRendered"), kDrawnOff);
        g.mem.put<u8>(drawn_prop + off::fbool_property::ByteOffset, 0);
        g.mem.put<u8>(drawn_prop + off::fbool_property::ByteMask, 0x01);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), kAnimOff, drawn_prop));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bDead"), 0x80C));
        const FakeGame::CharRig shown =
            g.make_char(cls_npc, g.intern("Shown_Npc"), cls_comp, cls_anim, 1000.0);
        const FakeGame::CharRig unseen =
            g.make_char(cls_npc, g.intern("Unseen_Npc"), cls_comp, cls_anim, 1200.0);
        g.mem.put<addr_t>(shown.comp + kAnimOff, shown.anim);
        g.mem.put<addr_t>(unseen.comp + kAnimOff, unseen.anim);
        // The one flag the whole test is about: bit 7 of actor+0x078.
        g.mem.put<u8>(unseen.actor + off::aactor::HiddenFlags, off::aactor::HiddenBit);
        g.make_world({shown.actor, unseen.actor}, {});
        g.finish();

        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        const auto run = [&] { cache.UpdateBulk(&g.sdk, cam, proj, true, true); };

        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Name == "Shown_Npc",
              "an actor the client IS rendering is collected");
        Check(!cache.Entities().empty() && !cache.Entities()[0].ClientHidden,
              "...and reports ClientHidden == false");

        cfg.esp_hidden = true;
        run();
        bool unseen_flagged = false;
        for (const auto& e : cache.Entities())
            if (e.Name == "Unseen_Npc") unseen_flagged = e.ClientHidden;
        Check(cache.Entities().size() == 2 && unseen_flagged,
              "esp_hidden keeps the actor the client is not rendering, flagged ClientHidden");
        cfg.esp_hidden = false;
        run();
        Check(cache.Entities().size() == 1,
              "the default drops it again: no box on a monster that has not appeared");

        // ---- the correction: bHidden is not "the client is not rendering this" ----------------
        // Measured live with --dump-gates: an NPC at 34 m read bHidden=1 while the renderer's own
        // bRecentlyRendered read 1, and characters past 128 m read bHidden=0 with nothing drawn past
        // 45 m. So the byte alone dropped actors the game was showing on screen.
        g.mem.put<u8>(unseen.comp + kDrawnOff, 1);  // the renderer DID draw it this frame
        run();
        Check(cache.Entities().size() == 2,
              "bHidden set but the renderer drew it: the actor is collected, not dropped");
        bool still_flagged = false;
        for (const auto& e : cache.Entities())
            if (e.Name == "Unseen_Npc") still_flagged = e.ClientHidden;
        Check(!still_flagged, "...and it is not reported as ClientHidden");

        // Hidden and undrawn: the actor the gate exists for still drops.
        g.mem.put<u8>(unseen.comp + kDrawnOff, 0);
        run();
        Check(cache.Entities().size() == 1, "hidden AND undrawn is still dropped by default");

        // A failed read of the drawn byte is UNKNOWN, and unknown never hides a live actor.
        g.mem.put<u8>(unseen.comp + kDrawnOff, 1);
        g.mem.deny.push_back({unseen.comp + kDrawnOff, unseen.comp + kDrawnOff + 1});
        run();
        Check(cache.Entities().size() == 2,
              "a FAILED bRecentlyRendered read leaves the actor drawn");
        g.mem.deny.clear();
        run();
    }

    // ---- a node the client reports as spent: the actor stays in the level ---------------------
    {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_node  = g.intern("BP_EnvObj_Common_C");
        const u32 n_comp  = g.intern("SkeletalMeshComponent");
        const u32 n_anim  = g.intern("EnvObjAnimInstance");
        const u32 n_misc  = g.intern("NodeObj");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_node = g.obj(n_node, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), kAnimOff));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bInactiveCondition"), kInactiveOff));
        // A world object is not a character: its mesh hangs off AAionEnvObj::SkeletalMeshComponent
        // (0x430), not off ACharacter::Mesh (0x3C0), which is the whole trap here.
        const addr_t node = g.obj(g.intern("Gather_Node_1"), cls_node, 0x480);
        const addr_t comp = g.obj(n_misc, cls_comp, 0xA00);
        const addr_t anim = g.obj(n_misc, cls_anim, 0x900);
        const addr_t root = g.obj(n_misc, 0, 0x1C0);
        g.mem.put<double>(root + off::uscene_component::RelativeLocation, 300.0);
        g.mem.put<addr_t>(node + off::aactor::RootComponent, root);
        g.mem.put<addr_t>(node + off::aionenvobj::SkeletalMeshComponent, comp);
        g.mem.put<addr_t>(comp + kAnimOff, anim);
        g.mem.put<u8>(anim + kInactiveOff, 0);
        g.make_world({node}, {});
        g.finish();

        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        const auto run = [&] { cache.UpdateBulk(&g.sdk, cam, proj, true, true); };

        run();
        Check(cache.Entities().size() == 1, "an unspent gathering node is drawn");
        g.mem.put<u8>(anim + kInactiveOff, 1);
        run();
        Check(cache.Entities().empty(),
              "a node the client reports as spent is not drawn (it stays in the level's array)");
        // The same byte, made unreadable: UNKNOWN is not spent.
        g.mem.deny.push_back(
            {anim + (addr_t)kInactiveOff, anim + (addr_t)kInactiveOff + 1});
        run();
        Check(cache.Entities().size() == 1, "a FAILED spent read leaves the node drawn");
        g.mem.deny.clear();
    }

    // ---- two monsters on one mesh: the per-actor name is not vetoed by the model's label ------
    {
        FakeGame g;
        g.begin();
        const addr_t actor = g.alloc(0x200);
        const addr_t str = g.alloc(0x100);
        const std::string name = "Baby Canyon Airon";
        for (std::size_t i = 0; i < name.size(); ++i)
            g.mem.put<char16_t>(str + i * 2, (char16_t)(unsigned char)name[i]);
        const u32 off = 0x100;
        g.mem.put<addr_t>(actor + off, str);          // FString header: TCHAR*, length
        g.mem.put<i32>(actor + off + 8, (i32)name.size());

        NameTable t;
        Check(t.Add("MOB_Airon_01", "Canyon Airon"), "the first sighting names the model");
        Check(t.Find("MOB_Airon_01") && *t.Find("MOB_Airon_01") == "Canyon Airon",
              "...and the table resolves that model");
        t.SetDirect(off, NameFieldKind::FString);
        Check(t.DirectName(&g.mem, actor, "MOB_Airon_01") == name,
              "a per-actor name that DISAGREES with the model's label is kept, not refused");
        Check(t.Add("MOB_Airon_01", "Baby Canyon Airon"),
              "a second, different name on the same mesh is taken as a sighting");
        Check(t.Find("MOB_Airon_01") == nullptr,
              "two names on one mesh stop it being a name key, so each actor uses its own name");
        Check(t.DirectName(&g.mem, actor, "MOB_Airon_01") == name,
              "...and the per-actor field still names it");
    }

    // ---- the renderer: own colour, own range, and a marker for a box with no extent -----------
    {
        Camera cam;
        cam.valid = true;
        Config hcfg;                    // shipped defaults, with the switch on
        hcfg.esp_hidden = true;
        hcfg.snapline = SnaplineOff;
        hcfg.esp_name = false;
        hcfg.esp_class = false;
        hcfg.esp_distance = false;
        Entity seen;
        seen.Name = "Seen";
        seen.Type = EntityType::Monster;
        seen.OnScreen = true;
        seen.Distance = 500.0;
        seen.BoundsMin = {100.f, 200.f};
        seen.BoundsMax = {200.f, 400.f};
        Entity unseen = seen;
        unseen.ClientHidden = true;

        Recorder rs, rh;
        EspRenderer().RenderEntities(rs, cam, std::vector<Entity>{seen}, hcfg);
        EspRenderer().RenderEntities(rh, cam, std::vector<Entity>{unseen}, hcfg);
        Check(!rs.lines.empty() && !rh.lines.empty(), "esp_hidden draws an actor the client hides");
        const Color want = RgbColor(hcfg.color_hidden);
        const bool recoloured = !rs.line_colors.empty() && !rh.line_colors.empty() &&
                                std::abs(rh.line_colors[0].R - want.R) < 0.01f &&
                                std::abs(rh.line_colors[0].G - want.G) < 0.01f &&
                                std::abs(rh.line_colors[0].B - want.B) < 0.01f &&
                                std::abs(rs.line_colors[0].R - want.R) > 0.01f;
        Check(recoloured, "...in color_hidden, not the category colour");

        Entity distant = unseen;        // NB: not `far` - windows.h #defines far to nothing
        distant.Distance = 40000.0;     // 400 m: past max_distance_m, inside hidden_range_m
        Recorder rf;
        EspRenderer().RenderEntities(rf, cam, std::vector<Entity>{distant}, hcfg);
        Check(!rf.lines.empty(), "hidden_range_m reaches past max_distance_m for it");

        Config hoff = hcfg;
        hoff.esp_hidden = false;
        Recorder ro;
        EspRenderer().RenderEntities(ro, cam, std::vector<Entity>{unseen}, hoff);
        Check(ro.lines.empty() && ro.rects.empty() && ro.texts == 0,
              "with esp_hidden off the renderer draws nothing even if the cache kept it");

        // A box whose bounds did not resolve: one projected point fell behind the camera plane, so
        // the extent is empty. This used to be dropped outright, drawing nothing at all.
        Entity degen = seen;
        degen.BoundsMin = {640.f, 360.f};
        degen.BoundsMax = {640.f, 360.f};
        Config dcfg = hoff;
        dcfg.esp_offscreen = true;
        dcfg.offscreen_range_m = 60.0;
        Recorder rd;
        EspRenderer().RenderEntities(rd, cam, std::vector<Entity>{degen}, dcfg);
        Check(!rd.lines.empty(),
              "a box whose bounds failed to resolve becomes a border marker, not nothing");
        dcfg.esp_offscreen = false;
        Recorder rd2;
        EspRenderer().RenderEntities(rd2, cam, std::vector<Entity>{degen}, dcfg);
        Check(rd2.lines.empty() && rd2.rects.empty() && rd2.texts == 0,
              "...and nothing at all when off-screen markers are off");
    }

    cfg.cat_npc = save_npc;
    cfg.cat_gather = save_gather;
    cfg.cat_interact = save_interact;
    cfg.esp_show_all = save_all;
    cfg.esp_hidden = save_hidden;
}

}  // namespace

// Classification is substring-keyed off the class, object and mesh names, so a node the world
// identifies only through an asset family has to be pinned by hand. Everything checked here is a
// live identification: the string on the left is what the ESP's own class dump printed.
static void CheckClassification() {
    printf("-- classification: nodes pinned by asset family --\n");
    // A shell gatherable: class ShellSherry, displayed as "Conide". Neither string matched any
    // rule, so it fell through to Object - a category that is off by default, so the node was
    // never drawn at all.
    Check(ClassifyEntity("ShellSherry", "Conide") == EntityType::Gather,
          "class ShellSherry (displayed \"Conide\") is a gathering node");
    Check(ClassifyEntity("ShellSherry", "") == EntityType::Gather,
          "...and is one before its display name resolves");
    // The pin has to stay narrow: the family name, not the bare material word.
    Check(ClassifyEntity("ShellTurtle", "ShellTurtle") != EntityType::Gather,
          "the pin is the exact family, not the bare word \"Shell\"");
    // The rules the ESP actually classifies with (aion.ini lists included) must agree.
    EntityCache cache;
    Check(cache.ClassifyWithRules("ShellSherry", "Conide", "") == EntityType::Gather,
          "...and through the rule-aware path the cache uses");
    // The nodes pinned the same way keep working, class-keyed and mesh-keyed.
    Check(ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "ShellSherry") == EntityType::Gather,
          "...and the mesh route resolves it too");
    Check(ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "TreeBetua") == EntityType::Gather,
          "TreeBetua is still a gathering node");

    // The invisible-placeable marker is not a monster. It is the actor the client spawns for a
    // mechanism whose visuals come from a different actor - the abyss portal, trap and laser
    // emitters, skill shards - and having no mesh at all, a box on it marks nothing. One live zone
    // held 62 of them, every one drawn as a monster, which is the screen full of garbage they make.
    Check(ClassifyEntity("BPCh_NpcBase_C", "x", nullptr, "MOB_Invisible") == EntityType::Noise,
          "the invisible placeable marker is never a monster");
    // MOB_ is Aion's "mobile placeable" family, not a hostiles-only one, so the asset identifies a
    // monster only on a creature class. A prop carrying one has to reach the rules that read its
    // mesh, which is what actually says what it is.
    Check(ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "MOB_AbyssPortal_01") ==
              EntityType::Portal,
          "a portal carrying a MOB_ asset is a portal, not a monster");
    // ...and the real monster is untouched: the recorded in-world capture's monster pair.
    Check(ClassifyEntity("BPCh_NpcBase_C", "x", nullptr, "MOB_WindEle_01") == EntityType::Monster,
          "a creature carrying a MOB_ mesh is still a monster");
    Check(ClassifyEntity("BP_Pet_C", "x", nullptr, "MOB_DranaVar_03") == EntityType::Pet,
          "a pet carrying a MOB_ mesh is still a pet");

    // Containers are a category, not a dropped family: the client states whether one has been opened
    // (AionEnvObjAnimInstance::MainState), and the ones still holding loot are the reason to walk a
    // dungeon. This is the live pair read off the running client - the chest in a starting-area camp
    // is class BP_EnvObj_Common_C carrying the mesh RewardBox_02, and since that class is the shared
    // world-object class that nodes use too, the mesh is the only thing that identifies it.
    Check(ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "RewardBox_02") == EntityType::Chest,
          "the live chest pair (BP_EnvObj_Common_C + RewardBox_02) is a chest");
    Check(!AlwaysHidden("BP_EnvObj_Common_C", "RewardBox_02"),
          "...and the never-draw rule no longer takes it");
    Check(ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "LootBox_01") == EntityType::Chest &&
              ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "Treasure_Chest_A") ==
                  EntityType::Chest &&
              ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "Bag_03") == EntityType::Chest &&
              ClassifyEntity("BP_EnvObj_Common_C", "x", nullptr, "Pouch_Small") == EntityType::Chest &&
              ClassifyEntity("RewardChest_C", "x") == EntityType::Chest,
          "...and the rest of the container family is a chest, by mesh and by class");
    // The client's own act containers are mechanism actors: their class name matches "Container",
    // and claiming them would put boxes on machinery nobody can loot.
    Check(ClassifyEntity("AionMapEnvActContainerActor", "x") != EntityType::Chest,
          "the client's act containers are not chests");
    // The rule must not reach the things the ESP exists for: the nodes on that same shared class,
    // and the reward cubes, which are drawn on purpose and are exactly why "Box" and "Cube" are
    // deliberately not container patterns.
    Check(!AlwaysHidden("BP_EnvObj_Common_C", "OdSmall") &&
              !AlwaysHidden("BP_EnvObj_Common_C", "TreeBetua") &&
              !AlwaysHidden("BP_EnvObj_Common_C", "Food_Lv1_Vegetable_A"),
          "...while the gathering nodes on the same class keep drawing");
    Check(!AlwaysHidden("BP_EnvObj_Common_C", "OdEnergyCube") &&
              !AlwaysHidden("BP_EnvObj_Common_C", "HiddenCube"),
          "...and the reward cubes are not caught by it");

    // End to end, a container: the client's own MainState decides. An unopened chest is drawn as a
    // chest, the one the client reports as Finish is dropped, and a chest whose state cannot be read
    // stays visible - unknown is not "opened".
    {
        FakeGame g;
        g.begin();
        constexpr i32 kAnimOff = 0x988;       // fake "AnimScriptInstance" property offset
        constexpr i32 kMainStateOff = 0x620;  // fake "MainState" property offset
        const u32 n_class = g.intern("Class");
        const u32 n_comp = g.intern("SkeletalMeshComponent");
        const u32 n_anim = g.intern("EnvObjAnimInstance");
        const u32 n_misc = g.intern("BoxObj");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_box = g.obj(g.intern("RewardBox_C"), meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), kAnimOff));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("MainState"), kMainStateOff));
        // A container is a world object, like a node: its mesh hangs off
        // AAionEnvObj::SkeletalMeshComponent (not ACharacter::Mesh), and its state off that
        // component's anim instance.
        const addr_t box = g.obj(g.intern("RewardBox_1"), cls_box, 0x480);
        const addr_t comp = g.obj(n_misc, cls_comp, 0xA00);
        const addr_t anim = g.obj(n_misc, cls_anim, 0x900);
        const addr_t root = g.obj(n_misc, 0, 0x1C0);
        g.mem.put<double>(root + off::uscene_component::RelativeLocation, 300.0);
        g.mem.put<addr_t>(box + off::aactor::RootComponent, root);
        g.mem.put<addr_t>(box + off::aionenvobj::SkeletalMeshComponent, comp);
        g.mem.put<addr_t>(comp + kAnimOff, anim);
        g.mem.put<u8>(anim + kMainStateOff, off::aionenvobj_anim::MainStateAvailable);
        g.make_world({box}, {});
        g.finish();

        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        const auto run = [&] { cache.UpdateBulk(&g.sdk, cam, proj, true, true); };

        run();
        Check(cache.Entities().size() == 1 && cache.Entities()[0].Type == EntityType::Chest,
              "an unopened chest is drawn, as a chest");
        g.mem.put<u8>(anim + kMainStateOff, off::aionenvobj_anim::MainStateFinish);
        run();
        Check(cache.Entities().empty(), "a chest the client reports as opened is not drawn");
        g.mem.deny.push_back({anim + (addr_t)kMainStateOff, anim + (addr_t)kMainStateOff + 1});
        run();
        Check(cache.Entities().size() == 1, "a FAILED MainState read leaves the chest drawn");
        g.mem.deny.clear();
    }
}

// The debug panel is the only surface that reports the reader's own health, so its structure is
// pinned here: the five section headings, the one-word verdict in each state it can be in, the
// values it was handed, and - the failure mode this panel actually has - the background being tall
// enough for its rows. That last one is a row COUNT, so adding a row without bumping it overflows
// silently and looks fine until text runs off the panel.
static void CheckHud() {
    printf("-- debug HUD: sections, verdict, and a panel tall enough for its rows --\n");
    Config& cfg = Config::Instance();
    const bool save_debug = cfg.esp_debug;
    const bool save_auto = cfg.esp_font_auto;
    const int save_px = cfg.esp_font_px;
    cfg.esp_debug = true;
    // Pin the reference font: the panel's units are the default label size on a 1080p client, and
    // this test is about the layout, not about whatever the ambient config happens to hold.
    cfg.esp_font_auto = true;
    cfg.esp_font_px = 14;

    HudStats st;
    st.fps = 143.2;
    st.update_ms = 0.41;
    st.reproj_ms = 0.19;
    st.draw_ms = 0.62;
    st.flush_ms = 0.11;
    st.present_ms = 1.05;
    st.verts = 4211;
    st.listed = 3979;
    st.kept = 43;
    st.culled = 118;
    st.validated = 37;
    st.drawn = 36;
    st.handle_reopens = 0;
    st.stripped = 0;
    st.cam_valid = true;
    st.fov = 90.f;
    st.cam_x = 1;
    st.cam_y = 2;
    st.cam_z = 3;
    st.dcomp = true;
    st.width = 2560;
    st.height = 1440;
    st.display_hz = 165;

    const auto draw = [&](const HudStats& s) {
        Recorder rec;
        DrawHud(rec, cfg, s);
        return rec;
    };
    const auto has = [](const Recorder& r, const char* text) {
        for (const auto& t : r.texts_v)
            if (t.s == text) return true;
        return false;
    };

    Recorder rec = draw(st);
    Check(has(rec, "FRAME") && has(rec, "WORLD") && has(rec, "DEVICE") && has(rec, "CAMERA") &&
              has(rec, "READER"),
          "the panel is split into its five coloured sections");
    Check(has(rec, "OK"), "...with a healthy verdict on the title line");
    char padded[32];
    std::snprintf(padded, sizeof padded, "%7d", 4211);
    Check(has(rec, padded) && has(rec, "2560x1440") && has(rec, "fov 90.0"),
          "...and the values it was handed, not placeholders");
    Check(has(rec, "reopens") && has(rec, "stripped"),
          "...including both reader counters, the ones that answer 'is my handle alive'");

    // The verdict is the whole point of the title line, so every state it can report is checked.
    HudStats stripped = st;
    stripped.stripped = 1;
    Check(has(draw(stripped), "STRIPPED"), "a read strip turns the verdict into STRIPPED");
    HudStats nopawn = st;
    nopawn.cam_valid = false;
    Check(has(draw(nopawn), "NO CAMERA"),
          "no camera outranks everything: nothing on the panel means anything without it");
    HudStats slow = st;
    slow.update_ms = 9.0;
    Check(has(draw(slow), "OVER BUDGET"), "a frame past the display's budget says so");

    // The panel background is the largest fill; every row and the bar have to stay inside it.
    float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f, best = 0.f;
    for (const auto& f : rec.fills) {
        const float area = (f.x1 - f.x0) * (f.y1 - f.y0);
        if (area > best) {
            best = area;
            left = f.x0;
            top = f.y0;
            right = f.x1;
            bottom = f.y1;
        }
    }
    float lowest = 0.f;
    bool outside = false;
    for (const auto& t : rec.texts_v) {
        if (t.x < left - 1.f || t.x > right + 1.f || t.y < top - 1.f) outside = true;
        if (t.y > lowest) lowest = t.y;
    }
    const float line_h = rec.TextHeight(IOverlay::FontSize::Small);
    Check(best > 0.f && !outside && lowest + line_h <= bottom,
          "...and every row fits inside the panel background (the row count is in step)");

    // A 1440p client is where this panel came apart: esp_font_auto rasterizes the label font 1.33x
    // larger there, and the geometry used to stay fixed pixels - so the rows grew with the text and
    // the columns did not, the values ran into the notes and the notes off the panel's right edge.
    // Drawn at that size, no two labels on a row may touch and nothing may leave the background.
    {
        Recorder big;
        big.h_px = 1440;  // EspFontPx() then resolves the default 14 to 19, as the log shows there
        big.small_h = 19.f;
        DrawHud(big, cfg, st);

        float l2 = 0.f, r2 = 0.f, t2 = 0.f, b2 = 0.f, best2 = 0.f;
        for (const auto& f : big.fills) {
            const float area = (f.x1 - f.x0) * (f.y1 - f.y0);
            if (area > best2) {
                best2 = area;
                l2 = f.x0;
                t2 = f.y0;
                r2 = f.x1;
                b2 = f.y1;
            }
        }
        const float bh = big.TextHeight(IOverlay::FontSize::Small);
        bool inside = best2 > 0.f, overlap = false;
        for (std::size_t i = 0; i < big.texts_v.size(); ++i) {
            const auto& a = big.texts_v[i];
            const float aw = big.TextWidth(a.s, 1.f, IOverlay::FontSize::Small);
            if (a.x < l2 - 0.5f || a.y < t2 - 0.5f || a.x + aw > r2 + 0.5f ||
                a.y + bh > b2 + 0.5f)
                inside = false;
            for (std::size_t j = i + 1; j < big.texts_v.size(); ++j) {
                const auto& b = big.texts_v[j];
                if (std::abs(a.y - b.y) > 0.5f) continue;
                const float bw = big.TextWidth(b.s, 1.f, IOverlay::FontSize::Small);
                if (a.x < b.x + bw - 0.5f && b.x < a.x + aw - 0.5f) overlap = true;
            }
        }
        Check(inside && !overlap,
              "at a 1440p client no row overlaps and nothing leaves the panel");
    }

    cfg.esp_debug = save_debug;
    cfg.esp_font_auto = save_auto;
    cfg.esp_font_px = save_px;
}

// The handle counters are the only on-screen evidence that a handle was stripped, so the baseline
// has to be right: Attach opens the query handle and the read handle is created lazily by the first
// read, which used to be counted as a re-open - so the HUD read "reopens 1" and stayed amber for the
// whole session, hiding the one event the row exists to show. Self-attached: needs no game running.
static void CheckReader() {
    printf("-- reader handles: re-open counter baseline --\n");
    ExternalMemory mem;
    if (!mem.Attach(GetCurrentProcessId(), false)) {
        printf("  [SKIP] could not open our own process\n");
        return;
    }
    int probe = 0x12345678, back = 0;
    Check(mem.read(reinterpret_cast<addr_t>(&probe), &back, sizeof back) && back == probe,
          "the reader reads a live process through the VM_READ handle");
    Check(mem.reopen_count() == 0,
          "...and that first open is acquisition, so reopens starts at 0, not 1");
    Check(mem.stripped_count() == 0, "...with nothing counted as stripped");

    back = 0;
    Check(mem.read(reinterpret_cast<addr_t>(&probe), &back, sizeof back) && back == probe,
          "a second read reuses the handle");
    Check(mem.reopen_count() == 0 && mem.stripped_count() == 0,
          "...and neither counter moves while the handle is alive");

    // Only a dead handle is a strip: an unmapped address fails, and must fail WITHOUT being
    // reported as the anti-cheat (ERROR_PARTIAL_COPY, not INVALID_HANDLE/ACCESS_DENIED).
    Check(!mem.read(static_cast<addr_t>(1), &back, sizeof back), "an unmapped read fails");
    Check(mem.stripped_count() == 0 && mem.reopen_count() == 0,
          "...and a failed read on a live handle is not counted as a strip");
}

// The shuttering report: an entity that should draw as ONE box, drawing as TWO while the camera
// moves. Every candidate cause is forced here, so the acceptance is checked rather than asserted:
//
//   H1  the same actor listed twice          - a duplicate address must be one entity, not two
//   H2  one entity emitting two visuals      - the draw path's primitive count per entity, over a
//                                              camera sweep: one box, in every frame
//   H3  geometry built against a stale camera - reprojection must replace the box, never leave it
//   H4  two overlay instances                - refused, because two instances are two
//                                              independently-timed copies of the same boxes: they
//                                              coincide while the camera is still and split the
//                                              moment it moves, which is exactly the symptom
static void CheckShuttering() {
    printf("-- shuttering: one entity, one visual --\n");

    // ---- H2: one box, over a camera sweep, for a stationary entity --------------------------
    {
        Config cfg;  // shipped defaults: corner brackets, a 1 px stroke
        cfg.snapline = SnaplineOff;   // the box alone, so the count is unambiguous
        cfg.esp_name = cfg.esp_class = cfg.esp_distance = false;
        cfg.esp_offscreen = false;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Recorder rec;
        int wrong_frames = 0, reversals = 0, steps = 0, dir = 0;
        double prev_cx = 0.0, prev_h = 0.0, max_h_step = 0.0;
        for (int step = -25; step <= 25; ++step) {
            Camera cam;
            cam.Location = {0, 0, 0};
            cam.Rotation = {0, step * 1.0, 0};   // FRotator is {Pitch, Yaw, Roll}: yaw here
            cam.FOV = 90.f;
            cam.valid = true;
            const Entity e = MakeEntity("Sweep", "AionSimpleCharacter", {2000, 0, 0}, cam, proj);
            rec.lines.clear();
            rec.rects.clear();
            rec.texts = 0;
            EspRenderer().RenderEntities(rec, cam, std::vector<Entity>{e}, cfg);
            // One box in corner mode is exactly 8 strokes and no closed rectangle. 16 strokes is
            // two boxes; a rect beside them is the "border AND box" case.
            if (rec.lines.size() != 8 || !rec.rects.empty()) ++wrong_frames;
            const double cx = (e.BoundsMin.X + e.BoundsMax.X) * 0.5;
            const double h = e.BoundsMax.Y - e.BoundsMin.Y;
            if (steps > 0) {
                // The box must travel one way across the sweep. A reversal is the camera's motion
                // and the geometry's disagreeing about the direction - what a stale projection
                // drawn beside a fresh one looks like.
                const double d = cx - prev_cx;
                if (std::abs(d) > 0.3) {
                    const int s = d > 0.0 ? 1 : -1;
                    if (dir != 0 && s != dir) ++reversals;
                    dir = s;
                }
                const double hd = std::abs(h - prev_h);
                if (hd > max_h_step) max_h_step = hd;
            }
            prev_cx = cx;
            prev_h = h;
            ++steps;
        }
        Check(wrong_frames == 0,
              "camera sweep over a still entity: every frame is exactly one box, never two");
        Check(reversals == 0, "the box moves with the camera in one direction, never back and forth");
        Check(max_h_step < 20.0,
              "the box's height changes smoothly across the sweep (no split by a stale projection)");
    }

    // ---- H3: the drawn box is the newest camera's, and a stale one is never left behind -------
    {
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera a, b;
        a.Location = b.Location = {0, 0, 0};
        a.Rotation = {0, 0, 0};
        b.Rotation = {0, 0, 6.0};   // the camera turned between the transform read and the draw
        a.FOV = b.FOV = 90.f;
        a.valid = b.valid = true;
        Entity e = MakeEntity("Sweep", "AionSimpleCharacter", {2000, 0, 0}, a, proj);
        const Entity fresh = MakeEntity("Sweep", "AionSimpleCharacter", {2000, 0, 0}, b, proj);
        EntityCache cache;
        cache.ProjectEntity(e, b, proj);   // the frame loop's Reproject
        const bool same = std::abs(e.BoundsMin.X - fresh.BoundsMin.X) < 0.01f &&
                          std::abs(e.BoundsMin.Y - fresh.BoundsMin.Y) < 0.01f &&
                          std::abs(e.BoundsMax.X - fresh.BoundsMax.X) < 0.01f &&
                          std::abs(e.BoundsMax.Y - fresh.BoundsMax.Y) < 0.01f;
        Check(same, "reprojection replaces the box with the newest camera's (no stale half)");

        // The same entity, now behind the camera: the previous frame's box must not survive it.
        Camera back;
        back.Location = {0, 0, 0};
        back.Rotation = {0, 180.0, 0};   // {Pitch, Yaw, Roll}: turned to face the other way
        back.FOV = 90.f;
        back.valid = true;
        cache.ProjectEntity(e, back, proj);
        Check(!e.OnScreen && e.BoundsMin.X == 0.f && e.BoundsMax.X == 0.f &&
                  e.BoundsMin.Y == 0.f && e.BoundsMax.Y == 0.f,
              "an entity that goes behind the camera loses its box (no ghost box left at the old "
              "position)");
    }

    // ---- H1: one actor, one entity, however many level lists name it --------------------------
    {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_npc = g.intern("BPCh_NpcBase_C");
        const u32 n_comp = g.intern("SkeletalMeshComponent");
        const u32 n_anim = g.intern("AionCharAnimInstance");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_npc = g.obj(n_npc, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), 0x988));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bDead"), 0x80C));
        const FakeGame::CharRig r = g.make_char(cls_npc, g.intern("Twice_NPC"), cls_comp,
                                                cls_anim, 1000.0);
        g.mem.put<addr_t>(r.comp + 0x988, r.anim);
        // The same actor named by the persistent level AND by a streaming level: two actor arrays,
        // one actor.
        g.make_world({r.actor}, {{r.actor}});
        g.finish();

        Config cfg;
        cfg.cat_npc = true;
        cfg.snapline = SnaplineOff;
        cfg.esp_name = cfg.esp_class = cfg.esp_distance = false;
        // The cache reads the process-wide config for its category cull (NPCs are off by default),
        // so the synthetic NPC has to be enabled there, not only in the renderer's copy.
        Config& gcfg = Config::Instance();
        const bool save_npc = gcfg.cat_npc;
        gcfg.cat_npc = true;
        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        cache.UpdateBulk(&g.sdk, cam, proj, true, true);
        cache.UpdateBulk(&g.sdk, cam, proj, true, true);
        int copies = 0;
        for (const auto& e : cache.Entities())
            if (e.Addr == r.actor) ++copies;
        Check(copies == 1,
              "an actor named by two level lists is ONE entity (one box), not two");
        Recorder rec;
        EspRenderer().RenderEntities(rec, cam, cache.Entities(), cfg);
        Check(rec.lines.size() == 8, "...and the frame carries one box for it, not two");
        gcfg.cat_npc = save_npc;
    }

    // ---- H4: a second overlay instance would draw the same boxes twice -------------------------
    {
        // `OverlayInstanceRunning()` is machine-wide, and the user's own session may well be up while
        // this runs, so the check is asserted against the state the machine had before the test
        // touched it rather than against "nothing is running".
        const bool other = app::OverlayInstanceRunning();
        HWND first = app::CreateOverlayWindow(nullptr, /*exclusive=*/false);
        Check(first != nullptr, "a headless surface is created even while another overlay runs");
        Check(app::OverlayInstanceRunning(), "an already-running overlay is detectable");
        HWND second = app::CreateOverlayWindow(nullptr, /*exclusive=*/true);
        Check(second == nullptr,
              "a second INTERACTIVE overlay is refused: two instances draw every box twice while "
              "the camera moves");
        if (first) DestroyWindow(first);
        Check(app::OverlayInstanceRunning() == other,
              "...and the machine-wide check is exactly as it was before the test");
    }
}

// The local player's own HP/MP: where the value comes from (the client's own HUD bar, resolved by
// property name - there is no HP field on any gameplay object), and what happens when it cannot be
// read. The one forbidden outcome is a plausible 0% for "I do not know".
static void CheckSelfVitals() {
    printf("-- self HP/MP: the client's own HUD bar --\n");
    // The synthetic graph the reader walks: a class named for the local HUD with the two bar
    // properties, one instance of it, and progress bars carrying Percent.
    constexpr i32 kHp = 0x668, kMp = 0x688, kPct = 0x510;
    {
        FakeGame g;
        g.begin();
        const addr_t meta = g.obj(g.intern("Class"), 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_hud = g.obj(g.intern("AionWidget_HUD_UserInfo"), meta, 0x800);
        const addr_t cls_bar = g.obj(g.intern("AionProgressBar"), meta, 0x800);
        g.mem.put<addr_t>(cls_hud + off::ustruct::ChildProperties,
                          g.prop(g.intern("P_PBar_Hp"), kHp,
                                 g.prop(g.intern("P_PBar_Mp"), kMp)));
        g.mem.put<addr_t>(cls_bar + off::ustruct::ChildProperties, g.prop(g.intern("Percent"), kPct));
        const addr_t hud = g.obj(g.intern("AionWidget_HUD_UserInfo_0"), cls_hud, 0x800);
        const addr_t hp_bar = g.obj(g.intern("P_PBar_Hp_0"), cls_bar, 0x600);
        const addr_t mp_bar = g.obj(g.intern("P_PBar_Mp_0"), cls_bar, 0x600);
        g.mem.put<addr_t>(hud + kHp, hp_bar);
        g.mem.put<addr_t>(hud + kMp, mp_bar);
        g.mem.put<float>(hp_bar + kPct, 0.42f);
        g.mem.put<float>(mp_bar + kPct, 0.75f);
        g.finish();

        SelfVitalsReader reader;
        const SelfVitals v = reader.Read(&g.sdk);
        Check(v.found && v.hp_live && v.mp_live && std::abs(v.hp - 0.42f) < 1e-5f &&
                  std::abs(v.mp - 0.75f) < 1e-5f,
              "HP and MP are read through the HUD widget, by property name, no anchored offset");

        // A bar whose value is outside 0..1 is not a percentage: unknown, not clamped into a lie.
        g.mem.put<float>(hp_bar + kPct, 4.2f);
        const SelfVitals bad = reader.Read(&g.sdk);
        Check(bad.hp < 0.f && bad.mp_live, "a bar outside 0..1 is unknown, not reported as 100%+");
    }

    // The HUD is not loaded (or the class is not in the table): unknown, and the panel must say so.
    {
        FakeGame g;
        g.begin();
        const addr_t meta = g.obj(g.intern("Class"), 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        g.finish();
        SelfVitalsReader reader;
        const SelfVitals v = reader.Read(&g.sdk);
        Check(!v.found && v.hp < 0.f && v.mp < 0.f, "no HUD widget: HP and MP are unknown, not 0");
        Recorder rec;
        Config cfg;
        cfg.self_vitals = true;
        SelfBarsRenderer().Render(rec, cfg, v);
        bool unknown = false, zero = false;
        for (const auto& t : rec.texts_v) {
            if (t.s.find("unknown") != std::string::npos) unknown = true;
            if (t.s.find("0%") != std::string::npos) zero = true;
        }
        Check(unknown && !zero, "the panel says '[unknown]' and never '0%'");
        Check(rec.fills.size() >= 1 && rec.rects.size() == 1,
              "...the panel is drawn and no bar frame is, so a state never reads as an empty bar");
    }

    // A value read on an earlier frame and not on this one: shown, marked as last-known.
    {
        Recorder rec;
        Config cfg;
        cfg.self_vitals = true;
        SelfVitals stale;
        stale.hp = 0.42f;
        stale.mp = 0.75f;
        stale.hp_live = stale.mp_live = false;
        stale.found = true;
        SelfBarsRenderer().Render(rec, cfg, stale);
        bool q = false, plain = false;
        for (const auto& t : rec.texts_v) {
            if (t.s.find("42%?") != std::string::npos) q = true;
            if (t.s.find("75%") != std::string::npos && t.s.find("75%?") == std::string::npos)
                plain = true;
        }
        Check(q && !plain, "a value from an earlier frame reads '42%?' - last-known, marked as such");
    }

    // The readable bit, and the honest MP row: the client's own low-health warning says LOW or ok,
    // a bit that could not be read says unknown, and MP says it is not exposed rather than drawing an
    // empty bar that would read as zero.
    {
        Recorder rec;
        Config cfg;
        cfg.self_vitals = true;
        SelfVitals v;
        v.found = true;
        v.hp_low_known = true;
        v.hp_low = true;
        SelfBarsRenderer().Render(rec, cfg, v);
        bool low = false, exposed = false;
        for (const auto& t : rec.texts_v) {
            if (t.s == "[LOW]") low = true;
            if (t.s.find("not exposed") != std::string::npos) exposed = true;
        }
        Check(low && exposed,
              "the panel shows the client's own LOW warning, and says MP is not exposed");

        v.hp_low = false;
        rec.texts_v.clear();
        SelfBarsRenderer().Render(rec, cfg, v);
        bool ok_bad = false;
        for (const auto& t : rec.texts_v)
            if (t.s == "[ok]") ok_bad = true;
        Check(ok_bad, "...and ok when that bit is clear");

        v.hp_low_known = false;
        rec.texts_v.clear();
        SelfBarsRenderer().Render(rec, cfg, v);
        bool unknown = false;
        for (const auto& t : rec.texts_v)
            if (t.s == "[unknown]") unknown = true;
        Check(unknown, "...and unknown when the bit could not be read (never 0, never ok)");
    }
}

// The entity pass must cost what the ESP DRAWS, not what the level happens to list. This is the
// regression test for the frame that read "over budget" in the debug HUD: the identity check ran once
// per listed actor per frame, which on the live client is ~8000 reads for a handful of drawn actors.
// It counts the reads, so the claim is measured rather than asserted.
static void CheckBulkCost() {
    printf("-- entity pass: reads scale with drawn actors, not listed ones --\n");
    const auto reads_for = [](int count) -> std::size_t {
        FakeGame g;
        g.begin();
        const u32 n_class = g.intern("Class");
        const u32 n_npc = g.intern("BPCh_NpcBase_C");
        const u32 n_comp = g.intern("SkeletalMeshComponent");
        const u32 n_anim = g.intern("AionCharAnimInstance");
        const addr_t meta = g.obj(n_class, 0, 0x400);
        g.mem.put<addr_t>(meta + off::uobject::Class, meta);
        const addr_t cls_npc = g.obj(n_npc, meta, 0x400);
        const addr_t cls_comp = g.obj(n_comp, meta, 0x400);
        const addr_t cls_anim = g.obj(n_anim, meta, 0x400);
        g.mem.put<addr_t>(cls_comp + off::ustruct::ChildProperties,
                          g.prop(g.intern("AnimScriptInstance"), 0x988));
        g.mem.put<addr_t>(cls_anim + off::ustruct::ChildProperties,
                          g.prop(g.intern("bDead"), 0x80C));
        std::vector<addr_t> actors;
        for (int i = 0; i < count; ++i) {
            const std::string nm = "Cull_Npc_" + std::to_string(i);
            const FakeGame::CharRig r =
                g.make_char(cls_npc, g.intern(nm.c_str()), cls_comp, cls_anim, 1000.0 + i);
            g.mem.put<addr_t>(r.comp + 0x988, r.anim);
            actors.push_back(r.actor);
        }
        g.make_world(actors, {});
        g.finish();

        Config& cfg = Config::Instance();
        const bool save_npc = cfg.cat_npc;
        cfg.cat_npc = false;  // every one of them is listed, then culled before it is drawn
        EntityCache cache;
        Projection proj;
        proj.SetViewport(1920, 1080);
        Camera cam;
        cam.Location = {0, 0, 0};
        cam.Rotation = {0, 0, 0};
        cam.FOV = 90.f;
        cam.valid = true;
        // Let the identity cache settle first: validation itself is budgeted at 48 actors a frame, so
        // the measured frame must be one where nothing is being re-read for validation.
        for (int f = 0; f < 12; ++f) cache.UpdateBulk(&g.sdk, cam, proj, true, true);
        g.mem.reads = 0;
        cache.UpdateBulk(&g.sdk, cam, proj, true, true);
        const std::size_t reads = g.mem.reads;
        cfg.cat_npc = save_npc;
        return reads;
    };
    const std::size_t few = reads_for(40);
    const std::size_t many = reads_for(400);
    printf("      40 listed actors: %zu read(s)   400 listed actors: %zu read(s)\n", few, many);
    Check(many <= few + 20,
          "400 actors the ESP does not draw cost no more per frame than 40 (no per-actor read for "
          "what is culled)");
}

int main() {
    CheckLabels();
    // Unbuffered: when this harness crashes, the last line printed has to be the last thing that ran,
    // not whatever filled the stdio buffer before it.
    setvbuf(stdout, nullptr, _IONBF, 0);
    CheckConfig();
    CheckDefaultIni();
    CheckListFiles();
    CheckGeometry();
    CheckFontScale();
    CheckResolutions();
    CheckEntityCache();
    CheckShuttering();
    CheckBulkCost();
    CheckSelfVitals();
    CheckClientVisibility();
    CheckProjection();
    CheckClassification();
    CheckHud();
    CheckReader();

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AionOverlayTest";
    RegisterClassA(&wc);
    // Borderless, like the overlay's own window: the layered present sizes the window to the surface
    // it hands DWM while ImGui lays out against the client area, so a framed window here would make
    // the two disagree about how big the surface is.
    HWND hwnd = CreateWindowExA(0, "AionOverlayTest", "overlay test", WS_POPUP,
                                0, 0, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("window failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    D3D11Overlay overlay;
    // The dirty-region checks below are about the UpdateLayeredWindow path: it hands the compositor
    // only the regions marked dirty, where the composition path presents the whole surface. The
    // capture is the same either way - both save the surface the frame was drawn into.
    overlay.set_use_dcomp(false);
    // And no present rate cap: the checks below ask whether a frame's content reached the
    // compositor, and a throttled frame answers that question with "no" for the wrong reason.
    overlay.set_present_fps(0);
    if (!overlay.InitWindow(hwnd)) { printf("overlay init failed\n"); return 2; }
    // The menu is ImGui and draws through this layer on the overlay's own device. Rendering it here
    // is the only way to see the menu without the game running, which is why this test captures it.
    ImGuiLayer imgui;
    const bool imgui_ok = imgui.Init(hwnd, overlay);
    // The fonts are rebuilt below, once the config that names them exists.
    Check(imgui_ok, "the ImGui layer initialises on the overlay's device");
    {
        // Shipped defaults, checked through a fresh Config so a file loaded later cannot hide them.
        Config fresh;
        // The stroke is feathered, so a sub-pixel default draws that fraction of the colour: the old
        // 0.20 default made the boxes and the rings read as invisible on a bright screen. The default
        // has to be a width that actually shows.
        Check(fresh.esp_thickness >= 1.0,
              "the default line width is a solid pixel, not a sub-pixel hairline");
        Check(fresh.snapline == SnaplinePlayer, "the default snapline origin is the character");
    }
    printf("overlay ready=%d %dx%d\n", (int)overlay.Ready(), overlay.Width(), overlay.Height());

    Camera cam;
    cam.Location = {0, 0, 0};
    cam.Rotation = {0, 0, 0};  // forward = +X
    cam.FOV = 90.f;
    cam.valid = true;

    Projection proj;
    proj.SetViewport(overlay.Width(), overlay.Height());

    std::vector<Entity> ents;
    ents.push_back(MakeEntity("Target_A", "AionSimpleCharacter", {1000, 0, 0}, cam, proj));
    ents.push_back(MakeEntity("Target_B", "BPCh_NpcBase_C", {1500, -300, 120}, cam, proj));
    ents.push_back(MakeEntity("Target_C", "BPCh_PcBase_C", {800, 320, -60}, cam, proj));
    ents.push_back(MakeEntity("Target_D", "BP_Pet_C", {2500, 100, 40}, cam, proj));
    ents.push_back(MakeEntity("Behind", "Actor", {-500, 0, 0}, cam, proj));

    Config& cfg = Config::Instance();
    cfg.snapline = SnaplineBottom;
    cfg.esp_class = true;
    cfg.esp_debug = true;

    Menu menu(&cfg);
    // Fonts from the same config the menu reads, so the capture matches what the game shows.
    if (imgui_ok) imgui.RebuildFonts(cfg.font_face, cfg.menu_font_px);
    menu.Toggle();
    menu.SetStatus("dump classes: 42 classes, 118 actors");
    // The session column is drawn under every pane, so the capture has to have it filled the way the
    // host fills it - otherwise the layout it verifies is not the layout that ships.
    {
        MenuSession s;
        s.game = "AION2";
        s.process = "AION2.exe";
        s.pid = 4242;
        s.module = "0x140000000";
        s.image = "149.4 MB, verified";
        s.elevated = true;
        s.attached = true;
        menu.set_session(s);
    }

    // The menu is the second pass into the same surface as the ESP. The UpdateLayeredWindow path
    // presents the union of the regions marked dirty during the frame, so a pass that marks nothing
    // is invisible wherever the ESP did not draw, is never repainted once it stops drawing (a menu
    // that will not close), and comes and goes with whatever the ESP happens to draw (a menu that
    // vanishes when a category is toggled). These two frames are before anything has been presented,
    // so "presented" answers exactly whether the frame's content claimed an area.
    if (imgui_ok) {
        overlay.BeginFrame();
        overlay.EndFrame();
        Check(!overlay.stats().presented, "a frame with nothing drawn is not presented");

        // Two frames, and either one passing is enough: a window's first frame in a fresh context can
        // legitimately be a measuring frame that draws nothing, and this check is about the menu
        // claiming its own region - not about how many frames a window needs to appear. With the
        // menu's region unclaimed both frames stay empty and the check still fails, which is the
        // property it exists for.
        bool presented = false;
        for (int n = 0; n < 2; ++n) {
            overlay.BeginFrame();
            imgui.BeginFrame();
            menu.BuildImGui(imgui);
            overlay.EndFrame();
            presented = presented || overlay.stats().presented;
        }
        Check(presented,
              "a frame that draws only the menu reaches the compositor (the menu claims its region)");
    }

    // The panel shows its own live numbers under the game, so feed it impossible ones here:
    // if the panel ever renders a placeholder instead of what it was given, that shows.
    HudStats hud;
    hud.fps = 412.0;
    hud.update_ms = 3.42;
    hud.reproj_ms = 0.18;
    hud.draw_ms = 1.06;
    hud.flush_ms = 0.21;
    hud.present_ms = 0.34;
    hud.verts = 18422;
    hud.listed = 1183;
    hud.kept = 214;
    hud.culled = 921;
    hud.validated = 48;
    hud.drawn = 36;
    hud.handle_reopens = 2;
    hud.cam_valid = true;
    hud.fov = 90.f;
    hud.cam_x = 0;
    hud.cam_y = 0;
    hud.cam_z = 0;
    hud.dcomp = true;
    hud.width = overlay.Width();
    hud.height = overlay.Height();
    hud.display_hz = 165;

    // Pump a few frames so the driver settles, then capture one.
    MSG msg;
    for (int frame = 0; frame < 3; ++frame) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        overlay.BeginFrame();
        EspRenderer().RenderEntities(overlay, cam, ents, cfg);
        if (imgui_ok) {
            // The same numbers the old panel showed, handed to the same pane that shows them live.
            MenuStats ms;
            ms.fps = hud.fps;
            ms.update_ms = hud.update_ms;
            ms.reproj_ms = hud.reproj_ms;
            ms.draw_ms = hud.draw_ms;
            ms.flush_ms = hud.flush_ms;
            ms.present_ms = hud.present_ms;
            ms.verts = hud.verts;
            ms.listed = (int)hud.listed;
            ms.kept = (int)hud.kept;
            ms.culled = (int)hud.culled;
            ms.validated = (int)hud.validated;
            ms.drawn = hud.drawn;
            ms.handle_reopens = hud.handle_reopens;
            ms.cam_valid = hud.cam_valid;
            ms.dcomp = hud.dcomp;
            ms.width = hud.width;
            ms.height = hud.height;
            ms.display_hz = hud.display_hz;
            ms.fov = (float)hud.fov;
            ms.cam_x = (float)hud.cam_x;
            ms.cam_y = (float)hud.cam_y;
            ms.cam_z = (float)hud.cam_z;
            ms.names_learned = 128;
            ms.names_forced = 3;
            ms.names_hidden = 1;
            menu.set_stats(ms);
            imgui.BeginFrame();
            menu.BuildImGui(imgui);
        }
        DrawHud(overlay, cfg, hud);
        overlay.EndFrame();
        if (frame == 2) {
            bool ok = overlay.SaveBackbufferToBmp("overlay_test.bmp");
            printf("capture saved=%d\n", (int)ok);
            Check(ok, "overlay_test.bmp written");
        }
    }

    // Sanity: print projections.
    for (const auto& e : ents) {
        printf("  %-10s d=%8.1f on=%d scr=(%.1f,%.1f)\n", e.Name.c_str(), e.Distance,
               (int)e.OnScreen, e.Screen.X, e.Screen.Y);
    }
    overlay.Shutdown();
    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK", g_fails, g_fails == 1 ? "" : "s");
    return g_fails ? 1 : 0;
}
