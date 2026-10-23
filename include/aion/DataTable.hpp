// DataTable.hpp — reading a live UDataTable's rows out of the target process.
//
// A UDataTable holds its rows in an FScriptMap whose elements are {FName Key, uint8* Row}
// followed by the row struct's bytes. The element stride is not stored in the map, it
// follows from the row struct's size, so it is computed from PropertiesSize and then
// verified against the keys: a wrong stride cannot decode every key as a live FName.
#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "Memory.hpp"
#include "NamePool.hpp"
#include "Offsets.hpp"
#include "Strings.hpp"

namespace aion {

struct DataTableRows {
    addr_t table = 0;
    addr_t row_struct = 0;
    addr_t data = 0;      // FScriptMap element array
    i32 num = 0;          // live rows
    addr_t stride = 0;    // element stride, i.e. the distance between two keys
    addr_t row_size = 0;  // the row struct's PropertiesSize
};

inline addr_t MapElementStride(addr_t value_size, addr_t alignment) {
    const addr_t a = alignment ? alignment : 8;
    const addr_t want = 8 + value_size;  // FName key, then the value
    return (want + a - 1) / a * a;
}

// Reads the row map of a populated UDataTable object; empty and composite tables have no
// rows of their own and are rejected.
inline bool ReadTableRows(IMemory* mem, const NamePool& pool, addr_t table, DataTableRows* out) {
    const addr_t rs = mem->ptr_at(table + off::udatatable::RowStruct);
    if (!mem->valid_ptr(rs)) return false;
    const addr_t rcls = mem->ptr_at(rs + off::uobject::Class);
    if (!mem->valid_ptr(rcls)) return false;
    if (pool.Get(mem->u32_at(rcls + off::uobject::Name)) != "ScriptStruct") return false;
    const addr_t data = mem->ptr_at(table + off::udatatable::RowMap);
    const i32 num = mem->i32_at(table + off::udatatable::RowMap + 8);
    const i32 cap = mem->i32_at(table + off::udatatable::RowMap + 12);
    if (!mem->valid_ptr(data) || num <= 0 || cap < num || cap > 4000000) return false;
    const addr_t row_size = mem->u32_at(rs + off::ustruct::PropertiesSize);
    if (row_size < 8 || row_size > 0x10000) return false;
    addr_t stride = MapElementStride(row_size, mem->u32_at(rs + off::ustruct::MinAlignment));
    const i32 probe = std::min<i32>(num, 32);
    // A key only decodes if its FName index resolves, so the probe count scores a stride.
    const auto keys_ok = [&](addr_t s) {
        if (s < 16 || s > 0x10000) return 0;
        int ok = 0;
        for (i32 i = 0; i < probe; ++i) {
            const u32 idx = mem->u32_at(data + static_cast<addr_t>(i) * s);
            if (!idx || pool.Get(idx).empty()) break;
            ++ok;
        }
        return ok;
    };
    int best = keys_ok(stride);
    if (best < probe) {
        for (addr_t s = 16; s <= 0x4000; s += 8) {
            const int ok = keys_ok(s);
            if (ok > best) {
                best = ok;
                stride = s;
            }
            if (ok >= probe) break;
        }
    }
    if (best < 4) return false;
    *out = DataTableRows{table, rs, data, num, stride, row_size};
    return true;
}

// Every row of a populated table, as {key, row address} - the whole table, not a filtered
// view, which is what a search over the client's data needs.
inline std::vector<std::pair<std::string, addr_t>> AllRows(IMemory* mem, const NamePool& pool,
                                                          const DataTableRows& t,
                                                          std::size_t max_rows = 20000) {
    std::vector<std::pair<std::string, addr_t>> out;
    for (i32 i = 0; i < t.num && out.size() < max_rows; ++i) {
        const addr_t el = t.data + static_cast<addr_t>(i) * t.stride;
        const u32 idx = mem->u32_at(el);
        if (!idx) continue;
        const std::string key = pool.Get(idx);
        if (key.empty()) continue;
        const addr_t row = mem->ptr_at(el + 8);
        out.emplace_back(key, mem->valid_ptr(row) ? row : 0);
    }
    return out;
}

// Rows whose key contains `needle` (case-insensitive), as {key, row address}.
inline std::vector<std::pair<std::string, addr_t>> FindRows(IMemory* mem, const NamePool& pool,
                                                           const DataTableRows& t,
                                                           const std::string& needle,
                                                           std::size_t max_hits = 32) {
    std::vector<std::pair<std::string, addr_t>> hits;
    for (i32 i = 0; i < t.num && hits.size() < max_hits; ++i) {
        const addr_t el = t.data + static_cast<addr_t>(i) * t.stride;
        const u32 idx = mem->u32_at(el);
        if (!idx) continue;
        const std::string key = pool.Get(idx);
        if (key.empty() || !HasI(key, needle.c_str())) continue;
        const addr_t row = mem->ptr_at(el + 8);
        hits.emplace_back(key, mem->valid_ptr(row) ? row : 0);
    }
    return hits;
}

// Every string-looking value inside a row, with its offset. The row struct describes the
// columns, but strings identify a column well enough to find data whose struct has not been
// dumped. FText is tried first: its shared reference also looks like a string header.
inline std::vector<std::pair<addr_t, std::string>> RowStrings(IMemory* mem, addr_t row,
                                                             addr_t row_size,
                                                             std::size_t max_hits = 24) {
    std::vector<std::pair<addr_t, std::string>> out;
    const addr_t end = std::min<addr_t>(row_size, 0x1000);
    for (addr_t off = 0; off + 0x10 <= end && out.size() < max_hits; off += 8) {
        std::string s = mem->read_ftext(row + off, 256, true);
        if (s.size() < 2) s = mem->read_fstring(row + off, 512, true);
        if (s.size() >= 2) out.emplace_back(off, s);
    }
    return out;
}

}  // namespace aion
