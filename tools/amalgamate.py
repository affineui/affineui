#!/usr/bin/env python3
"""
Amalgamator: produce the two-file distribution of AffineUI.

Output:
    <out>/affineui.h    — declarations, types, inline templates
    <out>/affineui.cpp  — all implementation, including vendored deps

End-users copy these two files into their project. No CMake required
for AffineUI itself. Vendored dependencies are generated into the
implementation file from the curated external snapshots.

Strategy (sqlite-amalgamation style):
  1. Concatenate the public headers in topological order into
     ``affineui.h``. Strip ``#pragma once`` and internal ``#include``
     directives; dedupe top-level ``#include <stdlib>`` directives to
     a single banner block. Wrap the result in one outer include
     guard.
  2. Generate C++-compatible external source blocks and flatten them
     into ``affineui.cpp``.
  3. Concatenate the internal headers, then our C++ implementation
     translation units, then the vendored-C wrappers, into
     ``affineui.cpp``. Strip internal and vendored includes whose
     content is already inlined; leave conditional platform includes
     where they sit.

The output is deterministic: the same input tree always produces
byte-identical files. A CI smoke test compiles the amalgamation to
catch drift between the modular and amalgamated builds.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from lexbor_cpp_compat import (copy_source_tree, default_platform,
                               lexbor_c_sources, macos_sdk_flags,
                               repair_with_compiler, source_root,
                               transform as transform_lexbor,
                               unprefixed_global_symbols)

# ── Source manifest ──────────────────────────────────────────────────────────
# Ordering matters: each list is consumed top-to-bottom and the result is
# concatenated into a single TU. Within each list, files must appear in
# dependency order — a header that references a type from another header
# must come *after* that header. Implementation files don't need to follow
# dependency order (all declarations are visible via the inlined headers),
# but engine/dom files are kept ahead of c_api so the C ABI sees the
# definitions it forwards to.

PUBLIC_HEADERS = [
    "include/affineui/version.h",
    "include/affineui/types.h",
    "include/affineui/weak_ref.h",    # versioned weak refs; depends on types.h
    "include/affineui/callback.h",    # safe callbacks; depends on weak_ref.h
    "include/affineui/keymap.h",      # keybindings; depends on types.h
    "include/affineui/embed.h",       # depends only on types.h; before renderer/ui
    "include/affineui/memory.h",      # allocator seam + stats; depends on embed.h
    "include/affineui/log.h",         # diagnostics facility; depends on embed.h
    "include/affineui/themes.h",
    "include/affineui/geom.h",
    "include/affineui/style.h",
    "include/affineui/painter.h",
    "include/affineui/computed_style.h",
    "include/affineui/display_list.h",
    "include/affineui/document.h",
    "include/affineui/object.h",        # ObjectBase; needed by view + inspector
    "include/affineui/virtual_list.h",  # providers/flattener; fwd-declares View
    "include/affineui/view.h",          # depends on callback + document + types
    "include/affineui/components.h",    # depends on view
    "include/affineui/inspector.h",     # depends on object + view
    "include/affineui/automation.h",   # depends on document + types
    "include/affineui/renderer.h",
    "include/affineui/telemetry.h",   # FrameTelemetry R0/R1; depends on memory.h
    "include/affineui/tools.h",       # devtools hook surface (AFFINEUI_TOOLS=0 → stubs)
    "include/affineui/ui.h",
    "include/affineui/app.h",           # load_view(const View&)
    "include/affineui/imm.h",
    "include/affineui/c_api.h",       # extern "C" surface; self-contained
    "include/affineui/c_api_app.h",   # extern "C" App / Document / View surface
    "include/affineui/decius_bundle.h",  # small public surface for the embedded bundle
    "include/affineui/affineui.h",    # umbrella last
]

# Implementation-side headers. They never appear in ``affineui.h``; they
# inline into the top of ``affineui.cpp`` so the implementation TUs that
# follow can see them.
INTERNAL_HEADERS = [
    "src/core/embed_log.h",
    "src/core/log_internal.h",
    "src/core/diag.h",              # TraceSpan + sampler; used by every renderer TU
    "src/c_api_util.h",             # shared by c_api.cpp / c_api_app.cpp
    "external/tinyjson/tiny-json.h",# used by core/tools/tools_server.cpp
    "src/core/tools/tools_commands.h",  # server↔app command bridge (tools_server / document_tools)
    "src/renderer/style/element_id.h",
    "src/renderer/style/animated_style.h",
    "src/renderer/style/computed_style.h",
    "src/renderer/paint/layer.h",
    "src/renderer/paint/compositor.h",
    "src/renderer/paint/paint_internal.h",
    "src/renderer/paint/display_list.h",
    "src/renderer/paint/display_list_painter.h",
    "src/renderer/style/style_resolver.h",
    "src/renderer/style/style_store.h",
    "src/renderer/engine/box.h",
    "src/renderer/engine/layer.h",
    "src/renderer/engine/animation.h",
    "src/renderer/engine/compositor.h",
    "src/renderer/engine/restyle_queue.h",
    "src/renderer/engine/style_engine.h",
    "src/renderer/engine/layout_engine.h",
    "src/renderer/engine/paint_engine.h",
    "src/renderer/engine/rasterizer.h",
    "src/renderer/engine/engine.h",
    "src/renderer/layout/yoga_adapter.h",
    "src/framework/imm/imm_runtime.h",
    # Shared document-internal types (Block, DocumentImpl, CSS side tables).
    # Included by src/renderer/dom/document*.cpp; must follow every header it depends on
    # (style_store, computed_style, animated_style, style_resolver, imm_runtime).
    "src/renderer/dom/document_impl.h",
    # Generated by tools/embed_decius.py — refreshed at the top of main().
    # Byte-array data for the compile-time Decius CSS + font bundle;
    # consumed by src/decius_bundle.cpp below.
    "include/affineui/_decius_bundle.h",
]

# Our C++ TUs. Order here is not load-bearing (declarations come from the
# inlined headers above), but we keep a stable order so diffs of the
# generated file are readable.
ENGINE_SOURCES = [
    "src/framework/themes.cpp",
    "src/core/affineui.cpp",
    "src/core/memory.cpp",
    "src/core/log.cpp",
    "src/core/diag/sampler.cpp",
    "src/core/diag/telemetry.cpp",
    "src/core/tools/tools_server.cpp",  # Ctrl+Shift+I opens the affinetools viewer in every build
    "src/framework/app/app.cpp",
    "src/framework/app/event.cpp",
    "src/framework/app/automation.cpp",
    "src/framework/app/view.cpp",
    "src/renderer/dom/document_core.cpp",
    "src/renderer/dom/document_controls.cpp",
    "src/renderer/dom/document_dock.cpp",
    "src/renderer/dom/document_dispatch.cpp",
    "src/renderer/dom/document_draw.cpp",
    "src/renderer/dom/document_layout.cpp",
    "src/renderer/dom/document_restyle.cpp",
    "src/renderer/dom/document_style.cpp",
    "src/renderer/dom/document_text.cpp",
    "src/renderer/dom/document_tools.cpp",
    "src/renderer/dom/dom_view.cpp",
    "src/renderer/dom/lexbor_bridge.cpp",
    "src/renderer/style/cascade.cpp",
    "src/renderer/style/computed.cpp",
    "src/renderer/style/selector_bridge.cpp",
    "src/renderer/style/style_store.cpp",
    "src/renderer/style/units.cpp",
    "src/framework/ui.cpp",
    "src/renderer/layout/box.cpp",
    "src/renderer/layout/block.cpp",
    "src/renderer/layout/inline.cpp",
    "src/renderer/layout/flex.cpp",
    "src/renderer/layout/yoga_adapter.cpp",
    "src/renderer/text/shaper.cpp",
    "src/renderer/text/line_breaker.cpp",
    "src/renderer/text/font_manager.cpp",
    "src/renderer/paint/paint_driver.cpp",
    "src/renderer/paint/image_loader.cpp",
    "src/renderer/paint/nanovg_painter.cpp",
    "src/renderer/render/renderer.cpp",
    "src/renderer/engine/box.cpp",
    "src/renderer/engine/layer.cpp",
    "src/renderer/engine/display_list.cpp",
    "src/renderer/engine/restyle_queue.cpp",
    "src/renderer/engine/animation.cpp",
    "src/renderer/engine/style_engine.cpp",
    "src/renderer/engine/layout_engine.cpp",
    "src/renderer/engine/paint_engine.cpp",
    "src/renderer/engine/rasterizer.cpp",
    "src/renderer/engine/compositor.cpp",
    "src/renderer/engine/engine.cpp",
    "src/framework/imm/imm.cpp",
    "src/framework/imm/imm_runtime.cpp",
    "src/framework/imm/reconciler.cpp",
    "src/framework/imm/state_store.cpp",
    "src/c_api.cpp",
    "src/c_api_app.cpp",
    # Decius bundle loader (inflates the byte arrays defined in the
    # generated _decius_bundle.h into std::string on demand). Included
    # alongside c_api_app.cpp so `affineui_decius_apply` etc. link.
    "src/decius_bundle.cpp",
]

# Vendored-C wrapper TUs (one file each, ~50 LOC, mostly preprocessor
# stitching to bring in a single-header dep). They go inside one
# `extern "C" { ... }` block at the tail of ``affineui.cpp`` so their
# symbols keep C linkage when the file is compiled as C++. Order
# matters: stb_truetype must be in scope before fontstash uses it,
# nanovg core before the nanovg_sokol backend, and sokol_gfx before
# the sokol_app/glue implementation.
VENDORED_C_TUS = [
    "src/renderer/text/stb_impl.c",
    "src/renderer/text/fontstash_impl.c",
    "src/renderer/paint/nanovg_sokol.c",
    "src/renderer/paint/sokol_impl.c",
    "external/tinyjson/tiny-json.c",  # inbound wire parser for tools_server.cpp
]

REQUIRED_EXTERNAL_FILES = [
    "external/yoga/yoga/Yoga.h",
    "external/nanovg/src/nanovg.c",
    "external/nanovg/src/nanovg.h",
    "external/nanovg/src/nanovg_sokol.h",
    "external/nanovg/src/fontstash.h",
    "external/nanovg/src/stb_image.h",
    "external/nanovg/src/stb_truetype.h",
    "external/sokol/sokol_gfx.h",
    "external/sokol/sokol_app.h",
    "external/sokol/sokol_glue.h",
    "external/sokol/sokol_log.h",
]

# Public headers always live under ``include/affineui/``; internal/engine
# headers under ``src/internal/``, ``src/renderer/engine/``, and a single
# ``src/renderer/layout/yoga_adapter.h``. An ``#include "<prefix>…"`` matching one
# of these prefixes refers to a file that has already been inlined and
# must be stripped from its referring TU.
INTERNAL_PREFIXES = ("affineui/", "renderer/", "framework/", "core/",
                     "c_api_util.h")
INLINED_EXTERNAL_PREFIXES = (
    "lexbor/",
    "yoga/",
    "sokol_gfx.h",
    "sokol_app.h",
    "sokol_glue.h",
    "sokol_log.h",
    "nanovg.h",
    "nanovg_sokol.h",
    "fontstash.h",
    "stb_image.h",
    "stb_truetype.h",
    "tiny-json.h",  # vendored inbound wire parser (external/tinyjson)
)
LEXBOR_REPEATABLE_HEADERS = {
    "lexbor/core/cpp_compat.h",
    "lexbor/core/cpp_compat_undef.h",
    "lexbor/core/str_res.h",
    "lexbor/html/interface_res.h",
    "lexbor/html/tag_res.h",
    "lexbor/tag/res.h",
}
REPEATABLE_EXTERNAL_HEADERS = {
    "external/nanovg/src/fontstash.h",
    "external/nanovg/src/stb_image.h",
    "external/nanovg/src/stb_truetype.h",
    # sokol headers split declarations (guarded by *_INCLUDED) from the
    # implementation (guarded by SOKOL_IMPL). They are emitted once for
    # declarations (renderer block) and again for the implementation
    # (sokol_impl.c), so they must be re-emittable.
    "external/sokol/sokol_gfx.h",
    "external/sokol/sokol_app.h",
    "external/sokol/sokol_glue.h",
    "external/sokol/sokol_log.h",
}

# ── Regexes ──────────────────────────────────────────────────────────────────
# We match preprocessor directives only when ``#`` is directly followed by
# ``include`` / ``pragma`` — i.e. no whitespace between them. An indented
# ``#    include`` (the pattern used inside `#if defined(__APPLE__)` guards
# in the vendored-C wrappers) intentionally falls through so it stays where
# it is — moving a conditional include to the file's top banner would
# unconditionally pull in a platform header.

PRAGMA_ONCE_RE      = re.compile(r'^#pragma\s+once\s*(?://.*|/\*.*)?$')

# System includes are deduped to the top banner only when they sit at
# column zero — an indented `#    include <foo>` inside an `#if` guard
# must stay where it is.
SYSTEM_INCLUDE_RE   = re.compile(r'^#include\s*<([^>]+)>\s*(?://.*|/\*.*)?$')

# Quoted-include match is whitespace-tolerant: a conditional like
#   `#    include "affineui/imm.h"` inside an `#ifndef AFFINEUI_NO_IMM`
# guard still refers to a file that's been inlined above and must be
# dropped. We also allow a trailing line comment so
#   `#include "engine/box.h"  // LayerId`
# still parses as an include. We decide whether to strip by the path
# prefix, not by column.
QUOTED_INCLUDE_RE   = re.compile(r'^\s*#\s*include\s*"([^"]+)"\s*(?://.*|/\*.*)?$')
ANY_INCLUDE_RE      = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]\s*(?://.*|/\*.*)?$')


def is_internal_include(name: str) -> bool:
    return any(name.startswith(p) for p in INTERNAL_PREFIXES)


def is_inlined_external_include(name: str) -> bool:
    return any(name.startswith(p) for p in INLINED_EXTERNAL_PREFIXES)


# ── Processing ───────────────────────────────────────────────────────────────

@dataclass
class FileBlock:
    rel_path: str
    body: str                              # transformed file content
    system_includes: list[str] = field(default_factory=list)


def process_file(root: Path, rel_path: str, *,
                 expand_external: bool = False,
                 emitted_external_headers: set[Path] | None = None) -> FileBlock:
    """
    Read a file, strip ``#pragma once`` and inlined-internal ``#include``s,
    capture top-level ``#include <…>`` directives, and return the rest.

    Indented system includes (inside ``#if`` conditionals) are left intact:
    relocating them to the top would change their conditionality.
    """
    path = root / rel_path
    text = path.read_text(encoding="utf-8")
    out_lines: list[str] = []
    syshdr_order: list[str] = []
    seen_sys: set[str] = set()

    for line in text.splitlines():
        if PRAGMA_ONCE_RE.match(line):
            continue
        m = ANY_INCLUDE_RE.match(line)
        if m and expand_external:
            target = resolve_external_include(root, path, m.group(1))
            if target is not None:
                if emitted_external_headers is None:
                    emitted_external_headers = set()
                out_lines.append(expand_external_file(root, target,
                                                      emitted_external_headers))
                continue
        if m and is_inlined_external_include(m.group(1)):
            # Vendored source is inlined into affineui.cpp.
            continue
        m = SYSTEM_INCLUDE_RE.match(line)
        if m:
            name = m.group(1)
            if name not in seen_sys:
                seen_sys.add(name)
                syshdr_order.append(name)
            continue
        m = QUOTED_INCLUDE_RE.match(line)
        if m and is_internal_include(m.group(1)):
            # Already inlined upstream in the amalgamation.
            continue
        out_lines.append(line.rstrip())

    # Trim trailing blank lines so concatenated blocks don't accumulate gaps.
    while out_lines and not out_lines[-1].strip():
        out_lines.pop()

    return FileBlock(
        rel_path=rel_path,
        body="\n".join(out_lines) + "\n",
        system_includes=syshdr_order,
    )


def banner(version: str, kind: str) -> str:
    return (
        "// SPDX-License-Identifier: MIT\n"
        "//\n"
        f"// AffineUI {version} — two-file amalgamated distribution ({kind})\n"
        "//\n"
        "// This file is GENERATED by tools/amalgamate.py. Edit the modular\n"
        "// sources under include/ and src/ instead; re-running the\n"
        "// amalgamator regenerates this artifact.\n"
        "//\n"
        "// Bundled third-party components retain their licenses:\n"
        "//   - sokol        zlib       https://github.com/floooh/sokol\n"
        "//   - nanovg       zlib       https://github.com/memononen/nanovg\n"
        "//   - lexbor       Apache-2.0 https://github.com/lexbor/lexbor\n"
        "//   - yoga         MIT        https://github.com/facebook/yoga\n"
        "//   - stb_*        MIT / PD   https://github.com/nothings/stb\n"
        "//   - fontstash    zlib       https://github.com/memononen/fontstash\n"
        "//\n"
        "// Vendored sources are generated into this implementation file from\n"
        "// curated external snapshots; end users include only affineui.h and\n"
        "// compile only affineui.cpp.\n"
    )


def read_version(root: Path) -> str:
    cml = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for line in cml.splitlines():
        line = line.strip()
        if line.startswith("VERSION ") and "project(" not in line:
            return line.split()[1]
    return "0.0.0"


def render_block(block: FileBlock) -> str:
    rule = "─" * 72
    header = (
        f"// {rule}\n"
        f"// {block.rel_path}\n"
        f"// {rule}\n"
    )
    return header + block.body + "\n"


def render_external_header(rel_path: str) -> str:
    rule = "─" * 72
    return (
        f"// {rule}\n"
        f"// {rel_path}\n"
        f"// {rule}\n"
    )


def repo_rel(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def resolve_external_include(root: Path, current_file: Path | None,
                             name: str) -> Path | None:
    candidates: list[Path] = []
    if current_file is not None:
        candidates.append(current_file.parent / name)

    external_roots = [
        root / "external" / "nanovg" / "src",
        root / "external" / "sokol",
        root / "external" / "yoga",
        root / "external" / "tinyjson",
    ]
    for base in external_roots:
        candidates.append(base / name)

    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    return None


def read_external_text(root: Path, path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="ignore")
    rel = repo_rel(root, path)

    # Yoga builds cleanly as separate C++ TUs, but in a flattened single
    # TU its event.cpp private Node helper collides with facebook::yoga::Node.
    # Keep the source snapshot pristine and apply the narrow rename only
    # to generated output.
    if rel == "external/yoga/yoga/event/event.cpp":
        text = re.sub(r"\bNode\b", "EventSubscriberNode", text)
    if rel == "external/yoga/yoga/YGNodeStyle.cpp":
        # In the flattened TU, bare `Style` is ambiguous with affineui's
        # own Style type. Word-boundary-qualify every reference (covers
        # both `Style::foo` and bare-type uses like `Style next = ...`).
        text = re.sub(r"\bStyle\b", "facebook::yoga::Style", text)

    if rel.startswith("external/nanovg/src/"):
        # NanoVG and Fontstash do not expose an upstream static-prefix macro
        # for their C APIs. In the amalgamation those APIs are implementation
        # details, so give their declarations/definitions internal linkage.
        text = re.sub(
            r"(?m)^(?!static\b)((?:(?:const|unsigned|signed|struct)\s+)*"
            r"[A-Za-z_][A-Za-z0-9_]*\s*\*?\s+"
            r"(?:nvg|nvgl|nvglu|fons|stbi)[A-Za-z0-9_]*\s*\()",
            r"static \1",
            text,
        )

    return text


def clean_generated_line(line: str) -> str:
    line = line.rstrip()
    m = re.match(r"^[ \t]+", line)
    if m:
        prefix = re.sub(r" +(?=\t)", "", m.group(0))
        line = prefix + line[len(m.group(0)):]
    return line


def update_block_comment_state(line: str, in_block_comment: bool) -> bool:
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end == -1:
                return True
            in_block_comment = False
            i = end + 2
            continue

        line_comment = line.find("//", i)
        block_comment = line.find("/*", i)
        if block_comment == -1 or (line_comment != -1 and line_comment < block_comment):
            return False
        in_block_comment = True
        i = block_comment + 2
    return in_block_comment


def expand_external_file(root: Path, path: Path,
                         emitted_headers: set[Path],
                         active_stack: set[Path] | None = None) -> str:
    path = path.resolve()
    if active_stack is None:
        active_stack = set()
    rel_path = repo_rel(root, path)
    repeatable_header = rel_path in REPEATABLE_EXTERNAL_HEADERS

    if path.suffix == ".h" and not repeatable_header:
        if path in emitted_headers:
            return ""
        emitted_headers.add(path)

    active_stack.add(path)
    out_lines: list[str] = [render_external_header(rel_path)]
    in_block_comment = False
    for line in read_external_text(root, path).splitlines():
        can_process_directive = not in_block_comment
        next_in_block_comment = update_block_comment_state(line, in_block_comment)

        if can_process_directive and PRAGMA_ONCE_RE.match(line):
            in_block_comment = next_in_block_comment
            continue
        m = ANY_INCLUDE_RE.match(line) if can_process_directive else None
        if m:
            target = resolve_external_include(root, path, m.group(1))
            if target is not None:
                target = target.resolve()
                if target in active_stack:
                    out_lines.append(clean_generated_line(line))
                    in_block_comment = next_in_block_comment
                    continue
                out_lines.append(expand_external_file(root, target,
                                                      emitted_headers,
                                                      active_stack))
                in_block_comment = next_in_block_comment
                continue
        out_lines.append(clean_generated_line(line))
        in_block_comment = next_in_block_comment
    active_stack.remove(path)

    while out_lines and not out_lines[-1].strip():
        out_lines.pop()

    return "\n".join(out_lines) + "\n\n"


def yoga_cpp_sources(root: Path) -> list[Path]:
    yoga_root = root / "external" / "yoga" / "yoga"
    sources = sorted(yoga_root.rglob("*.cpp"))
    event_cpp = yoga_root / "event" / "event.cpp"
    return [p for p in sources if p != event_cpp] + [event_cpp]


def resolve_lexbor_include(staged_source: Path, name: str) -> Path | None:
    if not name.startswith("lexbor/"):
        return None

    path = staged_source / name
    return path if path.exists() else None


def expand_lexbor_file(staged_source: Path, path: Path,
                       emitted_headers: set[Path]) -> str:
    rel_path = path.relative_to(staged_source).as_posix()
    repeatable_header = rel_path in LEXBOR_REPEATABLE_HEADERS

    if path.suffix == ".h" and not repeatable_header:
        if path in emitted_headers:
            return ""
        emitted_headers.add(path)

    out_lines: list[str] = [render_external_header(f"external/lexbor/{rel_path}")]

    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if PRAGMA_ONCE_RE.match(line):
            continue
        m = ANY_INCLUDE_RE.match(line)
        if m:
            target = resolve_lexbor_include(staged_source, m.group(1))
            if target is not None:
                out_lines.append(expand_lexbor_file(staged_source, target,
                                                    emitted_headers))
                continue
        out_lines.append(line.rstrip())

    while out_lines and isinstance(out_lines[-1], str) and not out_lines[-1].strip():
        out_lines.pop()

    return "\n".join(out_lines) + "\n\n"


def emit_lexbor_block(root: Path, cxx: str) -> str:
    with tempfile.TemporaryDirectory(prefix="affineui-lexbor-") as tmp:
        tmp_path = Path(tmp)
        staged_source = copy_source_tree(source_root(root), Path(tmp) / "source")
        transform_lexbor(staged_source, "aui_")

        obj_dir = tmp_path / "obj"
        obj_dir.mkdir(parents=True, exist_ok=True)
        failures = repair_with_compiler(cxx, macos_sdk_flags(), staged_source,
                                        obj_dir, default_platform(), "aui_", 8)
        if failures:
            first = failures[0]
            rel = first.path.relative_to(staged_source)
            raise RuntimeError(
                f"Lexbor C++ staging failed while amalgamating: {rel}\n"
                + "\n".join(first.stderr.splitlines()[:40])
            )

        offenders = unprefixed_global_symbols(obj_dir)
        if offenders:
            raise RuntimeError(
                "Lexbor C++ staging left unprefixed globals:\n"
                + "\n".join(offenders[:20])
            )

        emitted_headers: set[Path] = set()
        parts: list[str] = [
            "// ─── Lexbor HTML/CSS engine (generated C++ staging tree) ─────────────\n",
            "#ifndef LEXBOR_STATIC\n",
            "#  define LEXBOR_STATIC\n",
            "#endif\n",
            "#if !defined(AFFINEUI_HTML_ENTITIES_FULL)\n",
            "#  define LXB_HTML_TOKENIZER_BASIC_ENTITIES\n",
            "#endif\n",
            "#if defined(__clang__)\n",
            "#  pragma clang diagnostic push\n",
            "#  pragma clang diagnostic ignored \"-Wc++11-narrowing\"\n",
            "#  pragma clang diagnostic ignored \"-Wc99-designator\"\n",
            "#  pragma clang diagnostic ignored \"-Wwritable-strings\"\n",
            "#endif\n",
            "\n",
        ]

        parts.append(expand_lexbor_file(staged_source,
                                        staged_source / "lexbor" / "core" / "base.h",
                                        emitted_headers))
        for path in lexbor_c_sources(staged_source, default_platform()):
            parts.append(expand_lexbor_file(staged_source, path, emitted_headers))

        parts.extend([
            "#if defined(__clang__)\n",
            "#  pragma clang diagnostic pop\n",
            "#endif\n",
            "\n",
        ])

        return "".join(parts)


def emit_yoga_block(root: Path) -> str:
    emitted_headers: set[Path] = set()
    parts: list[str] = [
        "// ─── Yoga flexbox layout engine ───────────────────────────────────────\n",
        "#if !defined(AFFINEUI_STUB_BUILD)\n",
        "#if defined(__clang__)\n",
        "#  pragma clang diagnostic push\n",
        "#  pragma clang diagnostic ignored \"-Wc++98-compat-extra-semi\"\n",
        "#endif\n",
        "\n",
    ]
    for path in yoga_cpp_sources(root):
        parts.append(expand_external_file(root, path, emitted_headers))
    parts.extend([
        "#if defined(__clang__)\n",
        "#  pragma clang diagnostic pop\n",
        "#endif\n",
        "#endif  // !AFFINEUI_STUB_BUILD\n",
        "\n",
    ])
    return "".join(parts)


def emit_renderer_declaration_block(root: Path) -> str:
    # Declarations the C++ implementation TUs (renderer.cpp, app.cpp,
    # nanovg_painter.cpp) reference: the sokol_gfx/app/glue API and the
    # NanoVG + nanovg_sokol backend API. The consumer selects the sokol
    # backend (SOKOL_GLCORE / SOKOL_METAL / SOKOL_D3D11) via a compile
    # define for the whole TU. These are the declaration-side includes;
    # the matching implementations come from the vendored-C block
    # (sokol_impl.c / nanovg_sokol.c), which is why the sokol headers are
    # listed as REPEATABLE.
    emitted_headers: set[Path] = set()
    parts: list[str] = [
        "// ─── Renderer/windowing vendored declarations ────────────────────────\n",
        "#if !defined(AFFINEUI_STUB_BUILD)\n",
        "#  if !defined(AFFINEUI_HOST_PROVIDES_SOKOL)\n",
    ]
    parts.append(expand_external_file(root, root / "external" / "sokol" / "sokol_log.h",
                                      emitted_headers))
    parts.append(expand_external_file(root, root / "external" / "sokol" / "sokol_gfx.h",
                                      emitted_headers))
    parts.append(expand_external_file(root, root / "external" / "sokol" / "sokol_app.h",
                                      emitted_headers))
    parts.append(expand_external_file(root, root / "external" / "sokol" / "sokol_glue.h",
                                      emitted_headers))
    parts.extend([
        "#  endif  // !AFFINEUI_HOST_PROVIDES_SOKOL\n",
        "\n",
        "#  if !defined(AFFINEUI_HOST_PROVIDES_NANOVG)\n",
    ])
    parts.append(expand_external_file(root, root / "external" / "nanovg" / "src" / "nanovg.h",
                                      emitted_headers))
    parts.append('extern "C" {\n')
    parts.append(expand_external_file(root, root / "external" / "nanovg" / "src" / "nanovg_sokol.h",
                                      emitted_headers))
    parts.append('}  // extern "C"\n')
    parts.extend([
        "#  endif  // !AFFINEUI_HOST_PROVIDES_NANOVG\n",
        "#endif  // !AFFINEUI_STUB_BUILD\n",
        "\n",
    ])
    return "".join(parts)


def emit_adapter_source(root: Path, rel_path: str,
                        strip_includes: set[str]) -> str:
    """Inline an opt-in windowing adapter (affineui/sokol.h or sdl.h) into
    the public header. Drops ``#pragma once`` and internal ``affineui/``
    includes (already inlined above), plus any include named in
    ``strip_includes`` (vendored deps inlined separately). Every other
    include is kept in place — notably ``<SDL.h>``, which the SDL adapter
    expects the consumer to provide."""
    text = (root / rel_path).read_text(encoding="utf-8")
    out: list[str] = [render_external_header(rel_path)]
    for line in text.splitlines():
        if PRAGMA_ONCE_RE.match(line):
            continue
        m = ANY_INCLUDE_RE.match(line)
        if m and (is_internal_include(m.group(1)) or m.group(1) in strip_includes):
            continue
        out.append(line.rstrip())
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out) + "\n"


def emit_adapter_block(root: Path) -> str:
    """Opt-in windowing adapters for the two-file SDK. The sokol adapter is
    fully self-contained: sokol is vendored, so its declarations inline
    under the AFFINEUI_WITH_SOKOL guard ahead of the adapter body. The SDL
    adapter keeps ``#include <SDL.h>`` — SDL2 is an external dependency the
    consumer links themselves."""
    sokol_strip = {"sokol_gfx.h", "sokol_app.h", "sokol_glue.h", "sokol_log.h"}
    parts: list[str] = [
        "// ─── windowing adapters (opt-in: define AFFINEUI_WITH_SOKOL / _SDL) ──────\n",
        "#if defined(AFFINEUI_WITH_SOKOL)\n",
    ]
    emitted_headers: set[Path] = set()
    for h in ("sokol_log.h", "sokol_gfx.h", "sokol_app.h", "sokol_glue.h"):
        parts.append(expand_external_file(root, root / "external" / "sokol" / h,
                                          emitted_headers))
    parts.append(emit_adapter_source(root, "include/affineui/sokol.h", sokol_strip))
    parts.extend([
        "#endif  // AFFINEUI_WITH_SOKOL\n",
        "\n",
        "#if defined(AFFINEUI_WITH_SDL)\n",
    ])
    parts.append(emit_adapter_source(root, "include/affineui/sdl.h", set()))
    parts.extend([
        "#endif  // AFFINEUI_WITH_SDL\n",
        "\n",
    ])
    return "".join(parts)


def render_external_source_block(root: Path, rel_path: str,
                                 emitted_external_headers: set[Path]) -> str:
    return render_block(process_file(root, rel_path,
                                     expand_external=True,
                                     emitted_external_headers=emitted_external_headers))


def emit_vendored_c_block(root: Path, cwrap_blocks: list[FileBlock],
                          vendored_external_headers: set[Path]) -> str:
    parts: list[str] = [
        "// ─── vendored-C wrappers (C linkage) ────────────────────────────────────\n\n",
        "#if !defined(AFFINEUI_STUB_BUILD) && !defined(AFFINEUI_HOST_PROVIDES_NANOVG)\n",
        'extern "C" {\n\n',
        expand_external_file(root,
                             root / "external" / "nanovg" / "src" / "nanovg.c",
                             vendored_external_headers),
        '}  // extern "C"\n',
        "#endif  // !AFFINEUI_STUB_BUILD && !AFFINEUI_HOST_PROVIDES_NANOVG\n\n",
        'extern "C" {\n\n',
    ]
    for b in cwrap_blocks:
        parts.append(render_block(b))
    parts.append('}  // extern "C"\n\n')
    parts.append(
        "// Implementation macros are single-use; clear them before any later\n"
        "// declaration-only repeat of these upstream headers (the renderer\n"
        "// declaration block re-includes the sokol + nanovg_sokol headers).\n"
        "#undef NANOVG_SOKOL_IMPLEMENTATION\n"
        "#undef SOKOL_IMPL\n"
        "#undef SOKOL_GFX_IMPL\n"
        "#undef SOKOL_APP_IMPL\n"
        "#undef SOKOL_GLUE_IMPL\n"
        "#undef SOKOL_LOG_IMPL\n"
        "#undef FONTSTASH_IMPLEMENTATION\n"
        "#undef STB_IMAGE_IMPLEMENTATION\n"
        "#undef STB_TRUETYPE_IMPLEMENTATION\n"
        "\n"
    )
    return "".join(parts)


def emit_header(root: Path, out_path: Path, version: str) -> int:
    blocks = [process_file(root, p) for p in PUBLIC_HEADERS]

    sys_seen: set[str] = set()
    sys_order: list[str] = []
    for b in blocks:
        for s in b.system_includes:
            if s not in sys_seen:
                sys_seen.add(s)
                sys_order.append(s)

    parts: list[str] = [
        banner(version, "public header"),
        "\n",
        "#ifndef AFFINEUI_AMALGAMATED_H\n",
        "#define AFFINEUI_AMALGAMATED_H\n",
        "\n",
    ]
    for s in sys_order:
        parts.append(f"#include <{s}>\n")
    parts.append("\n")
    for b in blocks:
        parts.append(render_block(b))
    # Opt-in windowing adapters last: they reference affineui::Ui / Event /
    # Key declared by the public headers above.
    parts.append(emit_adapter_block(root))
    parts.append("#endif  // AFFINEUI_AMALGAMATED_H\n")

    text = "".join(parts).rstrip() + "\n"
    out_path.write_text(text, encoding="utf-8")
    return len(text)


def emit_impl(root: Path, out_path: Path, version: str, cxx: str) -> int:
    header_blocks = [process_file(root, p) for p in INTERNAL_HEADERS]
    source_blocks = [process_file(root, p) for p in ENGINE_SOURCES]
    vendored_external_headers: set[Path] = set()
    cwrap_external_headers: set[Path] = set()
    cwrap_blocks  = [
        process_file(root, p, expand_external=True,
                     emitted_external_headers=cwrap_external_headers)
        for p in VENDORED_C_TUS
    ]

    sys_seen: set[str] = set()
    sys_order: list[str] = []
    for b in (*header_blocks, *source_blocks, *cwrap_blocks):
        for s in b.system_includes:
            if s not in sys_seen:
                sys_seen.add(s)
                sys_order.append(s)

    parts: list[str] = [
        banner(version, "implementation"),
        "\n",
        '#include "affineui.h"\n',
        "\n",
    ]
    for s in sys_order:
        parts.append(f"#include <{s}>\n")
    parts.append("\n")

    # ── Vendored-symbol shielding ─────────────────────────────────────────
    # The whole amalgamation is one TU, so every vendored single-header dep
    # we bundle — sokol, stb_image, stb_truetype, fontstash — can be
    # compiled with internal linkage using each library's upstream
    # static-prefix macro. The matching `AFFINEUI_HOST_PROVIDES_*` flag
    # disables the override so a host that brings its own dep keeps the
    # symbols externally visible and ours stays out of the way.
    #
    # NanoVG has no equivalent macro upstream; its `nvg*` symbols stay
    # externally linked. Set AFFINEUI_HOST_PROVIDES_NANOVG to disable our
    # copy if the host also links NanoVG.
    parts.append(
        "// ─── vendored-symbol shielding (TU-local linkage) ───────────────────────\n"
        "// Each define is honored by the matching upstream header; the\n"
        "// AFFINEUI_HOST_PROVIDES_* guard lets a host swap in its own copy.\n"
        "// sokol is shielded to internal linkage by default, but NOT when\n"
        "// AFFINEUI_WITH_SOKOL is set: that opts into the affineui::sokol\n"
        "// adapter, whose inline code (and the consumer's own sapp_run call)\n"
        "// must see sokol's API with external linkage.\n"
        "#if !defined(AFFINEUI_HOST_PROVIDES_SOKOL) && !defined(AFFINEUI_WITH_SOKOL)\n"
        "#  define SOKOL_API_DECL static\n"
        "#  define SOKOL_API_IMPL static\n"
        "#endif\n"
        "#ifndef AFFINEUI_HOST_PROVIDES_STB_IMAGE\n"
        "#  define STB_IMAGE_STATIC\n"
        "#endif\n"
        "#ifndef AFFINEUI_HOST_PROVIDES_STB_TRUETYPE\n"
        "#  define STB_TRUETYPE_STATIC\n"
        "#  define STBTT_STATIC\n"
        "#endif\n"
        "#ifndef AFFINEUI_HOST_PROVIDES_FONTSTASH\n"
        "#  define FONS_STATIC\n"
        "#endif\n"
        "\n"
    )

    parts.append(emit_lexbor_block(root, cxx))
    parts.append(emit_vendored_c_block(root, cwrap_blocks,
                                       vendored_external_headers))
    parts.append(emit_yoga_block(root))
    parts.append(emit_renderer_declaration_block(root))

    parts.append("// ─── internal headers ───────────────────────────────────────────────────\n\n")
    for b in header_blocks:
        parts.append(render_block(b))

    parts.append("// ─── C++ implementation ─────────────────────────────────────────────────\n\n")
    for b in source_blocks:
        parts.append(render_block(b))

    text = "".join(parts).rstrip() + "\n"
    out_path.write_text(text, encoding="utf-8")
    return len(text)


def emit_manifest(root: Path) -> None:
    def report(label: str, files: list[str]) -> None:
        print(f"  {label}:")
        for p in files:
            present = (root / p).exists()
            print(f"    [{'x' if present else ' '}] {p}")
        print()

    print("== AffineUI amalgamator manifest ==\n")
    report("Public headers (affineui.h)",          PUBLIC_HEADERS)
    report("Internal headers (affineui.cpp head)", INTERNAL_HEADERS)
    report("Engine sources (affineui.cpp body)",   ENGINE_SOURCES)
    report("Vendored-C wrappers (affineui.cpp tail)", VENDORED_C_TUS)
    report("External snapshots (inlined)", REQUIRED_EXTERNAL_FILES)


def verify_inputs(root: Path) -> list[str]:
    missing: list[str] = []
    for group in (PUBLIC_HEADERS, INTERNAL_HEADERS, ENGINE_SOURCES,
                  VENDORED_C_TUS, REQUIRED_EXTERNAL_FILES):
        for p in group:
            if not (root / p).exists():
                missing.append(p)
    return missing


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--root", required=True, help="repo root")
    ap.add_argument("--out",  required=True, help="output directory")
    ap.add_argument("--cxx", default="clang++",
                    help="C++ compiler used to repair generated Lexbor staging")
    ap.add_argument("--manifest-only", action="store_true",
                    help="print the input manifest and exit (no files written)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    out  = Path(args.out).resolve()

    if args.manifest_only:
        emit_manifest(root)
        return 0

    # Regenerate the Decius bundle header before scanning inputs.
    # embed_decius.py reads examples/frameworks/{css,fonts}/* and writes
    # include/affineui/_decius_bundle.h (byte arrays of the zlib-
    # compressed assets). The header is listed in INTERNAL_HEADERS below
    # so it gets inlined into affineui.cpp; running the embed here means
    # the drop-in ships with a fresh bundle every time the amalgamation
    # is regenerated — no stale bytes, no manual sync.
    embed_script = root / "tools" / "embed_decius.py"
    embed_out    = root / "include" / "affineui" / "_decius_bundle.h"
    if embed_script.exists():
        result = subprocess.run(
            [sys.executable, str(embed_script), str(embed_out)],
            cwd=str(root),
            check=False,
        )
        if result.returncode != 0:
            print("amalgamate: embed_decius.py failed", file=sys.stderr)
            return 1

    missing = verify_inputs(root)
    if missing:
        print("amalgamate: missing input files:", file=sys.stderr)
        for p in missing:
            print(f"  - {p}", file=sys.stderr)
        return 1

    out.mkdir(parents=True, exist_ok=True)
    version = read_version(root)

    h_path   = out / "affineui.h"
    cpp_path = out / "affineui.cpp"
    h_size   = emit_header(root, h_path,   version)
    cpp_size = emit_impl  (root, cpp_path, version, args.cxx)

    print(f"wrote {h_path}   ({h_size:>9,} bytes)")
    print(f"wrote {cpp_path} ({cpp_size:>9,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
