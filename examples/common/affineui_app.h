#pragma once

// ── affineui_app — the AffineUI app/DCC tool template ───────────────────────
//
// EXAMPLE / TEMPLATE CODE — NOT part of the AffineUI library.
//
// This umbrella bundles a compact, best-practices spine of the kind shared by
// DCC tools (Maya, 3ds Max, Unity, Photoshop): a command-based editing core —
// agnostic do/undo/redo actions, an undo stack with coalescing, a command
// registry with live enabled/checked state, a selection engine, a small scene
// document with minimal introspection, gestures/tools, and localization.
//
// It is entirely opt-in. The AffineUI library (`#include "affineui.h"`) has no
// dependency on any of this; a non-DCC app simply never includes it. Adopting
// this template is never required to use AffineUI — copy it into your app and
// extend it, or ignore it and wire your own app core to the View API.
//
//   #include "affineui.h"        // the UI library
//   #include "affineui_app.h"    // this template (commands, undo, selection…)
//
// (Keybindings live in the library itself — affineui::Keymap comes for free
// with affineui.h — because keyboard accelerators aren't DCC-specific.)

// The sidecar depends on the UI library, so it pulls it in itself: a user can
// include just affineui_app.h, in any order, and get both. (affineui.h has no
// reverse dependency on this template.)
#include <affineui/affineui.h>

#include "app/change_tracker.h"
#include "app/command.h"
#include "app/command_registry.h"
#include "app/command_stack.h"
#include "app/context.h"
#include "app/document.h"
#include "app/gesture.h"
#include "app/localization.h"
#include "app/selection.h"
#include "app/settings.h"
#include "app/standard_commands.h"
#include "app/styles.h"
#include "app/toml.h"
