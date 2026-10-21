# wt usage notes

Friction found while using `wt` to develop `wt`.

## Missing wt capabilities

Agents must use `wt`, not Git, for the worktree lifecycle (`AGENTS.md` section 4), so these lifecycle steps currently need a human.
Only gaps inside `wt`'s product boundary belong here; remove an item once `wt` covers it.

- **Delete a merged branch**: `wt merge` lands a task and `wt remove` removes its worktree, but `wt` can't delete the merged branch, so finished task branches accumulate.
  Cleanup needs a human running `git branch -d`; whether `wt` should do this, and with what safeguards, is an open question in `findings.md`.
- **Land a task that conflicts with the integration branch**: `wt merge` aborts on any conflict, and resolving it on `main` would need a raw `git merge`.
  The workaround, merging `main` into the task branch, put a backwards "Merge branch 'main' into ..." commit on `main` once it fast-forwarded.
  For now, resolve in a fresh `wt new` worktree (e.g. `git restore --source=<task> <files>` and commit as one squashed change), then `wt merge` it.

## Build directories always need `--delete-ignored`

Every task worktree here has an ignored `build/` directory, so `wt remove` refuses until rerun with `--delete-ignored`.
The refusal is intended, but it makes the flag routine, which weakens it as a guard for ignored files that matter (e.g. `.env`).
