# East Asian IME Support — Architecture & Plan

Status: **design accepted, implementation in progress** (branch `feat/ime-composition`).
Scope: composition (preedit) text input for Chinese / Japanese / Korean IMEs across
the standalone app shell, embedded hosts, and (eventually) mobile — plus the CJK
rendering gaps (font fallback, line breaking) that gate any of it being visible.

---

## 1. Background: how an IME actually talks to an app

An Input Method Editor turns key sequences into text the keyboard can't type
directly (`nihao` → 你好, `にほんご` → 日本語 → kanji candidates). The OS-level
contract is the same on every platform:

1. **Composition (preedit)** — while the user types, the IME owns an uncommitted
   string that the app must display *inline at the caret*, conventionally
   underlined, with the IME's own cursor position inside it and often a
   highlighted "active clause" (the segment currently being converted).
2. **Candidate window** — the IME shows its candidate list in *its own* popup,
   but the app must tell it where the caret is so the popup lands next to the
   text instead of a screen corner.
3. **Commit** — the user accepts text; it arrives as ordinary character input
   and the preedit disappears (possibly starting a new composition in the same
   message, e.g. continuous Japanese input).
4. **Cancel** — Esc drops the preedit without committing.
5. **Enable/disable** — the app signals whether a text field is focused so the
   OS can activate the IME (and, on mobile, raise the soft keyboard).

Per-platform realizations:

| Concern | Win32 | macOS | Linux/X11 | iOS (future) | Android (future) |
|---|---|---|---|---|---|
| Preedit in | `WM_IME_COMPOSITION` + `ImmGetCompositionString(GCS_COMPSTR/CURSORPOS/COMPATTR)` | `NSTextInputClient setMarkedText:` | XIM preedit callbacks (or over-the-spot) | `UITextInput setMarkedText:` | `InputConnection.setComposingText` |
| Commit in | `GCS_RESULTSTR` (→ `WM_CHAR` if unhandled) | `insertText:` | `XmbLookupString` | `insertText:` | `commitText` |
| Caret rect out | `ImmSetCandidateWindow` / `ImmSetCompositionWindow` | `firstRectForCharacterRange:` | `XNSpotLocation` | `firstRect(for:)` / keyboard avoidance | `CursorAnchorInfo` |
| Enable out | `ImmAssociateContext` | first responder | `XSetICFocus` | `becomeFirstResponder` | `showSoftInput` |

## 2. Current state (audited 2026-07-09)

- **sokol_app has no IME support.** win32 backend handles only `WM_CHAR`
  (`external/sokol/sokol_app.h` `_sapp_win32_wndproc`); macOS uses raw
  `keyDown` + `event.characters` and does **not** implement `NSTextInputClient`,
  so composition input is impossible on mac through sokol. Upstream has no
  desktop IME work (an Android IME PR, floooh/sokol#475, was rejected; nothing
  since). Practical effect today: on Windows, committed text leaks through as
  `WM_CHAR` with no preedit and a mispositioned candidate window; on macOS, CJK
  input is dead.
- **Core event model** (`include/affineui/types.h`): `EventType::TextInput`
  carries committed UTF-8; there is no composition/preedit event.
- **Text editing core** (`src/renderer/dom/document_text.cpp`) is fully
  UTF-8-boundary-aware — committed CJK already inserts/deletes/navigates
  correctly. The gap is composition, not storage.
- **Caret geometry is computed but not exposed**: `TextLayoutEntry`
  (`caret_offsets/caret_x/caret_lines`) exists for caret painting
  (`document_draw.cpp`); there is no `caret_rect()` API. `EMBEDDING_DESIGN.md`
  §"IME — the careful bit" already names `text_input_active()` + `caret_rect()`
  + a future `Composition` event as the planned surface.
- **Fonts**: embedded Roboto and the system candidate lists
  (`nanovg_painter.cpp`) carry no CJK glyphs, and fontstash's per-glyph
  fallback (`nvgAddFallbackFont`) is never wired — CJK renders as tofu even
  when pasted.
- **Line breaking**: the text-control wrapper breaks only at space/tab
  (`is_soft_break_space`, `document_text.cpp`), so CJK text (no spaces) will
  not wrap in multiline controls.

## 3. Prior art consulted

- **SDL3** — the cleanest neutral API and our primary model:
  `SDL_EVENT_TEXT_EDITING {text, start, length}` for preedit,
  `SDL_EVENT_TEXT_INPUT` for commit, `SDL_StartTextInput/StopTextInput` for
  enable, `SDL_SetTextInputArea(window, rect, cursor)` for candidate
  positioning. Our SDL adapter can map this 1:1, giving us a fully working
  IME path *without any sokol changes* — a reference implementation to
  validate the core against.
- **GLFW PR #2130** (unmerged, years-running) — preedit callback delivering
  the string plus *clause blocks* + focused block + caret, and
  `glfwSetPreeditCursorRectangle`. Confirms: preedit is push (event), caret
  rect is pull/set, clause info matters for Japanese.
- **Web platform** — `compositionstart/update/end`; browsers splice preedit
  into the DOM text at the caret and fire `input` on commit. We mirror the
  splice model but (unlike the web) do **not** fire widget change callbacks
  during preedit — AffineUI components emit on commit only.
- **Microsoft's "Using an IME in a Game"** — the canonical reference for
  suppressing default composition UI (`WM_IME_SETCONTEXT` /
  `ISC_SHOWUICOMPOSITIONWINDOW`) and drawing preedit yourself.

## 4. Architecture

Layering rule (see `docs/EMBEDDING_DESIGN.md`): the **core** speaks a
platform-neutral composition protocol; **platform shells** (sokol patch, SDL
adapter, host engines, future mobile glue) translate to/from the OS. Nothing
IME-platform-specific lives in the DOM/renderer.

```
 OS IME (IMM32 / NSTextInputClient / XIM / UITextInput / InputConnection / SDL)
        │  preedit + commit                     ▲  caret rect + input-active
        ▼                                       │
 platform shell (vendored-sokol patch, sdl.h adapter, host engine, mobile glue)
        │  Event{Composition}, Event{TextInput} │  Ui::caret_rect(), Ui::text_input_active()
        ▼                                       │
 Document::dispatch ──► composition state ──► preedit splice in text layout/draw
```

### 4.1 Core event model (`types.h`)

Add one event type and three fields:

```cpp
enum class EventType : std::uint8_t {
    ..., TextInput, Composition, ... };

struct Event {
    ...
    std::string text;               // TextInput: committed; Composition: preedit
    // Composition only — byte offsets into `text` (UTF-8):
    int composition_cursor{0};      // IME caret inside the preedit
    int composition_clause_begin{0};// active clause (highlighted segment);
    int composition_clause_end{0};  // begin==end → no clause info
};
```

**Protocol contract** (what platform shells must emit, what core guarantees):

- `Composition` with non-empty `text` — preedit created/updated. First such
  event deletes the active selection (browser semantics for
  `compositionstart`), which *is* a committed edit and participates in the
  control's normal live-value flow.
- `Composition` with empty `text` — preedit cleared (end or cancel; core does
  not distinguish).
- `TextInput` — commits at the caret. Core clears any preedit display first,
  then inserts. Win32 can deliver commit + new preedit in one
  `WM_IME_COMPOSITION` (continuous input); the shell sends `TextInput` then
  `Composition`, and core handles the sequence naturally.
- While a preedit is active, core **ignores editing `KeyDown`s**
  (Backspace/Delete/arrows/shortcuts) — the IME owns those keys. Platforms
  mostly swallow them anyway (`VK_PROCESSKEY`); the guard is defensive.
- Preedit text is **never** written to `text_value`, never reaches
  `live_text_values`, and never fires widget `on_input`/change callbacks.
  Cancel is therefore a pure display change.

### 4.2 Document / text-field model (`document_impl.h`, `document_text.cpp`)

Single active composition, owned by the document (an IME composes in exactly
one control at a time — the focused one):

```cpp
// DocumentImpl
std::string composition_text;        // empty ⇒ no active composition
std::size_t composition_cursor{0};   // byte offset into composition_text
std::size_t composition_clause_begin{0}, composition_clause_end{0};
```

Cleared on: `TextInput` commit, empty `Composition`, focus change, and any
programmatic value set. `text_input_active()` gates acceptance — a
`Composition` event with no focused text control is dropped.

**Rendering = splice, not state.** Layout and draw operate on an *effective*
string: `text_value` with `composition_text` inserted at `caret_offset`
(composed through `text_control_display_value`, so password masking et al.
still apply). Concretely:

- `ensure_text_layout_entry` builds its visual lines / caret tables from the
  effective string; the layout signature mixes in the composition state so
  cache invalidation is automatic.
- The caret paints at `caret_offset + composition_cursor`.
- Preedit decoration: a 1px underline across the preedit span; the active
  clause gets a 2px (or selection-tinted) underline. Drawn in the text-control
  paint path in `document_draw.cpp` from the caret tables — no new layout
  machinery.
- Offset mapping is centralized in `detail::composed_text_value` /
  `composed_caret_offset` / `composition_display_range`
  (document_text.cpp) so draw, layout, hit-testing, and `caret_rect()`
  can't disagree.
- Pointer caret placement is **ignored** while a composition is active
  (the caret table is in composed space and the IME owns the caret). The
  platform layer commits the composition on click — win32 shell:
  `ImmNotifyIME(CPS_COMPLETE)` on mouse-down (PR B) — after which the
  click lands normally.

### 4.3 Caret-intent API (the "out" direction)

Exactly the surface `EMBEDDING_DESIGN.md` planned:

```cpp
// Document (mirrored on Ui, App facade, and the C ABI)
bool text_input_active() const;   // focused editable text control?
Rect caret_rect() const;          // caret line rect, panel-local CSS points;
                                  // {0,0,0,0} when !text_input_active()
```

Implementation: derive from the block's geometry + the cached
`TextLayoutEntry` exactly as the caret painter does. Text measurement needs a
`Painter`; the layout entry is (re)built during paint, so the query returns
the entry as of the last layout/paint — correct by the time any IME popup can
appear, and cheap (pure lookup, honoring the interaction-budget envelope).

C ABI: `affineui_document_text_input_active()`, `affineui_document_caret_rect()`
(+ app-level mirrors) so Rust/C# hosts and engine embedders get it.

**Who consumes it:** after each dispatched event (and on focus-affecting
frames), the shell reads both and pushes to the platform — standalone app →
`sapp_ime_*` (below); SDL adapter → `SDL_StartTextInput` /
`SDL_SetTextInputArea`; engine hosts → whatever their engine exposes; mobile →
soft-keyboard show/hide + anchor info. Push-on-change (cheap comparison in the
shell), not per-frame spam.

### 4.4 Platform: vendored sokol patch (win32 first)

sokol upstream won't take this soon; we patch our vendored copy the same way
we maintain the nanovg/lexbor/yoga forks (patch lives in `external/sokol`,
marked `// AFFINEUI PATCH (ime)` for future upstream syncs).

New sokol surface (minimal, GLFW/SDL-informed):

```c
SAPP_EVENTTYPE_IME_COMPOSITION        // new event
// sapp_event additions:
char ime_composition[SAPP_MAX_IME_COMPOSITION_SIZE]; // UTF-8 preedit (512 B)
int  ime_composition_cursor;                         // byte offset (-1 = end)
int  ime_composition_clause_begin, ime_composition_clause_end;
// new functions:
void sapp_ime_set_rect(int x, int y, int w, int h); // caret rect, client px
void sapp_ime_set_enabled(bool enabled);            // ImmAssociateContext et al.
```

Win32 wndproc additions:

- `WM_IME_SETCONTEXT` — clear `ISC_SHOWUICOMPOSITIONWINDOW` so the IME's
  default inline-composition window never appears (we draw preedit ourselves).
- `WM_IME_STARTCOMPOSITION` — consume (no `DefWindowProc`).
- `WM_IME_COMPOSITION` — if `GCS_RESULTSTR`: read the committed UTF-16, feed
  it through the existing `_sapp_win32_char_event` path (surrogate handling
  preserved) → `SAPP_EVENTTYPE_CHAR` → `TextInput`. If `GCS_COMPSTR`: read
  preedit + `GCS_CURSORPOS` + `GCS_COMPATTR` (`ATTR_TARGET_*` → clause range),
  emit `SAPP_EVENTTYPE_COMPOSITION`. Both can occur in one message; result is
  processed first.
- `WM_IME_ENDCOMPOSITION` — emit an empty `COMPOSITION`.
- Candidate placement — on `sapp_ime_set_rect` and `IMN_OPENCANDIDATE`:
  `ImmSetCandidateWindow(CFS_EXCLUDE)` + `ImmSetCompositionWindow(CFS_POINT)`
  (the latter for legacy IMEs that anchor their own UI).
- DPI: the sapp API takes client-area **physical px**; `app.cpp` converts
  `caret_rect()` CSS points by `dpi_scale`.

`app.cpp` (`cb_event`) translates `SAPP_EVENTTYPE_COMPOSITION` → `Event`, and
pushes rect/enabled after dispatch. `include/affineui/sokol.h` gets the same
mapping for embedders who drive sokol themselves.

**macOS (follow-up PR):** patch sokol's view to adopt `NSTextInputClient` —
`keyDown:` routes through `interpretKeyEvents:`, `insertText:` → char events,
`setMarkedText:` → composition event, `firstRectForCharacterRange:` returns
the rect from `sapp_ime_set_rect`. This *changes the existing key event flow*
(dead keys start working properly too) and needs mac hardware to validate —
kept out of the Windows PR. Linux/X11 (XIM, likely over-the-spot
`XIMPreeditPosition` first) trails that.

### 4.5 SDL adapter (`include/affineui/sdl.h`) — free reference platform

SDL3 already implements every platform's IME. The adapter maps:
`SDL_EVENT_TEXT_EDITING{text,start,length}` → `Composition` (start/length are
in *codepoints* — convert to byte offsets), `SDL_EVENT_TEXT_INPUT` →
`TextInput`, and after dispatch: `text_input_active()` →
`SDL_StartTextInput/StopTextInput`, `caret_rect()` → `SDL_SetTextInputArea`.
This lands with the core PR and is how we exercise the whole pipeline against
real IMEs before the sokol patch exists.

### 4.6 Mobile (future port — designed for now, built later)

The lib is desktop-only today but a mobile port is expected. The surface above
is deliberately a clean subset of both mobile stacks:

- **iOS**: a `UITextInput`-conforming shim view. `setMarkedText:` →
  `Composition`; `insertText:` → `TextInput`; `firstRect(for:)` + keyboard
  avoidance ← `caret_rect()`; `text_input_active()` drives
  `becomeFirstResponder`/`resignFirstResponder` (soft keyboard).
- **Android**: an `InputConnection` shim. `setComposingText` → `Composition`;
  `commitText` → `TextInput`; `CursorAnchorInfo` ← `caret_rect()`;
  `text_input_active()` drives `showSoftInput`.
- **Known future extension** (explicitly *not* in v1): smart mobile IMEs also
  want *surrounding text* (`getTextBeforeCursor`, `deleteSurroundingText`,
  reconversion on desktop too). That becomes an optional core query later —
  `text_input_context()` returning value + caret/selection — additive, no
  redesign required.

### 4.7 CJK rendering prerequisites (independently valuable)

**Per-glyph font fallback.** Wire `nvgAddFallbackFontId` (exists in our
nanovg fork, currently unused) behind the painter's font registry:

- Fallback faces load **lazily** — CJK system fonts are 15–25 MB; register the
  file path eagerly, load + attach on the first codepoint ≥ U+2E80 requested
  (cheap range scan at shaping time), so Latin-only apps pay nothing.
- Candidates (first hit wins, system-locale's script ordered first):
  Windows `msyh.ttc` / `yugothm.ttc`(or `meiryo.ttc`) / `malgun.ttf` /
  `simsun.ttc`; Linux Noto Sans CJK; macOS Hiragino/PingFang (TTC — verify
  fontstash's stb_truetype face-index handling; index 0 is acceptable v1).
- Embedded-Roboto determinism (see `embedded-fonts` decision) is preserved for
  Latin UI; CJK metrics are system-dependent by necessity — documented
  limitation until we consider an optional embedded Noto CJK subset.

**CJK line breaking.** Extend the text-control wrapper (`document_text.cpp`)
with break opportunities between CJK codepoints (Han, Kana, Hangul, full-width
forms), with a minimal kinsoku rule: no break *before* closing
punctuation/small kana (。、」』ぁっー等), no break *after* opening brackets.
Full UAX #14 is out of scope. The block-flow (paragraph) breaker is a separate
code path — audit it with a conformance case (`conformance/`, A/B vs Chrome
with CJK text) and fix in the same style; if it balloons, it splits into its
own PR rather than blocking IME.

## 5. Testing strategy

- **Unit (doctest, headless)**: composition protocol state machine — update /
  commit / cancel / focus-change clears; selection-deleted-on-first-preedit;
  splice + caret math on multi-byte boundaries; layout-signature invalidation;
  KeyDown suppression during preedit; `caret_rect()` sanity + inactive cases;
  CJK wrap + kinsoku cases; fallback-font glyph resolution.
- **SDL reference run**: real-IME smoke test through the SDL adapter
  (windowed; needs the user — I can't operate a real IME).
- **Win32**: after the sokol patch, real-IME verification is **user testing**
  (Japanese + Chinese IME: preedit underline, clause highlight, candidate
  window at caret, continuous input, Esc cancel, commit into a DENDER text
  field). Synthetic `WM_IME_*` messages can't fake `ImmGetCompositionString`,
  so scripted input (`tools/drive-input`) only covers the non-IME regressions.
  Per project convention: **not "done" until user-verified in-window.**
- **Regression**: full test suite + GCC docker (`scripts/test_linux_docker.sh`)
  before each PR; conformance suite untouched by core changes.

## 6. Staged delivery (each PR green, user merges)

| Stage | Contents | Verifiable by |
|---|---|---|
| **PR A — core composition** | `EventType::Composition` + protocol; preedit state/splice/underline; KeyDown guard; `text_input_active()`/`caret_rect()`; C ABI; SDL adapter mapping; unit tests; `EMBEDDING_DESIGN.md` status flip | doctest headless + SDL smoke |
| **PR B — win32 shell** | vendored-sokol IME patch (events + `sapp_ime_*`); `app.cpp` + `sokol.h` adapter wiring; DPI handling | **user IME test** on Windows |
| **PR C — CJK rendering** | lazy per-glyph fallback fonts; text-control CJK breaks + minimal kinsoku; conformance case for paragraph CJK wrap | doctest + conformance + visual |
| later | macOS `NSTextInputClient` sokol patch | user test on mac |
| later | Linux XIM; surrounding-text query; mobile shims (with the port) | — |

A, B, C are independent enough to review separately; C is visible value even
without B (pasted/programmatic CJK stops rendering as tofu).

## 7. Open questions (flagged, not blocking)

1. **Clause styling** — underline-weight only (proposed) vs. selection-tint
   the active clause. Cosmetic; decide at review with screenshots.
2. **`sapp_ime_set_enabled` in PR B** — `ImmAssociateContext(nullptr)` when no
   text field is focused prevents the IME hotkey swallowing game-style
   shortcuts; small risk of fighting user IME toggles. Proposed: ship it,
   default-on, behind the intent (`text_input_active`).
3. **Embedded CJK font subset** — deterministic CJK layout would need shipping
   a Noto subset (megabytes). Deferred until a real consumer asks.
4. **Preedit in single-line overflow** — long preedit near the field edge
   scrolls with the existing single-line scroll logic; if an IME grows the
   preedit past the visible width the caret-follow behavior may need a tweak
   (watch during user testing).
