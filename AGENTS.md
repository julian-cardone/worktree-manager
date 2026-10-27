# AGENTS.md

Read this file at the start of every prompt. Do not rely on memory of previous instructions.

## What This Is

`wt` is a small C++ CLI for simplifying Git worktree management so developers and AI agents can work on multiple tasks concurrently.

**Product boundary:** `wt` owns the worktree lifecycle: creating, listing, inspecting, merging, removing task worktrees, and the branch cleanup that goes with them.

It is not a Git replacement. Staging, committing, history, diffs, and other general repository operations stay with Git; do not add `wt` commands that merely wrap them.

## 1. Project-Specific Engineering

- **TODOs:** Identify relevant deferred work, bring it to my attention first, and only then record it in `project-notes/TODO.md`. Do not leave TODO, FIXME, or similar deferred-work comments in source code.
- **README:** Document new CLI commands, options, and developer-facing commands in `README.md` as they are introduced.

## 2. Safety & Error Handling

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
| Errors explain recovery | Say what stopped the operation, whether `wt` changed anything, and what the user should do next. Never say nothing changed once a mutation may have started. |

### Practices

- Read Git through machine-oriented interfaces: porcelain formats, `-z`, `--format`, and exit codes. Never parse output meant for humans, including error messages, which are translated. Query one path per command when Git has no `-z`, since paths can contain newlines.
- Add a `WARNING:` comment at checks that uphold a safety invariant so they are not relaxed accidentally.
- Never bypass a `wt` safety check.

## 3. Dogfooding `wt`

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
- Record concise, non-routine friction in `project-notes/wt-usage.md`, including actual rebase conflicts, repeated manual workarounds, recurring cleanup steps, workflow interruptions, review passes that find issues, rework caused by parallel tasks, and unexpectedly token- or context-heavy tasks or reviews. For conflicts, name the task and conflicted files. For unusually expensive work, briefly note the cause when known. Do not log conflict-free rebases, routine approvals, routine successful commands, or normal token usage.
- Record significant product, architecture, safety, reliability, performance, or usability findings in `project-notes/findings.md` (Open) or its subsystem files (Addressed), and surface important findings when discovered.
- Do not log routine successful commands or generate unnecessary noise.

This repository keeps `main` linear. Before reporting a task complete or landing it, follow `docs/agent-workflows/landing-tasks.md`.

## 4. Project-Specific Testing

- Command logic takes its working directory and output streams from `wt::Context`, never from process-global state, so tests can run against an isolated sandbox (`tests/support/sandbox.hpp`).
- Before reporting a code-changing task complete or landing it, rebuild and run the full test suite against the exact final committed and rebased state that will be landed.

## 5. Task Handoff

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

Independent code review is required before landing a change that alters safety semantics, including:

- weakening or materially changing a safety invariant;
- changing destructive behavior, cleanup semantics, branch deletion, or ownership guarantees;
- changing what `wt` considers safe enough to mutate;
- changing the meaning or verification of a successful mutation;
- introducing a new safety-sensitive recovery path or user opt-in.

A change does not require independent review solely because it touches a `WARNING:` comment, mutation code, or verification code. Routine fixes that preserve an existing safety rule, tests, documentation, diagnostics, and behavior-preserving refactors may self-land after normal verification.

The implementing agent must not waive a review that is required by the criteria above.

When independent review is required, review only:

- the final diff;
- the specific safety invariant or behavior affected;
- the directly relevant surrounding code.

Do not re-audit the entire command or repository unless the diff reveals a concrete reason to widen the review.

The reviewer must return one of:

- `APPROVE` — brief rationale only;
- `FIX_REQUIRED` — identify the concrete issue and required fix;
- `DECISION_REQUIRED` — state the exact unresolved decision.

Only add broader analysis when needed to explain `FIX_REQUIRED` or `DECISION_REQUIRED`.

Documentation-only and test-only changes do not require independent review unless they change behavior or reveal a safety decision that needs input.

Only add a **Details** section when something unusual, risky, or non-obvious happened.

Do not repeat routine checks or implementation details unless they affect a decision.

If **Decision needed** is `None`, required independent review has passed or is not required, **Worth external review** is `No`, and verification found no issues, the task does not need additional review: land it and clean up (`wt merge <task> --cleanup`, following `docs/agent-workflows/landing-tasks.md`) without asking first, then report `LANDED`.

## 6. Project Notes

Keep project knowledge and decisions in this repository (`AGENTS.md`, `project-notes/`), not in agent-private memory. Claude auto-memory is disabled for this project in `.claude/settings.json`.

Use `project-notes/` for information that should persist but does not belong in source code:

- `project-notes/TODO.md` — deferred work that has already been surfaced to me and approved for recording.
- `project-notes/wt-usage.md` — missing `wt` capabilities, meaningful friction, observations from real use, and concise records of actual rebase conflicts.
- `project-notes/findings.md` and `project-notes/findings/` — significant product, architecture, safety, reliability, performance, or usability findings: **Open** ones, in `findings.md`, still need action; **Addressed** ones, in the subsystem file for their area, explain why existing checks and rules exist. Read the relevant subsystem file before changing that area. Move a finding to Addressed when it is handled.

Keep these files concise, current, and actionable. Remove resolved items instead of keeping them as permanent historical logs.

**Guiding Principle:** Build a reliable, safe, and maintainable CLI that developers and AI agents can trust. Dogfood `wt` so real worktree-lifecycle friction is discovered rather than bypassed.
