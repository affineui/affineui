// State store for use_state<T> / use_effect.
//
// Keyed by the call-site path hash. Each slot owns a destructor and
// raw bytes for T. On reconciliation, slots whose owning nodes
// disappeared for at least one full pass get their destructors invoked
// and storage reclaimed (React-style unmount cleanup).
//
// Threading: only the bound document's frame thread touches the store
// during a reconciliation pass; mutations from event handlers are
// queued and applied at pass boundaries.

namespace affineui::imm {

void state_store_stub() {
    // intentional no-op
}

}  // namespace affineui::imm
