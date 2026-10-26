# Landing Tasks

This repository keeps `main` linear. Every task must land as a fast-forward with no merge commits.

This is a repository policy only. `wt` itself follows each repository's Git settings and must not impose this strategy on other users.

## Before Reporting Complete or Landing

Do these steps before reporting a task complete, not only right before `wt merge`. A task finished against an older `main` is not finished.

1. Keep the task's worktree until the task has successfully landed. `wt` cannot currently create a worktree for an existing task branch, so a task removed early cannot be rebased.

2. Commit the task's current changes before final verification.

   After each code-changing commit, rebuild and run the relevant tests so the build reflects that commit.

3. Confirm the repository still uses fast-forward-only merges:

   ```bash
   git config --get merge.ff
   ```

   The expected value is:

   ```text
   only
   ```

   If it is missing, restore it:

   ```bash
   git config merge.ff only
   ```

4. Rebase the task branch onto the latest `main` from inside the task's own worktree:

   ```bash
   git rebase main
   ```

5. If conflicts occur, record one concise entry in `project-notes/wt-usage.md` naming the task and conflicted files, then resolve them in the task worktree:

   ```bash
   git add <resolved-files>
   git rebase --continue
   ```

   Repeat until the rebase finishes.

   Do not log conflict-free rebases.

6. Review the resulting diff against the current `main` and account for behavioral changes that landed while the task was in progress. Resolving textual conflicts alone is not sufficient.

7. Reconfigure and rebuild from the final committed and rebased state:

   ```bash
   cmake -S . -B build
   cmake --build build
   ```

8. Run the full test suite:

   ```bash
   ctest --test-dir build --output-on-failure
   ```

9. Run any required formatting or additional verification for the files changed.

10. Confirm the task worktree contains no unintended uncommitted changes.

11. If `AGENTS.md` requires independent code review, run it against the final diff and relevant surrounding code.

    The reviewer must return `APPROVE`, `FIX_REQUIRED`, or `DECISION_REQUIRED`.

    Do not land unless the result is `APPROVE`.

    If review causes code changes, commit them, rebuild and retest, then review the new final state again if required.

Do not report a code-changing task complete unless the exact final committed and rebased state has been rebuilt and tested.

## Land the Task

From any worktree in the repository other than the task's own, for example the primary checkout:

```bash
wt merge <task> --cleanup
```

`wt merge` should fast-forward the integration branch, then `--cleanup` removes the task's worktree and deletes its branch.

Run it from outside the task's worktree. `--cleanup` refuses before merging anything if it would have to remove the worktree it is running from.

If `wt merge` refuses because the task is behind `main`, return to the task worktree, rebase onto the latest `main`, rebuild, rerun the relevant verification and any required independent review, and try again.

Never merge `main` into the task branch as a conflict-resolution strategy. That creates backwards-looking merge history when the task later fast-forwards into `main`.

Do not use:

```bash
git rebase main <task>
```

Passing a branch name can check that branch out in the current worktree. Run `git rebase main` from inside the task's own worktree instead.

## Cleanup

`--cleanup` checks before merging that the task worktree can be removed afterwards.

If ignored files such as `build/` are present, it refuses and lists them. Inspect them before explicitly allowing their deletion:

```bash
wt merge <task> --cleanup --delete-ignored
```

If cleanup fails after the task has landed, `wt merge` says what was left behind.

Deal with that state, then rerun the command it prints. The task has already landed, so the rerun only completes cleanup.

Never delete a task branch with `git branch`. To clean up a task branch that has already landed, even if its worktree was already removed, run:

```bash
wt merge <task> --cleanup
```

## Parallel Tasks

Several task worktrees may be active at the same time.

As tasks finish:

1. Commit the task's current changes.
2. Rebase it onto the latest `main`.
3. If the rebase conflicts, record the task and conflicted files in `project-notes/wt-usage.md`.
4. Resolve conflicts and review behavioral changes in that task's worktree.
5. Reconfigure and rebuild from the rebased commit.
6. Rerun the full test suite and required formatting.
7. Complete any required independent code review.
8. Land and clean it up with:

   ```bash
   wt merge <task> --cleanup
   ```

A task that remains open while other tasks land will eventually fall behind `main`; this is expected.
