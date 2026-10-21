#pragma once

#include "commands/app.hpp"

#include <string>

namespace wt {

// `wt remove <task>`: removes the worktree wt created for <task>, refusing if
// that could lose anything not committed. Ignored files also block removal
// unless `delete_ignored` is set. Never deletes the branch.
int remove_command(const Context& ctx, const std::string& task,
                   bool delete_ignored);

}  // namespace wt
