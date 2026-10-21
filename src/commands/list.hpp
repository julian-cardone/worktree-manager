#pragma once

#include "commands/app.hpp"

namespace wt {

// `wt list`: prints each worktree's branch, status, and path.
int list_command(const Context& ctx);

}  // namespace wt
