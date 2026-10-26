# Findings

Significant product, architecture, safety, reliability, performance, or usability findings.
Before changing an area, read its findings file below.

## Open

Known gaps that still need action.

None right now.

## Addressed

Findings `wt` now handles live in the subsystem file for the area where their check or rule lives.
Each explains why a check or rule exists; keep it while that check or rule does, and revisit it before relaxing one.
When a finding is handled, move it from Open into the file for that area.

| File | Covers |
| --- | --- |
| [merge.md](findings/merge.md) | `wt merge` and `--cleanup`: merge configuration, conflicts and rollback, ignored-file overwrites, rebasing and stopped operations, branch deletion, and post-mutation verification for all commands |
| [remove.md](findings/remove.md) | `wt remove`: ignored and hidden changes, no force mode, missing worktrees, symlinks, the current worktree |
| [new.md](findings/new.md) | `wt new`: branch-name case conflicts, failed creation |
| [read-state.md](findings/read-state.md) | `wt status` and `wt list`: in-progress operations, missing versus unknown worktrees |
| [git-runtime.md](findings/git-runtime.md) | How `wt` runs Git in every command: inherited environment, machine-readable output, newline paths, macOS Unicode paths, localized messages, signals |
