#pragma once

#include "PixelsReadBindData.hpp"
#include "PixelsReadGlobalState.hpp"
#include "PixelsReadLocalState.hpp"

namespace duckdb::selective_scan {

void InitializeGlobal(PixelsReadGlobalState &state);
void InitializeLocal(PixelsReadGlobalState &global, PixelsReadLocalState &local);

bool StateNext(ClientContext &context, PixelsReadBindData &bind_data,
               PixelsReadLocalState &local, PixelsReadGlobalState &global,
               bool is_init_state);

} // namespace duckdb::selective_scan
