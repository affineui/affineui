#pragma once

// Versioned weak references for AffineUI.
//
// WHY THIS EXISTS
// ---------------
// A core, non-negotiable invariant of AffineUI is that it must be *hard to
// crash* from C++. A retained-mode UI keeps callbacks and references that
// outlive the stack frame that created them, so the classic crash is a widget
// event (or a deferred task, or another object) reaching for an object that
// has already been destroyed — a dangling pointer / dangling `this`. We refuse
// to let that be a crash. Instead, a reference to a dead object must resolve to
// null and the operation must quietly do nothing.
//
// HOW, AND WHY THIS SHAPE
// -----------------------
// We use the game-engine "handle" pattern: a small versioned slot table. A
// live object owns one slot; a weak reference is just `{slot index,
// generation}`. When the object dies it frees its slot and the generation is
// bumped, so every outstanding weak reference to that slot fails to resolve —
// safely, never dereferencing freed memory, and never confusing a recycled
// slot for the original object (that is exactly what the generation counter
// defeats).
//
// We chose this over, say, shared_ptr/weak_ptr control blocks because:
//   - It is cheap to retrofit: a participating object stores ONE 32-bit field
//     (its slot index) and frees it on destruction — nothing more. Existing
//     code does not have to convert ownership to shared_ptr.
//   - The lookup table is plain integers and raw pointers, so *the tracking
//     machinery itself cannot dangle* — the one thing that absolutely must not.
//   - It is the same mechanism the DOM layer already uses for nodes
//     (DomHandle): one stability idiom across the codebase, not two.
//
// This file provides that mechanism for arbitrary C++ objects so they can be
// the safe targets of UI callbacks (see callback.h, which layers Qt-style
// (object, method) callbacks on top).
//
// Opting in. Conformance is a structural concept (`WeaklyTrackable`) — a type
// just needs the right shape: expose an embedded WeakSlot via aui_weak_slot().
// There are two routes to that shape:
//
//   // Sensible default for AffineUI's own classes — inherit the base.
//   struct Editor : affineui::Trackable {
//       void save();
//   };
//
//   // Retrofit for an existing/foreign class — one macro line, no base class.
//   struct Existing {
//       void save();
//       AFFINEUI_WEAK_TRACKABLE();
//   };
//
// Either way:
//
//   affineui::WeakRef<Editor> ref = affineui::to_weak_ref(&editor);
//   if (Editor* e = ref.get()) e->save();        // null once editor dies
//
// Threading: the registry is single-threaded (UI thread), like the rest of
// AffineUI's UI layer. Create, resolve, and destroy trackable objects on it.

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace affineui {

namespace detail {

/// One entry in the process-wide weak-object table. `payload` is the live
/// object pointer (null when free); `generation` invalidates stale references
/// when a slot is reused.
struct WeakObjectSlot {
    void*         payload{nullptr};
    std::uint32_t generation{1};
};

/// Process-wide, single-threaded versioned slot table backing object weak
/// references. Slot 0 is reserved as "null", so a default WeakRef never
/// resolves. Slots are recycled; each reuse bumps the generation.
class WeakRegistry {
public:
    /// Acquire a fresh slot for `payload`. Returns the 1-based slot index.
    std::uint32_t acquire(void* payload) {
        // Reuse a free slot if one exists.
        for (std::size_t i = 1; i < slots_.size(); ++i) {
            if (slots_[i].payload == nullptr) {
                slots_[i].payload = payload;
                return static_cast<std::uint32_t>(i);
            }
        }
        if (slots_.empty()) slots_.push_back({});  // reserve index 0 as null
        slots_.push_back(WeakObjectSlot{payload, 1});
        return static_cast<std::uint32_t>(slots_.size() - 1);
    }

    /// Release `slot`, invalidating every weak reference that named it.
    void release(std::uint32_t slot) noexcept {
        if (slot == 0 || slot >= slots_.size()) return;
        slots_[slot].payload = nullptr;
        ++slots_[slot].generation;
        if (slots_[slot].generation == 0) slots_[slot].generation = 1;
    }

    /// Current generation of `slot`, or 0 for an invalid index.
    [[nodiscard]] std::uint32_t generation(std::uint32_t slot) const noexcept {
        if (slot == 0 || slot >= slots_.size()) return 0;
        return slots_[slot].generation;
    }

    /// The live payload for `slot` if `generation` still matches, else null.
    [[nodiscard]] void* resolve(std::uint32_t slot,
                                std::uint32_t generation) const noexcept {
        if (slot == 0 || slot >= slots_.size()) return nullptr;
        const auto& s = slots_[slot];
        if (s.generation != generation) return nullptr;
        return s.payload;
    }

private:
    std::vector<WeakObjectSlot> slots_;
};

/// The single process-wide registry. Defined inline so the header is
/// self-contained across translation units (C++17 inline variable).
inline WeakRegistry& weak_registry() {
    static WeakRegistry registry;
    return registry;
}

}  // namespace detail

/// RAII slot owner embedded as a member of any object that wants to be weakly
/// trackable. It acquires a registry slot for its owner on construction and
/// releases it on destruction — satisfying the invariant that a compatible
/// object stores its handle and kills it when it dies.
///
/// Non-copyable and non-movable: a weak reference resolves to the owner's
/// address, so that address must be stable for the owner's lifetime.
class WeakSlot {
public:
    template <typename T>
    explicit WeakSlot(T* owner)
        : slot_(detail::weak_registry().acquire(static_cast<void*>(owner))) {}

    WeakSlot(const WeakSlot&)            = delete;
    WeakSlot& operator=(const WeakSlot&) = delete;
    WeakSlot(WeakSlot&&)                 = delete;
    WeakSlot& operator=(WeakSlot&&)      = delete;

    ~WeakSlot() { detail::weak_registry().release(slot_); }

    [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
    [[nodiscard]] std::uint32_t generation() const noexcept {
        return detail::weak_registry().generation(slot_);
    }

private:
    std::uint32_t slot_{0};
};

/// Concept: a type is weakly trackable when it exposes its embedded WeakSlot
/// through `aui_weak_slot()`. No base class is mandated — any type that embeds
/// a WeakSlot member and returns it qualifies (it just has the right shape).
///
/// The mechanism (versioned slots) is what delivers crash prevention and
/// stability; this *concept* exists for ergonomics and easy retrofitting — it
/// lets `bind` / `to_weak_ref` accept anything of the right shape, whether it
/// reached that shape by inheriting Trackable (our default) or by dropping in
/// AFFINEUI_WEAK_TRACKABLE() (an existing class that can't change its base).
template <typename T>
concept WeaklyTrackable = requires(const T& t) {
    { t.aui_weak_slot() } -> std::same_as<const WeakSlot&>;
};

/// One-line opt-in for the weak-reference system, designed for retrofitting
/// existing classes: drop `AFFINEUI_WEAK_TRACKABLE()` into the class body and
/// it satisfies WeaklyTrackable. It adds exactly one 32-bit field (inside the
/// WeakSlot) plus the accessor the concept checks for.
///
/// No user-declared destructor is required: the embedded WeakSlot frees its
/// registry slot in its own destructor, which the compiler runs as part of the
/// class's destruction. (If the class is used polymorphically, give it a
/// virtual destructor as usual — the slot is still released correctly.)
///
///   class Existing {
///       // ... existing members ...
///       AFFINEUI_WEAK_TRACKABLE();
///   };
#define AFFINEUI_WEAK_TRACKABLE()                                       \
    ::affineui::WeakSlot aui_weak_slot_{this};                         \
    const ::affineui::WeakSlot& aui_weak_slot() const noexcept {       \
        return aui_weak_slot_;                                         \
    }

/// Base class that opts a type into the weak-reference system. For AffineUI's
/// own classes inheriting Trackable is the sensible default; the
/// AFFINEUI_WEAK_TRACKABLE() macro exists for retrofitting existing/foreign
/// classes that cannot change their inheritance. Both routes satisfy the same
/// WeaklyTrackable concept, so `bind` / `to_weak_ref` accept either uniformly.
///
/// When to use it: only objects that genuinely need to be *safe targets of
/// references that outlive a stack frame* — typically the long-lived
/// controllers/models that receive UI callbacks. You do NOT have to make every
/// object trackable. Plain value types, transient structs, and data you pass
/// by value or const& should stay plain: copies and value semantics are the
/// preferred default, and tracking adds a slot per instance for no benefit if
/// nothing ever holds a weak reference to it.
///
/// Trackable has a virtual destructor (objects of derived type are commonly
/// held and destroyed polymorphically) and is neither copyable nor movable: a
/// weak reference resolves to the object's address, which must stay stable for
/// its lifetime.
class Trackable {
public:
    Trackable() = default;

    Trackable(const Trackable&)            = delete;
    Trackable& operator=(const Trackable&) = delete;
    Trackable(Trackable&&)                 = delete;
    Trackable& operator=(Trackable&&)      = delete;

    virtual ~Trackable() = default;

    [[nodiscard]] const WeakSlot& aui_weak_slot() const noexcept {
        return aui_weak_slot_;
    }

private:
    WeakSlot aui_weak_slot_{this};
};

/// A typed, copyable weak reference: `{slot, generation}`. Resolves to the live
/// object via the registry, or null once the object has been destroyed (or the
/// slot recycled). Default-constructed it never resolves.
template <typename T>
class WeakRef {
public:
    WeakRef() = default;
    WeakRef(std::uint32_t slot, std::uint32_t generation) noexcept
        : slot_(slot), generation_(generation) {}

    /// A non-owning pointer to the live object, or null if it has been
    /// destroyed. This is a borrowed snapshot: it does not retain, pin, or
    /// synchronize access to the object. Resolve again after any re-entrant
    /// operation that could destroy the object, and do not store the returned
    /// pointer beyond a lifetime the caller independently knows is safe.
    [[nodiscard]] T* get() const noexcept {
        return static_cast<T*>(
            detail::weak_registry().resolve(slot_, generation_));
    }

    /// Compatibility alias for get(). Despite its historical name, this does
    /// not lock or extend the object's lifetime.
    [[deprecated("WeakRef::lock() does not extend lifetime; use get() for a non-owning pointer")]]
    [[nodiscard]] T* lock() const noexcept {
        return get();
    }

    /// True while the referenced object is still alive.
    [[nodiscard]] bool alive() const noexcept { return get() != nullptr; }

    /// True when this reference was ever bound to an object (vs default).
    [[nodiscard]] bool bound() const noexcept { return slot_ != 0; }

private:
    std::uint32_t slot_{0};
    std::uint32_t generation_{0};
};

/// Obtain a weak reference to a trackable object from a bare pointer. Returns
/// an empty (never-resolving) reference for null.
template <WeaklyTrackable T>
[[nodiscard]] WeakRef<T> to_weak_ref(T* obj) noexcept {
    if (obj == nullptr) return WeakRef<T>{};
    const WeakSlot& s = obj->aui_weak_slot();
    return WeakRef<T>(s.slot(), s.generation());
}

}  // namespace affineui
