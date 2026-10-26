# AGENTS.md

Read this file at the start of every prompt. Do not rely on memory of previous instructions.

## What This Is

`wt` is a small C++ CLI for simplifying Git worktree management so developers and AI agents can work on multiple tasks concurrently.

**Product boundary:** `wt` owns the worktree lifecycle: creating, listing, inspecting, merging, removing task worktrees, and the branch cleanup that goes with them.

It is not a Git replacement. Staging, committing, history, diffs, and other general repository operations stay with Git; do not add `wt` commands that merely wrap them.

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
- **Parallel Development:** Organize code and tests so independent areas can usually be changed independently. If a file repeatedly becomes a conflict hotspot across parallel tasks, split it along natural responsibility boundaries when doing so also improves maintainability. Do not fragment code solely to avoid Git conflicts.
- **TODOs:** Identify relevant deferred work, bring it to my attention first, and only then record it in `project-notes/TODO.md`. Do not leave TODO, FIXME, or similar deferred-work comments in source code.
- **Dead Code:** Remove unused or obsolete code.
- **Shared Code:** Extract common functionality when it genuinely reduces duplication or improves maintainability.
- **Constants:** Prefer constants at the lowest appropriate scope. Only share constants when they represent the same concept, not merely because their values match.
- **Significant Changes:** Surface substantial refactoring, architectural decisions, public behavior changes, or important trade-offs when they provide meaningful value.

Implement straightforward, clearly beneficial improvements where appropriate. Surface decisions that require my input before proceeding.

## 3. Safety & Error Handling

### Safety invariants

These must always hold. Review every command, new or changed, against each of them.

| Invariant | Meaning |
| --- | --- |
| Never silently discard working changes | Modified, staged, untracked, ignored, or conflicted files must survive unless the user explicitly chose to delete them. |
| Never force by default | No `--force`, hard resets, branch deletion, or similar behavior in normal operation. |
| Git is the source of truth | Do not maintain a second state database that can disagree with Git. |
| Validate immediately before mutation | Re-read the state the mutation depends on instead of relying on earlier checks. |
| Success is verified, not assumed | After mutating, confirm the intended result from Git and the filesystem; an exit code alone is not proof. |
| Failed operations leave understandable state | Especially for `new`, `merge`, and `remove`, explain what changed and what was left behind. |
| Only proven-landed branches are deleted | `wt remove` never deletes branches. Only `wt merge --cleanup` may delete the task branch, after proving it is contained in the integration branch, and only with `git branch -d`. |
| Conflict resolution stays manual | `wt` never guesses how a conflict should be resolved. |
| Unknown state means stop | Never treat uncertainty as safety. |
| User input never becomes shell syntax | Pass arguments as argv, never through a shell; quote commands printed for users to paste. |
| Errors explain recovery | Say what happened and what the user should do next. |

### Practices

- Validate and sanitize paths, branch names, command arguments, and other external inputs.
- Consider edge cases, partial failures, interrupted operations, and concurrent processes.
- Use retries only when appropriate; do not mask persistent failures.
- Add a `WARNING:` comment at checks that uphold a safety invariant so they are not relaxed accidentally.
- Never bypass a `wt` safety check.
- Structure consequential commands as: **discover → validate → plan → mutate → verify → report**.

## 4. Dogfooding `wt`

All development on this repository must exercise `wt` as the intended worktree workflow.

- All feature, fix, refactor, or other code-changing work must begin in a new worktree created through `wt`. Do not make development changes directly in the primary checkout.
- Use `wt` for every worktree lifecycle operation, and do not use agent-provided worktree tools.
- Do not use Git directly to:
  - create, remove, prune, move, lock, or otherwise manage worktrees;
  - switch or check out a different branch for a worktree;
  - delete or rename task branches (`wt merge --cleanup` deletes landed ones);
  - merge a task branch into the integration branch.
- General Git operations outside the `wt` product boundary are allowed, including `git status`, `git diff`, `git log`, `git add`, `git commit`, `git rebase`, and `git restore`.
- Use `git restore` for file-level restoration and conflict resolution; do not use `git checkout`.
- If `wt` is broken, unsafe, or lacks a required lifecycle capability, stop and report the limitation instead of bypassing it. Keep the missing-capabilities section of `project-notes/wt-usage.md` current as gaps are found or resolved.
- Friction in general Git workflows is not by itself a reason to add a `wt` command. Only propose new `wt` commands that fit the product boundary above.
- Record meaningful `wt` friction in `project-notes/wt-usage.md`.
- When a rebase produces conflicts, record one concise entry in `project-notes/wt-usage.md` naming the task and conflicted files. Do not log conflict-free rebases.
- Record significant product, architecture, safety, reliability, performance, or usability findings in `project-notes/findings.md`, and surface important findings when discovered.
- Do not log routine successful commands or generate unnecessary noise.

This repository keeps `main` linear. Before reporting a task complete or landing it, follow `docs/agent-workflows/landing-tasks.md`.

## 5. Code Quality & Documentation

- Document new CLI commands, options, and developer-facing commands in `README.md` as they are introduced.
- Keep documentation consistent with the implementation.
- Comments should explain **why** something is done, not duplicate technical documentation.
- Keep comments natural, concise, useful, and current.

## 6. Testing & Changes

- Create and maintain unit and end-to-end tests as development progresses.
- Cover normal behavior, relevant edge cases, errors, and failure scenarios.
- Tests must never be able to modify or delete real user repositories or data.
- Command logic takes its working directory and output streams from `wt::Context`, never from process-global state, so tests can run against an isolated sandbox (`tests/support/sandbox.hpp`).
- When modifying code, review the blast radius and update all affected code, references, documentation, comments, and tests.
- Do not leave stale or inconsistent code behind.
- After each code-changing commit, rebuild and run the relevant tests so the build reflects the committed state.
- Before reporting a code-changing task complete or landing it, rebuild and run the full test suite against the exact final committed and rebased state that will be landed.
- Do not claim verification unless the relevant build and tests were actually run.

## 7. Task Handoff

End each task with a concise handoff using this structure:

- **Status:** `READY`, `NEEDS_DECISION`, `BLOCKED`, or `LANDED`
- **Summary:** 1–2 sentences describing what changed and the outcome
- **Verification:**
  - tests: pass/fail and count;
  - formatting/build: pass/fail;
  - rebased onto latest `main`: yes/no;
  - `main` moved during the task: yes/no.
- **Review:**
  - safety issue found: yes/no;
  - public behavior changed: yes/no;
  - product scope changed: yes/no;
  - new finding/TODO: yes/no.
- **Decision needed:** `None`, or the exact decision
- **Worth external review:** `Yes` or `No`, with one sentence explaining why
- **Next action:** exact command or action, if any

Independent code review is required before landing any change that:

- touches code marked with a `WARNING:` safety comment; or
- changes a command's mutation or post-mutation verification path.

The implementing agent may not waive required independent review.

The reviewer must inspect the actual diff and relevant surrounding code and return `APPROVE`, `FIX_REQUIRED`, or `DECISION_REQUIRED`.

Documentation-only and test-only changes do not require independent review unless they also change behavior or reveal a decision that needs input.

Only add a **Details** section when something unusual, risky, or non-obvious happened.

Do not repeat routine checks or implementation details unless they affect a decision.

If **Decision needed** is `None`, required independent review has passed or is not required, **Worth external review** is `No`, and verification found no issues, the task does not need additional review: land it and clean up (`wt merge <task> --cleanup`, following `docs/agent-workflows/landing-tasks.md`) without asking first, then report `LANDED`.

## 8. Project Notes

Keep project knowledge and decisions in this repository (`AGENTS.md`, `project-notes/`), not in agent-private memory. Claude auto-memory is disabled for this project in `.claude/settings.json`.

Use `project-notes/` for information that should persist but does not belong in source code:

- `project-notes/TODO.md` — deferred work that has already been surfaced to me and approved for recording.
- `project-notes/wt-usage.md` — missing `wt` capabilities, meaningful friction, observations from real use, and concise records of actual rebase conflicts.
- `project-notes/findings.md` — significant product, architecture, safety, reliability, performance, or usability findings: **Open** ones still need action; **Addressed** ones explain why existing checks and rules exist. Move a finding to Addressed when it is handled.

Keep these files concise, current, and actionable. Remove resolved items instead of keeping them as permanent historical logs.

**Guiding Principle:** Build a reliable, safe, and maintainable CLI that developers and AI agents can trust. Dogfood `wt` so real worktree-lifecycle friction is discovered rather than bypassed.
