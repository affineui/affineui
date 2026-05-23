# AffineUI Lexbor Patch Stack

This branch carries the small Lexbor fixes AffineUI needs on top of
upstream `v2.4.0`. Keep patches focused, tested, and upstreamable.

## Patches

### html: guard style teardown when destroying cascade-matched nodes

`lxb_html_document_event_destroy` can fall through from the `el->style`
cleanup path to the inline-style list teardown even when `el->list` is
null. AffineUI hits this when imm-mode destroys cascade-matched nodes
that do not have inline `style=""` attributes.

### css/selectors: count simple pseudo-classes in specificity

Lexbor v2.4.0 parses simple pseudo-classes such as `:hover`,
`:active`, `:focus`, and `:first-child`, but did not increment the
selector specificity `b` bucket. AffineUI depends on browser cascade
ordering where `.btn:focus` outranks `.btn`.

### css: parse common framework shorthands

Adds `border-radius`, `border-color`, `background`, `box-shadow`, `gap`,
`row-gap`, and `column-gap` to the generated property table, including
parsing, serialization, and declaration tests for framework stylesheets.

### css: border-style and border-width shorthand + per-side longhands

Adds 6 new CSS properties to the lexbor property table:
- `border-style` shorthand: up to 4 style keywords (none/solid/dashed/dotted/hidden/double/groove/ridge/inset/outset); 1–4 value box-shorthand fill rules applied; global keywords (inherit/initial/unset/revert).
- `border-width` shorthand: up to 4 line-width values (thin/medium/thick or `<dimension>`); same box-shorthand fill rules; global keywords.
- `border-top-width`, `border-right-width`, `border-bottom-width`, `border-left-width`: single line-width longhands; identical parsing to each side of the shorthand.

**Types added** (`property.h`):
```c
typedef struct { lxb_css_value_type_t top, right, bottom, left; } lxb_css_property_border_style_t;
typedef struct { lxb_css_value_length_type_t top, right, bottom, left; } lxb_css_property_border_width_t;
typedef lxb_css_value_length_type_t lxb_css_property_border_{top,right,bottom,left}_width_t;
```

**SHS hash table** (`res.h`): 6 new entries at computed slots 89, 134, 135, 137, 138, 139, 140, 141 with chains correctly ordered ascending by key length for the SHS early-exit invariant.

**Enum values** (`property/const.h`): `LXB_CSS_PROPERTY_BORDER_STYLE` (0x0074) through `LXB_CSS_PROPERTY_BORDER_LEFT_WIDTH` (0x0079).

Wired in AffineUI's `cascade.cpp` via `BORDER_STYLE` (→ `ComputedStyle::border_style`) and `BORDER_WIDTH` / per-side longhands (→ `ComputedStyle::border_{top,right,bottom,left}`). Both new shorthands rank at 0 (broad shorthands, applied before per-side overrides).

### css: 2-stop gradient parsing in background property

Extends `lxb_css_property_background_t` with a `lxb_css_property_gradient_t`
member (kind + angle_deg + two color stops). The `lxb_css_property_state_background`
handler now detects `linear-gradient()` and `radial-gradient()` FUNCTION tokens
and dispatches to a new `lxb_css_property_state_gradient_args()` parser that
handles:
  - `linear-gradient([ <angle> | to <side> ]?, <color>, <color>)`
  - `radial-gradient([ circle ]?, <color>, <color>)`
The `to` and `circle` keywords are matched by direct string comparison (they
are not in the CSS value enum table). Angle units (deg, rad, turn, grad) are
converted to CSS degrees using `lxb_css_unit_angel_by_name` against the correct
`LXB_CSS_UNIT_DEG / LXB_CSS_UNIT_RAD / LXB_CSS_UNIT_TURN / LXB_CSS_UNIT_GRAD`
enum values. Error recovery skips to `)` on parse failure.

## Syncing Into AffineUI

From the AffineUI repo:

```sh
scripts/sync_lexbor_from_fork.sh
```

That copies this checkout into `affineui/external/lexbor`, excluding
`.git` and build artifacts.
