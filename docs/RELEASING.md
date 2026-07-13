# Releasing

Two rules shape everything here:

1. **A final release ships an RC's commit.** It does not build something new.
   You cannot cut `0.5.0` without first cutting and testing `0.5.0-rc.N`.
2. **The version lives in the repo, not in the workflow.** You type the number;
   the workflow verifies it matches what is committed and refuses otherwise.

Rule 2 is not bureaucracy. Every version bug we shipped in the 0.4.x series came
from a number being stamped in at build time from a source nobody reviewed — most
memorably `affineui.__version__ == "0.0.1"` inside a wheel whose `pip` metadata
correctly said `0.4.0`, because the stamper wrote four manifests and silently
missed a fifth. When the number is committed and reviewed, that cannot happen.

Rule 1 comes from the same series. We published four times in one day, and twice
a fix verified on one platform shipped a regression on another that no build had
ever exercised. Promoting a tested RC's commit means the thing you release *is*
the thing that passed.

## The version lives in five manifests

```
bindings/python/pyproject.toml              version = "0.5.0"
bindings/rust/Cargo.toml                    workspace.package.version = "0.5.0"
bindings/rust/affineui/Cargo.toml           affineui-sys = { …, version = "0.5.0" }
bindings/csharp/AffineUI/AffineUI.csproj    <Version>0.5.0</Version>
CMakeLists.txt                              project(… VERSION 0.5.0 …)
```

They all carry the **release** number — `0.5.0` — even while you are cutting
release candidates. The `-rc.N` / `-alpha.N` / `-beta.N` suffix is appended at
**publish time only**; it never appears in a manifest. That is what lets the
final release republish the RC's commit unchanged: the manifests already say
`0.5.0`, so there is nothing to rewrite.

`python scripts/set_version.py 0.5.0` writes all five. Run it, review the diff,
open a PR. That PR is the bump.

## Cutting a release

The **Cut release** workflow takes three inputs:

| Input | Meaning |
|---|---|
| `version` | The release number, typed. `0.5.0`. No `v`, no suffix. |
| `mode` | `rc` · `beta` · `alpha` · `release` |
| `source_tag` | Which commit to tag. Blank = HEAD of main. For `mode=release`, **required**: the RC tag being promoted. |

The pre-release **counter is automatic** — you never type `rc.2`.

### A normal cycle

```
1.  Draft release notes  →  review  →  merge
        docs/release-notes/v0.5.0.md

2.  PR: python scripts/set_version.py 0.5.0  →  review  →  merge
        (manifests now say 0.5.0)

3.  Cut release:  version=0.5.0  mode=rc  source_tag=(blank)
        → tags v0.5.0-rc.1 on main's HEAD
        → publishes 0.5.0-rc.1

4.  Test it. Found a bug? Fix it, merge, cut again with the SAME inputs —
    the counter increments itself to rc.2.

5.  Cut release:  version=0.5.0  mode=release  source_tag=v0.5.0-rc.2
        → tags v0.5.0 on rc.2's EXACT COMMIT
        → publishes 0.5.0
```

Step 5 builds nothing new. It republishes the commit you already tested, under
the number the manifests already carry.

### What the workflow refuses

All of this lives in [`scripts/resolve_release.py`](../scripts/resolve_release.py),
which is the entire rulebook in one testable place:

- **A release with no RC.** `mode=release` without a `source_tag` naming a
  pre-release of the same version. You ship what you tested.
- **A version that doesn't move forward.** Not greater than the last published
  release. Registries are immutable — a burned number can never be reused.
- **A version typed with a suffix.** You type `0.5.0` and pick `mode`; the
  counter is computed.
- **A version the manifests don't carry.** You forgot the bump PR.
- **Missing release notes** at `docs/release-notes/v<MAJOR.MINOR.PATCH>.md`.
- **A tag that already exists.**

### Promoting an RC whose commit is behind main

Expected and allowed. `mode=release` tags **the RC's commit**, not main's.
Anything merged to main after that RC is simply not in the release. The workflow
logs exactly which commits are being left behind, so the omission is never
silent.

If you want those commits, cut a new RC and test that instead.

## Release notes

Notes are keyed on the **core** version, so `v0.5.0-rc.1`, `v0.5.0-rc.2`, and
the final `v0.5.0` all share one `docs/release-notes/v0.5.0.md`. One story per
cycle, iterated through its pre-releases. The publish jobs fail loudly if the
file is missing, which is what makes reviewed notes the only path to a release.

The notes cover **everything since the last stable release** — if the last was
`v0.4.2`, then `v0.5.0.md` covers `v0.4.2..HEAD`, regardless of which RC you are
cutting.

**Draft release notes** (`workflow_dispatch`) takes the typed version, has
Copilot CLI draft from the commit log, and opens a PR. Edit it inline; it is a
starting point, not an oracle.

## What ships

| Artifact         | Registry           | Trigger                     |
|------------------|--------------------|-----------------------------|
| Python wheels    | pypi.org           | tag `vX.Y.Z` (final)        |
| Python wheels    | test.pypi.org      | any pre-release tag         |
| Rust crates      | crates.io          | any `v*` tag                |
| .NET / NuGet     | nuget.org          | any `v*` tag                |
| Amalgamated SDK  | GitHub Release     | any `v*` tag                |

### What each publisher does with a pre-release

- **crates.io** accepts `0.5.0-rc.1` verbatim. `cargo add affineui` picks the
  newest stable by default; users opt into pre-releases explicitly.
- **nuget.org** accepts it verbatim. `dotnet add package` skips pre-releases
  unless you pass `--prerelease`.
- **PyPI** has no hidden pre-release bucket, so pre-release tags publish to
  **TestPyPI** instead.
- **GitHub Release** is marked pre-release (badge; not "latest").

## What lives where

| File | Role |
|---|---|
| [`scripts/resolve_release.py`](../scripts/resolve_release.py) | The rulebook. Turns (version, mode, source_tag) into the tag + the commit to tag, and refuses bad publishes. |
| [`scripts/set_version.py`](../scripts/set_version.py) | Writes the version into the five manifests (`set_version.py 0.5.0`), and `--verify` asserts they already carry it. |
| [`.github/workflows/draft-release-notes.yml`](../.github/workflows/draft-release-notes.yml) | Drafts the notes, opens the PR. |
| [`.github/workflows/cut-release.yml`](../.github/workflows/cut-release.yml) | Validates, tags, pushes. |
| [`.github/workflows/release.yml`](../.github/workflows/release.yml) | On tag push: amalgamated SDK, crates.io, nuget.org, GitHub Release. |
| [`.github/workflows/wheels.yml`](../.github/workflows/wheels.yml) | On tag push: wheels + sdist across three OSes → PyPI or TestPyPI. |
| `docs/release-notes/v<CORE>.md` | The reviewed notes for a cycle. Shared by every tag in it. |

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
