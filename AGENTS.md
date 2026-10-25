# AGENTS.md

Read this file at the start of every prompt. Do not rely on memory of previous instructions.

## What This Is

`wt` is a small C++ CLI designed to simplify Git worktree management, making it easier for developers and AI agents (Claude, Codex, etc.) to work on multiple tasks concurrently.

**Product boundary:** `wt` owns the worktree lifecycle: creating, listing, inspecting, merging, and removing task worktrees, and the branch cleanup that goes with them.
It is not a Git replacement.
Staging, committing, history, diffs, and other general repository operations stay with Git; do not add `wt` commands that merely wrap them.

## 1. General Principles

- Prioritize correctness, safety, clarity, scalability, and maintainability.
- Read the current source code directly. Do not rely on stale builds, generated artifacts, or memory.
- Follow established C++ best practices and maintain a clean, consistent architecture.
- Prefer simple solutions over unnecessary complexity.
- Make improvements when they provide genuine value. Avoid refactoring solely for its own sake.

## 2. Code Review & Improvements

As we review and modify the codebase, watch for:

- **Bugs & Security:** Identify and surface bugs, security vulnerabilities, unsafe behavior, and meaningful risks.
- **Code & Architecture:** Identify improvements to code quality, maintainability, scalability, and architecture.
- **TODOs:** Identify relevant deferred work, bring it to my attention first, and only then record it in `project-notes/TODO.md`. Do not leave TODO, FIXME, or similar deferred-work comments in source code.
- **Dead Code:** Remove unused or obsolete code.
- **Shared Code:** Extract common functionality when it genuinely reduces duplication or improves maintainability.
- **Constants:** Prefer constants at the lowest appropriate scope. Only share constants when they represent the same concept, not merely because their values match.
- **Significant Changes:** Significant changes are OK to surface if they will genuinely add value.

Implement straightforward, clearly beneficial improvements where appropriate. Surface decisions that require my input before proceeding.

## 3. Safety & Error Handling

### Safety invariants

These must always hold.
Review every command, new or changed, against each of them.

| Invariant | Meaning |
| --- | --- |
| Never silently discard working changes | Modified, staged, untracked, ignored, or conflicted files must survive unless the user explicitly chose to delete them. |
| Never force by default | No `--force`, hard resets, branch deletion, or similar in normal operation. |
| Git is the source of truth | Don't keep a second state database that can disagree with Git. |
| Validate immediately before mutation | Don't rely on state checked much earlier; re-read what the mutation depends on. |
| Success is verified, not assumed | After mutating, confirm the intended result from Git's and the filesystem's current state; an exit code alone isn't proof (hooks can undo or change Git's work). |
| Failed operations leave understandable state | Especially `new`, `merge`, and `remove`: say exactly what changed and what was left behind. |
| The branch survives worktree cleanup | `wt remove` never deletes the branch. |
| Conflict resolution stays manual | `wt` never guesses how a conflict should be resolved. |
| Unknown state means stop | Never treat uncertainty as safety. |
| User input never becomes shell syntax | Pass arguments as an argv, never through a shell; quote anything printed for the user to paste. |
| Errors explain recovery | "Failed" alone isn't enough; say what to do next. |

### Practices

- Treat prevention of accidental data loss as a primary priority, especially around worktrees, branches, files, and uncommitted changes.
- Validate and sanitize paths, branch names, command arguments, and other external inputs.
- Consider edge cases, partial failures, interrupted operations, and concurrent processes (e.g. an agent committing while `wt` runs).
- Use retries only when appropriate; do not mask persistent failures.
- Add a `WARNING:` comment at any check that upholds an invariant, so it isn't relaxed by accident.
- Never bypass a `wt` safety check.
- Structure every consequential command as discover → validate → plan → mutate → verify → report: read and check everything the mutation depends on (Git's view, the filesystem, uncommitted changes, conflicts, in-progress operations) before changing anything.

## 4. Dogfooding `wt`

All development on this repository must exercise `wt` as the intended worktree workflow.

- All new feature, fix, refactor, or other code-changing work must begin in a new worktree created through `wt`. Do not make development changes directly in the primary checkout.
- Use `wt` for every worktree lifecycle operation, and do not use agent-provided worktree tools.
- Do not use Git directly to:
  - create, remove, prune, move, lock, or otherwise manage worktrees;
  - switch or check out a different branch for a worktree;
  - delete or rename task branches;
  - merge a task branch into the integration branch.
- General Git operations outside the `wt` product boundary are allowed, including `git status`, `git diff`, `git log`, `git add`, `git commit`, and `git restore`.
- Use `git restore` for file-level restoration and conflict resolution; do not use `git checkout`, because it can also change the branch checked out by a worktree.
- If `wt` is broken, unsafe, or lacks a required worktree-lifecycle capability, stop and report the limitation instead of bypassing it. Keep the missing-capabilities section of `project-notes/wt-usage.md` current as gaps are discovered or resolved.
- Friction in general Git workflows is not by itself a reason to add a `wt` command. Only propose commands that fit the product boundary above.
- Record meaningful `wt` friction, confusing behavior, failures, or safety concerns in `project-notes/wt-usage.md`.
- Record significant product, architecture, safety, reliability, performance, or usability findings in `project-notes/findings.md`, and surface important findings to me when discovered.
- Do not log routine successful commands or generate unnecessary noise.

### Landing tasks: linear `main`

This repository keeps `main` linear: every task lands as a fast-forward, with no merge commits.
This is a policy for this repository only; `wt` itself follows each repository's Git settings and must not impose it on other users.

- The repository is configured with `git config merge.ff only`, so `wt merge` lands a task only by fast-forwarding. Check it is still set before landing; set it again if not.
- Several tasks may be in progress at once. Land each with `wt merge <task>` when it is finished.
- Before reporting a task complete or landing it, run `git rebase main` in that task's own worktree, resolve any conflicts there (`git add`, then `git rebase --continue`), review the resulting diff against current `main`, and rerun the relevant tests. The same applies when `wt merge` refuses because the task is behind `main`.
- Resolving textual conflicts is not enough: account for behavior that landed on `main` while the task was in progress, such as new code paths the task's changes must also cover.
- Never merge `main` into a task branch, and never pass a branch name to `git rebase` (e.g. `git rebase main <task>`), since that checks the branch out in the current worktree.
- Keep a task's worktree until `wt merge` has landed it; only then clean up with `wt remove`. `wt` can't give an existing branch a new worktree, so a task removed early can't be rebased.
- Landed task branches are kept and accumulate until a human deletes them (`git branch -d`). This is known cleanup friction, not a reason to stop.

The purpose of these rules is to continuously exercise `wt` through real development so its worktree-lifecycle limitations and friction are discovered rather than bypassed.

## 5. Code Quality & Documentation

- Document new CLI commands, options, and developer-facing commands in `README.md` as they are introduced.
- Keep documentation consistent with the implementation.
- Comments should explain **why** something is done, not duplicate technical documentation.
- Add concise warnings around code that would be dangerous to modify incorrectly.
- Keep comments natural, concise, and useful.
- Update or remove outdated, redundant, or misleading comments.

## 6. Testing & Changes

- Create and maintain unit and end-to-end tests as development progresses.
- Cover relevant normal behavior, edge cases, errors, and failure scenarios.
- Tests must never be able to modify or delete real user repositories or data.
- Command logic takes its working directory and output streams from `wt::Context`, never from process-global state, so tests can run against an isolated sandbox (`tests/support/sandbox.hpp`).
- When modifying code, review the blast radius and update all affected code, references, documentation, comments, and tests.
- Do not leave stale or inconsistent code behind.
- Verify changes with the relevant builds and tests. Do not claim verification unless it was actually performed.

## 7. Project Notes

Keep project knowledge and decisions in this repository (`AGENTS.md`, `project-notes/`), not in agent-private memory. Claude auto-memory is disabled for this project in `.claude/settings.json`.

Use `project-notes/` for information that should persist but does not belong in source code:

- `project-notes/TODO.md` — deferred work that has already been surfaced to me and approved for recording.
- `project-notes/wt-usage.md` — missing `wt` capabilities, meaningful friction, and observations from real use.
- `project-notes/findings.md` — significant product, architecture, safety, reliability, performance, or usability findings.

Keep these files concise, current, and actionable. Remove resolved items instead of keeping them as permanent historical logs.

**Guiding Principle:** Build a reliable, safe, and maintainable CLI that developers and AI agents can trust. Use `wt` itself throughout development so real-world friction and limitations are discovered early rather than bypassed.
