#include <doctest/doctest.h>

#include "affineui/callback.h"
#include "affineui/weak_ref.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using affineui::Callback;
using affineui::ChangeCallback;
using affineui::ClickCallback;
using affineui::Trackable;
using affineui::WeaklyTrackable;
using affineui::WeakRef;
using affineui::WeakSlot;

namespace {

// Sensible default for our own classes: inherit Trackable.
struct Counter : Trackable {
    int clicks{0};
    std::string last_value;

    void click() { ++clicks; }
    void change(std::string_view value) { last_value = std::string(value); }
    void click_const() const {}  // exercises the const-method bind overload
};

static_assert(WeaklyTrackable<Counter>,
              "a Trackable-derived type should satisfy the concept");

// Retrofit path: an existing class with no base class opts in via the macro.
struct Retrofitted {
    int pings{0};
    void ping() { ++pings; }
    AFFINEUI_WEAK_TRACKABLE();
};

static_assert(WeaklyTrackable<Retrofitted>,
              "AFFINEUI_WEAK_TRACKABLE() should satisfy the concept");

struct LeadingPolymorphicBase {
    virtual ~LeadingPolymorphicBase() = default;
    int prefix{17};
};

struct OffsetTrackable : LeadingPolymorphicBase, Trackable {
    int value{23};
};

static_assert(WeaklyTrackable<OffsetTrackable>);
static_assert(!std::is_constructible_v<WeakRef<int>, std::uint32_t,
                                       std::uint32_t>);
static_assert(!std::is_constructible_v<WeakRef<int>,
                                       affineui::detail::WeakRegistry*,
                                       std::uint32_t, std::uint32_t>);

}  // namespace

TEST_CASE("bound member callback invokes the target while it is alive") {
    Counter counter;
    ClickCallback cb = affineui::bind(&counter, &Counter::click);
    CHECK(static_cast<bool>(cb));
    cb();
    cb();
    CHECK(counter.clicks == 2);
}

TEST_CASE("bound member callback forwards arguments") {
    Counter counter;
    ChangeCallback cb = affineui::bind(&counter, &Counter::change);
    cb("hello");
    CHECK(counter.last_value == "hello");
}

TEST_CASE("invoking a callback after its target dies is a safe no-op") {
    // This is the core hard-to-crash invariant: a widget firing into a handler
    // whose owner has been destroyed must not crash and must not run.
    std::optional<ClickCallback> cb;
    {
        Counter counter;
        cb = affineui::bind(&counter, &Counter::click);
        (*cb)();
        CHECK(counter.clicks == 1);
    }
    // counter is gone. The callback must quietly do nothing.
    (*cb)();  // must not crash / must not touch freed memory
    CHECK(static_cast<bool>(*cb));  // still "non-empty", just inert
    CHECK_MESSAGE(true, "no crash invoking callback after target destroyed");
}

TEST_CASE("the std::function conversion keeps honouring the liveness guard") {
    // The View stores handlers as std::function, so the guard must survive the
    // conversion that happens when a Callback is dropped into that slot.
    std::function<void()> fn;
    auto counter = std::make_unique<Counter>();
    fn = affineui::bind(counter.get(), &Counter::click);  // implicit -> Fn
    fn();
    CHECK(counter->clicks == 1);

    counter.reset();  // destroy the target
    fn();             // converted std::function must also no-op safely
    CHECK_MESSAGE(true, "no crash after target destroyed via std::function path");
}

TEST_CASE("change-shaped std::function conversion guards correctly") {
    std::function<void(std::string_view)> fn;
    auto counter = std::make_unique<Counter>();
    fn = affineui::bind(counter.get(), &Counter::change);
    fn("alive");
    CHECK(counter->last_value == "alive");
    counter.reset();
    fn("dead");  // no-op, no crash
    CHECK_MESSAGE(true, "change callback safe after destruction");
}

TEST_CASE("free lambda callbacks run with no liveness guard") {
    int hits = 0;
    ClickCallback cb = [&hits] { ++hits; };
    cb();
    cb();
    CHECK(hits == 2);

    // A lambda-built callback reports unbound, so its std::function conversion
    // is returned unwrapped (no per-call guard overhead).
    std::function<void()> fn = cb;
    fn();
    CHECK(hits == 3);
}

TEST_CASE("default-constructed callback is empty and safe to invoke") {
    ClickCallback cb;
    CHECK_FALSE(static_cast<bool>(cb));
    cb();  // no-op
    ChangeCallback cc;
    cc("x");  // no-op
    CHECK_MESSAGE(true, "empty callbacks are inert");
}

TEST_CASE("binding a null object pointer yields an empty callback") {
    Counter* none = nullptr;
    ClickCallback cb = affineui::bind(none, &Counter::click);
    CHECK_FALSE(static_cast<bool>(cb));
    cb();  // no-op
}

TEST_CASE("const member methods can be bound") {
    Counter counter;
    ClickCallback cb = affineui::bind(&counter, &Counter::click_const);
    cb();  // compiles and runs via the const overload
    CHECK_MESSAGE(true, "const-method bind overload selected");
}

TEST_CASE("WeakRef reflects target lifetime") {
    WeakRef<Counter> ref;
    Counter* borrowed = nullptr;
    {
        Counter counter;
        ref = affineui::to_weak_ref(&counter);
        CHECK(ref.bound());
        CHECK(ref.alive());
        borrowed = ref.get();
        CHECK(borrowed == &counter);
    }
    // get() returns a non-owning borrow; it never pins the target. Do not
    // dereference `borrowed` here. Resolving the WeakRef again is safe/null.
    CHECK(ref.bound());
    CHECK_FALSE(ref.alive());     // expired after destruction
    CHECK(ref.get() == nullptr);  // resolves to null, never dangling
}

TEST_CASE("a default WeakRef is unbound and never resolves") {
    WeakRef<Counter> ref;
    CHECK_FALSE(ref.bound());
    CHECK_FALSE(ref.alive());
    CHECK(ref.get() == nullptr);
}

TEST_CASE("to_weak_ref on a null pointer yields an empty reference") {
    Counter* none = nullptr;
    WeakRef<Counter> ref = affineui::to_weak_ref(none);
    CHECK_FALSE(ref.bound());
    CHECK(ref.get() == nullptr);
}

TEST_CASE("a recycled registry slot invalidates the old WeakRef") {
    // The versioned slot is the crash-prevention core: when an object dies and
    // its slot is later reused by a different object, the stale reference must
    // not resolve to the new occupant.
    WeakRef<Counter> stale;
    {
        Counter first;
        stale = affineui::to_weak_ref(&first);
        CHECK(stale.get() == &first);
    }
    // `first` is gone; its slot is free. A new Counter may take that slot.
    Counter second;
    WeakRef<Counter> fresh = affineui::to_weak_ref(&second);
    CHECK(fresh.alive());
    CHECK_FALSE(stale.alive());  // generation bump defeats slot reuse
    CHECK(stale.get() != &second);
}

TEST_CASE("WeakRef resolves through the registry that issued its slot") {
    // Python wheels load _affineui and example sidecars as separate extension
    // modules. Both may statically link AffineUI, so the same numeric slot can
    // exist in two registries. A reference must carry its issuing table.
    affineui::detail::WeakRegistry first_registry;
    affineui::detail::WeakRegistry second_registry;
    int first = 1;
    int second = 2;

    const auto first_slot = first_registry.acquire(&first);
    const auto second_slot = second_registry.acquire(&second);
    REQUIRE(first_slot == second_slot);

    auto first_ref = affineui::detail::WeakRefFactory::from_slot<int>(
        &first_registry, first_slot, first_registry.generation(first_slot));
    auto second_ref = affineui::detail::WeakRefFactory::from_slot<int>(
        &second_registry, second_slot,
        second_registry.generation(second_slot));
    CHECK(first_ref.get() == &first);
    CHECK(second_ref.get() == &second);

    first_registry.release(first_slot);
    CHECK(first_ref.get() == nullptr);
    CHECK(second_ref.get() == &second);
    second_registry.release(second_slot);
    CHECK(second_ref.get() == nullptr);
}

TEST_CASE("WeakRef adjusts a non-first Trackable base to the derived object") {
    WeakRef<OffsetTrackable> ref;
    {
        OffsetTrackable target;
        auto* base = static_cast<Trackable*>(&target);
        REQUIRE(static_cast<void*>(base) != static_cast<void*>(&target));

        ref = affineui::to_weak_ref(&target);
        REQUIRE(ref.get() == &target);
        CHECK(ref.get()->prefix == 17);
        CHECK(ref.get()->value == 23);
    }
    CHECK(ref.get() == nullptr);
}

TEST_CASE("copying a bound callback shares the same liveness guard") {
    std::optional<ClickCallback> original;
    ClickCallback copy;
    {
        Counter counter;
        original = affineui::bind(&counter, &Counter::click);
        copy = *original;  // copy while alive
        copy();
        CHECK(counter.clicks == 1);
    }
    // Both the original and the copy must be inert now.
    (*original)();
    copy();
    CHECK_MESSAGE(true, "copied callback observes the same destruction");
}

TEST_CASE("macro-retrofitted classes bind and guard like base-class ones") {
    std::optional<ClickCallback> cb;
    {
        Retrofitted r;
        cb = affineui::bind(&r, &Retrofitted::ping);
        (*cb)();
        CHECK(r.pings == 1);
    }
    (*cb)();  // target gone — safe no-op via the macro-provided slot
    CHECK_MESSAGE(true, "retrofitted target safe after destruction");
}

TEST_CASE("guard protects arbitrary void and value-returning callbacks") {
    std::function<void()> action;
    std::function<bool(int)> predicate;
    auto counter = std::make_unique<Counter>();
    action = affineui::guard(counter.get(), [](Counter& live) {
        ++live.clicks;
    });
    predicate = affineui::guard(counter.get(), [](Counter&, int value) {
        return value == 7;
    });

    action();
    CHECK(counter->clicks == 1);
    CHECK(predicate(7));

    counter.reset();
    CHECK_NOTHROW(action());
    CHECK_FALSE(predicate(7));
}
