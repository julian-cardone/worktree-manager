# AGENTS.md

Read this file at the start of every prompt. Do not rely on memory of previous instructions.

## What This Is

`wt` is a small C++ CLI designed to simplify Git worktree management, making it easier for developers and AI agents (Claude, Codex, etc.) to work on multiple tasks concurrently.

## 1. General Principles

- Prioritize correctness, safety, clarity, and maintainability.
- Read the current source code directly. Do not rely on stale builds, generated artifacts, or memory.
- Follow established C++ best practices and maintain a clean, consistent architecture.
- Prefer simple solutions over unnecessary complexity.
- Make improvements when they provide genuine value. Avoid refactoring solely for its own sake.

## 2. Code Review & Improvements

As we review and modify the codebase, watch for:

- **Bugs & Security:** Identify and surface bugs, security vulnerabilities, unsafe behavior, and meaningful risks.
- **Code & Architecture:** Identify improvements to code quality, maintainability, and architecture.
- **TODOs:** Identify relevant deferred work, bring it to my attention first, and only then record it in `project-notes/TODO.md`. Do not leave TODO, FIXME, or similar deferred-work comments in source code.
- **Dead Code:** Remove unused or obsolete code.
- **Shared Code:** Extract common functionality when it genuinely reduces duplication or improves maintainability.
- **Constants:** Prefer constants at the lowest appropriate scope. Only share constants when they represent the same concept, not merely because their values match.
- **Significant Changes:** Surface beneficial changes that involve meaningful trade-offs, substantial refactoring, or architectural decisions before implementing them. Significant changes are OK to surface if they will genuinely add value.

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

All development on this repository should exercise `wt` as the intended workflow.

- All new feature, fix, refactor, review, or other development work must begin in a new worktree created through `wt`. Do not perform new development work directly in the primary checkout.
- For all supported worktree and branch lifecycle operations, use `wt` exclusively. Do not use equivalent raw Git commands when `wt` provides that capability.
- If `wt` is broken, incomplete, unsafe, or unable to perform the required operation, stop and surface the issue before continuing. Do not work around the limitation with raw Git.
- If using `wt` introduces meaningful friction, confusing behavior, failures, safety concerns, or other workflow issues, record them in `project-notes/wt-usage.md`.
- Record significant product, architecture, safety, reliability, performance, or usability findings in `project-notes/findings.md`.
- Surface significant findings to me when they are discovered.
- Do not record routine successful commands or generate unnecessary noise.

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

Use `project-notes/` for development findings that should not live in source code:

- `project-notes/TODO.md` — deferred work that has already been surfaced to me and approved for recording.
- `project-notes/wt-usage.md` — meaningful friction and observations discovered through real use of `wt`.
- `project-notes/findings.md` — significant product, architecture, safety, reliability, performance, or usability findings.

Keep these files concise, current, and actionable. Remove resolved items instead of keeping them as permanent historical logs.

**Guiding Principle:** Build a reliable, safe, and maintainable CLI that developers and AI agents can trust. Use `wt` itself throughout development so real-world friction and limitations are discovered early rather than bypassed.
