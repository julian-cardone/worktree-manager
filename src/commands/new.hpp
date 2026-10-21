#pragma once

#include "commands/app.hpp"

#include <string>

namespace wt {

// `wt new <task>`: creates branch <task> from the integration branch, checked
// out in a new worktree inside the repository's wt container. Prints only the
// new worktree's path on stdout.
int new_command(const Context& ctx, const std::string& task);

}  // namespace wt
