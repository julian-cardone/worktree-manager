# wt usage notes

Friction found while using `wt` to develop `wt`.

## Missing wt capabilities

Agents must use `wt`, not Git, for the worktree lifecycle (`AGENTS.md` section 4), so these lifecycle steps currently need a human.
Only gaps inside `wt`'s product boundary belong here; remove an item once `wt` covers it.

- **Give an existing branch a worktree**: `wt new` only creates new branches, so a task whose worktree was removed before it landed can't be rebased onto `main` through `wt`.
  Deliberately not solved yet: the workflow keeps a task's worktree until it lands (`docs/agent-workflows/landing-tasks.md`).
  If removing a worktree too early happens repeatedly, that justifies something like `wt attach <branch>` or extending `wt new`; note each occurrence here.

## Build directories always need `--delete-ignored`

Every task worktree here has an ignored `build/` directory, so `wt remove` and `wt merge --cleanup` refuse until rerun with `--delete-ignored`.
The refusal is intended, but it makes the flag routine, which weakens it as a guard for ignored files that matter (e.g. `.env`).
`wt remove` and `wt merge --cleanup` now list the ignored entries they would delete, so the flag is at least an informed choice; a configurable list of disposable paths was deferred until this proves insufficient.

## Rebase conflicts

- `git-locale-independence`: `src/git/worktree.cpp` conflicted with `c125008`, which landed the same exit-code-129 change in parallel; kept `main`'s version and reapplied only the SIGHUP guard.
