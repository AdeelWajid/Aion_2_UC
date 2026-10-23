// src/app/Names.hpp — headless name resolution: searching the client's own tables for a display
// name, and harvesting every nameplate the client has already built.
//
// Both are read-only and both write their result to the learned-name table (RealNames.hpp), which
// is what lets a model be named everywhere from then on.
#pragma once
#include <string>

namespace aion {

class Sdk;
class EntityCache;
class NameTable;

namespace app {

// Is there a table the client reads every name from? The nameplate shows the resolved text, so the
// literal is searched where the client could hold it: object names, every data table row, and the
// memory of string-table objects. Writes what it found to the log.
void FindNameText(Sdk* sdk, const std::string& text);

// Resolves every nameplate the client has built, in one pass, and merges the names into the table.
void HarvestPlates(Sdk* sdk, EntityCache* cache, NameTable* names);

}  // namespace app
}  // namespace aion
