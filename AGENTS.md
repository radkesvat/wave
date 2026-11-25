# AGENTS.md — Wave

Wave is a web panel under development for the WaterWall core, which is maintained
as a separate project. Wave will have a frontend and a backend, use C++, and
target WebAssembly.

## 1. Task Authority, Scope, And Evidence

### What a request authorizes

- For **review, diagnose, explain, or plan** requests, inspect relevant code,
  history, tests, and documentation. Report findings without editing files
  unless the request includes changes.
- For **fix, change, build, implement, or review-and-fix** requests, make the
  requested in-scope changes and run relevant non-destructive validation.
- Read-only investigation and in-scope edits, builds, tests, and formatting need
  no confirmation. Ask about unresolved consequential decisions, destructive
  actions, or external work outside the authorized scope.
- When ambiguity materially affects behavior, scope, or contracts, ask focused
  questions. Use reversible assumptions for low-impact details and state them.
- Preserve unrelated worktree changes.

### Durable project knowledge

Keep all Wave project documentation under [`doc/`](doc/), organized by topic.
Update the relevant document when an in-scope change alters a documented
contract. Use evidence from the code, tests, and documentation to resolve
questions about existing behavior. Report conflicts or gaps that affect the task.

## 2. Repository Map And Reference Material

| Path | Purpose |
| --- | --- |
| `local/WaterWall/` | Upstream core source checkout |
| `local/WaterWall-Docs/` | Upstream documentation checkout |

Wave's `.gitignore` covers `local/`. Keep reference checkouts and other local
resources there. Treat everything under `local/`, the upstream WaterWall core,
and the upstream documentation as read-only. Consult the upstream source and
documentation when Wave work depends on WaterWall behavior or configuration.
Prepare justified upstream suggestions with supporting evidence and submit them
when authorized.

## 3. Implementation Workflow

1. Inspect the worktree, relevant diff or history, and nearby callers and
   dependencies. Use `rg` and `rg --files` for searches.
2. Trace the behavior and failure paths before making changes. Check existing
   patterns against their contracts and callers.
3. Implement the smallest complete fix that addresses the root cause. Avoid
   unrelated cleanup and speculative abstractions.
4. Add or update a focused regression test when existing coverage misses a
   reproducible defect or a meaningful behavior change.
5. Review the final diff for accidental changes, missing cleanup, and coverage
   of the changed behavior.

## 4. Code Review Rules

- Use the request and acceptance criteria, agreed plan and clarifications,
  applicable contracts and tests, and required compatibility and security
  behavior as the review baseline.
- Inspect changed behavior, dependencies, and failure paths for concrete defects.
  Consider correctness, security, compatibility, portability, and build and
  test behavior where relevant.
- For each blocker, cite the file and line, severity, concrete trigger and
  impact, and the requirement or evidence it violates. Report all known blockers
  together with a safe correction and specific acceptance conditions.
- Report optional, stylistic, speculative, or out-of-scope findings separately.
  In a follow-up, verify the requested fixes and check for regressions in the
  new changes. Reopen accepted decisions or resolved blockers only when new
  material evidence warrants it.
- End each review with **accepted**, **accepted with optional follow-ups**,
  **changes required**, or **inconclusive**. For required changes, state the
  remaining blockers and how to verify them. For an inconclusive review, name
  the missing evidence.

## 5. Build, Format, Test, And Validate

- Use the project's build, test, and formatting configuration when available.
  Limit formatting to the affected files or changed ranges.
- Run focused checks that exercise changed behavior, affected callers, and
  relevant failure paths. Use broader tests when the change's reach warrants
  them. For prose, review the diff and verify changed links or commands.
- Use build and runtime checks where the change requires them. Report any
  validation blocked by missing tools, timeouts, or environment failures, along
  with what remains unverified.
