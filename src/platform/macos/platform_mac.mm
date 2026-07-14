// macOS shell: the system menu bar and the window's chrome.
//
// This is the only place in the tree that talks to AppKit about menus and
// window decoration. It is deliberately OUTSIDE the vendored sokol tree: the
// previous arrangement patched a minimal Cmd-Q menu straight into
// sokol_app.h's applicationDidFinishLaunching (marked TEMPORARY, see #60/#61),
// which meant the menu could never be app-supplied. sokol already exposes
// sapp_macos_get_window(), and its init callback runs from inside
// applicationDidFinishLaunching — by which point NSApp and the NSWindow both
// exist — so everything here can be driven from the app layer with no patch at
// all. That patch is now gone.

#include "platform/platform.h"

// Guarded, not just excluded by the build files: the single-file distribution
// concatenates every engine source into one TU, so this has to exclude itself
// on the platforms it does not belong to. (affineui.cpp is already compiled as
// Objective-C++ on Apple — sokol_app's macOS backend is ObjC — so it folds in.)
#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

// sokol's macOS accessors. Declared rather than pulled in, so this file does
// not need sokol's build configuration.
extern "C" {
const void* sapp_macos_get_window(void);
void        sapp_request_quit(void);
}

namespace affineui::platform {
namespace {

NSWindow* window() {
    return (__bridge NSWindow*) sapp_macos_get_window();
}

NSString* to_ns(std::string_view s) {
    return [[NSString alloc] initWithBytes:s.data()
                                    length:(NSUInteger) s.size()
                                  encoding:NSUTF8StringEncoding]
               ?: @"";
}

// The role callbacks the core services (edit + quit + close). Held for the
// lifetime of the installed menu; replaced wholesale on each install_menu().
RoleHandler g_on_role;
// Item callbacks, indexed by NSMenuItem.tag. Rebuilt on each install_menu() —
// a menu is re-set whenever its checked/enabled state changes, so these must
// not accumulate.
std::vector<std::function<void()>> g_actions;

}  // namespace
}  // namespace affineui::platform

// Target for every item we own. AppKit needs an object that responds to the
// action selector; routing through one target keeps the std::functions on our
// side of the bridge.
@interface AffineUIMenuTarget : NSObject
- (void)itemSelected:(NSMenuItem*)sender;
- (void)roleSelected:(NSMenuItem*)sender;
@end

@implementation AffineUIMenuTarget
- (void)itemSelected:(NSMenuItem*)sender {
    const auto tag = (std::size_t) sender.tag;
    if (tag < affineui::platform::g_actions.size()) {
        if (auto& fn = affineui::platform::g_actions[tag]) fn();
    }
}
- (void)roleSelected:(NSMenuItem*)sender {
    if (affineui::platform::g_on_role) {
        affineui::platform::g_on_role((affineui::MenuRole) sender.tag);
    }
}
@end

namespace affineui::platform {
namespace {

// Leaked deliberately: it must outlive every menu, and it lives for the whole
// process.
AffineUIMenuTarget* target() {
    static AffineUIMenuTarget* t = [[AffineUIMenuTarget alloc] init];
    return t;
}

// ─── Roles ────────────────────────────────────────────────────────────

struct RoleSpec {
    const char* label;
    const char* accel;      // Electron-style, so it parses like an app's own
    SEL         selector;   // AppKit's own action, or nil to call back
};

// The standard label/accelerator/behavior for each role. Where AppKit has a
// working action already (Hide, Minimize, the About panel), we use it. The
// edit group does NOT: those must act on the focused DOM text control, which
// is not in the AppKit responder chain, so they come back to the core. Quit and
// Close come back too — they have to run the close-request intent.
RoleSpec spec_for(MenuRole role) {
    switch (role) {
        case MenuRole::About:
            return {"About", "", @selector(orderFrontStandardAboutPanel:)};
        case MenuRole::Services:      return {"Services", "", nil};
        case MenuRole::Hide:
            return {"Hide", "CmdOrCtrl+H", @selector(hide:)};
        case MenuRole::HideOthers:
            return {"Hide Others", "Alt+CmdOrCtrl+H",
                    @selector(hideOtherApplications:)};
        case MenuRole::Unhide:
            return {"Show All", "", @selector(unhideAllApplications:)};
        case MenuRole::Preferences:
            return {"Settings…", "CmdOrCtrl+,", nil};
        case MenuRole::Quit:          return {"Quit", "CmdOrCtrl+Q", nil};

        case MenuRole::Undo:      return {"Undo", "CmdOrCtrl+Z", nil};
        case MenuRole::Redo:      return {"Redo", "Shift+CmdOrCtrl+Z", nil};
        case MenuRole::Cut:       return {"Cut", "CmdOrCtrl+X", nil};
        case MenuRole::Copy:      return {"Copy", "CmdOrCtrl+C", nil};
        case MenuRole::Paste:     return {"Paste", "CmdOrCtrl+V", nil};
        case MenuRole::SelectAll: return {"Select All", "CmdOrCtrl+A", nil};

        case MenuRole::Minimize:
            return {"Minimize", "CmdOrCtrl+M", @selector(performMiniaturize:)};
        case MenuRole::Zoom:  return {"Zoom", "", @selector(performZoom:)};
        case MenuRole::Close: return {"Close Window", "CmdOrCtrl+W", nil};
        case MenuRole::ToggleFullscreen:
            return {"Toggle Full Screen", "Ctrl+CmdOrCtrl+F",
                    @selector(toggleFullScreen:)};
        case MenuRole::None: break;
    }
    return {"", "", nil};
}

// ─── Accelerators ─────────────────────────────────────────────────────

// AppKit wants the key equivalent as a *character*, with the modifiers in a
// mask. Function/navigation keys are the unicode values AppKit defines.
NSString* key_equivalent(const std::string& key) {
    if (key.empty()) return @"";
    if (key.size() == 1) {
        // Lowercase: an uppercase keyEquivalent implies Shift to AppKit, which
        // would double up with an explicit Shift in the mask.
        return [to_ns(key) lowercaseString];
    }
    if (key == "Enter" || key == "Return") return @"\r";
    if (key == "Tab") return @"\t";
    if (key == "Backspace") return @"\b";
    if (key == "Delete") return [NSString stringWithFormat:@"%C", (unichar) NSDeleteCharacter];
    if (key == "Escape") return @"\033";
    if (key == "Space") return @" ";
    if (key == "Left")  return [NSString stringWithFormat:@"%C", (unichar) NSLeftArrowFunctionKey];
    if (key == "Right") return [NSString stringWithFormat:@"%C", (unichar) NSRightArrowFunctionKey];
    if (key == "Up")    return [NSString stringWithFormat:@"%C", (unichar) NSUpArrowFunctionKey];
    if (key == "Down")  return [NSString stringWithFormat:@"%C", (unichar) NSDownArrowFunctionKey];
    if (key.size() >= 2 && (key[0] == 'F' || key[0] == 'f')) {
        const int n = std::atoi(key.c_str() + 1);
        if (n >= 1 && n <= 20) {
            return [NSString stringWithFormat:@"%C",
                                              (unichar) (NSF1FunctionKey + n - 1)];
        }
    }
    return [to_ns(key) lowercaseString];
}

NSEventModifierFlags modifier_mask(const Accelerator& a) {
    NSEventModifierFlags m = 0;
    if (a.super) m |= NSEventModifierFlagCommand;
    if (a.ctrl) m |= NSEventModifierFlagControl;
    if (a.alt) m |= NSEventModifierFlagOption;
    if (a.shift) m |= NSEventModifierFlagShift;
    return m;
}

void apply_accelerator(NSMenuItem* item, std::string_view spec) {
    const Accelerator a = parse_accelerator(spec);
    if (!a.valid()) return;
    item.keyEquivalent             = key_equivalent(a.key);
    item.keyEquivalentModifierMask = modifier_mask(a);
}

// ─── Swatches ─────────────────────────────────────────────────────────

// A solid color chip, so an accent picker's custom-drawn swatch row survives
// the trip into a native menu as a real NSMenuItem image.
NSImage* swatch_image(Color c) {
    const CGFloat side = 12.0;
    NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
    [img lockFocus];
    [[NSColor colorWithSRGBRed:c.r / 255.0
                         green:c.g / 255.0
                          blue:c.b / 255.0
                         alpha:c.a / 255.0] setFill];
    NSBezierPath* p =
        [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, side, side)
                                        xRadius:3
                                        yRadius:3];
    [p fill];
    [img unlockFocus];
    return img;
}

// ─── Building ─────────────────────────────────────────────────────────

void build_items(NSMenu* into, const std::vector<MenuItem>& items,
                 std::string_view app_name);

NSMenuItem* build_item(const MenuItem& m, std::string_view app_name) {
    if (m.type == MenuItemType::Separator) return [NSMenuItem separatorItem];

    const RoleSpec spec = spec_for(m.item_role);
    const bool is_role  = m.item_role != MenuRole::None;

    // A role's label is the platform's, unless the app overrode it. About and
    // Quit take the app's name, the way every Mac app's do.
    std::string label = m.label;
    if (label.empty() && is_role) {
        label = spec.label;
        if (m.item_role == MenuRole::About || m.item_role == MenuRole::Quit) {
            label += " ";
            label += app_name;
        }
    }

    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:to_ns(label)
                                                  action:nil
                                           keyEquivalent:@""];

    const std::string accel =
        !m.accelerator.empty() ? m.accelerator
                               : (is_role ? std::string(spec.accel) : std::string());
    apply_accelerator(item, accel);

    if (!m.submenu.empty()) {
        NSMenu* sub = [[NSMenu alloc] initWithTitle:to_ns(label)];
        build_items(sub, m.submenu, app_name);
        item.submenu = sub;
        if (m.item_role == MenuRole::Services) NSApp.servicesMenu = sub;
    } else if (is_role) {
        if (m.item_role == MenuRole::Services) {
            // Services is a submenu AppKit populates; give it an empty one.
            NSMenu* sub    = [[NSMenu alloc] initWithTitle:@"Services"];
            item.submenu   = sub;
            NSApp.servicesMenu = sub;
        } else if (spec.selector != nil && !m.on_select) {
            item.action = spec.selector;
            item.target = nil;  // responder chain / NSApp
        } else if (m.on_select) {
            item.action = @selector(itemSelected:);
            item.target = target();
            item.tag    = (NSInteger) g_actions.size();
            g_actions.push_back(m.on_select);
        } else {
            // The core services this one (edit group, Quit, Close).
            item.action = @selector(roleSelected:);
            item.target = target();
            item.tag    = (NSInteger) m.item_role;
        }
    } else if (m.on_select) {
        item.action = @selector(itemSelected:);
        item.target = target();
        item.tag    = (NSInteger) g_actions.size();
        g_actions.push_back(m.on_select);
    }

    item.enabled = m.enabled ? YES : NO;
    if (m.type == MenuItemType::Checkbox || m.type == MenuItemType::Radio) {
        item.state = m.checked ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (m.swatch.a != 0) item.image = swatch_image(m.swatch);
    item.hidden = m.visible ? NO : YES;
    return item;
}

void build_items(NSMenu* into, const std::vector<MenuItem>& items,
                 std::string_view app_name) {
    // We set enabled/disabled ourselves; without this AppKit disables every
    // item whose action nothing in the responder chain validates.
    into.autoenablesItems = NO;
    for (const auto& m : items) [into addItem:build_item(m, app_name)];
}

}  // namespace

bool has_native_menus() noexcept { return true; }

void install_menu(const Menu& menu, std::string_view app_name,
                  RoleHandler on_role) {
    @autoreleasepool {
        g_on_role = std::move(on_role);
        g_actions.clear();

        NSMenu* menubar      = [[NSMenu alloc] init];
        menubar.autoenablesItems = NO;

        for (const auto& top : menu) {
            // Every top-level entry is a submenu holder — that is how NSMenu
            // models a menu bar, whatever the model said.
            NSMenuItem* holder = [[NSMenuItem alloc] init];
            NSMenu* sub = [[NSMenu alloc] initWithTitle:to_ns(top.label)];
            build_items(sub, top.submenu, app_name);
            holder.submenu = sub;
            holder.title   = to_ns(top.label);
            [menubar addItem:holder];
        }
        NSApp.mainMenu = menubar;
    }
}

// ─── Window chrome ────────────────────────────────────────────────────

namespace {

// Traffic lights live in window coordinates measured from the BOTTOM-left, so
// a resize moves them relative to the top. Re-place them on every resize.
void place_traffic_lights(TitleBarStyle style, Point pos) {
    NSWindow* w = window();
    if (!w) return;

    NSButton* buttons[] = {[w standardWindowButton:NSWindowCloseButton],
                           [w standardWindowButton:NSWindowMiniaturizeButton],
                           [w standardWindowButton:NSWindowZoomButton]};

    if (style == TitleBarStyle::Frameless) {
        // The app draws its own; hide the system ones rather than let them
        // sit on top of the app's.
        for (NSButton* b : buttons) b.hidden = YES;
        return;
    }
    for (NSButton* b : buttons) b.hidden = NO;

    // Zero means "whatever the style's default is": Hidden leaves them where
    // macOS puts them; HiddenInset nudges them in, which is the whole
    // difference between the two.
    float x = pos.x;
    float y = pos.y;
    if (x == 0 && y == 0) {
        if (style != TitleBarStyle::HiddenInset) return;  // Hidden: leave as-is
        x = 13;
        y = 16;
    }

    // Snapshot the reference BEFORE moving anything. Each button keeps its
    // offset from the close button, so that offset has to be measured against
    // where the close button STARTED — read it inside the loop and the first
    // iteration (which moves the close button) would corrupt the reference for
    // the other two: their dx would come out as `original_x - x`, and
    // `x + dx` would put them straight back where they were, leaving the three
    // lights split apart.
    const CGFloat origin_x = buttons[0].frame.origin.x;

    for (NSButton* b : buttons) {
        NSView* row = b.superview;
        if (!row) continue;
        NSRect f = b.frame;
        // y is given from the window top; AppKit measures from the bottom.
        f.origin.y = row.bounds.size.height - y - f.size.height;
        // Keep the OS's own spacing between the three.
        const CGFloat dx = f.origin.x - origin_x;
        f.origin.x = x + dx;
        b.frame    = f;
    }
}

}  // namespace

Rect window_controls_rect(TitleBarStyle style, Point traffic_light_position) {
    // Default: the OS owns the whole bar, and the app draws below it, so it
    // reserves nothing. Frameless: no system buttons at all.
    if (style == TitleBarStyle::Default || style == TitleBarStyle::Frameless) {
        return {};
    }
    @autoreleasepool {
        NSWindow* w = window();
        if (!w) return {};
        NSButton* close = [w standardWindowButton:NSWindowCloseButton];
        NSButton* mini  = [w standardWindowButton:NSWindowMiniaturizeButton];
        NSButton* zoom  = [w standardWindowButton:NSWindowZoomButton];
        if (!close || !mini || !zoom) return {};

        // Measured, not assumed: the lights move with traffic_light_position and
        // their size is the OS's business. The band runs from the window's left
        // edge to the far side of the last light, plus one more gap so app
        // content doesn't crowd them.
        //
        // That trailing gap is the spacing BETWEEN the lights, not their margin
        // from the window edge (which is larger, and left a visibly wider hole
        // before the app's title than the lights have between themselves). Using
        // their own rhythm makes the title sit in the row as if it were a fourth
        // element.
        const NSRect cf = close.frame;
        const NSRect zf = zoom.frame;
        const CGFloat spacing = NSMinX(mini.frame) - NSMaxX(cf);
        const CGFloat gap     = spacing > 0 ? spacing : 8.0;

        Rect r;
        r.x = 0;
        r.y = 0;
        r.w = static_cast<int>(NSMaxX(zf) + gap);
        r.h = static_cast<int>(NSMaxY(cf) + gap);
        (void) traffic_light_position;  // already reflected in the frames
        return r;
    }
}

void apply_titlebar_style(TitleBarStyle style, Point traffic_light_position) {
    @autoreleasepool {
        NSWindow* w = window();
        if (!w || style == TitleBarStyle::Default) return;

        // Content fills the window, title bar goes transparent and loses its
        // text — the window stays titled+resizable, so edge resize, snapping
        // and full-screen keep working. (A truly borderless NSWindow loses all
        // of that and has to reimplement it.)
        w.styleMask |= NSWindowStyleMaskFullSizeContentView;
        w.titlebarAppearsTransparent = YES;
        w.titleVisibility            = NSWindowTitleHidden;
        w.movableByWindowBackground  = NO;  // we route drags via app regions

        place_traffic_lights(style, traffic_light_position);
    }
}

void sync_titlebar_chrome(TitleBarStyle style, Point traffic_light_position) {
    if (style == TitleBarStyle::Default) return;
    @autoreleasepool {
        place_traffic_lights(style, traffic_light_position);
    }
}

bool begin_window_drag() {
    @autoreleasepool {
        NSWindow* w = window();
        if (!w) return false;
        // The event currently being dispatched — we are called synchronously
        // from inside sokol's event callback, so this is the mouse-down that
        // landed in the drag region.
        NSEvent* ev = NSApp.currentEvent;
        if (!ev) return false;

        // Double-click on a title bar zooms. macOS actually honors a system
        // preference here, but zoom is the default and the only one that makes
        // sense for a drawn bar.
        if (ev.clickCount >= 2) {
            [w zoom:nil];
            return true;
        }
        [w performWindowDragWithEvent:ev];  // native drag: snapping, Spaces
        return true;
    }
}

void minimize_window() {
    @autoreleasepool {
        if (NSWindow* w = window()) [w miniaturize:nil];
    }
}

void toggle_maximize_window() {
    @autoreleasepool {
        if (NSWindow* w = window()) [w zoom:nil];
    }
}

bool window_is_maximized() {
    @autoreleasepool {
        NSWindow* w = window();
        return w ? (bool) w.isZoomed : false;
    }
}

void set_window_fullscreen(bool on) {
    @autoreleasepool {
        NSWindow* w = window();
        if (!w) return;
        const bool is_fs = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (is_fs != on) [w toggleFullScreen:nil];
    }
}

bool window_is_fullscreen() {
    @autoreleasepool {
        NSWindow* w = window();
        return w ? (w.styleMask & NSWindowStyleMaskFullScreen) != 0 : false;
    }
}

}  // namespace affineui::platform

#endif  // __APPLE__
