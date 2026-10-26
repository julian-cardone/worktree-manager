#pragma once

#include "commands/app.hpp"

namespace wt {

// `wt list`: prints each worktree's branch, status, and path, as a table or,
// with `json`, as a JSON document.
int list_command(const Context& ctx, bool json);

}  // namespace wt
