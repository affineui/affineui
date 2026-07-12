#import <AppKit/AppKit.h>

#include "affineui/sokol.h"
#include "affineui/ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMoveCount = 300;
constexpr int kStartX = 100;
constexpr auto kPostInterval = std::chrono::milliseconds(1);
constexpr auto kFrameWork = std::chrono::milliseconds(20);
constexpr auto kNestedPump = std::chrono::milliseconds(12);
constexpr NSEventModifierFlags kProbeModifiers =
    NSEventModifierFlagShift | NSEventModifierFlagControl |
    NSEventModifierFlagOption | NSEventModifierFlagCommand;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

struct Probe {
    affineui::Ui ui;
    std::array<std::atomic<std::int64_t>, kMoveCount> posted_ns{};
    std::atomic<std::int64_t> mouse_up_posted_ns{0};
    std::atomic<int> tagged_moves_received{0};
    std::atomic<int> out_of_order{0};
    std::atomic<int> last_x{-1};
    std::atomic<std::int64_t> max_age_ns{0};
    std::atomic<int> frame_count{0};
    std::atomic<int> frame_depth{0};
    std::atomic<int> max_frame_depth{0};
    std::atomic<int> events_during_frame{0};
    std::atomic<bool> producer_started{false};
    std::atomic<bool> reported{false};
    const bool nested_pump_enabled{
        std::getenv("AFFINEUI_PROBE_NO_NESTED") == nullptr};

    void record_age(std::int64_t age_ns) {
        auto previous = max_age_ns.load(std::memory_order_relaxed);
        while (age_ns > previous &&
               !max_age_ns.compare_exchange_weak(
                   previous, age_ns, std::memory_order_relaxed)) {
        }
    }

    void post_mouse_event(NSEventType type, NSInteger window_number,
                          int sequence, NSPoint location) {
        @autoreleasepool {
            NSEvent* event = [NSEvent mouseEventWithType:type
                                               location:location
                                          modifierFlags:kProbeModifiers
                                              timestamp:NSProcessInfo.processInfo.systemUptime
                                           windowNumber:window_number
                                                context:nil
                                            eventNumber:sequence
                                             clickCount:0
                                               pressure:0.0];
            [NSApp postEvent:event atStart:NO];
        }
    }

    void start_producer(NSInteger window_number) {
        std::thread([this, window_number] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            post_mouse_event(NSEventTypeLeftMouseDown, window_number, -1,
                             NSMakePoint(kStartX, 100));
            for (int i = 0; i < kMoveCount; ++i) {
                posted_ns[static_cast<std::size_t>(i)].store(
                    now_ns(), std::memory_order_release);
                post_mouse_event(NSEventTypeLeftMouseDragged, window_number, i,
                                 NSMakePoint(kStartX + i, 100));
                std::this_thread::sleep_for(kPostInterval);
            }
            mouse_up_posted_ns.store(now_ns(), std::memory_order_release);
            post_mouse_event(NSEventTypeLeftMouseUp, window_number, kMoveCount,
                             NSMakePoint(kStartX + kMoveCount - 1, 100));
        }).detach();
    }

    void report_and_quit() {
        if (reported.exchange(true)) return;
        const int tagged = tagged_moves_received.load();
        const int missing = std::max(0, kMoveCount - tagged);
        const int duplicates = std::max(0, tagged - kMoveCount);
        const auto up_age_ns =
            now_ns() - mouse_up_posted_ns.load(std::memory_order_acquire);
        std::printf(
            "RESULT tagged=%d missing=%d duplicates=%d out_of_order=%d "
            "max_age_ms=%.3f mouse_up_age_ms=%.3f frames=%d "
            "max_frame_depth=%d events_during_frame=%d\n",
            tagged, missing, duplicates, out_of_order.load(),
            static_cast<double>(max_age_ns.load()) / 1.0e6,
            static_cast<double>(up_age_ns) / 1.0e6, frame_count.load(),
            max_frame_depth.load(), events_during_frame.load());
        std::fflush(stdout);
        sapp_request_quit();
    }

    void configure() {
        ui.html("<div style='width:100%;height:100%'></div>");
        ui.on_event([this](
                        const affineui::Event& event,
                        const std::vector<affineui::Document::HoverInfo>&) {
            if (!(event.shift && event.ctrl && event.alt && event.super)) {
                return false;
            }
            if (frame_depth.load(std::memory_order_acquire) != 0) {
                events_during_frame.fetch_add(1, std::memory_order_relaxed);
            }
            if (event.type == affineui::EventType::MouseMove) {
                const int sequence = tagged_moves_received.fetch_add(1);
                const int previous_x = last_x.exchange(event.pos.x);
                if (previous_x >= event.pos.x) out_of_order.fetch_add(1);
                if (sequence >= 0 && sequence < kMoveCount) {
                    const auto index = static_cast<std::size_t>(sequence);
                    const auto posted =
                        posted_ns[index].load(std::memory_order_acquire);
                    if (posted != 0) record_age(now_ns() - posted);
                }
            } else if (event.type == affineui::EventType::MouseUp) {
                report_and_quit();
            }
            return false;
        });
        ui.on_frame([this](double) {
            const int depth = frame_depth.fetch_add(1) + 1;
            auto previous_max = max_frame_depth.load();
            while (depth > previous_max &&
                   !max_frame_depth.compare_exchange_weak(previous_max,
                                                          depth)) {
            }
            frame_count.fetch_add(1);
            if (!producer_started.load(std::memory_order_acquire)) {
                NSWindow* window = NSApp.keyWindow;
                bool expected = false;
                if (window && producer_started.compare_exchange_strong(
                                  expected, true,
                                  std::memory_order_acq_rel)) {
                    start_producer(window.windowNumber);
                }
            }
            if (depth == 1) {
                // Reproduce a modal/native helper that services a nested run
                // loop during a frame. A serialized scheduler defers the
                // display tick; a direct CADisplayLink callback re-enters.
                if (nested_pump_enabled) {
                    [NSRunLoop.currentRunLoop
                        runMode:NSDefaultRunLoopMode
                        beforeDate:[NSDate dateWithTimeIntervalSinceNow:
                                               std::chrono::duration<double>(
                                                   kNestedPump)
                                                   .count()]];
                }
                std::this_thread::sleep_for(kFrameWork);
                ui.mark_dirty();
            }
            frame_depth.fetch_sub(1);
        });
    }
};

}  // namespace

int main() {
    static Probe probe;
    probe.configure();
    sapp_desc desc{};
    desc.width = kStartX + kMoveCount + 100;
    desc.height = 240;
    desc.window_title = "AffineUI macOS input queue probe";
    desc.high_dpi = true;
    desc.swap_interval = 1;
    affineui::sokol::PerfHudOptions options{};
    options.record_mouse = false;
    affineui::sokol::wire(desc, probe.ui, options);
    sapp_run(&desc);
    return 0;
}
