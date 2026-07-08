// macOS-only helper: drain the NSEvent queue so every queued OS event
// dispatches to sokol_app's NSResponder callbacks (and thence to cb_event)
// within a single call, instead of one-per-run-loop-iteration.
//
// During modal mouse-drag tracking on macOS, AppKit's default is to deliver
// mouseDragged: at run-loop-iteration rate — which becomes display-link rate
// when cb_frame is slow. With sokol's setMouseCoalescingEnabled:NO on top,
// historical events queue in Cocoa and mouse-up ends up buried behind them,
// producing a multi-second unresponsive tail after any drag.
//
// Calling this at the top of App::should_render()/render() (i.e. once per
// app pulse) breaks the coupling: we pump the full NSEvent queue every
// pulse regardless of AppKit's throttle, so cb_event fires N times per
// pulse and mouse-up dispatches on the same pulse the user released.
//
// __APPLE__ guard makes the file safe to compile on non-Apple platforms —
// necessary because the amalgamator concatenates every ENGINE_SOURCES
// entry into the cross-platform affineui.cpp.

#if defined(__APPLE__)

#import <AppKit/AppKit.h>

extern "C" void affineui_mac_drain_native_events(void) {
    // NSEvent queue is polled via nextEventMatchingMask on the shared
    // NSApp; dequeue:YES pops it, sendEvent: routes through the responder
    // chain (which reaches sokol_app's mouseMoved:/mouseDragged:).
    //
    // We check both the current run-loop mode and NSDefaultRunLoopMode:
    // during a modal drag the loop is in NSEventTrackingRunLoopMode, and
    // events queued for a specific mode aren't visible to nextEvent unless
    // we ask for that mode. Polling both catches drag + non-drag events.
    NSString* current_mode = [[NSRunLoop currentRunLoop] currentMode];
    NSString* modes[2] = {
        current_mode ? current_mode : NSDefaultRunLoopMode,
        NSDefaultRunLoopMode,
    };
    const int mode_count =
        (current_mode && ![current_mode isEqualToString:NSDefaultRunLoopMode]) ? 2 : 1;
    for (int i = 0; i < mode_count; ++i) {
        for (;;) {
            @autoreleasepool {
                NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:modes[i]
                                                  dequeue:YES];
                if (!e) break;
                [NSApp sendEvent:e];
            }
        }
    }
}

#endif  // __APPLE__
