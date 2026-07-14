# Releasing

Three rules shape everything here:

1. **A final release ships an RC's exact binaries.** It does not build something
   new. It does not even rebuild the same source. The files that go to PyPI,
   crates.io, and nuget.org are the files you tested.
2. **The version lives in the repo, not in the workflow.** You type the number;
   the workflow verifies it matches what is committed and refuses otherwise.
3. **One action, one outcome.** When a release action goes green, that thing is
   *done*. No workflow kicks off another workflow that you then have to go and
   check.

Rule 2 is not bureaucracy. Every version bug we shipped in the 0.4.x series came
from a number being stamped in at build time from a source nobody reviewed — most
memorably `affineui.__version__ == "0.0.1"` inside a wheel whose `pip` metadata
correctly said `0.4.0`, because the stamper wrote four manifests and silently
missed a fifth. When the number is committed and reviewed, that cannot happen.

Rule 1 comes from the same series. We published four times in one day, and twice
a fix verified on one platform shipped a regression on another that no build had
ever exercised. Promoting a tested RC's *binaries* means the thing you release
**is** the thing that passed — not a rebuild that ought to be equivalent.

Rule 3 comes from the release process itself. It used to be `Cut release` →
`gh workflow run` → two more workflows in two other runs. The button went green
before anything had been published, and you had to hunt down the other runs to
find out whether the release actually happened. Now each action does its whole
job inside its own run.

## The version lives in five manifests

```
bindings/python/pyproject.toml              version = "0.5.0"
bindings/rust/Cargo.toml                    workspace.package.version = "0.5.0"
bindings/rust/affineui/Cargo.toml           affineui-sys = { …, version = "0.5.0" }
bindings/csharp/AffineUI/AffineUI.csproj    <Version>0.5.0</Version>
CMakeLists.txt                              project(… VERSION 0.5.0 …)
```

They all carry the **release** number — `0.5.0` — even while you are cutting
release candidates. The `-rc.N` suffix is appended at **publish time only**; it
never appears in a manifest.

`python scripts/set_version.py 0.5.0` writes all five. Run it, review the diff,
open a PR. That PR is the bump.

## Why an RC's binaries can just *become* the release

This is the mechanism behind rule 1, and it is worth understanding because it is
load-bearing.

**The pre-release suffix never reaches the compiler.** The manifests carry the
bare release core, and CMake's `project(VERSION …)` cannot express a suffix
anyway — so `AFFINEUI_VERSION_{MAJOR,MINOR,PATCH}`, the only version the compiler
ever sees, is *identical* for `0.5.0-rc.2` and for `0.5.0`.

So the binary built for the RC already reports `0.5.0` from `affineui_version()`.
It does not start lying when you promote it. The suffix exists only here:

| The suffix lives here                    | It never reaches here                      |
|------------------------------------------|--------------------------------------------|
| the wheel's `METADATA`, `RECORD`, filename | `_affineui.*.pyd` / `.so` (the extension) |
| the `.nuspec` and `.psmdcp`              | `affineui_c.dll` / `.so` / `.dylib`        |
| `Cargo.toml`                             | `AffineUI.dll` (no `AssemblyVersion`)      |

Promotion is therefore a **metadata rewrite**, not a rebuild.
[`scripts/repackage_release.py`](../scripts/repackage_release.py) rewrites those
few strings, copies every compiled member byte-for-byte, and then **proves** it:
it hashes every compiled member of every artifact, compares each against the
manifest the RC recorded, and refuses to publish if a single byte moved — or if
any artifact the RC built is missing.

> ⚠️ **Do not plumb the version suffix into compiled output** — a compile
> definition, a generated header, an assembly attribute. It would make the RC's
> binaries differ from the release's, and the compile-once model becomes a lie.
> CI fails on this (`Release tooling` → *"the version suffix must never reach the
> compiler"*), so you will find out on the PR rather than at release time.

## The tag means something

**Build a pre-release** creates its GitHub pre-release and pushes its tag **last**
— only after every build, every test, and every registry push has gone green.

So a `v*` tag existing is *proof that version worked*. A pre-release that died
halfway leaves no tag, and **Publish a release** refuses to promote it. You
cannot ship a broken release, because there is nothing to ship from.

Re-running after a failure is always safe: the counter advances (`rc.2` →
`rc.3`). A burned number is never reused — registries never forget.

---

## Cutting a release

### 1. Draft release notes

Dispatch **Draft release notes**. It drafts `docs/release-notes/v0.5.0.md` from
the commit log and opens a PR. Edit it inline — the AI draft is a starting point,
not an oracle. Merge it.

Notes are keyed on the **core** version, so `v0.5.0-rc.1`, `v0.5.0-rc.2`, and the
final `v0.5.0` all share one file. One story per cycle, and the release actions
only ever *read* it — publishing never rewrites your prose.

### 2. Bump the manifests

```bash
python scripts/set_version.py 0.5.0
```

Review the diff, PR it, merge it. Now the repo says `0.5.0`.

### 3. Build a pre-release

Dispatch **Build a pre-release** with `version = 0.5.0`. That is the whole input
— the counter is automatic.

It verifies the manifests carry `0.5.0` and the notes are on main, resolves the
next free counter (`0.5.0-rc.2`), **compiles everything once**, runs the full test
matrix on Linux/macOS/Windows, publishes to TestPyPI + crates.io + nuget.org, and
— only if all of that succeeded — creates the GitHub pre-release with the compiled
artifacts attached and pushes the tag.

When the run is green, the pre-release is out. Go exercise it:

```bash
pip install --index-url https://test.pypi.org/simple/ \
            --extra-index-url https://pypi.org/simple/ affineui==0.5.0rc2
cargo add affineui@0.5.0-rc.2
dotnet add package AffineUI --version 0.5.0-rc.2
```

Found a bug? Fix it, merge, and run **Build a pre-release** again with the same
`0.5.0`. You get `-rc.3`.

### 4. Publish a release

Dispatch **Publish a release** with `version = 0.5.0-rc.2` — the pre-release you
tested. Not a version number: a *tested artifact*.

It refuses unless:

- **`v0.5.0-rc.2` exists** — proof that RC built, tested, and published cleanly.
- **It is the newest RC** for `0.5.0`. No promoting a stale one.
- **`v0.5.0` does not exist.** Registries are append-only.
- **The manifests and notes** are in order.
- **Every compiled byte matches** the RC's manifest, and every artifact the RC
  built is accounted for.

Then it re-stamps the metadata, uploads to PyPI + crates.io + nuget.org, and tags
`v0.5.0` at **the RC's commit** — not main's HEAD. Nothing is recompiled.

If main has moved on since the RC, those commits are simply not in the release,
and the run says so explicitly. If you want them, cut a new RC and test that.

---

## The actions

| Action | It is done when the run is green |
|---|---|
| [**Draft release notes**](../.github/workflows/draft-release-notes.yml) | The notes PR is open. |
| [**Build a pre-release**](../.github/workflows/build-prerelease.yml) | The RC is built, tested, published to every pre-release channel, and tagged. |
| [**Publish a release**](../.github/workflows/publish-release.yml) | The release is on PyPI, crates.io, and nuget.org, and tagged. |

[`build-python.yml`](../.github/workflows/build-python.yml) and
[`build-native.yml`](../.github/workflows/build-native.yml) are **nested building
blocks** (`workflow_call` only — they have no triggers, so they cannot run on
their own). Their jobs execute *inside* the calling run: if one fails, the caller
fails and nothing is tagged. Nothing is ever dispatched to a run you have to
track separately.

| Script | Role |
|---|---|
| [`resolve_release.py`](../scripts/resolve_release.py) | The rulebook. Turns the typed version into the tag + the commit to tag, and refuses bad publishes. |
| [`set_version.py`](../scripts/set_version.py) | Writes the version into the five manifests; `--verify` asserts they already carry it. |
| [`repackage_release.py`](../scripts/repackage_release.py) | Re-stamps the RC's artifacts to the final version, preserving and hash-verifying every compiled byte. |

Tested by [`test_release_rules.py`](../scripts/test_release_rules.py) and
[`test_repackage_release.py`](../tests/test_repackage_release.py), both of which
run on every PR.

## What ships

| Artifact | Pre-release goes to | Release goes to |
|---|---|---|
| Python wheels + sdist | test.pypi.org | pypi.org |
| Rust crates | crates.io (suffixed) | crates.io |
| .NET / NuGet | nuget.org (suffixed) | nuget.org |
| Amalgamated SDK | GitHub pre-release | GitHub Release |

crates.io and nuget.org accept `0.5.0-rc.2` verbatim and hide it from default
installs (`cargo add` / `dotnet add package` skip pre-releases unless you opt in).
PyPI has no hidden pre-release bucket, so pre-releases go to TestPyPI instead.

**crates.io is the one exception to rule 1**: `cargo publish` ships *source*, not
binaries, so there is nothing compiled to preserve. The release re-publishes the
RC's exact commit.

## Repo secrets

| Secret | Where you get it |
|---|---|
| `CARGO_REGISTRY_TOKEN` | crates.io → Account Settings → API Tokens |
| `NUGET_API_KEY` | nuget.org → Account → API Keys |
| `COPILOT_PAT` | Fine-grained PAT, `Copilot Requests: Read` (notes drafting) |

PyPI and TestPyPI use Trusted Publisher (OIDC) — no token.

> **Registering the Trusted Publisher:** name the **entry-point workflow**, not
> the nested building block. TestPyPI → `build-prerelease.yml` (environment
> `testpypi`); PyPI → `publish-release.yml` (environment `pypi`). PyPI matches the
> publisher on the workflow *filename* and ignores the OIDC token's
> `job_workflow_ref` claim, so a publish step inside a reusable workflow is
> rejected with `invalid-publisher` no matter how the permissions are set. That is
> why the upload steps live in the entry-point actions.
> ([PyPI docs](https://docs.pypi.org/trusted-publishers/troubleshooting/),
> [warehouse#11096](https://github.com/pypi/warehouse/issues/11096))

## Consuming AffineUI

```bash
cargo add affineui                                 # latest stable
cargo add affineui@0.5.0-rc.2                      # a pre-release, named exactly

dotnet add package AffineUI                        # latest stable
dotnet add package AffineUI --version 0.5.0-rc.2   # a pre-release

pip install affineui                               # latest stable, from PyPI
```

A bare requirement never resolves to a pre-release in any of the three, so a
routine `cargo update` / `dotnet restore` will not pull an RC on you.

Python pre-releases live on TestPyPI, and PEP 440 normalises the version — the tag
`v0.5.0-rc.2` becomes `0.5.0rc2` (no dash, no dot). Mirroring the tag spelling
exactly will silently miss:

```bash
pip install \
  --index-url https://test.pypi.org/simple/ \
  --extra-index-url https://pypi.org/simple/ \
  affineui==0.5.0rc2
```

`--extra-index-url` lets pip resolve the runtime dependencies from real PyPI while
pulling `affineui` itself from TestPyPI.

The amalgamated `.h`/`.cpp` drop-in is attached to every GitHub Release:

```bash
curl -LO https://github.com/affineui/affineui/releases/latest/download/affineui-0.5.0.zip
```

---

## Troubleshooting

**"Tag v0.5.0-rc.2 does not exist" when publishing.**
That RC never finished — a build, test, or registry push failed, so no tag was
pushed. The safety net worked. Fix it, run **Build a pre-release** again (you get
`-rc.3`), and publish that.

**"v0.5.0-rc.2 is not the newest pre-release."**
A newer RC exists. Publish that one, or cut a fresh one. Promoting a stale RC
almost always means shipping code you stopped testing.

**"BIT-IDENTITY VIOLATED" during publish.**
The compiled payload changed between the RC and the re-stamped artifact. Either
the repackager is broken, or someone plumbed the version suffix into compiled
output. **Do not work around this** — it is the check that guarantees you ship
what you tested.

**"manifest mismatch — release 0.5.0 was requested".**
You forgot the bump PR. `python scripts/set_version.py 0.5.0`, commit, merge.

**Need to yank a bad release.**
crates.io: `cargo yank --version X.Y.Z affineui`. PyPI: the Yank button (PyPI
never allows delete-and-reupload — the number is burned). NuGet: Unlist. GitHub:
delete the Release + tag. Then cut a new version through the normal flow.

**Need to publish without the AI drafter.**
Write `docs/release-notes/v<CORE>.md` by hand, PR it, merge. The drafter is a
convenience; the enforced contract is only that the file exists on main.
