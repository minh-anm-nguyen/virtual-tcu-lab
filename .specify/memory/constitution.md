<!--
Sync Impact Report
- Version change: template (unversioned) → 1.0.0
- Initial ratification; no prior project-specific values existed to preserve.
- Modified principles: none (all five newly defined)
- Added sections: Core Principles I–V, Technology Constraints, Development Workflow, Governance
- Removed sections: none
- Deferred TODOs: none
- Scratch material for amendment review; remove before committing the amended file.
-->

# Virtual TCU Lab Constitution

## Core Principles

### I. Spec Fidelity and Scope Discipline (NON-NEGOTIABLE)

Each project's `spec.md` is the single source of truth. Implementation MUST deliver exactly the
"Phạm vi v1" section and MUST NOT implement anything listed under "Ngoài phạm vi v1". Items such as
distributed messaging, durable queues, exactly-once delivery, hard real-time guarantees, thread
pools and zero-copy are deliberately deferred; adding them early is a violation, not an
improvement. A requirement absent from the spec MUST be added to the spec first, then built.

Rationale: the 30 designs are deliberately layered. Early over-building inflates the surface that
all 29 later projects must keep working.

### II. Library-First Reuse

The repository is a single monorepo. Each project is its own CMake target with a public header
surface; later projects MUST consume earlier projects through that target. Copying, vendoring or
re-implementing code that an earlier project already provides is forbidden. A shared need
discovered late MUST be fixed once in the owning project, not forked.

Rationale: project 01's event bus is a dependency of most later projects. Duplicating it turns one
bug into many.

### III. Test-First (NON-NEGOTIABLE)

Tests are written before implementation and MUST fail before they pass. Coverage is defined by the
spec, not by a percentage:

- Every functional requirement (`FR-xx`) MUST have at least one GoogleTest unit test naming it.
- Every Given/When/Then scenario in the spec's BDD section MUST have a corresponding integration
  test.
- Every acceptance criterion in the spec's "Definition of Done" MUST be observable by a test or a
  documented demo command.

Rationale: the specs already state behaviour precisely; tests are a transcription job, so there is
no excuse to defer them.

### IV. Simulation-Only

No real hardware, no real vehicle network, no real cloud endpoint, no real modem. CAN, SOME/IP,
GNSS, MQTT brokers, modems and cloud backends are simulated in-process or over loopback. Time,
randomness and network faults MUST be injectable so tests stay deterministic.

Rationale: the lab must run on any developer machine and in CI with reproducible results.

### V. Safe Observability

Structured logging and metrics are required where the spec asks for them. Secrets MUST NOT reach
logs, metrics, event payloads or test fixtures: no private keys, passwords, tokens, session
credentials or raw certificates. Sensitive fields are redacted at the point of logging, not
downstream. Callback and worker failures MUST be isolated and counted, never silently swallowed.

Rationale: a simulator that teaches unsafe logging habits transfers those habits to production
code.

## Technology Constraints

- Language: C++17. No compiler-specific extensions in library code.
- Build: CMake >= 3.20, out-of-source builds, one target per project.
- Tests: GoogleTest / GoogleMock, registered with CTest.
- Platform: must build and test on Linux; Windows is best-effort unless a spec states otherwise.
- Dependencies: standard library and already-present dependencies first. A new third-party
  dependency requires justification in the project's `plan.md` under Complexity Tracking.
- Each project owns its directory, its public headers, its tests and its own CMakeLists.txt.

## Development Workflow

Projects are built in ascending order 01 → 30, because each design's integration section names the
later projects that consume it. Per project:

1. `spec.md` is the design document, copied verbatim; it is edited in `specs/`, not regenerated.
2. `/speckit-plan` → `/speckit-tasks` → `/speckit-implement`.
3. Gate: the project is done only when the build is clean, all tests pass, and every acceptance
   criterion in the spec is satisfied. Report unmet criteria explicitly rather than closing the
   project.
4. Commit per project with the project number in the message.

Work interrupted mid-project resumes via `/speckit-converge`, which reconciles the existing code
against `spec.md` and appends the remaining work to `tasks.md`.

## Governance

This constitution supersedes conflicting habits, defaults and convenience. Every plan, task list
and review MUST verify compliance with Principles I–V.

- Amendments require editing this file, a version bump, and a one-line rationale in the Sync Impact
  Report.
- Versioning: MAJOR for removing or redefining a principle; MINOR for adding a principle or
  materially expanding guidance; PATCH for clarifications and wording.
- A deliberate deviation MUST be recorded in that project's `plan.md` Complexity Tracking section
  with the simpler alternative that was rejected and why. An unrecorded deviation is a defect.
- Compliance is reviewed at each project's completion gate, before the commit.

**Version**: 1.0.0 | **Ratified**: 2026-11-04 | **Last Amended**: 2026-11-04
