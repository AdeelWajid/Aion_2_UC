// src/app/Modes.cpp — see Modes.hpp.
//
// Every block in this file is the original one from src/external_main.cpp, moved. The statements,
// their order and their comments are unchanged; the only edit is that the command-line options the
// blocks read are qualified with `opt.` now that they live in Options, and that each block's names
// are bound to the context it is handed.
#include "Modes.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Client.hpp"     // DumpActorClasses
#include "Names.hpp"      // FindNameText, HarvestPlates
#include "aion/Aion.hpp"
#include "aion/Camera.hpp"
#include "aion/Config.hpp"
#include "aion/DataFile.hpp"
#include "aion/DataTable.hpp"
#include "aion/EntityCache.hpp"
#include "aion/Logger.hpp"
#include "aion/Memory.hpp"
#include "aion/Nameplate.hpp"
#include "aion/Obf.hpp"
#include "aion/Offsets.hpp"
#include "aion/Overlay.hpp"
#include "aion/RealNames.hpp"
#include "aion/Reflection.hpp"
#include "aion/Renderer.hpp"
#include "aion/Sdk.hpp"
#include "aion/SelfBars.hpp"
#include "aion/World.hpp"

namespace aion {
namespace app {
namespace {

// Decode one reflected property value for the diagnostic dumps. Only the common
// property classes are decoded; anything else (structs, arrays) is reported as empty so
// unknown fields still appear in the dump via their name/type/offset.
std::string PropValue(Sdk& sdk, addr_t p, const std::string& type) {
    char buf[256];
    auto object_desc = [&](addr_t o) -> std::string {
        if (!sdk.mem->valid_ptr(o)) return "null";
        const addr_t c = sdk.mem->ptr_at(o + off::uobject::Class);
        const std::string cn =
            sdk.mem->valid_ptr(c) ? sdk.Name(sdk.mem->u32_at(c + off::uobject::Name)) : "?";
        return cn + ":" + sdk.Name(sdk.mem->u32_at(o + off::uobject::Name));
    };
    if (type == "IntProperty") {
        std::snprintf(buf, sizeof buf, "%d", sdk.rd<i32>(p));
    } else if (type == "Int64Property") {
        std::snprintf(buf, sizeof buf, "%lld", (long long)sdk.rd<i64>(p));
    } else if (type == "UInt32Property") {
        std::snprintf(buf, sizeof buf, "%u", sdk.rd<u32>(p));
    } else if (type == "ByteProperty" || type == "EnumProperty") {
        std::snprintf(buf, sizeof buf, "%u", (unsigned)sdk.mem->u8_at(p));
    } else if (type == "FloatProperty") {
        std::snprintf(buf, sizeof buf, "%.3f", sdk.rd<float>(p));
    } else if (type == "DoubleProperty") {
        std::snprintf(buf, sizeof buf, "%.3f", sdk.rd<double>(p));
    } else if (type == "BoolProperty") {
        return sdk.mem->u8_at(p) ? "true" : "false";
    } else if (type == "NameProperty") {
        return sdk.Name(sdk.mem->u32_at(p));
    } else if (type.find("ObjectProperty") != std::string::npos || type == "ClassProperty" ||
               type == "SoftObjectProperty" || type == "WeakObjectProperty") {
        return object_desc(sdk.mem->ptr_at(p));
    } else if (type == "StrProperty") {
        return sdk.mem->read_fstring(p, 512, true);
    } else if (type == "TextProperty") {
        return sdk.mem->read_ftext(p, 256, true);
    } else {
        return {};
    }
    return buf;
}

}  // namespace

int ModeProps(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_props.txt", "wb");
        Reflection refl(&sdk);
        World w(&sdk);
        Level lv(&sdk);
        int shown = 0;
        // Cap per filter, not overall, so one run can cover several classes; skip lets a
        // pool of pre-allocated actors (nameplates) be stepped over to reach live ones.
        std::unordered_map<std::string, int> seen_per;
        auto matched = [&](const std::string& cn) -> const std::string* {
            for (const auto& ft : opt.dump_filters)
                if (cn.find(ft) != std::string::npos) return &ft;
            return nullptr;
        };
        // Print a class chain's properties for one object (indent keeps components
        // visually nested under their actor).
        auto dump_props_of = [&](addr_t obj, addr_t objcls, const char* indent) {
            addr_t c = objcls;
            for (int d = 0; d < 64 && c; ++d) {
                for (const auto& pi : refl.EnumerateProperties(c)) {
                    if (pi.Offset < 0) continue;
                    std::fprintf(f, "%s+0x%04X  %-44s %-20s %s\n", indent, pi.Offset,
                                 pi.Name.c_str(), pi.Type.c_str(),
                                 PropValue(sdk, obj + pi.Offset, pi.Type).c_str());
                }
                c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct);
            }
        };
        auto inspect = [&](addr_t lvl) {
            for (addr_t a : lv.Actors(lvl)) {
                if (!sdk.mem->valid_ptr(a)) continue;
                const addr_t cls = sdk.mem->ptr_at(a + off::uobject::Class);
                if (!sdk.mem->valid_ptr(cls)) continue;
                const std::string cn = sdk.Name(sdk.mem->u32_at(cls + off::uobject::Name));
                const std::string* mf = matched(cn);
                if (!mf) continue;
                // --dump-has keeps only actors that currently have a given object property
                // set: a nameplate widget component exists only while a nameplate is shown.
                if (!opt.dump_has.empty()) {
                    bool has = false;
                    for (addr_t cc = cls; cc && !has; cc = sdk.mem->ptr_at(cc + off::ustruct::SuperStruct))
                        for (const auto& pi : refl.EnumerateProperties(cc)) {
                            if (pi.Name != opt.dump_has) continue;
                            if (pi.Type.find("ObjectProperty") == std::string::npos) break;
                            if (sdk.mem->valid_ptr(sdk.mem->ptr_at(a + pi.Offset))) has = true;
                            break;
                        }
                    if (!has) continue;
                }
                const int seen = seen_per[*mf]++;
                if (seen < opt.dump_skip || seen >= opt.dump_skip + opt.dump_count) continue;
                ++shown;
                std::fprintf(f, "\n=== actor 0x%llX  class=%s  objname=%s\n",
                             (unsigned long long)a, cn.c_str(),
                             sdk.Name(sdk.mem->u32_at(a + off::uobject::Name)).c_str());
                dump_props_of(a, cls, "  ");
                // Components carry the gameplay identity: an AionActContainerComponent
                // knows which gatherable/item this object is, which the actor's own
                // name never does.
                std::vector<std::pair<std::string, addr_t>> comps;
                for (addr_t c = cls; c;) {
                    for (const auto& pi : refl.EnumerateProperties(c))
                        if (pi.Offset >= 0 && pi.Type.find("ObjectProperty") != std::string::npos) {
                            const addr_t o = sdk.mem->ptr_at(a + pi.Offset);
                            if (sdk.mem->valid_ptr(o)) comps.emplace_back(pi.Name, o);
                        }
                    c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct);
                }
                int dumped = 0;
                for (const auto& pr : comps) {
                    if (dumped >= opt.dump_comps) break;
                    const addr_t ccls = sdk.mem->ptr_at(pr.second + off::uobject::Class);
                    if (!sdk.mem->valid_ptr(ccls)) continue;
                    const std::string cc = sdk.Name(sdk.mem->u32_at(ccls + off::uobject::Name));
                    if (cc.find("Component") == std::string::npos) continue;
                    if (cc.find("MeshComponent") != std::string::npos) continue;  // no identity there
                    ++dumped;
                    std::fprintf(f, "  -- %s  (%s)\n", pr.first.c_str(), cc.c_str());
                    dump_props_of(pr.second, ccls, "     ");
                }
            }
        };
        inspect(w.PersistentLevel().Addr());
        for (addr_t l : w.Levels()) inspect(l);
        std::fclose(f);
        std::string spec;
        for (const auto& ft : opt.dump_filters) spec += (spec.empty() ? "" : ",") + ft;
        AION_INFO("dumped %d actor(s) properties for '%s' to aion_props.txt", shown, spec.c_str());
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeScanObjects(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_objects.txt", "wb");
        struct Hit {
            int count = 0;
            std::vector<std::string> samples;
            addr_t addr = 0;
        };
        std::unordered_map<std::string, Hit> hist;
        struct Match {
            std::string cls, name;
            addr_t addr = 0;
        };
        std::vector<Match> all;
        int live = 0;
        for (i32 i = 0; i < sdk.objects.Num(); ++i) {
            const addr_t o = sdk.objects.ObjectAt(i);
            if (!sdk.mem->valid_ptr(o)) continue;
            ++live;
            const addr_t c = sdk.mem->ptr_at(o + off::uobject::Class);
            if (!sdk.mem->valid_ptr(c)) continue;
            const std::string cn = sdk.Name(sdk.mem->u32_at(c + off::uobject::Name));
            const std::string on = sdk.Name(sdk.mem->u32_at(o + off::uobject::Name));
            if (cn.find(opt.scan_objects_substr) == std::string::npos &&
                on.find(opt.scan_objects_substr) == std::string::npos)
                continue;
            Hit& h = hist[cn];
            ++h.count;
            if (!h.addr) h.addr = o;
            if (opt.scan_samples <= 0) {
                all.push_back({cn, on, o});
            } else if (h.samples.size() < static_cast<std::size_t>(opt.scan_samples) &&
                       std::find(h.samples.begin(), h.samples.end(), on) == h.samples.end()) {
                h.samples.push_back(on);
            }
        }
        std::vector<std::pair<std::string, Hit>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.second.count > b.second.count;
        });
        std::fprintf(f, "# objects whose class or name contains '%s'; %d live objects scanned\n",
                     opt.scan_objects_substr.c_str(), live);
        if (opt.scan_samples <= 0) {
            std::fprintf(f, "# %zu objects:  class  name  @address\n", all.size());
            std::sort(all.begin(), all.end(), [](const Match& a, const Match& b) {
                return a.cls != b.cls ? a.cls < b.cls : a.name < b.name;
            });
            for (const Match& m : all)
                std::fprintf(f, "%-24s %-40s @0x%llX\n", m.cls.c_str(), m.name.c_str(),
                             (unsigned long long)m.addr);
        } else {
            std::fprintf(f, "#  count  class                          sample objects\n");
            for (const auto& p : v) {
                std::string samples;
                for (const auto& n : p.second.samples)
                    samples += (samples.empty() ? "" : ", ") + n;
                std::fprintf(f, "%7d  %-30s %s  [first @0x%llX]\n", p.second.count,
                             p.first.c_str(), samples.c_str(),
                             (unsigned long long)p.second.addr);
            }
        }
        std::fclose(f);
        AION_INFO("%zu classes match '%s' out of %d live objects -> aion_objects.txt", v.size(),
                  opt.scan_objects_substr.c_str(), live);
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeRaw(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_raw.txt", "wb");
        int shown = 0;
        for (i32 i = 0; i < sdk.objects.Num() && shown < 3; ++i) {
            const addr_t o = sdk.objects.ObjectAt(i);
            if (!sdk.mem->valid_ptr(o)) continue;
            const addr_t c = sdk.mem->ptr_at(o + off::uobject::Class);
            if (!sdk.mem->valid_ptr(c)) continue;
            const std::string cn = sdk.Name(sdk.mem->u32_at(c + off::uobject::Name));
            const std::string on = sdk.Name(sdk.mem->u32_at(o + off::uobject::Name));
            // Type objects and class defaults answer "what is this class", not "what is this
            // instance holding", so they are not dumped here.
            if (cn == "Class" || cn == "ScriptStruct" || cn == "Function" || cn == "Enum" ||
                cn == "Package" || cn.find("GeneratedClass") != std::string::npos ||
                on.rfind("Default__", 0) == 0)
                continue;
            if (cn.find(opt.dump_raw_substr) == std::string::npos &&
                on.find(opt.dump_raw_substr) == std::string::npos)
                continue;
            ++shown;
            std::fprintf(f, "\n=== obj 0x%llX  class=%s  name=%s\n", (unsigned long long)o,
                         cn.c_str(), on.c_str());
            for (addr_t off = 0; off < opt.raw_bytes; off += 8) {
                const u64 q = sdk.mem->u64_at(o + off);
                std::fprintf(f, "  +0x%03llX  %016llX  ", (unsigned long long)off,
                             (unsigned long long)q);
                const addr_t p = static_cast<addr_t>(q);
                bool decoded = false;
                if (sdk.mem->valid_ptr(p)) {
                    const addr_t pc = sdk.mem->ptr_at(p + off::uobject::Class);
                    if (sdk.mem->valid_ptr(pc)) {
                        const std::string pcn =
                            sdk.Name(sdk.mem->u32_at(pc + off::uobject::Name));
                        if (!pcn.empty()) {
                            std::fprintf(
                                f, "obj %s:%s", pcn.c_str(),
                                sdk.Name(sdk.mem->u32_at(p + off::uobject::Name)).c_str());
                            decoded = true;
                        }
                    }
                }
                if (!decoded)
                    std::fprintf(f, "i32 %d, %d", static_cast<i32>(q & 0xFFFFFFFFu),
                                 static_cast<i32>(q >> 32));
                // Native members are not reflected, so strings are what identify them: a
                // name or an id string shows up here even when no property describes it.
                std::string s = sdk.mem->read_ftext(o + off, 192, true);
                if (s.size() < 2) s = sdk.mem->read_fstring(o + off, 512, true);
                if (s.size() >= 2) std::fprintf(f, "   \"%s\"", s.c_str());
                std::fprintf(f, "\n");
            }
        }
        std::fclose(f);
        AION_INFO("dumped %d raw object(s) matching '%s' -> aion_raw.txt", shown,
                  opt.dump_raw_substr.c_str());
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeReflectedStruct(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_struct.txt", "wb");
        Reflection refl(&sdk);
        int shown = 0;
        for (i32 i = 0; i < sdk.objects.Num() && shown < 4; ++i) {
            const addr_t o = sdk.objects.ObjectAt(i);
            if (!sdk.mem->valid_ptr(o)) continue;
            const addr_t c = sdk.mem->ptr_at(o + off::uobject::Class);
            if (!sdk.mem->valid_ptr(c)) continue;
            const std::string cn = sdk.Name(sdk.mem->u32_at(c + off::uobject::Name));
            // Type objects only: script structs and classes, including the generated classes
            // that blueprint widgets and actors use.
            if (cn != "ScriptStruct" && cn != "Class" && cn.find("GeneratedClass") == std::string::npos)
                continue;
            const std::string on = sdk.Name(sdk.mem->u32_at(o + off::uobject::Name));
            if (on.find(opt.dump_struct_substr) == std::string::npos) continue;
            ++shown;
            const auto props = refl.EnumerateProperties(o);
            std::fprintf(f, "\n=== %s  (%s)  @0x%llX  %zu properties\n", on.c_str(), cn.c_str(),
                         (unsigned long long)o, props.size());
            for (const auto& pi : props)
                std::fprintf(f, "  +0x%04X  %-44s %s\n", pi.Offset, pi.Name.c_str(),
                             pi.Type.c_str());
        }
        std::fclose(f);
        AION_INFO("dumped %d type(s) matching '%s' -> aion_struct.txt", shown,
                  opt.dump_struct_substr.c_str());
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeDataRows(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_rows.txt", "wb");
        int tables = 0, rows = 0;
        for (i32 i = 0; i < sdk.objects.Num(); ++i) {
            const addr_t o = sdk.objects.ObjectAt(i);
            if (!sdk.mem->valid_ptr(o)) continue;
            const addr_t c = sdk.mem->ptr_at(o + off::uobject::Class);
            if (!sdk.mem->valid_ptr(c)) continue;
            const std::string cn = sdk.Name(sdk.mem->u32_at(c + off::uobject::Name));
            if (cn != "DataTable" && cn != "CompositeDataTable") continue;
            DataTableRows t;
            if (!ReadTableRows(sdk.mem, sdk.pool, o, &t)) continue;
            const auto hits = FindRows(sdk.mem, sdk.pool, t, opt.find_row_substr);
            if (hits.empty()) continue;
            ++tables;
            std::fprintf(f, "\n=== %s (%s)  rows=%d stride=0x%llX  rowstruct=%s\n",
                         sdk.Name(sdk.mem->u32_at(o + off::uobject::Name)).c_str(), cn.c_str(),
                         t.num, (unsigned long long)t.stride,
                         sdk.Name(sdk.mem->u32_at(t.row_struct + off::uobject::Name)).c_str());
            for (const auto& h : hits) {
                ++rows;
                std::fprintf(f, "  key=%-44s row=0x%llX\n", h.first.c_str(),
                             (unsigned long long)h.second);
                if (!h.second) continue;
                for (const auto& s : RowStrings(sdk.mem, h.second, t.row_size))
                    std::fprintf(f, "    +0x%04llX  %s\n", (unsigned long long)s.first,
                                 s.second.c_str());
            }
        }
        std::fclose(f);
        AION_INFO("%d table(s), %d row(s) matched '%s' -> aion_rows.txt", tables, rows,
                  opt.find_row_substr.c_str());
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeHarvestPlates(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    EntityCache& cache = mc.cache;
    NameTable& names = mc.names;
        HarvestPlates(&sdk, &cache, &names);
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeFindNameText(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    const Options& opt = mc.opt;
        FindNameText(&sdk, opt.find_name_text);
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeNameplates(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    EntityCache& cache = mc.cache;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_nameplates.txt", "wb");
        Reflection refl(&sdk);
        NameplateReader np(&sdk, &refl);
        World w(&sdk);
        Level lv(&sdk);
        int shown = 0, with_prop = 0;
        // Only actors that really declare the property: a fixed offset lands on unrelated
        // data on other classes (a volume "has" a nameplate widget at 0x2810 by accident).
        const auto np_offset = [&](addr_t cls) -> addr_t {
            for (addr_t c = cls; c; c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct))
                for (const auto& pi : refl.EnumerateProperties(c))
                    if (pi.Offset >= 0 && pi.Name == "NameplateWidgetComponent")
                        return static_cast<addr_t>(pi.Offset);
            return 0;
        };
        const auto scan = [&](addr_t lvl_addr) {
            for (addr_t a : lv.Actors(lvl_addr)) {
                if (!sdk.mem->valid_ptr(a)) continue;
                const addr_t cls = sdk.mem->ptr_at(a + off::uobject::Class);
                if (!sdk.mem->valid_ptr(cls)) continue;
                const addr_t noff = np_offset(cls);
                if (!noff) continue;
                if (!sdk.mem->valid_ptr(sdk.mem->ptr_at(a + noff))) continue;
                ++with_prop;
                // A widget only carries a tree once the game has built the nameplate; the
                // rest are pooled instances, and the actor keeps its pointer to the pool.
                const addr_t root = np.Root(a);
                if (!root || shown >= opt.dump_count) continue;
                ++shown;
                const std::string cn = sdk.Name(sdk.mem->u32_at(cls + off::uobject::Name));
                const std::string model = cache.ModelOf(&sdk, a, cn);
                const addr_t wc = np.Widget(a);
                std::fprintf(f, "\n=== %s  mesh=%s  @0x%llX  (prop +0x%llX)\n", cn.c_str(),
                             model.c_str(), (unsigned long long)a, (unsigned long long)noff);
                std::fprintf(f, "     widget=0x%llX  root=0x%llX\n", (unsigned long long)wc,
                             (unsigned long long)root);
                np.Walk(a, [&](addr_t wid, const std::string& wcn) {
                    const std::string t = np.TextOf(wid);
                    std::fprintf(f, "     %-30s @0x%llX%s%s\n", wcn.c_str(),
                                 (unsigned long long)wid, t.empty() ? "" : "  TEXT: ",
                                 t.c_str());
                    if (!t.empty())
                        std::fprintf(f, "       storage: %s\n", np.TextStorage(wid).c_str());
                });
                std::fprintf(f, "     NAME: %s\n", np.Name(a).c_str());
            }
        };
        scan(w.PersistentLevel().Addr());
        for (addr_t l : w.Levels()) scan(l);
        std::fclose(f);
        AION_INFO("%d actor(s) carry a nameplate property, %d have a built widget tree -> "
                  "aion_nameplates.txt",
                  with_prop, shown);
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeAiSurvey(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    NameTable& names = mc.names;
    const Options& opt = mc.opt;
        std::FILE* f = std::fopen("aion_ai.txt", "wb");
        if (!f) {
            AION_ERROR("could not write aion_ai.txt");
            gfx.Shutdown();
            DestroyWindow(overlay);
            mem.Close();
            return 0;
        }
        Reflection refl(&sdk);
        World w(&sdk);
        Level lv(&sdk);

        auto class_of = [&](addr_t o) { return sdk.mem->ptr_at(o + off::uobject::Class); };
        auto cname = [&](addr_t cls) -> std::string {
            return sdk.mem->valid_ptr(cls)
                       ? sdk.Name(sdk.mem->u32_at(cls + off::uobject::Name))
                       : std::string();
        };
        auto oname = [&](addr_t o) -> std::string {
            return sdk.mem->valid_ptr(o) ? sdk.Name(sdk.mem->u32_at(o + off::uobject::Name))
                                         : std::string();
        };
        auto has = [](const std::string& h, const char* n) {
            return h.find(n) != std::string::npos;
        };
        auto lower = [](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        auto is_a = [&](addr_t obj, const char* nm) {
            addr_t c = class_of(obj);
            for (int d = 0; d < 64 && c; ++d) {
                if (cname(c) == nm) return true;
                c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct);
            }
            return false;
        };
        // Field offsets are resolved by NAME against the running build. The SDK on disk is a
        // neighbouring build (measured: 22/33 anchors identical, the rest +/-8..80), so it
        // supplies names and inheritance only - never an offset this code trusts blindly.
        // Field offset by name up the class chain, with a validated constant as fallback.
        auto obj_field = [&](addr_t obj, addr_t cls, const char* prop, i32 fallback) -> addr_t {
            const i32 o = refl.GetPropertyOffset(cls, prop);
            const i32 use = o >= 0 ? o : fallback;
            return use < 0 ? 0 : sdk.mem->ptr_at(obj + (addr_t)use);
        };
        // Arrays/maps/structs are opaque to PropValue; show their shape instead of nothing,
        // because an aggro list is exactly an array of structs.
        auto extra = [&](addr_t p, const std::string& type) -> std::string {
            char b[256];
            if (has(type, "ArrayProperty") || has(type, "MapProperty") || has(type, "SetProperty")) {
                std::snprintf(b, sizeof b, "buffer=0x%llX num=%d max=%d",
                              (unsigned long long)sdk.mem->ptr_at(p), sdk.rd<i32>(p + 8),
                              sdk.rd<i32>(p + 12));
                return b;
            }
            if (has(type, "StructProperty")) {
                u8 raw[32] = {};
                if (!sdk.mem->read(p, raw, sizeof raw)) return {};
                std::string s = "raw[";
                for (u8 by : raw) {
                    char h[4];
                    std::snprintf(h, sizeof h, "%02X", by);
                    s += h;
                }
                return s + "]";
            }
            return {};
        };
        auto prop_dump = [&](addr_t obj, const char* indent) {
            addr_t c = class_of(obj);
            for (int k = 0; k < 64 && c; ++k) {
                std::fprintf(f, "%s[class %s]\n", indent, cname(c).c_str());
                for (const auto& pi : refl.EnumerateProperties(c)) {
                    if (pi.Offset < 0) continue;
                    std::string v = PropValue(sdk, obj + pi.Offset, pi.Type);
                    if (v.empty()) v = extra(obj + pi.Offset, pi.Type);
                    std::fprintf(f, "%s  +0x%04X %-44s %-22s %s\n", indent, pi.Offset,
                                 pi.Name.c_str(), pi.Type.c_str(), v.c_str());
                }
                c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct);
            }
        };
        // Declared layout of a class (no instance): the same walk prop_dump does, minus values.
        auto layout_dump = [&](addr_t cls, const char* indent) {
            for (int k = 0; k < 64 && cls; ++k) {
                for (const auto& pi : refl.EnumerateProperties(cls))
                    if (pi.Offset >= 0)
                        std::fprintf(f, "%s+0x%04X %-44s %s\n", indent, pi.Offset, pi.Name.c_str(),
                                     pi.Type.c_str());
                cls = sdk.mem->ptr_at(cls + off::ustruct::SuperStruct);
            }
        };
        static const char* kStrong[] = {"attack", "combat", "aggro", "threat", "hate",  "retaliate",
                                        "provoke", "sight", "detect", "brain", "behavior", "chase",
                                        "damage",  "percept", "skill", "consider"};
        static const char* kWeak[] = {"target", "move",  "stop",   "enable", "disable", "ability",
                                      "pause",  "logic", "die",    "dead",   "focus"};
        auto strong_hits = [&](addr_t cls, std::vector<std::string>* out) {
            const auto fns = refl.EnumerateFunctions(cls);
            int strong = 0, weak = 0, aion = 0, shown = 0;
            for (const auto& fi : fns) {
                const std::string n = lower(fi.Name);
                bool s = false;
                for (const char* k : kStrong)
                    if (has(n, k)) { s = true; break; }
                if (has(fi.Name, "Aion")) ++aion;
                if (!s) {
                    for (const char* k : kWeak)
                        if (has(n, k)) { ++weak; break; }
                    continue;
                }
                ++strong;
                ++shown;
                if (out) out->push_back(fi.Name);
                // A native entry point outside the module means UFunction::Func (0xD8, only
                // 'supplied') is wrong for this build - say so rather than print a bad address,
                // since a code patch would be aimed with it.
                const bool in_mod = fi.NativeFunc && sdk.mem->in_module(fi.NativeFunc);
                std::fprintf(f, "    %-50s flags=0x%08X native=0x%llX%s\n", fi.Name.c_str(), fi.Flags,
                             (unsigned long long)fi.NativeFunc,
                             in_mod ? "" : "   <-- not in module: native offset UNVALIDATED");
            }
            std::fprintf(f, "    [total UFunctions=%zu strong=%d weak=%d aion=%d shown=%d]\n",
                         fns.size(), strong, weak, aion, shown);
            return fns.size();
        };
        auto mesh_name = [&](addr_t a) -> std::string {
            const addr_t m = sdk.mem->ptr_at(a + off::acharacter::Mesh);
            if (!sdk.mem->valid_ptr(m)) return {};
            const u32 candidates[2] = {off::uskeletal_mesh_component::SkeletalMesh,
                                       off::ustatic_mesh_component::StaticMesh};
            for (u32 mo : candidates) {
                const addr_t asset = sdk.mem->ptr_at(m + mo);
                if (!sdk.mem->valid_ptr(asset)) continue;
                const std::string ac = cname(class_of(asset));
                if (ac == "SkeletalMesh" || ac == "StaticMesh") return oname(asset);
            }
            return {};
        };
        // Components whose class says they carry AI/combat state (Aggro/Sight/Stat/Ability...).
        auto comps_of_interest = [&](addr_t a, addr_t acls, std::vector<addr_t>* out) {
            const u32 arrays[2] = {off::aactor::BlueprintCreatedComponents,
                                   off::aactor::InstanceComponents};
            for (u32 arr : arrays)
                for (addr_t c : sdk.mem->read_tarray<addr_t>(a + arr, 128))
                    if (sdk.mem->valid_ptr(c)) out->push_back(c);
            for (addr_t c = acls; c;) {
                for (const auto& pi : refl.EnumerateProperties(c))
                    if (pi.Offset >= 0 && has(pi.Type, "ObjectProperty")) {
                        const addr_t o = sdk.mem->ptr_at(a + pi.Offset);
                        if (sdk.mem->valid_ptr(o)) out->push_back(o);
                    }
                c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct);
            }
            for (auto it = out->begin(); it != out->end();) {
                const std::string cc = cname(class_of(*it));
                const bool keep =
                    has(cc, "Aggro") || has(cc, "Sight") || has(cc, "Sensing") || has(cc, "Percept") ||
                    has(cc, "Brain") || has(cc, "Blackboard") || has(cc, "Behavior") ||
                    has(cc, "Detect") || has(cc, "PathFollowing") || has(cc, "Stat") ||
                    has(cc, "Ability") || has(cc, "Skill") || has(cc, "Target");
                if (!keep)
                    it = out->erase(it);
                else
                    ++it;
            }
        };

        // Local pawn, only for distances; the survey works without it. Reached the same way the
        // frame loop reaches it - UWorld -> OwningGameInstance -> LocalPlayers[0] ->
        // PlayerController -> AcknowledgedPawn. The GEngine/LocalPlayer globals that used to be read
        // here were removed: on this client the first points at a UClass object and the second at an
        // address with no class at all, so the old code silently measured every distance from
        // nowhere.
        addr_t local_pawn = 0;
        {
            World world(&sdk);
            const addr_t gi =
                sdk.mem->ptr_at(world.Get().Addr() + off::uworld::OwningGameInstance);
            if (sdk.mem->valid_ptr(gi)) {
                for (addr_t lp : sdk.mem->read_tarray<addr_t>(
                         gi + off::ugameinstance::LocalPlayers, 8)) {
                    if (!sdk.mem->valid_ptr(lp)) continue;
                    const addr_t pc = sdk.mem->ptr_at(lp + off::uplayer::PlayerController);
                    if (!sdk.mem->valid_ptr(pc)) continue;
                    local_pawn =
                        sdk.mem->ptr_at(pc + off::aplayercontroller::AcknowledgedPawn);
                    if (local_pawn) break;
                }
            }
        }
        FVector eye{};
        if (sdk.mem->valid_ptr(local_pawn)) {
            const addr_t rc = sdk.mem->ptr_at(local_pawn + off::aactor::RootComponent);
            if (sdk.mem->valid_ptr(rc))
                eye = sdk.ReadVector(rc + off::uscene_component::RelativeLocation);
        }

        struct Seen {
            addr_t addr = 0, ctrl = 0;
            std::string cls, name, mesh;
            double dist = 0;
        };
        std::vector<Seen> chars, ai_actors;
        std::unordered_map<std::string, int> char_cls, ctrl_cls;
        // APawn::Controller is declared on Pawn, so reflection resolves it once for the whole
        // sweep instead of per actor (which would re-walk every class chain thousands of times).
        // The UClass names carry no C++ prefix on this engine: "Pawn", not "APawn" (measured in the
        // live dump: CLASS Pawn super=Actor, CLASS Character super=Pawn). Testing the prefixed
        // names here found no character at all - the survey's character sections were always empty.
        const i32 off_controller = refl.GetPropertyOffset(refl.FindClass("Pawn").Addr(), "Controller");
        const i32 controller_off = off_controller >= 0 ? off_controller : (i32)off::apawn::Controller;

        auto visit = [&](addr_t level_addr) {
            for (addr_t a : lv.Actors(level_addr)) {
                if (!sdk.mem->valid_ptr(a)) continue;
                const addr_t ac = class_of(a);
                const std::string cls = cname(ac);
                if (cls.empty()) continue;
                const std::string nm = oname(a);
                if (!(is_a(a, "Character") || is_a(a, "Pawn"))) {
                    if (has(cls, "AIController") || has(cls, "Aggro") || has(cls, "Detect") ||
                        has(cls, "Sight") || has(cls, "TargetSelection") || has(cls, "TargetDistance"))
                        ai_actors.push_back({a, 0, cls, nm, {}});
                    continue;
                }
                Seen e;
                e.addr = a;
                e.cls = cls;
                e.name = nm;
                if (is_a(a, "Character")) e.mesh = mesh_name(a);
                if (local_pawn) {
                    const addr_t rc = sdk.mem->ptr_at(a + off::aactor::RootComponent);
                    if (sdk.mem->valid_ptr(rc))
                        e.dist = (sdk.ReadVector(rc + off::uscene_component::RelativeLocation) - eye)
                                     .Length() / 100.0;
                }
                e.ctrl = sdk.mem->ptr_at(a + (addr_t)controller_off);
                chars.push_back(e);
                ++char_cls[cls];
                const std::string cc = cname(class_of(e.ctrl));
                if (!cc.empty()) ++ctrl_cls[cc];
            }
        };
        const addr_t persistent = w.PersistentLevel().Addr();
        visit(persistent);
        for (addr_t l : w.Levels())
            if (l != persistent) visit(l);
        std::sort(chars.begin(), chars.end(),
                  [](const Seen& x, const Seen& y) { return x.dist < y.dist; });

        char when[64] = {};
        {
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
            localtime_s(&tmv, &t);
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
        }
        std::fprintf(f, "# aion_ai.txt - read-only survey of the NPC AI object graph; nothing was written.\n");
        std::fprintf(f, "# written %s | base=0x%llX | %zu characters, %zu AI helper actors\n",
                     when, (unsigned long long)sdk.base, chars.size(), ai_actors.size());
        std::fprintf(f, "# distances are metres (UE units / 100); 0.0 means no local pawn was found\n\n");

        std::fprintf(f, "=== 1. character classes in the world ===\n");
        for (const auto& kv : char_cls) std::fprintf(f, "  %6d  %s\n", kv.second, kv.first.c_str());
        std::fprintf(f, "\n=== 2. controller classes on those characters ===\n");
        for (const auto& kv : ctrl_cls) std::fprintf(f, "  %6d  %s\n", kv.second, kv.first.c_str());

        std::fprintf(f, "\n=== 3. characters in detail (nearest %d within %dm) ===\n", opt.ai_detail,
                     opt.ai_radius);
        int shown_chars = 0;
        for (const Seen& e : chars) {
            if (shown_chars >= opt.ai_detail) break;
            if (local_pawn && opt.ai_radius > 0 && e.dist > opt.ai_radius) continue;
            ++shown_chars;
            std::fprintf(f, "\n--- character %d --------------------------------------------------\n", shown_chars);
            std::fprintf(f, "0x%llX  class=%s  objname=%s  mesh=%s  dist=%.1fm  controller=0x%llX\n",
                         (unsigned long long)e.addr, e.cls.c_str(), e.name.c_str(), e.mesh.c_str(),
                         e.dist, (unsigned long long)e.ctrl);
            std::fprintf(f, "  [pawn properties]\n");
            prop_dump(e.addr, "    ");
            if (sdk.mem->valid_ptr(e.ctrl)) {
                const addr_t cc = class_of(e.ctrl);
                std::fprintf(f, "  [controller 0x%llX class=%s]\n", (unsigned long long)e.ctrl,
                             cname(cc).c_str());
                prop_dump(e.ctrl, "    ");
                // One level down: whatever the controller points at that looks like a brain,
                // blackboard, perception, sensing or path-following object.
                int sub = 0;
                for (addr_t c = cc; c && sub < 8; c = sdk.mem->ptr_at(c + off::ustruct::SuperStruct)) {
                    for (const auto& pi : refl.EnumerateProperties(c)) {
                        if (pi.Offset < 0 || !has(pi.Type, "ObjectProperty")) continue;
                        const addr_t o = sdk.mem->ptr_at(e.ctrl + pi.Offset);
                        if (!sdk.mem->valid_ptr(o)) continue;
                        const std::string oc = cname(class_of(o));
                        if (!(has(oc, "Brain") || has(oc, "Blackboard") || has(oc, "Percept") ||
                              has(oc, "Sensing") || has(oc, "PathFollowing") || has(oc, "Behavior") ||
                              has(oc, "Aggro") || has(oc, "Sight")))
                            continue;
                        if (sub++ >= 8) break;
                        std::fprintf(f, "    -- %s -> 0x%llX class=%s objname=%s\n", pi.Name.c_str(),
                                     (unsigned long long)o, oc.c_str(), oname(o).c_str());
                        prop_dump(o, "       ");
                    }
                }
                std::fprintf(f, "  [controller UFunctions]\n");
                strong_hits(cc, nullptr);
            }
            std::fprintf(f, "  [pawn UFunctions]\n");
            strong_hits(class_of(e.addr), nullptr);
            std::vector<addr_t> comps;
            comps_of_interest(e.addr, class_of(e.addr), &comps);
            std::fprintf(f, "  [AI-interest components on the pawn: %zu]\n", comps.size());
            int dumped = 0;
            for (addr_t c : comps) {
                if (dumped++ >= 8) break;
                std::fprintf(f, "    -- 0x%llX class=%s objname=%s\n", (unsigned long long)c,
                             cname(class_of(c)).c_str(), oname(c).c_str());
                prop_dump(c, "       ");
            }
            // One pasteable line per character. The whole client-side-AI question is whether a
            // mob's controller actually runs a behaviour tree (BrainComponent + live node
            // instances) or is a hollow shell, and what its sight sensor holds - neither needs
            // the 500 KB dump to travel back.
            const addr_t ccls = sdk.mem->valid_ptr(e.ctrl) ? class_of(e.ctrl) : 0;
            const addr_t brain = sdk.mem->valid_ptr(e.ctrl)
                                     ? obj_field(e.ctrl, ccls, "BrainComponent",
                                                 (i32)off::aicontroller::BrainComponent)
                                     : 0;
            const addr_t bb = sdk.mem->valid_ptr(e.ctrl)
                                  ? obj_field(e.ctrl, ccls, "Blackboard",
                                              (i32)off::aicontroller::Blackboard)
                                  : 0;
            int nodes = -1;
            if (sdk.mem->valid_ptr(brain) &&
                cname(class_of(brain)).find("BehaviorTree") != std::string::npos)
                nodes = sdk.rd<i32>(brain + (addr_t)off::ubehavior_tree_component::NodeInstances + 8);
            std::string sight = "absent";
            for (addr_t c : comps) {
                const std::string scc = cname(class_of(c));
                if (scc.find("Sight") == std::string::npos &&
                    scc.find("Sensing") == std::string::npos)
                    continue;
                char sb[128];
                std::snprintf(sb, sizeof sb, "%s radius=%.0f angle=%.0f onlyPC=%d", scc.c_str(),
                              sdk.rd<float>(c + off::aion_npc_sight_sensing_component::SightRadius),
                              sdk.rd<float>(c + off::aion_npc_sight_sensing_component::SightAngle),
                              (int)sdk.mem->u8_at(c +
                                                  off::aion_npc_sight_sensing_component::bOnlySensePC));
                sight = sb;
            }
            AION_INFO("AI: %s mesh=%s dist=%.0fm ctrl=%s brain=%s nodes=%d blackboard=%s sight=%s",
                      e.cls.c_str(), e.mesh.c_str(), e.dist,
                      sdk.mem->valid_ptr(e.ctrl) ? cname(ccls).c_str() : "none",
                      sdk.mem->valid_ptr(brain) ? cname(class_of(brain)).c_str() : "NULL", nodes,
                      sdk.mem->valid_ptr(bb) ? cname(class_of(bb)).c_str() : "NULL", sight.c_str());
        }

        std::fprintf(f, "\n=== 4. AI helper actors in the level (aggro range / target / detect) ===\n");
        std::unordered_map<std::string, int> ai_seen;
        int ai_dumped = 0;
        for (const Seen& e : ai_actors) {
            if (ai_seen[e.cls]++ >= 2 || ai_dumped >= 10) continue;
            ++ai_dumped;
            std::fprintf(f, "\n-- 0x%llX class=%s objname=%s\n", (unsigned long long)e.addr,
                         e.cls.c_str(), e.name.c_str());
            prop_dump(e.addr, "   ");
        }

        std::fprintf(f, "\n=== 5. AI/Aion classes: known runtime names, ranked candidates, layouts ===\n");
        // "Target" on its own matches TextureRenderTarget and MorphTarget, which is what filled
        // the first pass's cap with render targets. Names are scored, engine lookalikes excluded,
        // and the classes the live census actually reported are looked up by exact name.
        static const char* kNoise[] = {"TextureRenderTarget", "MorphTarget",  "ParticleModule",
                                       "RPCDoSDetection",     "RenderTarget", "Gizmo",
                                       "ControlRig",          "RigUnit",      "MovieScene",
                                       "IKRetarget",          "Dataflow",     "GeometryScript",
                                       "AnimNode",            "GraphNode",    "TargetPoint",
                                       "AimTarget",           "BlendTarget",  "RBFTarget"};
        auto ai_score = [&](const std::string& n) -> int {
            for (const char* k : kNoise)
                if (has(n, k)) return 0;
            int s = 0;
            if (n.rfind("Aion", 0) == 0) s += 100;
            if (has(n, "AIController")) s += 60;
            if (has(n, "Aggro")) s += 55;
            if (has(n, "Sight") || has(n, "Sensing")) s += 45;
            if (has(n, "Brain") || has(n, "Blackboard")) s += 45;
            if (has(n, "Detect")) s += 40;
            if (has(n, "Behavior")) s += 35;
            if (has(n, "Perception")) s += 35;
            if (has(n, "Npc") || has(n, "NPC")) s += 30;
            if (has(n, "Monster") || has(n, "Mob")) s += 25;
            if (has(n, "Skill")) s += 15;
            if (has(n, "Target")) s += 10;
            return s;
        };
        static const char* kKnown[] = {"AionDefaultAIController",  "AionPetAIController",
                                       "AionDummyController",     "AionPlayerController",
                                       "AionAggroRangeActor",     "AionTargetSelectionActor",
                                       "AionTargetDistanceActor", "AionNpcSightSensingComponent",
                                       "NpcBehaviorDetectSystem", "AionSkillNoticeObj",
                                       "BPCh_NpcBase_C",          "AionSimpleCharacter",
                                       "AionEffectCharacter",     "AIController",
                                       "BrainComponent",          "BehaviorTreeComponent",
                                       "BlackboardComponent",     "AIPerceptionComponent",
                                       "PawnSensingComponent"};
        // One pass over GObjects: index every UClass by name (so the 19 known names below are map
        // lookups instead of 19 full scans) and rank the AI-looking ones.
        std::unordered_map<std::string, addr_t> class_by_name;
        std::vector<std::pair<int, addr_t>> ranked;
        for (i32 i = 0; i < sdk.objects.Num(); ++i) {
            const addr_t o = sdk.objects.ObjectAt(i);
            if (!o) continue;
            const std::string ocn = cname(class_of(o));
            if (ocn != "Class" && ocn != "BlueprintGeneratedClass") continue;
            const std::string n = oname(o);
            if (class_by_name.find(n) == class_by_name.end()) class_by_name[n] = o;
            const int sc = ai_score(n);
            if (sc > 0) ranked.emplace_back(sc, o);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const std::pair<int, addr_t>& a, const std::pair<int, addr_t>& b) {
                             return a.first > b.first;
                         });
        // Instance counts: one set of classes of interest, then one pass over all objects.
        std::unordered_map<addr_t, int> inst_count;
        for (const auto& r : ranked) inst_count[r.second] = 0;
        for (const char* kn : kKnown) {
            auto it = class_by_name.find(kn);
            if (it != class_by_name.end()) inst_count[it->second] = 0;
        }
        for (i32 i = 0; i < sdk.objects.Num(); ++i) {
            const addr_t o = sdk.objects.ObjectAt(i);
            if (!o) continue;
            const addr_t oc = class_of(o);
            auto it = oc ? inst_count.find(oc) : inst_count.end();
            if (it != inst_count.end()) ++it->second;
        }

        std::fprintf(f, "\n=== 5a. known runtime classes (exact names, from the live census) ===\n");
        int known_found = 0;
        for (const char* kn : kKnown) {
            auto it = class_by_name.find(kn);
            if (it == class_by_name.end()) {
                std::fprintf(f, "\n-- %s : NOT PRESENT in this client\n", kn);
                continue;
            }
            const addr_t k = it->second;
            ++known_found;
            std::fprintf(f, "\n-- %s  class=0x%llX  instances=%d  super=%s\n", kn,
                         (unsigned long long)k, inst_count[k],
                         cname(sdk.mem->ptr_at(k + off::ustruct::SuperStruct)).c_str());
            layout_dump(k, "     ");
            std::fprintf(f, "   [UFunctions]\n");
            strong_hits(k, nullptr);
        }

        std::fprintf(f, "\n=== 5b. ranked AI candidates (%zu, top 150 listed) ===\n", ranked.size());
        int listed = 0;
        for (const auto& r : ranked) {
            if (listed++ >= 150) {
                std::fprintf(f, "     ... %zu more\n", ranked.size() - 150);
                break;
            }
            std::fprintf(f, "     %4d  %-50s instances=%-6d super=%s\n", r.first,
                         oname(r.second).c_str(), inst_count[r.second],
                         cname(sdk.mem->ptr_at(r.second + off::ustruct::SuperStruct)).c_str());
        }

        std::fprintf(f, "\n=== 5c. top candidates in detail ===\n");
        int cls_dumped = 0;
        for (const auto& r : ranked) {
            if (cls_dumped++ >= 12) break;
            std::fprintf(f, "\n-- %s  (instances=%d, super=%s, score=%d)\n", oname(r.second).c_str(),
                         inst_count[r.second],
                         cname(sdk.mem->ptr_at(r.second + off::ustruct::SuperStruct)).c_str(), r.first);
            layout_dump(r.second, "     ");
            std::fprintf(f, "   [UFunctions]\n");
            strong_hits(r.second, nullptr);
        }

        std::fclose(f);
        AION_INFO("AI survey -> aion_ai.txt: %zu characters, %zu controller classes, %zu AI helper "
                  "actors, %d/%zu known AI classes present, %zu ranked candidates (detail=%d, "
                  "radius=%dm)",
                  chars.size(), ctrl_cls.size(), ai_actors.size(), known_found,
                  sizeof(kKnown) / sizeof(kKnown[0]), ranked.size(), opt.ai_detail, opt.ai_radius);
        if (local_pawn == 0)
            AION_INFO("note: no local pawn resolved, so distances in the survey are 0");
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeActorClasses(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    EntityCache& cache = mc.cache;
        DumpActorClasses(sdk, cache);
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

int ModeRenderGates(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    NameTable& names = mc.names;
    const Options& opt = mc.opt;
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string need = lower(opt.gates_substr);
        Reflection refl(&sdk);
        World gw(&sdk);
        Level glv(&sdk);

        // Origin: the local pawn, reached through the same chain the frame loop uses.
        double ox = 0.0, oy = 0.0, oz = 0.0;
        bool have_origin = false;
        {
            const UObject gi = gw.OwningGameInstance();
            const auto lps =
                sdk.mem->read_tarray<addr_t>(gi.Addr() + off::ugameinstance::LocalPlayers, 8);
            if (!lps.empty()) {
                const addr_t pc = sdk.mem->ptr_at(lps[0] + off::uplayer::PlayerController);
                const addr_t pawn =
                    pc ? sdk.mem->ptr_at(pc + off::aplayercontroller::AcknowledgedPawn) : 0;
                const addr_t root = pawn ? sdk.mem->ptr_at(pawn + off::aactor::RootComponent) : 0;
                FVector loc{};
                if (root && sdk.mem->read(root + off::uscene_component::RelativeLocation, &loc,
                                          sizeof loc)) {
                    ox = loc.X;
                    oy = loc.Y;
                    oz = loc.Z;
                    have_origin = true;
                }
            }
        }

        // Per-class reflection is cached: 10k+ actors share a handful of classes, and walking a
        // property chain per actor would dominate the run.
        struct ActorClass {
            std::string name;
            i32 netcull = -1;
            i32 mesh = -1;
            bool done = false;
        };
        struct MeshClass {
            i32 recently = -1, cached_max = -1, min_dist = -1, anim_tick = -1;
            u8 recently_byte = 0, recently_mask = 0;
            bool done = false;
        };
        std::unordered_map<addr_t, ActorClass> acls;
        std::unordered_map<addr_t, MeshClass> mcls;
        std::string self_check = "bHidden reflection check: no actor class resolved";
        // One "the anim-instance gates resolve on this client" line, filled in by the first anim
        // class the sweep meets (see collect_gates).
        std::string anim_check;
        std::set<addr_t> anim_reported;
        bool checked = false;

        struct Row {
            double d = -1.0;
            std::string cls, name;
            int hidden = -1, drawn = -1, anim = -1;
            double cull_m = -1.0, max_m = -1.0, min_m = -1.0;
        };
        std::vector<Row> rows;
        std::size_t levels = 0;

        auto collect_gates = [&](addr_t lvl) {
            if (!lvl) return;
            ++levels;
            for (addr_t a : glv.Actors(lvl)) {
                if (!sdk.mem->valid_ptr(a)) continue;
                const addr_t cls = sdk.mem->ptr_at(a + off::uobject::Class);
                if (!cls) continue;
                ActorClass& ac = acls[cls];
                if (!ac.done) {
                    ac.done = true;
                    ac.name = sdk.Name(sdk.mem->u32_at(cls + off::uobject::Name));
                    ac.netcull = refl.GetPropertyOffset(cls, "NetCullDistanceSquared");
                    ac.mesh = refl.GetPropertyOffset(cls, "Mesh");
                    if (ac.mesh < 0)
                        ac.mesh = refl.GetPropertyOffset(cls, "SkeletalMeshComponent");
                }
                if (ac.name.empty() || lower(ac.name).find(need) == std::string::npos) continue;

                Row r;
                r.cls = ac.name;
                r.name = sdk.Name(sdk.mem->u32_at(a + off::uobject::Name));
                // The validated pair, plus a one-off cross-check through reflection.
                const u8 hidden_byte = sdk.rd<u8>(a + off::aactor::HiddenFlags);
                r.hidden = (hidden_byte & off::aactor::HiddenBit) ? 1 : 0;
                if (!checked) {
                    const PropertyInfo pi = refl.GetProperty(cls, "bHidden");
                    if (pi.Offset >= 0 && pi.Addr) {
                        const u8 bo = sdk.rd<u8>(pi.Addr + off::fbool_property::ByteOffset);
                        const u8 bm = sdk.rd<u8>(pi.Addr + off::fbool_property::ByteMask);
                        const bool ok = pi.Offset == static_cast<i32>(off::aactor::HiddenFlags) &&
                                        bo == 0 && bm == off::aactor::HiddenBit;
                        char buf[192];
                        std::snprintf(buf, sizeof buf,
                                      "bHidden via reflection: offset 0x%X byte 0x%X mask 0x%X "
                                      "-> %s",
                                      pi.Offset, bo, bm,
                                      ok ? "matches Offsets.hpp" : "MISMATCH, offsets moved");
                        self_check = buf;
                        checked = true;
                    }
                }
                if (ac.netcull >= 0) {
                    const float nc = sdk.rd<float>(a + ac.netcull);
                    if (nc > 0.0f) r.cull_m = std::sqrt(static_cast<double>(nc)) / 100.0;
                }
                if (ac.mesh >= 0) {
                    const addr_t comp = sdk.mem->ptr_at(a + ac.mesh);
                    const addr_t ccls =
                        sdk.mem->valid_ptr(comp) ? sdk.mem->ptr_at(comp + off::uobject::Class) : 0;
                    if (ccls) {
                        MeshClass& mc = mcls[ccls];
                        if (!mc.done) {
                            mc.done = true;
                            const PropertyInfo pi = refl.GetProperty(ccls, "bRecentlyRendered");
                            mc.recently = pi.Offset;
                            if (pi.Offset >= 0 && pi.Addr) {
                                mc.recently_byte =
                                    sdk.rd<u8>(pi.Addr + off::fbool_property::ByteOffset);
                                mc.recently_mask =
                                    sdk.rd<u8>(pi.Addr + off::fbool_property::ByteMask);
                            }
                            mc.cached_max =
                                refl.GetPropertyOffset(ccls, "CachedMaxDrawDistance");
                            mc.min_dist = refl.GetPropertyOffset(ccls, "MinDrawDistance");
                            mc.anim_tick =
                                refl.GetPropertyOffset(ccls, "VisibilityBasedAnimTickOption");
                        }
                        if (mc.recently >= 0) {
                            const u8 b = sdk.rd<u8>(comp + static_cast<addr_t>(mc.recently) +
                                                    mc.recently_byte);
                            r.drawn = (b & mc.recently_mask) ? 1 : 0;
                        }
                        if (mc.cached_max >= 0)
                            r.max_m =
                                sdk.rd<float>(comp + static_cast<addr_t>(mc.cached_max)) / 100.0;
                        if (mc.min_dist >= 0)
                            r.min_m =
                                sdk.rd<float>(comp + static_cast<addr_t>(mc.min_dist)) / 100.0;
                        if (mc.anim_tick >= 0)
                            r.anim = sdk.rd<u8>(comp + static_cast<addr_t>(mc.anim_tick));
                        // The anim instance the dead and low-health reads go through, reported once
                        // per anim class. These are the two property names the ESP resolves on the
                        // live client (bDead for the dead gate, bHpDangerAlert for the self panel's
                        // low-health state), and this line is where the client confirms they still
                        // exist - "absent" means the feature degrades, not that it read wrong.
                        const i32 ao = refl.GetPropertyOffset(ccls, "AnimScriptInstance");
                        if (ao >= 0) {
                            const addr_t anim = sdk.mem->ptr_at(comp + static_cast<addr_t>(ao));
                            const addr_t acls =
                                sdk.mem->valid_ptr(anim) ? sdk.mem->ptr_at(anim + off::uobject::Class)
                                                         : 0;
                            if (acls && !anim_reported.count(acls)) {
                                anim_reported.insert(acls);
                                const i32 dead_off = refl.GetPropertyOffset(acls, "bDead");
                                const i32 danger_off =
                                    refl.GetPropertyOffset(acls, "bHpDangerAlert");
                                char d1[24] = "absent", d2[24] = "absent";
                                if (dead_off >= 0) std::snprintf(d1, sizeof d1, "0x%X", dead_off);
                                if (danger_off >= 0)
                                    std::snprintf(d2, sizeof d2, "0x%X", danger_off);
                                const std::string an =
                                    sdk.Name(sdk.mem->u32_at(acls + off::uobject::Name));
                                char buf[256];
                                std::snprintf(buf, sizeof buf,
                                              "  anim gates on %s: bDead @%s, bHpDangerAlert @%s",
                                              an.c_str(), d1, d2);
                                if (anim_check.empty()) anim_check = buf;
                            }
                        }
                    }
                }
                if (have_origin) {
                    const addr_t root = sdk.mem->ptr_at(a + off::aactor::RootComponent);
                    FVector loc{};
                    if (root && sdk.mem->read(root + off::uscene_component::RelativeLocation, &loc,
                                              sizeof loc)) {
                        const double dx = loc.X - ox, dy = loc.Y - oy, dz = loc.Z - oz;
                        r.d = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
                    }
                }
                rows.push_back(std::move(r));
            }
        };
        collect_gates(gw.PersistentLevel().Addr());
        for (addr_t l : gw.Levels()) collect_gates(l);
        std::sort(rows.begin(), rows.end(),
                  [](const Row& x, const Row& y) { return x.d < y.d; });

        int drawn = 0, hidden = 0, mesh_ok = 0, no_pos = 0;
        double farthest_drawn = -1.0, nearest_undrawn = -1.0;
        std::vector<std::pair<int, int>> buckets;  // per 10 m: {known, drawn}
        std::size_t bucket_count = 0;
        for (const Row& r : rows) {
            if (r.drawn == 1) ++drawn;
            if (r.hidden == 1) ++hidden;
            if (r.drawn >= 0) ++mesh_ok;
            if (r.d < 0.0) {
                ++no_pos;
                continue;
            }
            const std::size_t bi = static_cast<std::size_t>(r.d / 10.0);
            if (bi + 1 > bucket_count) {
                bucket_count = bi + 1;
                buckets.resize(bucket_count);
            }
            ++buckets[bi].first;
            if (r.drawn == 1) ++buckets[bi].second;
            if (r.drawn == 1) farthest_drawn = r.d;
            if (r.drawn == 0 && nearest_undrawn < 0.0) nearest_undrawn = r.d;
        }

        const char* const kGatesFile = "aion_gates.txt";
        char gates_path[FILENAME_MAX] = {0};
        if (!_fullpath(gates_path, kGatesFile, sizeof gates_path) || !gates_path[0])
            std::snprintf(gates_path, sizeof gates_path, "%s", kGatesFile);
        char gates_when[32];
        {
            const std::time_t now = std::time(nullptr);
            std::tm tb{};
            localtime_s(&tb, &now);
            std::snprintf(gates_when, sizeof gates_when, "%04d-%02d-%02d %02d:%02d:%02d",
                          tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday, tb.tm_hour, tb.tm_min,
                          tb.tm_sec);
        }
        std::FILE* gf = std::fopen(kGatesFile, "wb");
        if (!gf) {
            AION_WARN("could not write %s", gates_path);
            gfx.Shutdown();
            DestroyWindow(overlay);
            mem.Close();
            return 0;
        }
        std::fprintf(gf, "# What the client does with actors whose class contains '%s'.\n",
                     opt.gates_substr.c_str());
        std::fprintf(gf, "# written %s to %s\n", gates_when, gates_path);
        std::fprintf(gf, "# every column but 'hidden' is resolved by property NAME at run time;\n");
        std::fprintf(gf, "# 'hidden' is AActor::bHidden via Offsets.hpp's validated byte+mask.\n");
        std::fprintf(gf, "# 'drawn' is SkinnedMeshComponent::bRecentlyRendered - the renderer's\n");
        std::fprintf(gf, "# own answer to whether it drew the actor. 'cull' is\n");
        std::fprintf(gf, "# sqrt(NetCullDistanceSquared) in metres: the server's replication radius.\n");
        std::fprintf(gf, "# dist(m)  hidden drawn maxdraw min  cull  anim  class / name\n");
        for (const Row& r : rows) {
            char line[512];
            std::snprintf(line, sizeof line, "%8.1f %6d %5d %7.1f %5.1f %6.1f %5d  %s / %s", r.d,
                          r.hidden, r.drawn, r.max_m, r.min_m, r.cull_m, r.anim, r.cls.c_str(),
                          r.name.c_str());
            std::fprintf(gf, "%s\n", line);
        }
        std::fclose(gf);
        AION_INFO("render gates: %zu actor(s) matching '%s' across %zu level(s) -> %s", rows.size(),
                  opt.gates_substr.c_str(), levels, gates_path);
        AION_INFO("  %s", self_check.c_str());
        if (!anim_check.empty()) AION_INFO("%s", anim_check.c_str());
        AION_INFO("  origin=%s  hidden=%d  mesh-resolved=%d  without-position=%d",
                  have_origin ? "local pawn" : "UNKNOWN", hidden, mesh_ok, no_pos);
        AION_INFO("  drawn(bRecentlyRendered=1)=%d  farthest drawn=%.1f m  nearest undrawn=%.1f m",
                  drawn, farthest_drawn, nearest_undrawn);
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            if (buckets[i].first == 0) continue;
            AION_INFO("  %5zu-%5zu m  drawn %3d / %3d", i * 10, i * 10 + 10, buckets[i].second,
                      buckets[i].first);
        }
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
}

// --diag-shutter: measure the entity list and the shipped draw path over N live frames, for the
// report "one entity renders as two boxes/lines while the camera moves".
//
// It runs the real pipeline in the real order (collect+project -> re-read the camera -> reproject ->
// draw through EspRenderer) and measures, per frame:
//
//   H1  an actor in the list twice          - address duplicates, and how far apart their boxes are
//   H1b two actors on one screen position   - distinct addresses whose boxes coincide
//   H2  one entity, two visuals             - primitives a SINGLETON render emits for that entity,
//                                             against exactly what one box+marker+snapline needs
//   H3  drawn against a stale camera        - the entity's box re-projected onto the camera read
//                                             AFTER the draw, in pixels; plus whether the frame's
//                                             own reprojection ran, and how far the camera moved
//   H4  size/thickness churn                - per-frame box-size and stroke-width change
//
// It never writes to the client and never presses a key: every read is a ReadProcessMemory through
// the same helpers the ESP uses.
int ModeShutterDiag(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    ExternalMemory& mem = mc.mem;
    EntityCache& cache = mc.cache;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    const Options& opt = mc.opt;
    Config& cfg = Config::Instance();

    const int frames_wanted = opt.shutter_frames > 0 ? opt.shutter_frames : 120;
    const std::string path =
        opt.shutter_path.empty() ? ResolveDataFile("esp_shutter_diag.txt") : opt.shutter_path;

    CameraExtractor camx(&sdk);
    Projection proj;
    proj.SetViewport(gfx.Width(), gfx.Height());

    // Same three-value read the frame loop uses, so the diagnostic can never disagree with it about
    // which camera the frame was built against.
    addr_t pc = 0, pawn = 0;
    Camera cam;
    auto read_pose = [&]() -> bool {
        World world(&sdk);
        const UObject gi = world.OwningGameInstance();
        if (!gi.Addr()) return false;
        const auto lps = mem.read_tarray<addr_t>(gi.Addr() + off::ugameinstance::LocalPlayers, 8);
        if (lps.empty()) return false;
        const addr_t pc_now = mem.ptr_at(lps[0] + off::uplayer::PlayerController);
        if (!pc_now) return false;
        const addr_t pawn_now = mem.ptr_at(pc_now + off::aplayercontroller::AcknowledgedPawn);
        const Camera cam_now = camx.FromPlayerController(pc_now);
        if (!cam_now.valid) return false;
        pc = pc_now;
        pawn = pawn_now;
        cam = cam_now;
        return true;
    };

    // Counts the primitives a frame emits, so "one entity, two visuals" can be answered by the
    // renderer itself instead of by reading its code.
    struct CountOverlay final : IOverlay {
        int w = 1920, h = 1080;
        int rects = 0, lines = 0, fills = 0, texts = 0;
        bool Ready() const override { return true; }
        int Width() const override { return w; }
        int Height() const override { return h; }
        void BeginFrame() override {}
        void Line(float, float, float, float, Color, float) override { ++lines; }
        void Rect(float, float, float, float, Color, float) override { ++rects; }
        void FillRect(float, float, float, float, Color) override { ++fills; }
        void Text(float, float, const std::string&, Color, float) override { ++texts; }
        void EndFrame() override {}
    };
    CountOverlay rec;
    rec.w = gfx.Width();
    rec.h = gfx.Height();

    struct Offender {
        std::string what;
        double v = 0.0;
        addr_t a = 0, b = 0;
        std::string na, nb, ca, cb;
        int count = 0;
    };
    std::vector<Offender> dupes, coincident, extra_prims;

    // Per-frame extremes, accumulated.
    double max_frame_move_px = 0.0, max_yaw_step = 0.0, max_cam_step_u = 0.0;
    double max_resid_px = 0.0, sum_resid_px = 0.0, max_box_h = 0.0, min_box_h = 1e30;
    std::size_t resid_samples = 0, frames_run = 0, frames_reproj = 0, frames_moving = 0;
    bool first = true;
    std::vector<FVector2D> prev_centres;
    std::vector<addr_t> prev_addrs;
    Camera prev_cam;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fprintf(f, "# ESP shuttering diagnostic - one entity, two visuals?\n");
        std::fprintf(f, "# config: esp_box=%d corners=%d thickness=%.3f snapline=%d offscreen=%d\n",
                     (int)cfg.esp_box, (int)cfg.esp_box_corners, cfg.esp_thickness,
                     cfg.snapline, (int)cfg.esp_offscreen);
        std::fprintf(f, "#         max_distance_m=%.1f esp_hidden=%d hidden_range_m=%.1f show_all=%d\n",
                     cfg.max_distance_m, (int)cfg.esp_hidden, cfg.hidden_range_m,
                     (int)cfg.esp_show_all);
        std::fprintf(f, "# viewport %dx%d, %d frames requested, pid-attached read-only\n\n",
                     gfx.Width(), gfx.Height(), frames_wanted);
    }

    for (int fr = 0; fr < frames_wanted && f; ++fr) {
        if (!read_pose()) continue;
        if (pawn) cache.set_local_pawn(pawn);
        // The shipped order: collect and project, then re-read the pose and reproject.
        cache.UpdateBulk(&sdk, cam, proj, cfg.hide_dead, true);
        const Camera cam_a = cam;
        const bool reproj_ran = read_pose();
        if (reproj_ran) cache.Reproject(proj, cam);
        const std::vector<Entity>& es = cache.Entities();
        ++frames_run;
        if (reproj_ran) ++frames_reproj;

        // ---- H3: how far the drawn geometry sits from the camera as it is NOW ----------------
        // A third read, after the draw pass the frame loop would have performed. The delta is the
        // pipeline's own age; it is not a defect unless the frame's reprojection did not happen.
        Camera cam_c;
        {
            const addr_t cam_mgr = mem.ptr_at(pc + off::aplayercontroller::PlayerCameraManager);
            cam_c = camx.FromCameraManager(cam_mgr);
            if (!cam_c.valid) cam_c = cam;
        }
        std::vector<Entity> now = es;
        if (cam_c.valid)
            for (auto& e : now) cache.ProjectEntity(e, cam_c, proj);

        double frame_move = 0.0, resid = 0.0;
        std::vector<FVector2D> centres;
        centres.reserve(es.size());
        for (std::size_t i = 0; i < es.size(); ++i) {
            const Entity& e = es[i];
            if (e.OnScreen && (e.BoundsMax.X - e.BoundsMin.X) >= 2.f &&
                (e.BoundsMax.Y - e.BoundsMin.Y) >= 2.f) {
                const FVector2D c{(e.BoundsMin.X + e.BoundsMax.X) * 0.5f,
                                  (e.BoundsMin.Y + e.BoundsMax.Y) * 0.5f};
                centres.push_back(c);
                const double bh = e.BoundsMax.Y - e.BoundsMin.Y;
                if (bh > max_box_h) max_box_h = bh;
                if (bh < min_box_h) min_box_h = bh;
                // H3 residual: the same point re-projected onto the newest camera.
                if (cam_c.valid && i < now.size() && now[i].OnScreen) {
                    const FVector2D c2{(now[i].BoundsMin.X + now[i].BoundsMax.X) * 0.5f,
                                       (now[i].BoundsMin.Y + now[i].BoundsMax.Y) * 0.5f};
                    const double d = std::sqrt((c2.X - c.X) * (c2.X - c.X) +
                                               (c2.Y - c.Y) * (c2.Y - c.Y));
                    if (d > resid) resid = d;
                    sum_resid_px += d;
                    ++resid_samples;
                }
            }

            // ---- H1: the same actor twice -----------------------------------------------------
            int count = 0;
            for (std::size_t j = 0; j < es.size(); ++j)
                if (es[j].Addr == e.Addr) ++count;
            if (count > 1 && i == 0) {
                // Reported once per address, from the first copy, with the spread of the copies'
                // boxes - two entries for one actor only read as two visuals if they differ.
                double spread = 0.0;
                bool have = false;
                FVector2D c0;
                for (const auto& o : es) {
                    if (o.Addr != e.Addr) continue;
                    const FVector2D oc{(o.BoundsMin.X + o.BoundsMax.X) * 0.5f,
                                       (o.BoundsMin.Y + o.BoundsMax.Y) * 0.5f};
                    if (!have) { c0 = oc; have = true; continue; }
                    const double d = std::sqrt((oc.X - c0.X) * (oc.X - c0.X) +
                                               (oc.Y - c0.Y) * (oc.Y - c0.Y));
                    if (d > spread) spread = d;
                }
                Offender off;
                off.what = "duplicate address in the entity list";
                off.a = e.Addr;
                off.count = count;
                off.v = spread;
                off.na = e.Name;
                off.ca = e.ClassName;
                dupes.push_back(std::move(off));
            }

            // ---- H1b: two DIFFERENT actors drawn on one screen position ----------------------
            // The box, not the origin: two actors standing together is what a user reads as one
            // entity drawn twice. Same-frame, so a stationary camera cannot hide it.
            if (!e.OnScreen) continue;
            const float ew = e.BoundsMax.X - e.BoundsMin.X, eh = e.BoundsMax.Y - e.BoundsMin.Y;
            if (ew < 2.f || eh < 2.f) continue;
            for (std::size_t j = i + 1; j < es.size(); ++j) {
                const Entity& o = es[j];
                if (o.Addr == e.Addr || !o.OnScreen) continue;
                const float ow = o.BoundsMax.X - o.BoundsMin.X, oh = o.BoundsMax.Y - o.BoundsMin.Y;
                if (ow < 2.f || oh < 2.f) continue;
                const float ix = (std::min)(e.BoundsMax.X, o.BoundsMax.X) -
                                 (std::max)(e.BoundsMin.X, o.BoundsMin.X);
                const float iy = (std::min)(e.BoundsMax.Y, o.BoundsMax.Y) -
                                 (std::max)(e.BoundsMin.Y, o.BoundsMin.Y);
                if (ix <= 0.f || iy <= 0.f) continue;
                const float inter = ix * iy;
                const float uni = ew * eh + ow * oh - inter;
                const double iou = uni > 0.f ? (double)inter / uni : 0.0;
                if (iou < 0.6) continue;
                Offender off;
                off.what = "two actors share one screen position";
                off.a = e.Addr;
                off.b = o.Addr;
                off.v = iou * 100.0;
                off.na = e.Name;
                off.nb = o.Name;
                off.ca = e.ClassName;
                off.cb = o.ClassName;
                coincident.push_back(std::move(off));
            }

            // ---- H2/H4: what the shipped renderer emits for THIS entity alone ----------------
            // A singleton list: one box and one snapline. Anything beyond that is a second visual
            // for one entity.
            {
                const std::vector<Entity> one{e};
                rec.rects = rec.lines = rec.fills = rec.texts = 0;
                EspRenderer().RenderEntities(rec, cam, one, cfg);
                const int want_strokes = cfg.esp_box_corners ? (cfg.esp_box ? 8 : 0) : 0;
                const int want_rects = (cfg.esp_box && !cfg.esp_box_corners) ? 1 : 0;
                const int want_lines = want_strokes +
                                       (cfg.snapline != SnaplineOff ? 1 : 0);
                const int extra_rects = rec.rects - want_rects;
                const int extra_lines = rec.lines - want_lines;
                if (extra_rects > 0 || extra_lines > 0) {
                    Offender off;
                    off.what = "one entity, more than one visual";
                    off.a = e.Addr;
                    off.count = (extra_rects > 0 ? extra_rects : extra_lines);
                    off.v = (double)(rec.rects * 100 + rec.lines);
                    off.na = e.Name;
                    off.ca = e.ClassName;
                    extra_prims.push_back(std::move(off));
                }
            }
        }

        // Frame-to-frame motion: how far the same actor's box centre moved since the last frame.
        if (!first) {
            for (std::size_t i = 0; i < centres.size() && i < prev_centres.size(); ++i) {
                if (i < prev_addrs.size() && prev_addrs[i] != es[i].Addr) continue;  // list re-ordered
                const double d = std::sqrt((centres[i].X - prev_centres[i].X) *
                                               (centres[i].X - prev_centres[i].X) +
                                           (centres[i].Y - prev_centres[i].Y) *
                                               (centres[i].Y - prev_centres[i].Y));
                if (d > frame_move) frame_move = d;
            }
            double dyaw = std::fabs(cam.Rotation.Yaw - prev_cam.Rotation.Yaw);
            if (dyaw > 180.0) dyaw = 360.0 - dyaw;
            if (dyaw > max_yaw_step) max_yaw_step = dyaw;
            const double step = std::sqrt(
                (cam.Location.X - prev_cam.Location.X) * (cam.Location.X - prev_cam.Location.X) +
                (cam.Location.Y - prev_cam.Location.Y) * (cam.Location.Y - prev_cam.Location.Y) +
                (cam.Location.Z - prev_cam.Location.Z) * (cam.Location.Z - prev_cam.Location.Z));
            if (step > max_cam_step_u) max_cam_step_u = step;
        }
        prev_centres = centres;
        prev_addrs.clear();
        for (const auto& e : es) prev_addrs.push_back(e.Addr);
        prev_cam = cam;
        first = false;

        if (frame_move > max_frame_move_px) max_frame_move_px = frame_move;
        if (resid > max_resid_px) max_resid_px = resid;
        if (frame_move > 0.5) ++frames_moving;

        // A line per frame that found something, plus a heartbeat every 20 frames.
        const bool interesting = !dupes.empty() || !coincident.empty() || !extra_prims.empty() ||
                                 resid > 2.0 || !reproj_ran || !cam_a.valid;
        if (f && (interesting || (fr % 20) == 0)) {
            std::fprintf(f,
                         "f=%3d entities=%3zu on=%3zu cam=(%.0f,%.0f,%.0f) yaw=%.2f reproj=%d "
                         "boxH=%.0f..%.0f moveMax=%.2fpx residMax=%.2fpx dupes=%zu coincident=%zu "
                         "extraPrims=%zu\n",
                         fr, es.size(), centres.size(), cam.Location.X, cam.Location.Y,
                         cam.Location.Z, cam.Rotation.Yaw, (int)reproj_ran,
                         centres.empty() ? 0.0 : min_box_h, max_box_h, frame_move, resid,
                         dupes.size(), coincident.size(), extra_prims.size());
        }
        // Keep the tables from growing without bound on a long run: the report only needs the
        // distinct causes, and each entry is keyed by the actors involved.
        if (dupes.size() > 200) dupes.resize(200);
        if (coincident.size() > 200) coincident.resize(200);
        if (extra_prims.size() > 200) extra_prims.resize(200);
    }

    const double mean_resid = resid_samples ? sum_resid_px / (double)resid_samples : 0.0;
    if (f) {
        std::fprintf(f, "\n## summary\n");
        std::fprintf(f, "frames=%zu reproj_ran=%zu (%.0f%%) camera_valid_frames=%zu\n", frames_run,
                     frames_reproj, frames_run ? 100.0 * (double)frames_reproj / (double)frames_run
                                               : 0.0,
                     frames_run);
        std::fprintf(f, "camera moved in %zu/%zu frames; max step %.1f u, max yaw step %.3f deg\n",
                     frames_moving, frames_run, max_cam_step_u, max_yaw_step);
        std::fprintf(f, "box height %.0f..%.0f px; max frame-to-frame box move %.2f px\n", min_box_h,
                     max_box_h, max_frame_move_px);
        std::fprintf(f, "H3 drawn-vs-newest-camera: mean %.3f px, max %.3f px over %zu samples\n",
                     mean_resid, max_resid_px, resid_samples);
        std::fprintf(f, "H1  duplicate addresses      : %zu\n", dupes.size());
        std::fprintf(f, "H1b coincident actor pairs   : %zu\n", coincident.size());
        std::fprintf(f, "H2  one entity, >1 visual    : %zu\n", extra_prims.size());
        const auto dump = [&](const char* title, const std::vector<Offender>& v) {
            if (v.empty()) return;
            std::fprintf(f, "\n### %s\n", title);
            for (std::size_t i = 0; i < v.size() && i < 40; ++i) {
                const Offender& o = v[i];
                if (o.b)
                    std::fprintf(f, "  0x%llX %s [%s] <> 0x%llX %s [%s]  iou=%.0f%%\n",
                                 (unsigned long long)o.a, o.na.c_str(), o.ca.c_str(),
                                 (unsigned long long)o.b, o.nb.c_str(), o.cb.c_str(), o.v);
                else
                    std::fprintf(f, "  0x%llX %s [%s]  n=%d spread/extra=%.2f\n",
                                 (unsigned long long)o.a, o.na.c_str(), o.ca.c_str(), o.count,
                                 o.v);
            }
        };
        dump("H1 duplicate addresses (same actor listed twice; spread = distance between copies)",
             dupes);
        dump("H1b coincident pairs (two different actors on one screen position; iou = overlap)",
             coincident);
        dump("H2 one entity, more than one visual (primitive counts over one box's worth)",
             extra_prims);
        std::fprintf(f, "\n## verdict input\n");
        std::fprintf(f,
                     "one visual per entity: %s (H2=%zu, H1=%zu)\n",
                     (extra_prims.empty() && dupes.empty()) ? "yes" : "NO", extra_prims.size(),
                     dupes.size());
        std::fprintf(f,
                     "two actors on one position: %s (H1b=%zu) - this is the only way the DRAW path "
                     "can show two boxes for what the user calls one entity\n",
                     coincident.empty() ? "none seen" : "SEEN", coincident.size());
        std::fprintf(f,
                     "drawn geometry vs newest camera: %.3f px mean - %s\n", mean_resid,
                     (mean_resid < 2.0 || frames_run == 0)
                         ? "frame built against the camera it was drawn with"
                         : "geometry lags the camera (see reproj column)");
        std::fclose(f);
    }

    AION_INFO("shutter diag: %zu frame(s), report -> %s", frames_run, path.c_str());
    AION_INFO("  camera moved in %zu/%zu frames (max %.3f deg yaw, %.1f u step); box move max %.2f px",
              frames_moving, frames_run, max_yaw_step, max_cam_step_u, max_frame_move_px);
    AION_INFO("  H1 duplicate addresses=%zu  H1b coincident pairs=%zu  H2 extra visuals=%zu",
              dupes.size(), coincident.size(), extra_prims.size());
    AION_INFO("  H3 drawn-vs-newest-camera %zu sample(s): mean %.3f px, max %.3f px; reproj ran %zu/%zu",
              resid_samples, mean_resid, max_resid_px, frames_reproj, frames_run);
    for (std::size_t i = 0; i < coincident.size() && i < 5; ++i)
        AION_INFO("  coincident: %s [%s] <> %s [%s] iou=%.0f%%", coincident[i].na.c_str(),
                  coincident[i].ca.c_str(), coincident[i].nb.c_str(), coincident[i].cb.c_str(),
                  coincident[i].v);
    for (std::size_t i = 0; i < dupes.size() && i < 5; ++i)
        AION_INFO("  duplicate: %s [%s] listed %d times, copies %.1f px apart", dupes[i].na.c_str(),
                  dupes[i].ca.c_str(), dupes[i].count, dupes[i].v);
    for (std::size_t i = 0; i < extra_prims.size() && i < 5; ++i)
        AION_INFO("  extra visual: %s [%s] +%d primitive(s)", extra_prims[i].na.c_str(),
                  extra_prims[i].ca.c_str(), extra_prims[i].count);

    gfx.Shutdown();
    DestroyWindow(overlay);
    mem.Close();
    return 0;
}

// --find-hud: locate the client's own-character HUD widget and read its HP/MP bars. Read-only.
//
// This is the same search the frame reader runs, with an unbudgeted slice, so it is how that reader
// is checked against a live client - and how a "HP/MP unknown" gets answered with what the object
// table actually contains instead of with a guess.
int ModeFindHud(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    ExternalMemory& mem = mc.mem;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;

    SelfHudFinder finder;
    finder.Reset();
    const auto t0 = std::chrono::steady_clock::now();
    while (!finder.Step(&sdk, 20000, true) && !finder.finished()) {
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    AION_INFO("find-hud: %d/%d object(s) in %.2fs -> %zu instance(s) of a class starting with '%s'",
              finder.scanned(), finder.total(), secs, finder.hits().size(), SelfHudClassPrefix());
    Reflection refl(&sdk);
    if (finder.found()) {
        // Every candidate, with the state of its two bar pointers: the live HUD is the one that has
        // them, a stale instance from an earlier UI state is the one that does not.
        for (std::size_t i = 0; i < finder.hits().size() && i < 8; ++i) {
            const addr_t w = finder.hits()[i];
            const addr_t cls = sdk.mem->ptr_at(w + off::uobject::Class);
            const std::string own = sdk.Name(sdk.mem->u32_at(w + off::uobject::Name));
            const i32 hp = refl.GetPropertyOffset(cls, "P_PBar_Hp");
            const i32 mp = refl.GetPropertyOffset(cls, "P_PBar_Mp");
            const addr_t hb = hp >= 0 ? sdk.mem->ptr_at(w + (addr_t)hp) : 0;
            const addr_t mb = mp >= 0 ? sdk.mem->ptr_at(w + (addr_t)mp) : 0;
            const addr_t bcls = hb ? sdk.mem->ptr_at(hb + off::uobject::Class) : 0;
            const i32 pct = bcls ? refl.GetPropertyOffset(bcls, "Percent") : -1;
            AION_INFO("  [%zu] 0x%llX own='%s' class='%s' hp@+0x%X bar=%s mp=%s Percent@+0x%X=%.4f", i,
                      (unsigned long long)w, own.c_str(),
                      sdk.Name(sdk.mem->u32_at(cls + off::uobject::Name)).c_str(),
                      hp < 0 ? 0u : (unsigned)hp, hb ? "yes" : "NULL", mb ? "yes" : "NULL", pct,
                      (pct >= 0 && hb) ? sdk.rd<float>(hb + (addr_t)pct) : -1.0f);
        }
        // Then the shipped path, driven exactly as the frame loop drives it: the reader searches,
        // picks the first candidate whose bars exist, and reads them.
        SelfVitalsReader reader;
        for (int i = 0; i < 800 && !reader.ready(); ++i) reader.Read(&sdk, /*search=*/true);
        const SelfVitals v = reader.Read(&sdk, /*search=*/true);
        AION_INFO("  reader: widget %s, HP %.0f%% (live=%d) MP %.0f%% (live=%d) note='%s'",
                  reader.ready() ? "RESOLVED" : "not resolved", v.hp < 0 ? -1.0 : v.hp * 100.0,
                  (int)v.hp_live, v.mp < 0 ? -1.0 : v.mp * 100.0, (int)v.mp_live,
                  reader.last_note().c_str());
    } else {
        AION_INFO("  no instance of a class starting with '%s' in this client.",
                  SelfHudClassPrefix());
    }
    // The census always runs: when the widget cannot be read, this is what answers "so what IS in
    // there?" - every class name carrying "HUD", and every object whose own name carries "UserInfo".
    if (!finder.hud_classes().empty()) {
        // A class with two or more objects is the interesting one: a class's own default object is
        // always present, so 2+ means something actually instantiated it - which is what a live HUD
        // widget looks like. Those are printed first, by count.
        std::vector<std::pair<std::string, int>> cls = finder.hud_classes();
        std::sort(cls.begin(), cls.end(),
                  [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                      return a.second > b.second;
                  });
        AION_INFO("  class names carrying 'HUD' (%zu unique), most instantiated first:",
                  cls.size());
        for (std::size_t i = 0; i < cls.size() && i < 28; ++i)
            AION_INFO("    %-56s x%d", cls[i].first.c_str(), cls[i].second);
    }
    if (!finder.userinfo_objects().empty()) {
        AION_INFO("  objects whose own name carries 'UserInfo' (%zu):",
                  finder.userinfo_objects().size());
        for (const auto& u : finder.userinfo_objects()) AION_INFO("    %s", u.c_str());
    }
    gfx.Shutdown();
    DestroyWindow(overlay);
    mem.Close();
    return 0;
}

// --dialog-map: the last read-only route to the local player's real HP/MP.
//
// AionGameInstance +0x03C8 -> AionUISystem, whose own LoadedDialogMap is at +0x0060: a reflected
// MapProperty, 0x50 bytes (build/sdk_dump.txt line 13776). Every HUD widget class in this client has
// exactly one object in the table - its class default object, whose bar pointers are null by
// construction (build/self_hpmp_research.md) - so a live HUD instance could only live inside a
// container the client owns, and this map is the one candidate. This walks it.
//
// The element layout is established, never assumed. FMapProperty's two inner properties are found by
// validating pointers on the property object itself (each must carry an FFieldClass in the FProperty
// family and a readable FName); the pair stride and the value offset are then derived from those two
// properties' own reflected ElementSize, not from a constant. Every element is finally checked
// against that derivation: a wrong stride cannot produce slot after slot that reads as a named
// UObject. If that chain does not hold the report says UNRESOLVED rather than inventing a layout.
static const char* const kPropertyClassNames[] = {
    "ArrayProperty", "BoolProperty", "ByteProperty", "ClassProperty", "ClassPtrProperty",
    "DelegateProperty", "DoubleProperty", "EnumProperty", "FieldPathProperty", "FloatProperty",
    "IntProperty", "InterfaceProperty", "LazyObjectProperty", "MapProperty",
    "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty", "NameProperty",
    "ObjectProperty", "OptionalProperty", "SetProperty", "SoftClassProperty", "SoftObjectProperty",
    "StrProperty", "StructProperty", "TextProperty", "WeakObjectProperty"};

static bool IsPropertyClassName(const std::string& n) {
    for (const char* c : kPropertyClassNames)
        if (n == c) return true;
    return false;
}

// The FProperty family name of an FField, from its FFieldClass (which begins with the FName).
static std::string FFieldClassName(Sdk& sdk, addr_t field) {
    const addr_t fc = sdk.mem->ptr_at(field + off::ffield::ClassPrivate);
    if (!fc || !sdk.mem->valid_ptr(fc)) return {};
    return sdk.Name(sdk.mem->u32_at(fc));
}

// A pointer is taken to be a UObject only when both of its FNames resolve: an arbitrary 8 bytes
// almost never lands on a pair of readable names. Reads only.
static bool ReadableObject(Sdk& sdk, addr_t p, std::string* own, std::string* cls) {
    if (!p || !sdk.mem->valid_ptr(p)) return false;
    const addr_t c = sdk.mem->ptr_at(p + off::uobject::Class);
    if (!c || !sdk.mem->valid_ptr(c)) return false;
    const std::string cn = sdk.Name(sdk.mem->u32_at(c + off::uobject::Name));
    const std::string on = sdk.Name(sdk.mem->u32_at(p + off::uobject::Name));
    if (cn.empty() || on.empty()) return false;
    for (char ch : cn)
        if (ch < ' ' || ch > '~') return false;
    if (own) *own = on;
    if (cls) *cls = cn;
    return true;
}

static i32 AlignUp(i32 v, i32 a) { return a <= 1 ? v : (v + a - 1) / a * a; }

int ModeDialogMap(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    ExternalMemory& mem = mc.mem;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;

    auto finish = [&](const char* verdict) {
        AION_INFO("dialog-map: VERDICT=%s", verdict);
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return 0;
    };

    Reflection refl(&sdk);
    AionSystems sys(&sdk, &refl);
    const UObject ui = sys.UISystem();
    if (!ui.Addr()) {
        AION_INFO("dialog-map: AionUISystem unresolved (GameInstance or its +0x3C8 empty)");
        return finish("UNRESOLVED");
    }
    const addr_t ui_cls = mem.ptr_at(ui.Addr() + off::uobject::Class);
    AION_INFO("dialog-map: AionUISystem=0x%llX class='%s'", (unsigned long long)ui.Addr(),
              sdk.Name(mem.u32_at(ui_cls + off::uobject::Name)).c_str());
    const PropertyInfo map = refl.GetProperty(ui_cls, "LoadedDialogMap");
    AION_INFO("dialog-map: LoadedDialogMap type='%s' offset=+0x%X size=%d property=0x%llX",
              map.Type.c_str(), (unsigned)map.Offset, map.Size, (unsigned long long)map.Addr);
    if (map.Addr == 0 || map.Offset < 0) {
        AION_INFO("dialog-map: the class does not declare LoadedDialogMap -> nothing to walk");
        return finish("UNRESOLVED");
    }

    // 1. The map's inner properties, by validation on the property object (no fixed offsets).
    struct InnerProp {
        addr_t addr;
        u32 at;
        std::string type, name;
        i32 size;
    };
    std::vector<InnerProp> inner;
    for (u32 at = 0x18; at + 8 <= 0x80; at += 8) {
        const addr_t p = mem.ptr_at(map.Addr + at);
        if (!p || !mem.valid_ptr(p)) continue;
        const std::string t = FFieldClassName(sdk, p);
        if (!IsPropertyClassName(t)) continue;
        const std::string nm = sdk.Name(mem.u32_at(p + off::ffield::NamePrivate));
        if (nm.empty()) continue;
        inner.push_back({p, at, t, nm, sdk.rd<i32>(p + off::fproperty::ElementSize)});
        AION_INFO("  inner[%zu] at property+0x%02X: 0x%llX '%s' %s size=%d", inner.size() - 1, at,
                  (unsigned long long)p, nm.c_str(), t.c_str(), inner.back().size);
    }
    // The pair's two properties are named by UHT after the map itself: the key is "<Name>_Key" and
    // the value carries the map's own name. Read live from this client: 'LoadedDialogMap_Key'
    // StructProperty size=12 and 'LoadedDialogMap' ObjectProperty size=8 - so the map is
    // key -> UObject*, which is exactly the shape a widget container has. The naming is checked, not
    // assumed; the values are then checked against the layout it implies.
    const std::string want_key = map.Name + "_Key";
    const InnerProp* key = nullptr;
    const InnerProp* val = nullptr;
    for (const auto& p : inner)
        if (!key && p.name == want_key) key = &p;
    for (const auto& p : inner)
        if (!val && p.name == map.Name && &p != key) val = &p;
    if (!key || !val) {
        AION_INFO("dialog-map: the map property exposes %zu inner candidate(s) but none pair as "
                  "'%s' / '%s' -> its element schema is not established", inner.size(),
                  want_key.c_str(), map.Name.c_str());
        return finish("UNRESOLVED");
    }
    const i32 ksz = key->size > 0 ? key->size : 8;
    const i32 vsz = val->size > 0 ? val->size : 8;
    AION_INFO("dialog-map: key '%s' %s size=%d | value '%s' %s size=%d", key->name.c_str(),
              key->type.c_str(), ksz, val->name.c_str(), val->type.c_str(), vsz);

    // What the key actually is, when it is a struct: the FStructProperty points at a UScriptStruct,
    // whose own class name is "ScriptStruct". Found by validating pointers, not by a fixed offset.
    if (key->type == "StructProperty") {
        for (u32 at = 0x30; at + 8 <= 0x80; at += 8) {
            const addr_t p = mem.ptr_at(key->addr + at);
            if (!p || !mem.valid_ptr(p)) continue;
            const addr_t pc = mem.ptr_at(p + off::uobject::Class);
            if (!pc) continue;
            if (sdk.Name(mem.u32_at(pc + off::uobject::Name)) != "ScriptStruct") continue;
            const std::string sn = sdk.Name(mem.u32_at(p + off::uobject::Name));
            if (sn.empty()) continue;
            AION_INFO("    key struct is '%s'", sn.c_str());
            break;
        }
    }

    // The pair's stride and value offset follow from the two properties' own reflected sizes: a
    // TPair<Key,Value> is the key aligned up, then the value. The alignment of the pair is the
    // larger of its members' - 8 while the value is a pointer, 4 if a patch ever made the pair
    // 4-aligned. Both are carried, and the data decides which one holds.
    struct Layout {
        i32 stride, voff;
        const char* why;
    } hyps[2] = {{AlignUp(ksz, 8) + AlignUp(vsz, 8), AlignUp(ksz, 8), "pair alignment 8"},
                 {AlignUp(ksz, 4) + AlignUp(vsz, 4), AlignUp(ksz, 4), "pair alignment 4"}};
    if (hyps[0].stride <= 0 || hyps[0].stride > 0x100) {
        AION_INFO("dialog-map: derived stride %d is not a plausible element size", hyps[0].stride);
        return finish("UNRESOLVED");
    }

    // 2. The container: a TSet of pairs, so {TArray<Element> Data; TBitArray AllocationFlags; ...}.
    const addr_t m = ui.Addr() + (addr_t)map.Offset;
    const addr_t arr = mem.ptr_at(m + 0x00);
    const i32 num = sdk.rd<i32>(m + 0x08);
    const i32 cap = sdk.rd<i32>(m + 0x0C);
    const addr_t flags = mem.ptr_at(m + 0x10);
    const i32 nbits = sdk.rd<i32>(m + 0x18);
    AION_INFO("dialog-map: TMap data=0x%llX num=%d max=%d allocFlags=0x%llX numBits=%d",
              (unsigned long long)arr, num, cap, (unsigned long long)flags, nbits);
    if (!arr || !mem.valid_ptr(arr) || num < 0 || cap < num || cap > (1 << 21)) {
        AION_INFO("dialog-map: the map header does not read as a container -> cannot walk it");
        return finish("UNRESOLVED");
    }
    const bool use_flags = flags && mem.valid_ptr(flags) && nbits >= num && nbits <= (1 << 21);
    auto allocated = [&](i32 i) {
        if (!use_flags) return true;
        return ((mem.u32_at(flags + (addr_t)(i / 32) * 4) >> (i % 32)) & 1u) != 0;
    };

    // 3. Let the data choose between the candidate layouts: every allocated slot is read at every
    // 4-byte offset inside the element, and the layout whose value offset reads as a named UObject
    // most often wins. A wrong stride cannot do that slot after slot.
    const i32 slots = num < 64 ? num : 64;
    int allocated_seen = 0;
    i32 use_off = -1, use_stride = hyps[0].stride;
    int use_hits = 0;
    for (int h = 0; h < 2; ++h) {
        if (h == 1 && hyps[1].stride == hyps[0].stride) continue;
        int hist[64] = {0};
        int seen = 0;
        for (i32 i = 0; i < slots; ++i) {
            if (!allocated(i)) continue;
            ++seen;
            for (i32 o = 0; o + 8 <= hyps[h].stride && o / 4 < 64; o += 4) {
                std::string a, b;
                if (ReadableObject(sdk, mem.ptr_at(arr + (addr_t)i * hyps[h].stride + o), &a, &b))
                    hist[o / 4]++;
            }
        }
        if (h == 0) allocated_seen = seen;
        for (i32 o = 0; o + 8 <= hyps[h].stride && o / 4 < 64; o += 4)
            if (hist[o / 4])
                AION_INFO("    layout %s (stride %d): offset +%d reads as a named UObject in %d/%d "
                          "slot(s)", hyps[h].why, hyps[h].stride, o, hist[o / 4], seen);
        const int derived = hist[hyps[h].voff / 4];
        if (derived > use_hits) {
            use_hits = derived;
            use_off = hyps[h].voff;
            use_stride = hyps[h].stride;
        }
        if (h == 0 && derived == 0) {
            // Nothing at the offset the sizes imply: fall back to whichever offset inside the
            // element the data does confirm, as long as more than one slot confirms it.
            i32 best = -1, best_hits = 0;
            for (i32 o = 0; o + 8 <= hyps[h].stride && o / 4 < 64; o += 4)
                if (hist[o / 4] > best_hits) {
                    best_hits = hist[o / 4];
                    best = o;
                }
            if (best >= 0 && best_hits >= 2) {
                AION_INFO("    the derived value offset +%d matched nothing; +%d matched %d - using "
                          "the offset the data confirms", hyps[h].voff, best, best_hits);
                use_off = best;
                use_stride = hyps[h].stride;
                use_hits = best_hits;
            }
        }
    }
    if (use_off < 0 || use_hits == 0) {
        AION_INFO("dialog-map: no offset inside the element reads as a named UObject in any of the "
                  "%d allocated slot(s) -> the element layout is not established", allocated_seen);
        return finish("UNRESOLVED");
    }
    AION_INFO("dialog-map: walking with stride %d, value at +%d (%d/%d slot(s) confirmed)",
              use_stride, use_off, use_hits, allocated_seen);

    // 4. Walk the elements. Each is reported (up to 16); a widget is followed to its HP/MP bars the
    // same way the frame reader does it, by property name.
    int valid_objs = 0, widgets = 0, bars_live = 0;
    float hp_val = -1.f, mp_val = -1.f;
    const i32 show = num < 16 ? num : 16;
    for (i32 i = 0; i < num; ++i) {
        if (!allocated(i)) continue;
        const addr_t el = arr + (addr_t)i * use_stride;
        std::string keytxt;
        if (key->type == "NameProperty") {
            const u32 cmp = mem.u32_at(el);
            keytxt = sdk.Name(cmp);
            if (keytxt.empty()) keytxt = "FName(raw " + std::to_string(cmp) + ")";
        } else if (key->type == "IntProperty" || key->type == "EnumProperty" ||
                   key->type == "ByteProperty") {
            keytxt = std::to_string(mem.u32_at(el));
        } else {
            // A struct key: its first 8 bytes are reported raw, which is what the evidence supports.
            char buf[48];
            std::snprintf(buf, sizeof buf, "<%s 0x%08X%08X>", key->type.c_str(), mem.u32_at(el + 4),
                          mem.u32_at(el));
            keytxt = buf;
        }
        const addr_t cand = mem.ptr_at(el + use_off);
        std::string own, cls;
        if (!ReadableObject(sdk, cand, &own, &cls)) {
            if (i < show)
                AION_INFO("  [%d] key='%s' value@+%d=0x%llX does not read as a UObject", i,
                          keytxt.c_str(), use_off, (unsigned long long)cand);
            continue;
        }
        ++valid_objs;
        if (i < show)
            AION_INFO("  [%d] key='%s' value=0x%llX '%s' class='%s'", i, keytxt.c_str(),
                      (unsigned long long)cand, own.c_str(), cls.c_str());
        if (!refl.IsA(cand, "AionWidget")) continue;
        ++widgets;
        const addr_t wcls = mem.ptr_at(cand + off::uobject::Class);
        const i32 ho = refl.GetPropertyOffset(wcls, "P_PBar_Hp");
        const i32 mo = refl.GetPropertyOffset(wcls, "P_PBar_Mp");
        const addr_t hb = ho >= 0 ? mem.ptr_at(cand + (addr_t)ho) : 0;
        const addr_t mb = mo >= 0 ? mem.ptr_at(cand + (addr_t)mo) : 0;
        const i32 po =
            hb ? refl.GetPropertyOffset(mem.ptr_at(hb + off::uobject::Class), "Percent") : -1;
        const i32 pmo =
            mb ? refl.GetPropertyOffset(mem.ptr_at(mb + off::uobject::Class), "Percent") : -1;
        const float h = po >= 0 ? sdk.rd<float>(hb + (addr_t)po) : -1.f;
        const float mp = pmo >= 0 ? sdk.rd<float>(mb + (addr_t)pmo) : -1.f;
        const bool h_ok = h >= 0.f && h <= 1.f;
        const bool mp_ok = mp >= 0.f && mp <= 1.f;
        if (h_ok || mp_ok) {
            ++bars_live;
            if (h_ok) hp_val = h;
            if (mp_ok) mp_val = mp;
        }
        AION_INFO("      widget 0x%llX: P_PBar_Hp@+0x%X bar=%s %.4f | P_PBar_Mp@+0x%X bar=%s %.4f",
                  (unsigned long long)cand, (unsigned)ho, hb ? "LIVE" : "NULL", h, (unsigned)mo,
                  mb ? "LIVE" : "NULL", mp);
    }

    // 5. Independently of the map: is there a live instance of the HUD widget class anywhere in the
    // table? Classes first, so the object pass is one read each.
    std::unordered_map<addr_t, int> derived;
    for (i32 i = 0; i < sdk.objects.Num(); ++i) {
        const addr_t o = sdk.objects.ObjectAt(i);
        if (!o) continue;
        const addr_t c = mem.ptr_at(o + off::uobject::Class);
        if (!c || sdk.Name(mem.u32_at(c + off::uobject::Name)) != "Class") continue;
        for (addr_t k = o; k; k = mem.ptr_at(k + off::ustruct::SuperStruct)) {
            if (sdk.Name(mem.u32_at(k + off::uobject::Name)) == "AionWidget_HUD_UserInfo") {
                derived[o] = 1;
                break;
            }
        }
    }
    int instances = 0;
    std::vector<std::string> names;
    for (i32 i = 0; i < sdk.objects.Num(); ++i) {
        const addr_t o = sdk.objects.ObjectAt(i);
        if (!o) continue;
        const addr_t c = mem.ptr_at(o + off::uobject::Class);
        if (!derived.count(c)) continue;
        const std::string on = sdk.Name(mem.u32_at(o + off::uobject::Name));
        if (on.compare(0, 9, "Default__") == 0) continue;   // the CDO is not an instance
        ++instances;
        if (names.size() < 8)
            names.push_back(on + " (" + sdk.Name(mem.u32_at(c + off::uobject::Name)) + ")");
    }
    AION_INFO("dialog-map: %zu class(es) derive from AionWidget_HUD_UserInfo; %d live instance(s) "
              "in the object table (CDOs excluded)", derived.size(), instances);
    for (const auto& n : names) AION_INFO("    %s", n.c_str());

    if (bars_live > 0) {
        AION_INFO("dialog-map: live bars: HP=%.4f MP=%.4f", hp_val, mp_val);
        return finish("REACHABLE");
    }
    AION_INFO("dialog-map: map holds num=%d (%d read as a named UObject, %d widget(s), 0 with live "
              "bars); %d live HUD instance(s) in the whole table", num, valid_objs, widgets,
              instances);
    return finish("NOT_REACHABLE");
}


// --dump-relation: read the client's own friend/foe marker off the nameplate.
//
// The relation between the local player and another actor is not a property of the actor: it comes
// from the server (DevPacketData_Common_CharState::_relationship_entity +0xCD, and the live update
// DevPacketData_GameServer_RelationshipEntityUpdate_NT::_relationship_entity +0x10) and the client
// *presents* it through the nameplate. The presentation is a widget switcher -
// AionWidget_NamePlateChar::P_WS_MarkerType (+0x740), with P_WS_GuildRelation (+0x7B0) for the
// guild-war marker - whose active child is the marker the client draws, and whose children are
// built from the widget blueprint, so their object names are the client's own vocabulary for the
// marker types. Printing both per actor makes the index -> meaning mapping readable from the
// client rather than guessed. Read-only: nothing here writes and nothing is pressed.
int ModeRelation(ModeContext& mc) {
    Sdk& sdk = mc.sdk;
    D3D11Overlay& gfx = mc.gfx;
    HWND overlay = mc.overlay;
    ExternalMemory& mem = mc.mem;
    EntityCache& cache = mc.cache;

    Reflection refl(&sdk);
    NameplateReader plates(&sdk, &refl);
    // The entity list without a camera: this mode reads names and markers, not positions.
    Camera cam;
    Projection proj;
    cache.Update(&sdk, cam, proj);

    int rows = 0, with_plate = 0;
    for (const auto& e : cache.Entities()) {
        if (rows >= 24) break;
        if (!IsCharacterType(e.Type)) continue;  // the marker is a character-nameplate widget
        addr_t plate = 0;
        auto find_plate = [&](addr_t root) {
            if (!root) return;
            plates.WalkWidget(root, [&](addr_t w, const std::string& cn) {
                if (!plate && cn.find("AionWidget_NamePlateChar") != std::string::npos) plate = w;
            });
        };
        find_plate(plates.Root(e.Addr));
        // No live plate (the client is not showing one right now, or the entity is out of range):
        // fall back to the pooled helper the actor keeps, which still holds the last plate it built
        // - the same reach Names::Texts() uses.
        if (!plate) find_plate(plates.Widget(e.Addr));
        if (!plate) {
            // No plate class matched. Say what the tree does hold, so the class name this build
            // actually uses is read off the client instead of guessed.
            if (rows < 6) {
                std::string seen;
                auto collect = [&](addr_t, const std::string& cn) {
                    if (seen.find(cn) != std::string::npos) return;
                    if (!seen.empty()) seen += " ";
                    seen += cn;
                };
                plates.WalkWidget(plates.Root(e.Addr), collect);
                if (seen.empty()) plates.WalkWidget(plates.Widget(e.Addr), collect);
                AION_INFO("relation: %-22s %-18s %-8s tree=[%s]", e.Name.c_str(),
                          e.ClassName.c_str(), EntityTypeName(e.Type), seen.c_str());
                ++rows;
            }
            continue;
        }
        ++with_plate;
        const addr_t pcls = mem.ptr_at(plate + off::uobject::Class);
        // A named member widget of the plate, resolved by reflection (never a hardcoded offset).
        auto member = [&](const char* prop) {
            const PropertyInfo pi = refl.GetProperty(pcls, prop);
            return pi.Offset >= 0 ? mem.ptr_at(plate + (addr_t)pi.Offset) : (addr_t)0;
        };
        // The active child index of a widget switcher, resolved by reflection off its own class.
        auto active = [&](addr_t sw) {
            if (!mem.valid_ptr(sw)) return -1;
            const addr_t scls = mem.ptr_at(sw + off::uobject::Class);
            const PropertyInfo ai = refl.GetProperty(scls, "ActiveWidgetIndex");
            return ai.Offset >= 0 ? (int)sdk.rd<i32>(sw + (addr_t)ai.Offset) : -1;
        };
        const addr_t marker_sw = member("P_WS_MarkerType");
        const addr_t guild_sw = member("P_WS_GuildRelation");
        // The switcher's children: the client's own names for the marker types it can show.
        std::string kids;
        if (mem.valid_ptr(marker_sw)) {
            const addr_t scls = mem.ptr_at(marker_sw + off::uobject::Class);
            const PropertyInfo sl = refl.GetProperty(scls, "Slots");
            if (sl.Offset >= 0)
                for (addr_t slot : mem.read_tarray<addr_t>(marker_sw + (addr_t)sl.Offset, 32)) {
                    if (!mem.valid_ptr(slot)) continue;
                    const addr_t scl = mem.ptr_at(slot + off::uobject::Class);
                    const PropertyInfo ct = refl.GetProperty(scl, "Content");
                    if (ct.Offset < 0) continue;
                    const addr_t cw = mem.ptr_at(slot + (addr_t)ct.Offset);
                    if (!mem.valid_ptr(cw)) continue;
                    if (!kids.empty()) kids += " | ";
                    kids += sdk.Name(mem.u32_at(cw + off::uobject::Name));
                }
        }
        AION_INFO("relation: %-22s %-18s %-8s marker=%d guild=%d children=[%s]", e.Name.c_str(),
                  e.ClassName.c_str(), EntityTypeName(e.Type), active(marker_sw), active(guild_sw),
                  kids.c_str());
        ++rows;
    }
    AION_INFO("relation: %d character(s) carried a nameplate out of %zu entities", with_plate,
              cache.Entities().size());
    gfx.Shutdown();
    DestroyWindow(overlay);
    mem.Close();
    return 0;
}

// The order below is the order the orchestrator has always checked the modes in, so a command line
// that selects more than one still runs the same one. -1 means none was selected.
int RunDumpModes(ModeContext& mc) {
    const Options& opt = mc.opt;
    // Every mode, in the order they have always been checked (first match wins), each with the name
    // its heading prints. Keeping the name beside the predicate is what stops the heading from
    // drifting away from the mode it announces.
    struct Mode {
        const char* name;
        bool selected;
        int (*run)(ModeContext&);
    };
    const Mode modes[] = {
        {"properties",      !opt.dump_filters.empty(),        &ModeProps},
        {"object scan",     !opt.scan_objects_substr.empty(), &ModeScanObjects},
        {"raw objects",     !opt.dump_raw_substr.empty(),     &ModeRaw},
        {"reflected type",  !opt.dump_struct_substr.empty(),  &ModeReflectedStruct},
        {"data table rows", !opt.find_row_substr.empty(),     &ModeDataRows},
        {"harvest plates",  opt.harvest_plates_mode,          &ModeHarvestPlates},
        {"find name",       !opt.find_name_text.empty(),      &ModeFindNameText},
        {"dump nameplate",  opt.dump_nameplate_mode,          &ModeNameplates},
        {"relation",        opt.dump_relation_mode,           &ModeRelation},
        {"AI survey",       opt.ai_dump_mode,                 &ModeAiSurvey},
        {"actor classes",   opt.dump_classes_mode,            &ModeActorClasses},
        {"shutter diag",    opt.shutter_frames > 0,           &ModeShutterDiag},
        {"find hud",        opt.find_hud_mode,                &ModeFindHud},
        {"dialog map",      opt.dialog_map_mode,              &ModeDialogMap},
        {"render gates",    !opt.gates_substr.empty(),        &ModeRenderGates},
    };
    for (const Mode& mode : modes) {
        if (!mode.selected) continue;
        AION_SECTION(mode.name);
        return mode.run(mc);
    }
    return -1;
}

int SaveFontAtlas(D3D11Overlay& gfx, HWND overlay, ExternalMemory& mem, const std::string& path) {
    if (path.empty()) return -1;   // not requested: the caller carries on into the menu setup
        const bool ok = gfx.SaveFontAtlas(path);
        AION_INFO("font atlas '%s' saved=%d (%dx%d)", path.c_str(), (int)ok, gfx.Width(),
                  gfx.Height());
        gfx.Shutdown();
        DestroyWindow(overlay);
        mem.Close();
        return ok ? 0 : 6;
}

}  // namespace app
}  // namespace aion
