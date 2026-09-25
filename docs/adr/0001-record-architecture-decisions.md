# ADR 0001: Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-09-24
- **Workflow:** W1 (Foundation and RAW probe)

## Context

Lucent is built in many increments (`docs/development/roadmap.md`), often by several developers
or agents working in parallel, and it is expected to live for years: recipes, catalogs and job
records written today must still open after future updates (blueprint section 8). Many choices
made early (dependency management, linking model, error model, data contracts) are expensive to
reverse, and their reasons are easy to forget. Commit messages capture *what* changed but are hard
to find later and rarely weigh alternatives. The blueprint
(`docs/blueprint/lucent-desktop-raw-processing-blueprint.md`) sets product direction, but it does
not record how each implementation decision was reached or where the implementation deliberately
deviates from it.

We need a lightweight, reviewable record, kept next to the code, that explains why the codebase
looks the way it does.

## Decision

Lucent records architecturally significant decisions as Architecture Decision Records in
`docs/adr/`, in a lightweight form of [MADR](https://adr.github.io/madr/) (Markdown Architectural
Decision Records).

1. **When to write one.** Write an ADR for a decision with lasting consequences, in particular:
   - a new third-party dependency, a changed linking model, or a licensing choice;
   - a persisted or exchanged format or contract (recipes, catalog schema, worker protocol, asset
     manifests, the decoded-RAW data contract), or a change to its versioning rules;
   - a cross-module interface or a change to the module layering in
     `docs/architecture/overview.md`;
   - a deliberate deviation from the blueprint, or a choice between options the blueprint leaves
     open;
   - build, toolchain or platform choices that affect every developer.

   Local implementation choices that are easy to change do not need an ADR; a code comment or a
   commit message is enough.
2. **File name and numbering.** `NNNN-short-kebab-title.md`, numbered sequentially from `0001`.
   Numbers are never reused. `docs/adr/README.md` is the index; every new ADR is added to it in the
   same change.
3. **Structure.** Each ADR starts with a status block (Status, Date, Workflow, and optionally
   Supersedes / Superseded by) followed by the sections **Context**, **Decision**,
   **Consequences** and **Alternatives considered**. `docs/adr/template.md` is the starting point.
4. **Status values.** `Proposed` (under review), `Accepted`, `Superseded by ADR NNNN`, `Deprecated`
   (no longer relevant, not replaced) and `Rejected` (recorded so the discussion is not repeated).
5. **Immutability.** Once accepted, an ADR's decision is not rewritten. A changed decision gets a
   new ADR that supersedes the old one; the old ADR only has its status line updated and a link to
   the successor added. Typo fixes, broken links and clarifications that do not change meaning are
   allowed.
6. **Review.** ADRs are reviewed like code and land in the same branch as the change they justify,
   so the decision and its first implementation are reviewed together.
7. **Relationship to other documents.** The blueprint remains the product authority;
   `docs/architecture/overview.md` describes the current structure; `docs/development/conventions.md`
   states the coding rules. ADRs explain the reasoning behind those documents and link to them
   rather than duplicating them.

## Consequences

- New contributors can learn why things are the way they are without reconstructing history from
  commits.
- Reviews of significant changes have an explicit artefact to discuss, including the alternatives
  that were rejected.
- Writing an ADR costs some time. The "lasting consequences" test keeps them to decisions that
  justify the cost.
- Superseded ADRs remain in the repository, so the index must make the current status obvious.

## Alternatives considered

- **No formal records (commit messages and code comments only).** Cheapest, but reasoning is
  scattered, alternatives are rarely written down, and the history of a decision is hard to find.
  Rejected.
- **A wiki or external document store.** Separates decisions from the code they govern, is not
  versioned with the code, and is unavailable offline or from a plain checkout. Rejected.
- **Nygard's original ADR format (Context / Decision / Status / Consequences).** Very close to what
  was chosen; MADR adds an explicit "Alternatives considered" section, which matters for Lucent
  because many decisions (dependencies, licences) are choices among several viable options.
- **A heavyweight RFC process with formal sign-off.** Disproportionate for the current team size.
  It can be introduced later by superseding this ADR.
