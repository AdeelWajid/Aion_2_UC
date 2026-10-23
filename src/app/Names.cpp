// src/app/Names.cpp — see Names.hpp.
#include "Names.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "aion/DataTable.hpp"
#include "aion/EntityCache.hpp"
#include "aion/Logger.hpp"
#include "aion/Memory.hpp"
#include "aion/Nameplate.hpp"
#include "aion/Obf.hpp"
#include "aion/RealNames.hpp"
#include "aion/Reflection.hpp"
#include "aion/Sdk.hpp"
#include "aion/World.hpp"

namespace aion {
namespace app {

// ------------------------------------------------------------- learned names ----

// Byte offset of a UTF-16LE literal inside [addr, addr+span), or -1. One read per span, which
// is what makes searching many rows and objects affordable.
static long long FindLiteral(IMemory* m, addr_t addr, addr_t span, const std::vector<u16>& lit) {
    std::vector<char> buf(static_cast<std::size_t>(span));
    if (!m->read(addr, buf.data(), buf.size())) return -1;
    const std::size_t bytes = lit.size() * 2;
    if (buf.size() < bytes) return -1;
    for (std::size_t i = 0; i + bytes <= buf.size(); i += 2)
        if (std::memcmp(buf.data() + i, lit.data(), bytes) == 0) return static_cast<long long>(i);
    return -1;
}

static std::vector<u16> Wide(const std::string& s) {
    std::vector<u16> w;
    w.reserve(s.size());
    for (char c : s) w.push_back(static_cast<u16>(static_cast<unsigned char>(c)));
    return w;
}

// Is there a table the client reads every name from? The nameplate shows the resolved text,
// so the literal is searched where the client could hold it: object names, every data table
// row (raw, one read per row) and the memory of string-table objects.
void FindNameText(Sdk* sdk, const std::string& text) {
    std::FILE* f = std::fopen("aion_findname.txt", "wb");
    if (!f) return;
    const std::vector<u16> lit = Wide(text);
    int obj_hits = 0, row_hits = 0, str_hits = 0, tables = 0, rows = 0;
    for (i32 i = 0; i < sdk->objects.Num(); ++i) {
        const addr_t o = sdk->objects.ObjectAt(i);
        if (!sdk->mem->valid_ptr(o)) continue;
        const addr_t c = sdk->mem->ptr_at(o + off::uobject::Class);
        if (!sdk->mem->valid_ptr(c)) continue;
        const std::string cn = sdk->Name(sdk->mem->u32_at(c + off::uobject::Name));
        const std::string on = sdk->Name(sdk->mem->u32_at(o + off::uobject::Name));
        if (HasI(on, text.c_str())) {
            std::fprintf(f, "object      %-34s class=%-24s @0x%llX\n", on.c_str(), cn.c_str(),
                         (unsigned long long)o);
            ++obj_hits;
        }
        if (cn == "DataTable" || cn == "CompositeDataTable") {
            DataTableRows t;
            if (!ReadTableRows(sdk->mem, sdk->pool, o, &t)) continue;
            ++tables;
            const addr_t span = std::min<addr_t>(t.row_size, 0x800);
            for (const auto& row : AllRows(sdk->mem, sdk->pool, t)) {
                ++rows;
                if (!row.second) continue;
                const long long at = FindLiteral(sdk->mem, row.second, span, lit);
                if (at < 0) continue;
                std::fprintf(f, "table row   %-34s key=%-30s +0x%llX\n", on.c_str(),
                             row.first.c_str(), (unsigned long long)at);
                ++row_hits;
            }
        } else if (HasI(cn, "StringTable")) {
            const long long at = FindLiteral(sdk->mem, o, 0x8000, lit);
            if (at < 0) continue;
            std::fprintf(f, "stringtable %-34s class=%-24s +0x%llX\n", on.c_str(), cn.c_str(),
                         (unsigned long long)at);
            ++str_hits;
        }
    }
    std::fclose(f);
    AION_INFO("'%s': %d object name(s), %d/%d row(s) in %d table(s), %d string table(s) -> "
              "aion_findname.txt",
              text.c_str(), obj_hits, row_hits, rows, tables, str_hits);
}

// Resolves every nameplate the client has built, in one pass, and merges the names into the
// table. This is the fast way to fill it: run it in a busy area and the models seen there are
// named in every later session, at any distance, whether or not a nameplate is ever built for
// them again.
void HarvestPlates(Sdk* sdk, EntityCache* cache, NameTable* names) {
    Reflection refl(sdk);
    NameplateReader np(sdk, &refl);
    NameFieldLearner learner;
    World world(sdk);
    Level level(sdk);
    int actors = 0, named = 0, fresh = 0, skipped = 0;
    const auto scan = [&](addr_t lvl) {
        for (addr_t a : level.Actors(lvl)) {
            if (!sdk->mem->valid_ptr(a)) continue;
            const addr_t cls = sdk->mem->ptr_at(a + off::uobject::Class);
            if (!sdk->mem->valid_ptr(cls)) continue;
            // Widget() only answers for a class that declares a nameplate member, so this is
            // also the check that the actor can carry one at all (characters do; world props
            // do only while the client shows theirs).
            if (!np.Widget(a)) continue;
            ++actors;
            const std::string nm = np.Name(a);
            if (nm.empty()) continue;
            ++named;
            const std::string cn = sdk->Name(sdk->mem->u32_at(cls + off::uobject::Name));
            const std::string on = sdk->Name(sdk->mem->u32_at(a + off::uobject::Name));
            const std::string model = cache->ModelOf(sdk, a, cn);
            if (model.empty()) continue;
            // Only a model that identifies its entity may key the table. Every player of a
            // gender shares one mesh and a pet's name belongs to its owner, so storing those
            // would hand one entity another entity's name (a live harvest did exactly that
            // before this rule).
            if (!ModelNamesEntity(cache->ClassifyWithRules(cn, on, model))) {
                ++skipped;
                continue;
            }
            if (names->Add(model, nm)) {
                ++fresh;
                AION_INFO("  %-30s = %s", model.c_str(), nm.c_str());
            }
            learner.Add(a, nm);
        }
    };
    scan(world.PersistentLevel().Addr());
    for (addr_t l : world.Levels()) scan(l);
    u32 off = 0;
    NameFieldKind kind = NameFieldKind::None;
    if (!names->direct_offset() && learner.Learn(sdk->mem, &off, &kind)) {
        names->SetDirect(off, kind);
        AION_INFO("name field learned: actor+0x%X (%s)", off, NameFieldKindName(kind));
    }
    names->Save();
    AION_INFO("%d nameplate actor(s), %d named, %d skipped (name is not model-bound), %d new "
              "name(s), table now %zu entry(ies)",
              actors, named, skipped, fresh, names->Size());
}

}  // namespace app
}  // namespace aion
