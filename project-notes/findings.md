# Findings

Significant product, architecture, safety, reliability, performance, or usability findings.
Before changing an area, read its findings file below.

## Open

Known gaps that still need action.

- **`wt merge --cleanup` on an already-landed task words its refusal as if a merge were pending.**
  If the task's worktree has uncommitted changes, `check_source` (`src/commands/merge_preflight.cpp`) refuses with "…which the merge wouldn't include" and "Nothing was merged.", although there is nothing left to merge and what is actually blocked is the cleanup.
  Refusing is correct; only the message misleads. Fix by saying the changes block the cleanup when the task has already landed.

## Addressed

Findings `wt` now handles live in the subsystem file for the area where their check or rule lives.
Each explains why a check or rule exists; keep it while that check or rule does, and revisit it before relaxing one.
When a finding is handled, move it from Open into the file for that area.

| File | Covers |
| --- | --- |
| [merge.md](findings/merge.md) | `wt merge` and `--cleanup`: merge configuration, conflicts and rollback, ignored-file overwrites, rebasing and stopped operations, branch deletion, and post-mutation verification for all commands |
| [remove.md](findings/remove.md) | `wt remove`: ignored and hidden changes, no force mode, missing worktrees, symlinks, the current worktree, abandoning a task's branch |
| [new.md](findings/new.md) | `wt new`: branch-name case conflicts, failed creation |
| [read-state.md](findings/read-state.md) | `wt status` and `wt list`: in-progress operations, missing versus unknown worktrees |
| [git-runtime.md](findings/git-runtime.md) | How `wt` runs Git in every command: inherited environment, machine-readable output, newline paths, macOS Unicode paths, localized messages, signals |
