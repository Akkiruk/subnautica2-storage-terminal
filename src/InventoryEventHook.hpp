#pragma once

#include <functional>

namespace StorageTerminal {

// Wires into UE4SS's own GLOBAL ProcessEvent dispatcher (already hooked once
// by UE4SS core, used successfully by the example EventViewerMod) and filters
// it down to a small set of real UWEInventoryComponent change functions by
// identity, so the mod reacts to real inventory changes instead of polling on
// a fixed timer.
//
// A prior attempt used UFunction::RegisterPostHook, which swaps the target
// function's OWN function pointer via a per-function trampoline. That crashed
// the game hard the very first time it actually fired (see memory:
// feedback_ue4ss_reflection_bugs). This approach never touches any target
// function's own dispatch at all -- it only adds a read-only filter to the
// same shared ProcessEvent stream every reflected call already goes through,
// which is the pattern the known-working EventViewerMod example uses for
// exactly this kind of "notice when X is called" observation.
//
// The hook used to also parse each add/remove event's payload and hand it to
// a second callback, for the withdraw/deposit redirect in the abandoned
// mirror-inventory design. That consumer was deleted on 2026-07-28 and the
// parsing went with it. What is left is the only thing the mod ever needed: a
// "something moved" bit.
class InventoryEventHook {
public:
    // Registers on_changed to fire whenever any UWEInventoryComponent
    // instance registers, or has an item added to or removed from it.
    // on_changed runs inside the ProcessEvent post-dispatch: do cheap
    // bookkeeping only (set a flag); never call game functions from it.
    //
    // Returns true only if the hook was actually installed. A false return
    // means no target function could be resolved yet (typically because no
    // UWEInventoryComponent exists in the world at this instant) and the
    // caller must try again on a later tick -- treating a failed attempt as
    // final left the snapshot frozen for the whole session with no error.
    bool try_install(const std::function<void()>& on_changed);

    [[nodiscard]] bool is_installed() const
    {
        return m_installed;
    }

private:
    bool m_installed = false;
};

}
