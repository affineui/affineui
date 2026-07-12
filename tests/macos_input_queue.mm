#import <AppKit/AppKit.h>

#include "affineui/app.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kMoveCount = 180;
constexpr auto kFrameCost = 30ms;
constexpr auto kPostInterval = 1ms;
constexpr auto kMaxMouseUpLatency = 750ms;
constexpr auto kWatchdogTimeout = 8s;
constexpr NSEventModifierFlags kProbeModifiers =
    NSEventModifierFlagShift | NSEventModifierFlagControl |
    NSEventModifierFlagOption | NSEventModifierFlagCommand;

int sample_x(int index) {
    return 20 + index;
}

[[noreturn]] void fail(const char* reason) {
    std::fprintf(stderr, "macos_native_input_queue: FAIL: %s\n", reason);
    std::fflush(stderr);
    std::_Exit(2);
}

void post_mouse_event(NSEventType type, NSInteger window_number,
                      int sample_index) {
    @autoreleasepool {
        const NSPoint location = NSMakePoint(sample_x(sample_index), 80.0);
        NSEvent* event = [NSEvent mouseEventWithType:type
                                           location:location
                                      modifierFlags:kProbeModifiers
                                          timestamp:NSProcessInfo.processInfo.systemUptime
                                       windowNumber:window_number
                                            context:nil
                                        eventNumber:sample_index + 1
                                         clickCount:1
                                           pressure:0.0];
        if (!event) fail("could not create synthetic NSEvent");
        [NSApp postEvent:event atStart:NO];
    }
}

}  // namespace

int main() {
    // Turn the engine's defense-in-depth guard into a hard test failure. The
    // native scheduler must serialize frame opportunities before cb_frame.
    setenv("AFFINEUI_ABORT_ON_FRAME_REENTRY", "1", 1);

    affineui::App::Config config;
    config.title = "AffineUI native input queue regression";
    config.width = 360;
    config.height = 180;
    config.high_dpi = false;
    config.vsync = true;
    affineui::App app{config};

    int moves_seen = 0;
    int last_move_x = -1;
    std::atomic<bool> producer_started{false};
    std::atomic<bool> mouse_up_posted{false};
    std::atomic<bool> mouse_up_seen{false};
    std::atomic<std::int64_t> mouse_up_posted_ns{0};
    int frame_count = 0;
    int mouse_up_frame = -1;

    app.on_event([&](const affineui::Event& event,
                     const std::vector<affineui::Document::HoverInfo>&) {
        if (!(event.shift && event.ctrl && event.alt && event.super)) {
            return false;
        }
        if (event.type == affineui::EventType::MouseMove) {
            if (moves_seen >= kMoveCount) {
                fail("received more move callbacks than were posted");
            }
            if (event.pos.x <= last_move_x) {
                fail("move callbacks were dropped, duplicated, or reordered");
            }
            last_move_x = event.pos.x;
            ++moves_seen;
        } else if (event.type == affineui::EventType::MouseUp) {
            if (!mouse_up_posted.load(std::memory_order_acquire)) {
                fail("mouse-up callback preceded its native post");
            }
            if (moves_seen != kMoveCount) {
                fail("mouse-up overtook queued move callbacks");
            }
            const auto posted = Clock::time_point{
                std::chrono::nanoseconds{
                    mouse_up_posted_ns.load(std::memory_order_acquire)}};
            if (Clock::now() - posted > kMaxMouseUpLatency) {
                fail("native queue backed up behind slow frames");
            }
            mouse_up_frame = frame_count;
            mouse_up_seen.store(true, std::memory_order_release);
            app.invalidate();
        }
        return false;
    });

    app.on_frame([&](double) {
        ++frame_count;
        if (!producer_started.exchange(true, std::memory_order_acq_rel)) {
            NSWindow* window = NSApp.keyWindow;
            if (!window) fail("no key window on first frame");
            const NSInteger window_number = window.windowNumber;
            std::thread([window_number, &mouse_up_posted,
                         &mouse_up_posted_ns] {
                post_mouse_event(NSEventTypeLeftMouseDown, window_number, 0);
                for (int i = 0; i < kMoveCount; ++i) {
                    post_mouse_event(NSEventTypeLeftMouseDragged,
                                     window_number, i);
                    std::this_thread::sleep_for(kPostInterval);
                }
                mouse_up_posted_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()).count(),
                    std::memory_order_release);
                mouse_up_posted.store(true, std::memory_order_release);
                post_mouse_event(NSEventTypeLeftMouseUp, window_number,
                                 kMoveCount - 1);
            }).detach();
        }

        // Simulate an application whose update/render transaction is slower
        // than the native pointer sample rate. Input must still drain as a
        // batch before the next frame opportunity instead of one sample per
        // expensive frame.
        std::this_thread::sleep_for(kFrameCost);

        if (mouse_up_seen.load(std::memory_order_acquire) &&
            frame_count > mouse_up_frame) {
            if (moves_seen != kMoveCount) {
                fail("not every posted move reached the application callback");
            }
            std::fprintf(stderr,
                         "macos_native_input_queue: PASS: moves=%d frames=%d\n",
                         moves_seen, frame_count);
            std::fflush(stderr);
            std::_Exit(0);
        }
    });

    std::thread([] {
        std::this_thread::sleep_for(kWatchdogTimeout);
        fail("watchdog expired before input and rendering both progressed");
    }).detach();

    (void) app.run();
    fail("App::run returned before the regression completed");
}
