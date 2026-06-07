#pragma once

// Safe, Qt-style callbacks for AffineUI.
//
// A core invariant of AffineUI is that it must be *hard to crash* from C++.
// The most common way a retained-mode UI crashes is a widget event firing
// into a handler whose owning object has already been destroyed — a dangling
// `this`. AffineUI makes that impossible: a callback may be bound to a
// `(object, method)` pair guarded by a versioned weak reference (see
// weak_ref.h). Once the object dies its weak reference stops resolving and
// invoking the callback becomes a safe no-op. The lookup table that performs
// the resolution is plain integers and raw pointers, so it never dangles.
//
// Three interchangeable ways to make a callback:
//
//   struct Editor : affineui::Trackable {   // or AFFINEUI_WEAK_TRACKABLE()
//       void save();
//       void set_zoom(std::string_view value);
//   };
//   Editor editor;
//
//   // 1. Bound member (object, method) — auto-disabled after `editor` dies.
//   view.button("Save").on_click(affineui::bind(&editor, &Editor::save));
//
//   // 2. Bound member taking the change value.
//   view.input("Zoom", "100").on_change(
//       affineui::bind(&editor, &Editor::set_zoom));
//
//   // 3. A free lambda, exactly as before — no lifetime guard, for small
//   //    self-contained handlers (as one would use in Qt).
//   view.button("Quit").on_click([] { /* ... */ });
//
// `Callback<Args...>` converts implicitly to `std::function<void(Args...)>`,
// so it drops into every existing handler slot (`on_click`, `on_change`, …)
// with no change to those signatures. The std::function it produces carries
// the liveness guard with it.
//
// Threading: like the rest of AffineUI's UI layer, callbacks are single-
// threaded. Bind, invoke, and destroy targets on the UI thread.

#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "affineui/weak_ref.h"

namespace affineui {

namespace detail {

/// Type-erased liveness probe carried by a Callback. It stores a WeakRef<T>
/// for some concrete T and reports whether that object is still alive, without
/// the Callback needing to know T. Empty (default) means "always alive", so
/// free lambdas are never gated.
class LivenessGuard {
public:
    LivenessGuard() = default;

    template <typename T>
    explicit LivenessGuard(WeakRef<T> ref)
        : alive_([ref] { return ref.alive(); }), bound_(ref.bound()) {}

    [[nodiscard]] bool alive() const { return !alive_ || alive_(); }
    [[nodiscard]] bool bound() const noexcept { return bound_; }

private:
    std::function<bool()> alive_;
    bool                  bound_{false};
};

}  // namespace detail

/// A callable that optionally carries a liveness guard. Default-constructed it
/// is empty (invoking does nothing). Converts implicitly to
/// std::function<void(Args...)> so it works anywhere a std::function handler is
/// accepted.
template <typename... Args>
class Callback {
public:
    using Fn = std::function<void(Args...)>;

    Callback() = default;

    /// Construct from any callable (lambda, function pointer, std::function).
    /// No liveness guard — intended for small, self-contained handlers.
    template <typename F,
              typename = std::enable_if_t<
                  std::is_invocable_v<F&, Args...> &&
                  !std::is_same_v<std::decay_t<F>, Callback>>>
    Callback(F&& fn)  // NOLINT(google-explicit-constructor)
        : fn_(std::forward<F>(fn)) {}

    /// Construct with an explicit liveness guard plus callable. The callable is
    /// invoked only while the guard reports alive.
    Callback(detail::LivenessGuard guard, Fn fn)
        : guard_(std::move(guard)), fn_(std::move(fn)) {}

    /// Invoke if non-empty and (if guarded) the target is still alive.
    void operator()(Args... args) const {
        if (!fn_ || !guard_.alive()) return;
        fn_(std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(fn_);
    }

    /// Convert to a std::function that still honours the liveness guard, so it
    /// can be stored in existing std::function-based handler slots. An unbound
    /// callback returns its function unwrapped (no per-call guard cost).
    operator Fn() const {  // NOLINT(google-explicit-constructor)
        if (!fn_) return Fn{};
        if (!guard_.bound()) return fn_;
        detail::LivenessGuard guard = guard_;
        Fn fn = fn_;
        return [guard, fn](Args... args) {
            if (guard.alive()) fn(std::forward<Args>(args)...);
        };
    }

private:
    detail::LivenessGuard guard_;
    Fn                    fn_;
};

/// Bind a (object, method) pair into a safe Callback. The returned callback
/// no-ops after `obj` is destroyed. `T` must satisfy WeaklyTrackable.
template <WeaklyTrackable T, typename R, typename... Args>
Callback<Args...> bind(T* obj, R (T::*method)(Args...)) {
    if (obj == nullptr) return Callback<Args...>{};
    detail::LivenessGuard guard{to_weak_ref(obj)};
    return Callback<Args...>(std::move(guard),
                             [obj, method](Args... args) {
                                 (obj->*method)(std::forward<Args>(args)...);
                             });
}

/// const-method overload.
template <WeaklyTrackable T, typename R, typename... Args>
Callback<Args...> bind(T* obj, R (T::*method)(Args...) const) {
    if (obj == nullptr) return Callback<Args...>{};
    detail::LivenessGuard guard{to_weak_ref(obj)};
    return Callback<Args...>(std::move(guard),
                             [obj, method](Args... args) {
                                 (obj->*method)(std::forward<Args>(args)...);
                             });
}

/// Bind a (object, method) pair together with the leading argument(s),
/// producing a zero-argument Callback. Useful for per-item handlers that need
/// to remember which item they act on — e.g. a list row's click that selects a
/// particular id: `row.on_click(bind(this, &T::select, id))`. The bound
/// arguments are captured by value; the callback still no-ops after `obj` dies.
///
/// Constrained to at least one bound argument so it never competes with the
/// plain `bind(obj, method)` overload above.
template <WeaklyTrackable T, typename R, typename... MArgs, typename... Bound,
          typename = std::enable_if_t<(sizeof...(Bound) > 0)>>
Callback<> bind(T* obj, R (T::*method)(MArgs...), Bound... bound) {
    static_assert(sizeof...(Bound) == sizeof...(MArgs),
                  "bind with arguments must supply exactly the method's args");
    if (obj == nullptr) return Callback<>{};
    detail::LivenessGuard guard{to_weak_ref(obj)};
    return Callback<>(std::move(guard),
                      [obj, method, bound...]() {
                          (obj->*method)(bound...);
                      });
}

/// const-method overload of the bound-arguments form.
template <WeaklyTrackable T, typename R, typename... MArgs, typename... Bound,
          typename = std::enable_if_t<(sizeof...(Bound) > 0)>>
Callback<> bind(T* obj, R (T::*method)(MArgs...) const, Bound... bound) {
    static_assert(sizeof...(Bound) == sizeof...(MArgs),
                  "bind with arguments must supply exactly the method's args");
    if (obj == nullptr) return Callback<>{};
    detail::LivenessGuard guard{to_weak_ref(obj)};
    return Callback<>(std::move(guard),
                      [obj, method, bound...]() {
                          (obj->*method)(bound...);
                      });
}

/// Convenience aliases for the two handler shapes AffineUI uses today.
using ClickCallback  = Callback<>;
using ChangeCallback = Callback<std::string_view>;

}  // namespace affineui
