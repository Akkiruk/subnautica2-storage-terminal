#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include <Unreal/UObject.hpp>

namespace StorageTerminal {

// Adds a "Storage Network" option to every NoA computer terminal's dialogue
// menu, and reports when the player picks it.
//
// This is the mod's ONLY entry point. There is deliberately no hotkey: the
// base's storage index should be something you walk up to a terminal and ask
// NoA for, not something available from anywhere in the world.
//
// How the option gets there: the terminal's root menu is built from
// UWEComputerTextInterfaceComponent's DefaultRootDialogueData +
// ExtraRootDialogueData. The named API for this, AddRootDialogueOption(), was
// confirmed in-game NOT to do the job -- it left ExtraRootDialogueData
// untouched and only added an entry to MergedRootDialogueData that never
// appeared on screen. So the option is appended straight to
// ExtraRootDialogueData through the array's own real growth logic. See
// docs/UE4SS_API_REFERENCE.md.
//
// Click detection observes the shared ProcessEvent stream and compares the
// clicked data object against the ones this class created, by pointer. The
// callback does nothing but store a pointer and set a flag: acting on a click
// from inside the dispatch that delivered it is how this project produced four
// separate crash/corruption incidents.
//
// THREADING: the ProcessEvent callback runs on the game thread, while
// refresh_terminals()/consume_open_request() run on UE4SS's own mod-update
// thread. So the set of "our" data objects the hook compares against is a
// fixed array of atomics, not the bookkeeping vector -- the hook never touches
// anything that can be reallocated underneath it.
class NoaTerminal {
public:
    static constexpr size_t kMaxTerminals = 32;

    // Scans for terminals that do not yet carry our option and installs it.
    // Cheap and idempotent; call on the mod's normal check cadence so
    // newly-built terminals pick it up. Returns how many terminals the option
    // was newly added to.
    int32_t refresh_terminals();

    // Installs the click watcher. Returns false if no terminal exists yet, in
    // which case the caller should try again later -- treating one failed
    // attempt as final is how the inventory hook silently disabled itself for
    // a whole session.
    bool try_install_click_watch();

    [[nodiscard]] bool is_click_watch_installed() const
    {
        return m_clickWatchInstalled;
    }

    [[nodiscard]] int32_t terminal_count() const
    {
        return static_cast<int32_t>(m_options.size());
    }

    // True once, if the player picked our option since the last call. Consume
    // this from the mod's update loop, never from inside a hook.
    bool consume_open_request();

    // Closes the NoA dialogue UI on the terminal whose option was picked, so
    // the storage screen is not fighting it for the Modal layer. Call from the
    // update loop, after consume_open_request.
    void close_terminal_ui();

private:
    struct TerminalOption {
        RC::Unreal::UObject* component = nullptr; // the CTI component
        RC::Unreal::UObject* data = nullptr;      // our dialogue data object
    };

    // Bookkeeping, touched only on the mod-update thread.
    std::vector<TerminalOption> m_options;
    bool m_clickWatchInstalled = false;

    // Read by the hook on the game thread; written here. Pointer identity
    // only -- never dereferenced by the hook.
    std::array<std::atomic<RC::Unreal::UObject*>, kMaxTerminals> m_optionSlots{};

    std::atomic<RC::Unreal::UObject*> m_pendingComponent{nullptr};
    std::atomic<bool> m_openRequested{false};

    RC::Unreal::UObject* create_option_for(RC::Unreal::UObject* component);

    // Writes the option's label/response and verifies it by reading back.
    // Re-run on every rescan: UE4SS copies FText without taking a reference,
    // so a mod-written FText left sitting in a game field can come back empty
    // (observed in game as a blank dialogue button).
    bool ensure_option_text(RC::Unreal::UObject* data);
};

}
