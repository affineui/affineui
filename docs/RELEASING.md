# Releasing

The AffineUI release process is a two-step, human-in-the-loop flow driven
entirely by two `workflow_dispatch` UIs on the GitHub Actions tab. Nobody
tags by hand, nobody edits `pyproject.toml` by hand, nobody types a version
number twice. This document describes the model and the concrete steps.

## The model

We ship four artifacts on every release:

| Artifact         | Ecosystem   | Registry           | Trigger                     |
|------------------|-------------|--------------------|-----------------------------|
| Python wheels    | PyPI        | pypi.org           | tag `vX.Y.Z` (final)        |
| Python wheels    | TestPyPI    | test.pypi.org      | any prerelease tag (`vX.Y.Z-rc.N`, `-beta.N`, `-alpha.N`) |
| Rust crates      | crates.io   | crates.io          | any `v*` tag                |
| .NET / NuGet     | nuget.org   | nuget.org          | any `v*` tag                |
| Amalgamated SDK  | GitHub      | GitHub Release     | any `v*` tag                |

Every published version MUST correspond to a merged, reviewed release
notes file. Notes are keyed on the **core** version (`MAJOR.MINOR.PATCH`),
so `v1.1.3-rc.1`, `v1.1.3-rc.2`, and the final `v1.1.3` all share the
SAME `docs/release-notes/v1.1.3.md` file — one story per release cycle,
iterated through its pre-releases and its eventual final. That's the
enforceable contract — the publish jobs fail loud if the core-versioned
notes file is missing. This makes the two-step flow the only sanctioned
path to a release.

The notes cover **everything since the last stable release**, not just
the diff since the interim pre-release. If the last stable was `v1.1.2`,
then `docs/release-notes/v1.1.3.md` covers everything from `v1.1.2..HEAD`
regardless of which iteration (`-rc.1`, `-rc.2`, or the final) you're
about to cut. Re-drafting during RC iteration picks up any newly-merged
PRs.

### The two steps

**Step 1 — [Draft release notes](../.github/workflows/draft-release-notes.yml)** —
`workflow_dispatch`. Pick which segment to bump (`patch`, `minor`, `major`,
or `none`) and which channel (`release`, `rc`, `beta`, `alpha`). The job:

1. Reads the last `v*` tag on `origin` (both "last of any kind" and
   "last stable" — the former for series continuation, the latter for
   the changelog range).
2. Runs [`scripts/next_version.py`](../scripts/next_version.py) to compute
   the next version. Understands series continuation
   (`v1.1.3-rc.1` + bump=none + mode=rc → `1.1.3-rc.2`), promotion
   (`v1.1.3-rc.2` + bump=none + mode=release → `1.1.3`), and refuses
   no-ops. Also emits the `core` (MAJOR.MINOR.PATCH) for the file path.
3. Installs `@github/copilot` (the GitHub Copilot CLI) and feeds it the
   git log since the last **stable** tag.
4. Copilot drafts `docs/release-notes/v<CORE>.md` in the requested
   Highlights / New features / Changes / Bug fixes shape. Same file
   regardless of whether we're drafting an rc.1, rc.2, or final —
   the whole cycle shares one notes doc.
5. Opens (or refreshes) a PR titled `Release notes for v<CORE>` off
   the branch `release-notes/v<CORE>`.

The workflow is **idempotent**. Both the branch and the notes file are
keyed on `<CORE>`, so:

- Re-drafting from `-rc.1` to `-rc.2` (bump=none, mode=rc, same core)
  refreshes the SAME PR with any newly-merged PRs picked up from the
  log range.
- Drafting from `-rc.N` to final (bump=none, mode=release, same core)
  refreshes the SAME PR — usually with only cosmetic tightening.

Force-with-lease guards against clobbering human edits merged to `main`.
Concurrency-gated so a re-dispatch supersedes an in-flight draft.

**Step 2 — reviewer merges the notes PR.** Edit `docs/release-notes/v<CORE>.md`
inline in the PR — the AI draft is a starting point, not the final copy.
The Highlights section in particular usually wants a human pass. Squash-
or rebase-merge as usual.

**Step 3 — [Cut release](../.github/workflows/cut-release.yml)** —
`workflow_dispatch`. Pass the same version (bare, e.g. `1.2.4-rc.1`). The
job:

1. Re-validates the version via `scripts/set_version.py --check`.
2. Confirms `docs/release-notes/v<CORE>.md` exists on `main` (same file
   shared across every pre-release + the final in this cycle).
3. Confirms `origin` has no tag with that name yet.
4. Creates an annotated tag `v<VERSION>` (the FULL version, including any
   `-rc.N` suffix) **with the notes file's contents as the tag message**.
5. Pushes.

The existing [`release.yml`](../.github/workflows/release.yml) and
[`wheels.yml`](../.github/workflows/wheels.yml) workflows already trigger
on `push: tags: 'v*'` and take it from there — cargo, nuget, PyPI or
TestPyPI, GitHub Release with the amalgamation zip attached.

## Repo secrets

Add these once at Settings → Secrets and variables → Actions:

| Secret                 | Scope     | Where you get it                                             |
|------------------------|-----------|--------------------------------------------------------------|
| `CARGO_REGISTRY_TOKEN` | crates.io | crates.io Account Settings → API Tokens                      |
| `NUGET_API_KEY`        | nuget.org | nuget.org Account → API Keys                                 |

That's the full list. Two ecosystems don't need a secret:

- **PyPI + TestPyPI**: Trusted Publisher (OIDC) via the `pypi` / `testpypi`
  GitHub environments already configured on the repo.
- **Copilot CLI** (release-notes drafting): the built-in `GITHUB_TOKEN`
  works directly — no PAT required. Per the GitHub docs, "using
  `GITHUB_TOKEN` (recommended for organization-owned repositories) — no
  PAT or stored secrets required."

## The initial retro-tag

On a fresh repo (no `v*` tags on `origin`), the workflows need a baseline
tag to compute "since when". Because the binding manifests
(`bindings/python/pyproject.toml`, `bindings/rust/Cargo.toml`) have been
declaring `0.0.3` for a while, we retro-tag the current baseline as
`v0.3.0` — that becomes the last-stable reference for the first real
release cycle.

```bash
# From a clean origin/main checkout:
git tag -a v0.3.0 -m "baseline tag for release automation (retro-tag; not a published release)"
git push origin v0.3.0
```

Because there is no `docs/release-notes/v0.3.0.md`, the publish gate in
`release.yml` + `wheels.yml` fails fast on the retro-tag — the tag is
created on `origin` (so `next_version.py` has something to read), but
nothing gets published to any registry. That's the intended shape:
`v0.3.0` is a REFERENCE POINT, not a published release.

After that, the first real cycle runs through the Draft → Cut flow like
any other release. Typical first move: dispatch **Draft release notes**
with `bump=minor`, `mode=release` → produces `v0.4.0` (or higher, if you
want a bigger bump). The notes cover everything since `v0.3.0`, which is
what you want.

## Version scheme

Semver 2.0.0 with a `v` prefix on tags:

```text
vMAJOR.MINOR.PATCH[-PRE][+BUILD]
```

`MAJOR.MINOR.PATCH` are non-negative integers. `-PRE` is optional and
identifies pre-releases (`-rc.1`, `-beta.2`, `-alpha.3`). `+BUILD` is
optional build metadata — **not** a pre-release marker, even though it
also contains a `-`.

### Bump semantics

| bump   | Effect on `X.Y.Z`      |
|--------|------------------------|
| `major`| `(X+1).0.0`            |
| `minor`| `X.(Y+1).0`            |
| `patch`| `X.Y.(Z+1)`            |
| `none` | `X.Y.Z` (unchanged)    |

`none` is the right pick when you want to bump only the pre-release
counter (e.g. rc.1 → rc.2) or promote a pre-release to the stable
triplet.

### Mode semantics

| mode      | Suffix     | Counter                                                                 |
|-----------|------------|-------------------------------------------------------------------------|
| `release` | (none)     | n/a — strips any existing pre-release                                   |
| `rc`      | `-rc.N`    | Continues if last tag was same-core `-rc.M` (`N = M+1`); else starts at 1 |
| `beta`    | `-beta.N`  | Same continuation rule                                                  |
| `alpha`   | `-alpha.N` | Same continuation rule                                                  |

The counter continues only when both bump=none AND the mode matches the
last tag's suffix. Bumping the core (patch/minor/major) always starts a
fresh series at `.1`. Switching modes (rc → beta) also starts fresh.

## Cutting a release: five common recipes

### Cut a bug-fix patch release

Last stable `v1.2.3`. Want `v1.2.4`.

1. Draft: bump=`patch`, mode=`release` → PR opens with
   `docs/release-notes/v1.2.4.md` (notes covering `v1.2.3..HEAD`).
2. Review + merge.
3. Cut: version=`1.2.4`.

### Cut a patch RC series and iterate

Last stable `v1.2.3`. Want `v1.2.4-rc.1`, then iterate through `-rc.2`,
`-rc.3`, then promote to final.

1. Draft: bump=`patch`, mode=`rc` → PR opens with
   `docs/release-notes/v1.2.4.md`. Same core, same file. Merge.
2. Cut: version=`1.2.4-rc.1`. → wheels ship to TestPyPI; cargo + nuget
   publish as pre-release; both use `docs/release-notes/v1.2.4.md` as
   the release body.
3. Fix issues, land PRs on `main`.
4. Draft: bump=`none`, mode=`rc` → **refreshes the SAME PR** with an
   updated `docs/release-notes/v1.2.4.md` (Copilot re-drafts against
   `v1.2.3..HEAD`, picking up the freshly-merged PRs). Merge. Cut
   version=`1.2.4-rc.2`.
5. Repeat.
6. Promote: draft with bump=`none`, mode=`release` → SAME PR refreshes
   one last time (notes still against `v1.2.3`, still in the same file).
   Merge. Cut version=`1.2.4` → wheels ship to real PyPI, `-rc` suffix
   drops.

Key point: **one PR + one notes file per release cycle.** Every dispatch
during the v1.2.4 cycle updates `docs/release-notes/v1.2.4.md` — never a
separate file per RC.

### Cut a minor release

Last stable `v1.2.3`. Want `v1.3.0`.

1. Draft: bump=`minor`, mode=`release` → PR for
   `docs/release-notes/v1.3.0.md`. Merge.
2. Cut: version=`1.3.0`.

Skip the RC dance when you're confident. A minor bump directly to
`release` is normal — the RC flow is for when you want a soak period
against real users first.

### Cut a major release

Same as minor, with bump=`major`. Consider running an RC series first
for anything users have been depending on for a while.

### Promote an RC to release without any new commits

Last cut `v1.2.4-rc.2` (last stable is still `v1.2.3`). You're ready to
ship.

1. Draft: bump=`none`, mode=`release`. → refreshes
   `docs/release-notes/v1.2.4.md` (notes still range against `v1.2.3`,
   still cover the same delta). Usually only cosmetic edits over the
   RC's notes. Merge.
2. Cut: version=`1.2.4`. → wheels ship to real PyPI, cargo + nuget drop
   the `-rc` suffix, GitHub Release is marked final.

## What lives where

| File                                                  | Role                                                                                  |
|-------------------------------------------------------|---------------------------------------------------------------------------------------|
| [`scripts/set_version.py`](../scripts/set_version.py) | Patches every version-holding file in the repo to a given version, then validates. Also runs in `--check` mode to classify pre-release vs stable. |
| [`scripts/next_version.py`](../scripts/next_version.py) | Computes the next version from `(last_tag, bump, mode)`.                              |
| [`.github/workflows/draft-release-notes.yml`](../.github/workflows/draft-release-notes.yml) | Step 1 — draft + PR.                                                                  |
| [`.github/workflows/cut-release.yml`](../.github/workflows/cut-release.yml)                 | Step 3 — validate + tag + push.                                                       |
| [`.github/workflows/release.yml`](../.github/workflows/release.yml)                         | Triggered by tag push. Amalgamates the two-file SDK, publishes cargo + nuget, creates GitHub Release. |
| [`.github/workflows/wheels.yml`](../.github/workflows/wheels.yml)                           | Triggered by tag push. Builds Python wheels + sdist across three OSes, publishes to PyPI (final) or TestPyPI (pre-release). |
| `docs/release-notes/v<CORE>.md`                       | The reviewed notes for a release cycle. Committed to `main` before cutting. Same file is used for every `-rc/-beta/-alpha` in the cycle and for the final. |

## What each publisher does with a pre-release

- **crates.io** accepts `1.2.4-rc.1` verbatim. `cargo add affineui` picks the newest stable version by default; users opt into pre-releases explicitly.
- **nuget.org** accepts `1.2.4-rc.1` verbatim. `dotnet add package` skips pre-releases by default; users opt in with `--prerelease` or an explicit version.
- **PyPI** doesn't have a "hidden pre-release" bucket — for us, pre-release tags publish to **TestPyPI** instead. Users install pre-releases from that index.
- **GitHub Release** is marked as pre-release (a UI badge, and it doesn't count as "latest").

## Consuming AffineUI

Concrete install commands for consumers, split by ecosystem and by
whether you want the latest stable or a specific pre-release.

### Rust — crates.io

Stable, latest:

```bash
cargo add affineui
```

Stable, a specific version:

```bash
cargo add affineui@1.2.3
```

Pre-release (crates.io accepts `-rc.N`/`-beta.N`/`-alpha.N` inline):

```bash
cargo add affineui@1.2.4-rc.1
```

Or add it in `Cargo.toml` directly — cargo requires you to name the
pre-release exactly, it will not auto-resolve to a suffix:

```toml
[dependencies]
affineui = "1.2.4-rc.1"     # exact opt-in
# or a semver range that INCLUDES pre-releases in that core:
affineui = ">=1.2.4-rc.1, <1.3.0"
```

Cargo will not pick up `1.2.4-rc.2` from `affineui = "1.2.4"` — a bare
version requirement excludes all pre-releases. That's what you want as a
downstream: your `cargo update` won't accidentally pull in an RC.

### .NET — nuget.org

Stable, latest:

```bash
dotnet add package AffineUI
```

Pre-release (nuget's opt-in is a flag on `add package`):

```bash
dotnet add package AffineUI --prerelease
```

That resolves to the newest version including pre-releases. To pin an
exact pre-release:

```bash
dotnet add package AffineUI --version 1.2.4-rc.1
```

Or in the `.csproj`:

```xml
<ItemGroup>
  <PackageReference Include="AffineUI" Version="1.2.4-rc.1" />
</ItemGroup>
```

Same rule as cargo: a bare stable-versioned `PackageReference` will not
resolve to a pre-release, even if a newer pre-release exists.

### Python — PyPI / TestPyPI

Stable, latest, from real PyPI:

```bash
pip install affineui
```

Stable, exact version:

```bash
pip install affineui==1.2.3
```

Pre-release, from TestPyPI (that's where we route pre-releases):

```bash
pip install \
  --index-url https://test.pypi.org/simple/ \
  --extra-index-url https://pypi.org/simple/ \
  affineui==1.2.4rc1
```

The `--extra-index-url` on PyPI is what lets `pip` still find the
transitive dependencies (numpy, pybind11 runtime, etc.) that live on real
PyPI while pulling `affineui` itself from TestPyPI. Order matters —
`--index-url` is preferred over `--extra-index-url`, which is how
`affineui` ends up coming from TestPyPI even though it's on both.

PEP 440 normalises the version — the tag `v1.2.4-rc.1` becomes the PyPI
version `1.2.4rc1` (no `-`, no dot before `rc`). If your `pip install`
command mirrors the tag exactly it will silently miss.

To pin in a `requirements.txt` or `pyproject.toml`:

```text
affineui==1.2.4rc1
```

Or, if you want `pip install --pre` to consider pre-releases without
naming one:

```bash
pip install --pre \
  --index-url https://test.pypi.org/simple/ \
  --extra-index-url https://pypi.org/simple/ \
  affineui
```

Without `--pre`, `pip` ignores every pre-release even when the index is
TestPyPI.

### Amalgamated .h / .cpp (no package manager)

Every `v*` tag gets a GitHub Release with `affineui-<VERSION>.zip`
attached. Contains `affineui.h`, `affineui.cpp`, `LICENSE`, `README.md`.

Latest stable:

```bash
curl -LO https://github.com/affineui/affineui/releases/latest/download/affineui-latest.zip
```

Note: GitHub's `latest` redirect is only to the newest **non-prerelease**
Release. Pre-release SDK zips must be named by version:

```bash
curl -LO https://github.com/affineui/affineui/releases/download/v1.2.4-rc.1/affineui-1.2.4-rc.1.zip
```

## Troubleshooting

**"docs/release-notes/vX.Y.Z.md not found on this ref" during `release.yml` prepare or `wheels.yml` publish.**
That's the notes-file gate. The file it's looking for is keyed on the
**core** version (`X.Y.Z` with any `-pre`/`+build` stripped), so
`v1.2.4-rc.1` looks for `docs/release-notes/v1.2.4.md`. Either you're
pushing the retro-tag (in which case this failure is what you wanted),
or you forgot to run the Draft flow and merge the notes PR. Run the
Draft workflow, review + merge, then re-dispatch Cut.

**"Tag vX.Y.Z already exists on origin" during Cut.**
Cutting the same version twice isn't supported — the registries reject
duplicates anyway. Bump the version (e.g. `-rc.N+1` or a patch).

**Draft-notes PR looks bad / Copilot got confused.**
Just edit the notes file inline in the PR. The AI draft is a starting
point; nothing downstream cares whether Copilot or a human authored the
final markdown. Re-dispatching Draft with the same `(bump, mode)`
overwrites the file with a fresh draft (force-with-lease), so you can
also re-run if you want to start over.

**Need to yank a bad release.**
crates.io: `cargo yank --version X.Y.Z affineui`. PyPI: `twine yank` or
the "Yank" button on the release page — a yanked version stops appearing
in default resolves but stays downloadable by explicit pin (PyPI does
NOT let you delete + reupload; the version number is burned). NuGet:
"Unlist" via the site. GitHub Release: delete the Release + tag from the
UI. Then cut a new fixed version (via the normal Draft → Cut flow) and
note the yanked release in its notes.

**Need to publish without going through the AI drafts.**
Author `docs/release-notes/v<CORE>.md` by hand, PR it, merge, then
dispatch Cut with the full version (pre-release or final). The Draft
workflow is a convenience — the enforceable contract is only "a merged
notes file at the core-version path must exist".
