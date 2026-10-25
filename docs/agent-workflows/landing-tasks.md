# Landing Tasks

This repository keeps `main` linear. Every task must land as a fast-forward with no merge commits.

This is a repository policy only. `wt` itself follows each repository's Git settings and must not impose this strategy on other users.

## Before Reporting Complete or Landing

Do these steps before reporting a task complete, not only right before `wt merge`; a task that was finished against an older `main` is not finished.

1. Keep the task's worktree until the task has successfully landed. `wt` can't currently create a worktree for an existing task branch, so a task removed early can't be rebased.
2. Confirm the repository still uses fast-forward-only merges:

   ```bash
   git config --get merge.ff
   ```

   The expected value is:

   ```text
   only
   ```

   If it is missing, restore it before landing:

   ```bash
   git config merge.ff only
   ```

3. Rebase the task branch onto the latest `main` from inside the task's own worktree:

   ```bash
   git rebase main
   ```

4. If conflicts occur, resolve them in the task worktree:

   ```bash
   git add <resolved-files>
   git rebase --continue
   ```

   Repeat until the rebase finishes.

5. Review the resulting diff against the current `main` and account for behavioral changes that landed while the task was in progress. Resolving textual conflicts alone is not sufficient.

6. Rerun the relevant builds and tests after the rebase.

## Land the Task

From any worktree in the repository:

```bash
wt merge <task>
```

`wt merge` should fast-forward the integration branch. If it refuses because the task is behind `main`, return to the task worktree, rebase onto the latest `main`, rerun the relevant verification, and try again.

Never merge `main` into the task branch as a conflict-resolution strategy. That creates backwards-looking merge history when the task later fast-forwards into `main`.

Do not use:

```bash
git rebase main <task>
```

Passing a branch name can check that branch out in the current worktree. Run `git rebase main` from inside the task's own worktree instead.

## Cleanup

Only after `wt merge <task>` succeeds:

```bash
wt remove <task>
```

If ignored files such as `build/` are present, inspect the files reported by `wt remove` before explicitly allowing their deletion:

```bash
wt remove <task> --delete-ignored
```

`wt remove` keeps the task branch. Merged task branches currently accumulate until a human deletes them; this is known non-blocking cleanup friction.

## Parallel Tasks

Several task worktrees may be active at the same time.

As tasks finish:

1. Rebase each finished task onto the latest `main`.
2. Resolve conflicts and review behavioral changes in that task's worktree.
3. Rerun relevant tests.
4. Land it with `wt merge <task>`.
5. Remove its worktree with `wt remove <task>`.

A task that remains open while other tasks land will eventually be behind `main`; this is expected.
