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
- **Significant Changes:** Surface changes that provide meaningful value but involve substantial refactoring, architectural decisions, or important trade-offs before implementing them.

Implement straightforward, clearly beneficial improvements where appropriate. Surface decisions that require my input before proceeding.

## 3. Safety & Error Handling

- Treat prevention of accidental data loss as a primary priority, especially around worktrees, branches, files, and uncommitted changes.
- Never silently discard user work.
- Validate and sanitize paths, branch names, command arguments, and other external inputs.
- Handle errors explicitly with clear and actionable messages.
- Consider edge cases, partial failures, and interrupted operations.
- Use retries only when appropriate; do not mask persistent failures.
- Be especially careful around file deletion, worktree removal, branch deletion, destructive Git commands, and other irreversible operations.
- Never bypass a `wt` safety check.

## 4. Dogfooding `wt`

All development on this repository must exercise `wt` as the intended workflow.

- All new feature, fix, refactor, review, or other development work must begin in a new worktree created through `wt`. Do not perform new development work directly in the primary checkout.
- Use `wt` for every worktree lifecycle operation, and do not use agent-provided worktree tools. Never do these with Git directly:
  - `git worktree` in any form (`add`, `remove`, `prune`, `move`, `lock`, ...);
  - `git switch` or `git checkout` to create or change the branch a worktree has checked out;
  - deleting or renaming task branches (`git branch -d`, `-D`, `-m`, `-M`);
  - merging a task into the integration branch (use `wt merge`).
- This prohibition applies regardless of how Git is invoked, including `git -C`, absolute paths, wrappers, shell indirection, command substitution, or scripts. `.claude/settings.json` denies the common forms; the rest relies on you following this rule.
- General Git operations that `wt` does not intend to replace are allowed, e.g. `git status`, `git diff`, `git log`, `git add`, `git commit`, and `git restore`.
- Do file-level work, including conflict resolution, with `git restore` (e.g. `git restore --ours <file>`, `git restore --source=<commit> <file>`), never `git checkout`. Because `git checkout` can also switch branches, `.claude/settings.json` denies it outright.
- If `wt` is broken, unsafe, or lacks a lifecycle capability required to continue, stop and report the limitation instead of working around it. Keep the missing-capabilities section of `project-notes/wt-usage.md` current as gaps are discovered or resolved.
- Friction in general Git workflows is not by itself a reason to add a `wt` command; propose one only when it falls within the product boundary above.
- Record meaningful friction, confusing behavior, failures, or safety concerns encountered while using `wt` in `project-notes/wt-usage.md`.
- Record significant product, architecture, safety, reliability, performance, or usability findings in `project-notes/findings.md`, and surface important findings to me when discovered.
- Do not log routine successful commands or generate unnecessary noise.

The purpose of these rules is to continuously exercise `wt` through real development so its limitations and friction are discovered rather than bypassed.

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
