#pragma once

#include "commands/app.hpp"

#include <string>

namespace wt {

// `wt status <task>`: summarizes the worktree that has branch <task> checked
// out: its HEAD, uncommitted changes, and how far it has diverged from the
// integration branch. With `json`, prints it as a JSON document.
int status_command(const Context& ctx, const std::string& task, bool json);

}  // namespace wt
