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
