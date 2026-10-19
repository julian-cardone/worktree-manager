# AGENTS.md

Read this file at the start of every prompt. Do not rely on memory of previous instructions.

## What This Is

`wt` is a small C++ CLI designed to simplify Git worktree management, making it easier for developers and AI agents (Claude, Codex, etc.) to work on multiple tasks concurrently.

## 1. General Principles

- Prioritize correctness, safety, clarity, and maintainability.
- Make improvements when they provide genuine value. Refactoring is OK if it genuinely provides value.
- Read the current source code directly. Do not rely on stale builds, generated artifacts, or memory.
- Follow established C++ best practices and maintain a clean, consistent architecture.
- Prefer simple solutions over unnecessary complexity.

## 2. Code Review & Improvements

As we review the codebase file by file, watch for the following:

- **Bugs & Security:** Identify and surface glaring bugs, security vulnerabilities, and potential risks.
- **Code & Architecture:** Look for meaningful improvements to code quality, maintainability, and overall architecture.
- **TODOs:** Identify relevant TODO items and bring them to my attention.
- **Dead Code:** Remove unused or obsolete code.
- **Shared Code:** Extract common functionality when it genuinely reduces duplication or improves maintainability.
- **Constants:** Identify opportunities for constants, preferring the lowest appropriate scope. Only share constants when they represent the same concept, not merely because their values match.
- **Significant Changes:** Surface beneficial improvements even if they require substantial refactoring or architectural changes.

Implement straightforward, clearly beneficial improvements where appropriate. Surface significant changes, trade-offs, or decisions that require my input. Avoid changes solely for the sake of refactoring.

## 3. Safety & Error Handling

- Treat the prevention of accidental data loss as a primary priority, especially when managing Git worktrees, branches, and uncommitted changes.
- Identify potentially dangerous operations, including file deletion, worktree removal, branch deletion, and destructive Git commands. Fix unsafe behavior or surface decisions that require my input.
- Validate and sanitize inputs appropriately including (but not limited to) paths, branch names, and command arguments.
- Handle errors explicitly, with clear and actionable messages.
- Implement retries when appropriate, avoiding unnecessary retries or masking persistent failures.
- Consider edge cases and failure scenarios, including partially completed operations.
- Never silently discard user work.

## 4. Code Quality & Documentation

- Document new CLI commands, options, and developer-facing commands in the `README.md` as they are introduced.
- Keep documentation consistent with the actual implementation.
- Code comments should explain **why** something is done, not serve as technical documentation.
- Add concise warnings around code that could be dangerous to modify.
- Keep comments natural, concise, and easy to understand. Avoid unnecessary or AI-generated-sounding commentary.
- Update or remove comments that are outdated, redundant, or misleading.

## 5. Testing & Changes

- Create and maintain unit tests and end-to-end tests as development progresses.
- Ensure tests accurately reflect intended behavior and cover relevant edge cases, errors, and failure scenarios.
- Ensure tests cannot accidentally modify or delete real user repositories or data.
- Command logic takes its working directory and output streams from `wt::Context`, never from process-global state, so tests can run it against an isolated sandbox (`tests/support/sandbox.hpp`).
- When modifying code, review the blast radius and update all affected code, references, documentation, comments, and tests.
- Avoid leaving stale or inconsistent code behind.
- Verify changes through relevant tests and builds. Do not claim verification unless it was actually performed.

**Guiding Principle:** Build a reliable, safe, and maintainable CLI that developers and AI agents can trust. Favor meaningful improvements over unnecessary complexity.
