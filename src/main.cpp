// Subnautica 2 -- Storage Network (search & navigate)
//
// ARCHITECTURE (Option B, adopted 2026-07-28 after A and C were tested to
// destruction -- see docs/archive/ARCHITECTURE_OPTIONS.md):
//
//   The mod READS game state and OPENS the game's own storage screens.
//   It never writes game state, never holds items, never creates inventories.
//
// F5 opens a REAL locker's native screen -- the same one the player sees
// walking up to it. Typing searches every communal locker in the base at
// once (plus the player's own pockets), the screen reports the matches by
// locker NAME and distance, and Page Up/Down switch the screen to whichever
// locker holds what they want. Taking items out is then just the game's own
// storage screen, which already does that perfectly.
//
// Why not a merged grid: every design that gave the mod custody of items
// failed in the save. Copies shared the originals' ItemId GUIDs, corrupting
// id-keyed operations and duplicating items; a synthetic inventory poisoned
// the game's inventory-id allocator; and the screen's viewmodel
// authoritatively rebuilds from its bound inventory, so injected entries
// never survived (measured: 35 pushed, 17 alive a second later, and the
// widget's own content list never rose above 0).
//
// The only non-read call is InteractWithInventoryInteractionComponent --
// exactly what pressing interact on a locker does, and only when the
// component's own InventoryInteractionEnabled gate says it is allowed -- so
// this is multiplayer-safe and cannot corrupt a save.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cwctype>
#include <string>
#include <vector>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "HookTargets.hpp"
#include "InventoryEventHook.hpp"
#include "NoaTerminal.hpp"
#include "PropertyReflection.hpp"
#include "ReflectionUtils.hpp"
#include "SnapshotBuilder.hpp"
#include "SnapshotModel.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace {

struct WorldPoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Unreal world units are centimetres.
constexpr double kUnitsPerMetre = 100.0;

std::wstring format_metres(double units)
{
    const auto metres = static_cast<int32_t>(units / kUnitsPerMetre + 0.5);
    return std::to_wstring(metres) + L"m";
}

// FNV-1a over a string, used only to detect "this log line is identical to the
// one I already wrote" and skip it. Verbose-logging the same content on a
// repeating cadence is itself a real IO cost over a long session -- a previous
// build had this suppression, and it was lost somewhere along the way.
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnv1a(std::wstring_view text, uint64_t seed = kFnvOffsetBasis)
{
    uint64_t hash = seed;
    for (const wchar_t ch : text) {
        hash ^= static_cast<uint64_t>(static_cast<uint16_t>(ch));
        hash *= kFnvPrime;
    }
    return hash;
}

}

class StorageTerminalMod final : public CppUserModBase {
    bool m_bootLogged = false;
    bool m_worldReadyLogged = false;
    uint32_t m_ticksSinceCheck = 0;
    // Rate limits. The inventory hook fires constantly (it observes a
    // subsystem-wide broadcast), so an unthrottled rebuild ran several times
    // a second, and each one scans every object in the world. Combined with
    // auto-advance -- which opens a locker, which fires more events -- that
    // became a self-feeding loop that hung the game with no crash dump
    // (2026-07-28).
    std::chrono::steady_clock::time_point m_lastBrowse{};
    uint32_t m_checksSinceRebuild = 0;
    uint32_t m_checksSinceOpen = 0;
    // MEASURED 2026-07-29 from UE4SS.log: two terminal rescans 20 checks apart
    // were 3.51s apart, so one check is ~0.176s and on_update runs ~170x/sec --
    // NOT the ~60/sec these constants were originally written against. Every
    // "~Ns" comment in this file was therefore about 3x optimistic, which means
    // these two throttles -- the guards against the self-feeding rebuild loop
    // that hung the game in 2026-07-28 -- were only ~0.7s, not the 2s intended.
    // Sized to the measured rate now.
    static constexpr uint32_t kMinChecksBetweenRebuilds = 12;  // ~2.1s
    static constexpr uint32_t kQuietChecksAfterOpen = 12;      // ~2.1s
    std::atomic<bool> m_inventoryDirty{true};

    StorageTerminal::SnapshotBuilder m_snapshotBuilder{};
    StorageTerminal::InventoryEventHook m_inventoryEventHook{};
    StorageTerminal::NoaTerminal m_noaTerminal{};
    StorageTerminal::InventorySnapshot m_snapshot{};

    // Opening the storage screen is deferred a couple of checks after NoA's
    // option is picked: the terminal's own dialogue screen is still on the
    // Modal layer at click time, and interact is a confirmed no-op while a
    // screen is up. So we ask NoA to close, then wait for the layer to clear.
    // (Doing this inline from the click hook is precisely the mistake that
    // produced four separate crash/corruption incidents in this project.)
    int32_t m_pendingOpenChecks = -1;
    static constexpr int32_t kOpenAfterCloseChecks = 18; // give up after ~3.2s
    uint32_t m_terminalRescanChecks = 0;
    static constexpr uint32_t kChecksBetweenTerminalScans = 20; // ~3.5s

    // A locker switch in flight. Switching cannot be done inline: the pop and
    // the interact have to be separated by at least one frame, because interact
    // is a confirmed no-op while a screen is still on the Modal layer.
    //
    // Measured from UE4SS.log (2026-07-29): every single browse press logged
    // "interact on inventory N did not put its screen up", eight times in a
    // row, then "screen went away". The pop DID land, a frame or two later,
    // with no screen to replace it -- leaving the game with an empty Modal
    // layer and its input still configured for the screen that was popped.
    // That is the "mouse stops working" report.
    int32_t m_pendingLockerId = -1;
    int32_t m_pendingLockerChecks = 0;
    static constexpr int32_t kSwitchAfterPopChecks = 10; // give up after ~1.8s

    // True while OUR screen is up. Gates the typing capture and the close
    // handler, so keys and Escape behave completely normally otherwise.
    // Always reconciled against the WindowManager before it is trusted --
    // see reconcile_screen_state().
    bool m_screenOpen = false;

    // The modal widget that was on top immediately after we opened a locker.
    // Compared by ADDRESS ONLY, never dereferenced, so a stale pointer is
    // harmless: if the widget on the Modal layer is no longer this one, the
    // player closed or replaced our screen and the mod stands down. Without
    // this, m_screenOpen was a belief that nothing ever checked, and a stale
    // "true" meant the next Escape popped an unrelated modal.
    UObject* m_ownedScreen = nullptr;

    // The grid we last wrote text into, plus the ShowInventoryTitle value it
    // had before we forced it on, so the flag can be put back.
    UObject* m_touchedGrid = nullptr;
    bool m_touchedGridHadTitle = false;

    std::wstring m_query;
    int32_t m_openInventoryId = -1;

    // ---- cached reflection handles --------------------------------------
    //
    // UFunction* and FProperty* are class-level: once resolved they stay valid
    // for the life of the process, so they are cached unconditionally.
    // Re-resolving them per call is exactly what the third lesson in
    // feedback_ue4ss_reflection_bugs warned about, and SnapshotBuilder already
    // honoured it with ResolvedHandles while this file did not.
    //
    // FProperty lookups are additionally memoized inside UE4SS itself
    // (UStruct::FindProperty keeps a {UStruct*, FName} map), so only the
    // FUNCTION handles strictly need caching here -- GetFunctionByNameInChain
    // walks the chain on every call. Both are cached for symmetry.
    //
    // The WindowManager OBJECT is deliberately NOT cached: it is dereferenced
    // via ProcessEvent, so a pointer that outlived its GameInstance would be a
    // crash rather than a stale read. Instead it is resolved once per operation
    // and passed down, which removes the redundant lookups (open_locker did
    // five) without betting on subsystem lifetime.
    UFunction* m_getActiveWidgetFn = nullptr;
    FProperty* m_getActiveLayerParam = nullptr;
    FProperty* m_getActiveReturnParam = nullptr;
    UFunction* m_popFn = nullptr;
    FProperty* m_popWidgetParam = nullptr;
    bool m_screenHandlesResolved = false;
    bool m_loggedScreenHandleFailure = false;

    // Reused params buffer for the reflected calls made from this file. Every
    // call used to heap-allocate a fresh vector.
    std::vector<uint8_t> m_callParams;

    // The player's position, refreshed once per check rather than once per
    // keystroke. Resolving it costs a full object-array scan (player
    // controller lookup) and it is only ever rendered at whole-metre
    // precision, so half a second of staleness is invisible.
    WorldPoint m_playerLocation{};
    bool m_havePlayerLocation = false;

    // Signatures of the last lines actually written, so identical content on a
    // repeating cadence is not re-logged. See fnv1a above.
    uint64_t m_lastSnapshotLogSig = 0;
    uint64_t m_lastSearchLogSig = 0;

    // Latched world-ready state. This used to run a full object-array scan on
    // every check -- twice a second for the whole session -- to answer a
    // question that is true from the first success onward.
    bool m_worldReady = false;
    uint32_t m_worldRecheckChecks = 0;
    static constexpr uint32_t kChecksBetweenWorldRechecks = 120; // ~21s

    // One row per (locker, item type) that matched.
    struct Match {
        int32_t inventory_id = -1;
        std::wstring locker_name;
        std::wstring item_name;
        int32_t count = 0;
        double distance = 0.0;
        bool has_distance = false;
        bool is_player = false;
        bool is_fresh = true;
    };
    std::vector<Match> m_matches;

    // The lockers that currently hold something matching the query, nearest
    // first, plus which one we are looking at. Page Up/Down walk this list;
    // it is rebuilt whenever storage changes, so a locker you empty simply
    // drops out and the view moves on. The player's own inventory is never
    // in here -- you cannot navigate to your own pockets.
    std::vector<Match> m_browse;
    size_t m_browseIndex = 0;

    static constexpr uint32_t kCheckIntervalTicks = 30;
    static constexpr size_t kMaxResultRows = 8;

    static void log(const wchar_t* message)
    {
        Output::send<LogLevel::Verbose>(STR("[StorageTerminal] {}\n"), message);
    }

    static void log_line(const std::wstring& message)
    {
        Output::send<LogLevel::Verbose>(STR("[StorageTerminal] {}\n"), message);
    }

    static std::wstring to_lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    // find_first already excludes the ClientLobby menu world via
    // is_transient, so finding any world at all is the whole test. (There
    // used to be a second ClientLobby name check here that could never fire.)
    //
    // The scan behind this is a full walk of every live UObject, so it is
    // latched: once a world has been seen, re-verify only every ~20s instead of
    // twice a second. A world teardown is still noticed, just not instantly --
    // and nothing here depends on sub-second detection.
    bool is_game_world_ready()
    {
        // Inside the latch window: answer from cache, no scan.
        if (m_worldReady && m_worldRecheckChecks > 0) {
            --m_worldRecheckChecks;
            return true;
        }

        m_worldReady = StorageTerminal::ReflectionUtils::find_first(STR("World")) != nullptr;
        if (m_worldReady) {
            m_worldRecheckChecks = kChecksBetweenWorldRechecks;
        } else {
            // A world went away (or never arrived): drop everything derived
            // from it so nothing stale is trusted when the next one loads, and
            // keep scanning every check until one shows up.
            m_worldRecheckChecks = 0;
            m_havePlayerLocation = false;
        }
        return m_worldReady;
    }

    static UObject* find_player_controller()
    {
        return StorageTerminal::ReflectionUtils::find_first(StorageTerminalTargets::kPlayerControllerClass);
    }

    // Derives the pawn from an ALREADY-RESOLVED controller -- no object scan.
    // Callers that need both should resolve the controller once and use this,
    // rather than calling read_player_pawn() and scanning a second time.
    static UObject* read_pawn_of(UObject* player_controller)
    {
        if (!player_controller) {
            return nullptr;
        }
        auto* pawn_field = StorageTerminal::PropertyReflection::find_property(
            player_controller->GetClassPrivate(), StorageTerminalTargets::kFieldPawn);
        return pawn_field
            ? StorageTerminal::PropertyReflection::read_object(pawn_field, reinterpret_cast<const uint8_t*>(player_controller))
            : nullptr;
    }

    static UObject* read_player_pawn()
    {
        return read_pawn_of(find_player_controller());
    }

    // Refreshes m_playerLocation. Called once per check from on_update, so the
    // search path can read the cached value instead of scanning for the player
    // controller on every keystroke.
    void refresh_player_location()
    {
        auto* pawn = Cast<AActor>(read_player_pawn());
        if (!pawn) {
            m_havePlayerLocation = false;
            return;
        }
        const auto location = pawn->K2_GetActorLocation();
        m_playerLocation = WorldPoint{location.X(), location.Y(), location.Z()};
        m_havePlayerLocation = true;
    }

    // The cached player position, refreshing on first use if a keystroke beat
    // the first check to it.
    bool player_location(WorldPoint& out_point)
    {
        if (!m_havePlayerLocation) {
            refresh_player_location();
        }
        out_point = m_playerLocation;
        return m_havePlayerLocation;
    }

    // ---- search (pure reads over the snapshot) -------------------------

    void rebuild_matches()
    {
        m_matches.clear();
        const auto needle = to_lower(m_query);

        WorldPoint player{};
        const bool have_player = player_location(player);

        for (const auto& source : m_snapshot.sources()) {
            for (const auto& item : source.items) {
                if (item.count <= 0) {
                    continue;
                }
                // Match the player-facing name first; the asset name stays a
                // secondary key so an item whose FText is empty, or a player
                // who knows the internal name, still resolves.
                if (!needle.empty()
                    && to_lower(item.display_name).find(needle) == std::wstring::npos
                    && to_lower(item.asset_name).find(needle) == std::wstring::npos) {
                    continue;
                }

                Match match{};
                match.inventory_id = source.inventory_id;
                match.locker_name = source.display_name;
                match.item_name = item.display_name;
                match.count = item.count;
                match.is_player = source.is_player;
                match.is_fresh = StorageTerminal::InventorySnapshot::is_fresh(source);
                if (have_player && source.has_location && !source.is_player) {
                    const double dx = source.x - player.x;
                    const double dy = source.y - player.y;
                    const double dz = source.z - player.z;
                    match.distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                    match.has_distance = true;
                }
                m_matches.push_back(std::move(match));
            }
        }

        // Nearest first. "Where is it" is the question the player is actually
        // asking, and the nearest locker holding the thing is the one they
        // should walk to -- sorting by quantity (the old order) answered a
        // question nobody asked. Their own pockets sort first of all, because
        // "you already have this" ends the search immediately.
        std::sort(m_matches.begin(), m_matches.end(), [](const Match& a, const Match& b) {
            if (a.is_player != b.is_player) {
                return a.is_player;
            }
            if (a.has_distance != b.has_distance) {
                return a.has_distance;
            }
            if (a.has_distance && a.distance != b.distance) {
                return a.distance < b.distance;
            }
            if (a.count != b.count) {
                return a.count > b.count;
            }
            return a.inventory_id < b.inventory_id;
        });

        rebuild_browse_list();
    }

    // Builds a browse row for a locker, with no particular item attached.
    Match row_for_source(const StorageTerminal::InventorySourceSnapshot& source,
                         const WorldPoint& player,
                         bool have_player) const
    {
        Match row{};
        row.inventory_id = source.inventory_id;
        row.locker_name = source.display_name;
        row.count = 0;
        row.is_player = source.is_player;
        row.is_fresh = StorageTerminal::InventorySnapshot::is_fresh(source);
        if (have_player && source.has_location && !source.is_player) {
            const double dx = source.x - player.x;
            const double dy = source.y - player.y;
            const double dz = source.z - player.z;
            row.distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            row.has_distance = true;
        }
        return row;
    }

    // Collapses matches to ONE row per locker (a locker either has what you
    // searched for or it does not) and keeps the current locker selected
    // across rebuilds where possible.
    void rebuild_browse_list()
    {
        const int32_t previously_open = m_openInventoryId;

        m_browse.clear();

        if (m_query.empty()) {
            // NO SEARCH TERM: browse every locker, INCLUDING EMPTY ONES.
            //
            // This list used to be derived from m_matches, and a match is
            // produced per ITEM -- so a locker with nothing in it produced no
            // matches and was simply unreachable. An empty locker is exactly
            // the one you want to walk to when you are looking for somewhere to
            // put something, so with no query the browse list is every locker
            // the snapshot knows about, nearest first.
            WorldPoint player{};
            const bool have_player = player_location(player);

            for (const auto& source : m_snapshot.sources()) {
                if (source.is_player) {
                    continue; // you are already standing in your own inventory
                }
                m_browse.push_back(row_for_source(source, player, have_player));
            }

            std::sort(m_browse.begin(), m_browse.end(), [](const Match& a, const Match& b) {
                if (a.has_distance != b.has_distance) {
                    return a.has_distance;
                }
                if (a.has_distance && a.distance != b.distance) {
                    return a.distance < b.distance;
                }
                return a.inventory_id < b.inventory_id;
            });
        } else {
            // A SEARCH TERM is active, so only lockers that actually hold it
            // belong here -- an empty locker cannot contain what you searched.
            for (const auto& match : m_matches) {
                if (match.is_player) {
                    continue;
                }
                const bool already = std::any_of(m_browse.begin(), m_browse.end(), [&](const Match& row) {
                    return row.inventory_id == match.inventory_id;
                });
                if (!already) {
                    m_browse.push_back(match);
                }
            }
        }

        // Stay on the locker we are looking at if it still qualifies.
        const auto still_there = std::find_if(m_browse.begin(), m_browse.end(), [&](const Match& row) {
            return row.inventory_id == previously_open;
        });
        if (still_there != m_browse.end()) {
            m_browseIndex = static_cast<size_t>(std::distance(m_browse.begin(), still_there));
        } else if (m_browseIndex >= m_browse.size()) {
            m_browseIndex = 0;
        }
    }

    // Moves to the next/previous locker holding the searched item, wrapping
    // around so browsing never dead-ends.
    void browse(int32_t direction)
    {
        // Nothing is logged before this guard: these keys are bound globally,
        // so logging first meant every arrow tap during normal play wrote a
        // line to UE4SS.log.
        if (!reconcile_screen_state() || m_browse.empty()) {
            return;
        }
        // Key repeat would otherwise pop/open a screen every frame.
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastBrowse < std::chrono::milliseconds(250)) {
            return;
        }
        m_lastBrowse = now;

        const size_t count = m_browse.size();
        m_browseIndex = (m_browseIndex + count + static_cast<size_t>((direction >= 0) ? 1 : -1)) % count;
        log_line(L"Browse " + std::to_wstring(direction) + L": locker "
                 + std::to_wstring(m_browseIndex + 1) + L" of " + std::to_wstring(count) + L".");
        // Deferred: the replacement screen goes up once the pop has landed, and
        // update_screen runs from there. Writing text into the current grid here
        // would be pointless -- it is about to be popped.
        request_locker(m_browse[m_browseIndex].inventory_id);
    }

    // ---- window manager -------------------------------------------------

    // The WindowManager subsystem. One full object-array scan, so callers that
    // need it more than once per operation resolve it here and pass it down.
    static UObject* find_window_manager()
    {
        return UObjectGlobals::FindFirstOf(std::wstring(StorageTerminalTargets::kWindowManagerClass));
    }

    // Resolves the GetActiveWidget/Pop handles once. They are class-level, so
    // any live WindowManager resolves the same ones.
    bool ensure_screen_handles(UObject* window_manager)
    {
        if (m_screenHandlesResolved) {
            return true;
        }
        if (!window_manager) {
            return false; // not resolved yet -- try again next call
        }

        m_getActiveWidgetFn = StorageTerminal::ReflectionUtils::find_function(
            window_manager, StorageTerminalTargets::kGetActiveWidget);
        if (m_getActiveWidgetFn) {
            m_getActiveLayerParam = StorageTerminal::PropertyReflection::find_property(
                m_getActiveWidgetFn, StorageTerminalTargets::kFieldLayerId);
            m_getActiveReturnParam = StorageTerminal::PropertyReflection::find_property(
                m_getActiveWidgetFn, StorageTerminalTargets::kFieldReturnValue);
            if (!m_getActiveLayerParam || !m_getActiveReturnParam) {
                m_getActiveWidgetFn = nullptr;
            }
        }

        m_popFn = StorageTerminal::ReflectionUtils::find_function(
            window_manager, StorageTerminalTargets::kWindowManagerPop);
        if (m_popFn) {
            m_popWidgetParam = StorageTerminal::PropertyReflection::find_property(
                m_popFn, StorageTerminalTargets::kFieldWidget);
            if (!m_popWidgetParam) {
                m_popFn = nullptr;
            }
        }

        // Latch ONLY on success, so a failed attempt is retried rather than
        // disabling screen tracking for the rest of the session -- the exact
        // failure mode the inventory hook and the NoA click watcher both had.
        // The warning is logged once so a real absence is still visible.
        if (!m_getActiveWidgetFn) {
            if (!m_loggedScreenHandleFailure) {
                m_loggedScreenHandleFailure = true;
                log(L"WindowManager::GetActiveWidget not resolved yet; will keep retrying.");
            }
            return false;
        }

        m_screenHandlesResolved = true;
        return true;
    }

    // The widget currently on top of the Modal layer, or null. This is the
    // mod's source of truth for whether its screen is still up.
    //
    // Takes the WindowManager rather than looking it up: open_locker needs this
    // three times and used to pay a separate scan for each.
    UObject* active_modal_widget(UObject* window_manager)
    {
        if (!window_manager || !ensure_screen_handles(window_manager)) {
            return nullptr;
        }

        m_callParams.assign(static_cast<size_t>(m_getActiveWidgetFn->GetPropertiesSize()), 0);
        StorageTerminal::PropertyReflection::write_byte(
            m_getActiveLayerParam, m_callParams.data(), StorageTerminalTargets::kStorageScreenLayer);
        window_manager->ProcessEvent(m_getActiveWidgetFn, m_callParams.data());
        return StorageTerminal::PropertyReflection::read_object(m_getActiveReturnParam, m_callParams.data());
    }

    // Convenience for the callers that only need it once.
    UObject* active_modal_widget()
    {
        return active_modal_widget(find_window_manager());
    }

    // Pops whatever is on the Modal layer WITHOUT touching our open-state
    // bookkeeping. Needed before opening a different locker: calling
    // interact while a storage screen is already up does nothing (the game
    // ignores it), which is why the view never left the first locker.
    void pop_modal_screen(UObject* window_manager)
    {
        auto* active = active_modal_widget(window_manager);
        if (!active || !m_popFn) {
            return;
        }

        // A local buffer rather than m_callParams, which active_modal_widget
        // just used. (`active` is a copied pointer, so reuse would in fact be
        // safe -- but popping is a rare path and not worth the subtlety.)
        std::vector<uint8_t> pop_params(static_cast<size_t>(m_popFn->GetPropertiesSize()), 0);
        StorageTerminal::PropertyReflection::write_object(m_popWidgetParam, pop_params.data(), active);
        window_manager->ProcessEvent(m_popFn, pop_params.data());
    }

    // Brings m_screenOpen back in line with what is actually on screen, and
    // returns whether our screen is genuinely up. Any route that closed or
    // replaced the screen behind our back (the player's own Escape, a game
    // event, another interaction) lands here and the mod quietly stands down
    // instead of acting on a screen it does not own.
    bool reconcile_screen_state()
    {
        if (!m_screenOpen) {
            return false;
        }
        // A locker switch is in flight, so the Modal layer is DELIBERATELY empty
        // between the pop and the replacement screen. Without this the mod read
        // its own pop as "the player closed us" and stood down mid-switch --
        // which is exactly what line 1099 of the 2026-07-29 log was.
        if (m_pendingLockerId >= 0) {
            return true;
        }
        if (active_modal_widget() == m_ownedScreen && m_ownedScreen != nullptr) {
            return true;
        }

        log(L"Storage Network screen went away; standing down.");
        forget_screen();
        return false;
    }

    void forget_screen()
    {
        m_screenOpen = false;
        m_ownedScreen = nullptr;
        m_touchedGrid = nullptr;
        m_query.clear();
        m_openInventoryId = -1;
        m_browse.clear();
        m_browseIndex = 0;

        m_pendingLockerId = -1;
        m_pendingLockerChecks = 0;

        // The cached player position is only refreshed while the screen is up,
        // so drop it here. Otherwise the next open would rank lockers by
        // distance from wherever the player was standing last time.
        m_havePlayerLocation = false;

        // Let the next session log its first result list even if it happens to
        // match the last one written.
        m_lastSearchLogSig = 0;
    }

    // ---- on-screen status ---------------------------------------------

    // Is this the grid bound to `inventory_id`?
    static bool is_grid_for_inventory(UObject* grid, int32_t inventory_id)
    {
        auto* view_model_field = StorageTerminal::PropertyReflection::find_property(
            grid->GetClassPrivate(), StorageTerminalTargets::kFieldViewModel);
        auto* view_model = view_model_field
            ? StorageTerminal::PropertyReflection::read_object(view_model_field, reinterpret_cast<const uint8_t*>(grid))
            : nullptr;
        if (!view_model) {
            return false;
        }
        auto* bound_field = StorageTerminal::PropertyReflection::find_property(
            view_model->GetClassPrivate(), StorageTerminalTargets::kFieldViewModelInventoryComponent);
        auto* bound = bound_field
            ? StorageTerminal::PropertyReflection::read_object(bound_field, reinterpret_cast<const uint8_t*>(view_model))
            : nullptr;
        if (!bound) {
            return false;
        }
        auto* id_field = StorageTerminal::PropertyReflection::find_property(
            bound->GetClassPrivate(), StorageTerminalTargets::kFieldInventoryId);
        return id_field
            && StorageTerminal::PropertyReflection::read_int(
                   id_field, reinterpret_cast<const uint8_t*>(bound)).value_or(-1) == inventory_id;
    }

    // A UMG widget's outer chain runs widget -> WidgetTree -> owning screen,
    // so the screen is reachable with a single real GetTypedOuter call.
    static bool belongs_to_screen(UObject* widget, UObject* screen)
    {
        if (!widget || !screen) {
            return false;
        }
        if (widget == screen) {
            return true;
        }
        auto* screen_class = screen->GetClassPrivate();
        return screen_class && widget->GetTypedOuter(screen_class) == screen;
    }

    // The one live grid for `inventory_id` on the screen we opened.
    //
    // Matching by inventory id alone was not enough: WBP_Inventory_C
    // instances from previous opens stay resolvable for a while and match
    // too, so the same text was being written to every one of them (observed
    // growing 1 -> 2 -> 3 -> 4 across four opens). Only the grid belonging to
    // the widget currently on the Modal layer is on screen.
    UObject* find_live_grid(int32_t inventory_id, UObject* screen)
    {
        // Without a screen to anchor to there is no safe way to tell a live
        // grid from one belonging to a screen we just popped, so do nothing.
        if (!screen) {
            return nullptr;
        }

        for (auto* grid : StorageTerminal::ReflectionUtils::find_all_unfiltered(StorageTerminalTargets::kInventoryWidgetClass)) {
            if (!grid) {
                continue;
            }
            // ANCESTRY IS CHECKED FIRST, AND THIS ORDER MATTERS.
            //
            // This enumeration returns every WBP_Inventory_C in the process,
            // including the ones belonging to screens the mod popped moments
            // ago and which are being torn down. is_grid_for_inventory walks
            // grid -> ViewModel -> InventoryComponent -> InventoryId, three
            // chained reflected reads; doing that to a half-destroyed widget is
            // the most likely cause of the crash while browsing quickly
            // (2026-07-30, crash dump written mid-browse). Checking ancestry
            // first means the deep walk only ever touches widgets that belong
            // to the screen currently on the Modal layer.
            if (!belongs_to_screen(grid, screen)) {
                continue;
            }
            if (is_grid_for_inventory(grid, inventory_id)) {
                return grid;
            }
        }

        // Deliberately NO fallback. The old code kept the last id-matching grid
        // even when it belonged to no live screen and wrote text into it --
        // which is precisely how a dying widget got touched.
        return nullptr;
    }

    static void set_widget_text(UObject* widget, const std::wstring& value)
    {
        auto* set_text = StorageTerminal::ReflectionUtils::find_function(widget, StorageTerminalTargets::kSetText);
        auto* in_text = set_text
            ? StorageTerminal::PropertyReflection::find_property(set_text, StorageTerminalTargets::kFieldInText)
            : nullptr;
        if (!in_text) {
            return;
        }
        std::vector<uint8_t> params(static_cast<size_t>(set_text->GetPropertiesSize()), 0);
        StorageTerminal::PropertyReflection::write_text(in_text, params.data(), value);
        widget->ProcessEvent(set_text, params.data());
    }

    static UObject* find_child_widget(UObject* grid, std::wstring_view field_name)
    {
        auto* field = StorageTerminal::PropertyReflection::find_property(grid->GetClassPrivate(), field_name);
        return field
            ? StorageTerminal::PropertyReflection::read_object(field, reinterpret_cast<const uint8_t*>(grid))
            : nullptr;
    }

    // ONE SHORT LINE. The title sits in a narrow, centred slot in a layout
    // that was built for a locker's name, and it is the only text surface on
    // this screen the mod may safely touch.
    //
    // A previous build also wrote a multi-line match list into the screen's
    // DescriptionText block. It rendered outside its container, straight over
    // the tab bar and the inventory header (confirmed by screenshot,
    // 2026-07-29). DescriptionText is laid out for a one-line item blurb, not
    // a list, and forcing it visible does not give it room. Do not reintroduce
    // that without a container of the mod's own; the search list belongs
    // somewhere with actual space, not squeezed into this screen.
    std::wstring build_title() const
    {
        if (m_query.empty()) {
            // No search term: browsing every locker, empty ones included, so
            // name the one being looked at and its place in the walk.
            if (m_browse.empty()) {
                return L"STORAGE NETWORK";
            }
            const auto& current = m_browse[std::min(m_browseIndex, m_browse.size() - 1)];
            std::wstring title = L"STORAGE NETWORK  --  " + current.locker_name;
            if (current.has_distance) {
                title += L" " + format_metres(current.distance);
            }
            if (m_browse.size() > 1) {
                title += L"  (" + std::to_wstring(m_browseIndex + 1) + L"/"
                       + std::to_wstring(m_browse.size()) + L")";
            }
            return title;
        }

        std::wstring title = L"'" + m_query + L"'";
        if (m_browse.empty()) {
            return title + L"  --  nothing stored";
        }

        const auto& current = m_browse[std::min(m_browseIndex, m_browse.size() - 1)];
        title += L"  " + current.item_name;
        if (current.count > 1) {
            title += L" x" + std::to_wstring(current.count);
        }
        title += L"  --  " + current.locker_name;
        if (current.has_distance) {
            title += L" " + format_metres(current.distance);
        }
        if (m_browse.size() > 1) {
            title += L"  (" + std::to_wstring(m_browseIndex + 1) + L"/"
                   + std::to_wstring(m_browse.size()) + L")";
        }
        return title;
    }

    // The full match list goes to the log, where it has room. This is also
    // the record for diagnosing what the search actually saw.
    void log_result_list() const
    {
        size_t row = 0;
        for (const auto& match : m_matches) {
            if (row >= kMaxResultRows) {
                log_line(L"  ... and " + std::to_wstring(m_matches.size() - row) + L" more");
                break;
            }
            std::wstring line = L"  " + match.item_name;
            if (match.count > 1) {
                line += L" x" + std::to_wstring(match.count);
            }
            line += L"  --  ";
            line += match.is_player ? L"carried" : match.locker_name;
            if (match.has_distance) {
                line += L", " + format_metres(match.distance);
            }
            log_line(line);
            ++row;
        }
    }

    // Writes the search state into the open screen's own title and
    // description text. The screen belongs to a real locker, so this is
    // cosmetic and transient. Only ever touches the grid on the screen WE
    // opened.
    void update_screen()
    {
        if (!m_screenOpen) {
            return;
        }
        auto* grid = find_live_grid(m_openInventoryId, m_ownedScreen);
        if (!grid) {
            log(L"Search: no live grid for the open locker; text not written.");
            return;
        }

        if (grid != m_touchedGrid) {
            // Remember what the flag was before we forced it on, so it can be
            // put back when the screen closes.
            if (auto* show_field = StorageTerminal::PropertyReflection::find_property(
                    grid->GetClassPrivate(), StorageTerminalTargets::kFieldShowInventoryTitle)) {
                m_touchedGridHadTitle = StorageTerminal::PropertyReflection::read_bool(
                    show_field, reinterpret_cast<const uint8_t*>(grid)).value_or(false);
                StorageTerminal::PropertyReflection::write_bool(show_field, reinterpret_cast<uint8_t*>(grid), true);
            }
            m_touchedGrid = grid;
        }

        for (const auto candidate : StorageTerminalTargets::kTitleWidgetCandidates) {
            if (auto* title_widget = find_child_widget(grid, candidate)) {
                set_widget_text(title_widget, build_title());
                break;
            }
        }

        // The result dump is up to nine formatted lines, and update_screen runs
        // on every keystroke AND on every rebuild while the screen is open --
        // so the same list was being written repeatedly with nothing changed.
        // Signature covers the query and every match row, so a genuine change
        // still logs and a no-op rebuild does not.
        uint64_t signature = fnv1a(m_query);
        for (const auto& match : m_matches) {
            signature = fnv1a(match.item_name, signature);
            signature = fnv1a(match.locker_name, signature);
            signature = fnv1a(std::to_wstring(match.count), signature);
        }
        if (signature == m_lastSearchLogSig) {
            return;
        }
        m_lastSearchLogSig = signature;

        log_line(L"Search '" + m_query + L"': " + std::to_wstring(m_matches.size())
                 + L" match(es) across " + std::to_wstring(m_browse.size()) + L" locker(s).");
        log_result_list();
    }

    void restore_touched_grid()
    {
        if (!m_touchedGrid) {
            return;
        }
        if (auto* show_field = StorageTerminal::PropertyReflection::find_property(
                m_touchedGrid->GetClassPrivate(), StorageTerminalTargets::kFieldShowInventoryTitle)) {
            StorageTerminal::PropertyReflection::write_bool(
                show_field, reinterpret_cast<uint8_t*>(m_touchedGrid), m_touchedGridHadTitle);
        }
        m_touchedGrid = nullptr;
    }

    // ---- opening a locker (the one real game action) -------------------

    UObject* find_component_for_inventory(int32_t inventory_id)
    {
        // The snapshot already holds the live component for every source it
        // scanned, so the common case needs no world scan at all.
        for (const auto& source : m_snapshot.sources()) {
            if (source.inventory_id == inventory_id
                && source.component
                && StorageTerminal::InventorySnapshot::is_fresh(source)) {
                return source.component;
            }
        }

        for (auto* component : StorageTerminal::ReflectionUtils::find_all(StorageTerminalTargets::kInventoryComponentClass)) {
            if (!component) {
                continue;
            }
            auto* id_field = StorageTerminal::PropertyReflection::find_property(
                component->GetClassPrivate(), StorageTerminalTargets::kFieldInventoryId);
            if (id_field
                && StorageTerminal::PropertyReflection::read_int(
                       id_field, reinterpret_cast<const uint8_t*>(component)).value_or(-1) == inventory_id) {
                return component;
            }
        }
        return nullptr;
    }

    // Opens a locker's own native screen, exactly as walking up to it and
    // pressing interact does. If one of our screens is already up it is
    // popped first, so this reads as "switch to that locker".
    //
    // Returns true only if a screen for that locker is actually on the Modal
    // layer afterwards. It used to return true unconditionally after the
    // ProcessEvent, so every caller treated a silent no-op (which is exactly
    // what interact does when a screen is already up) as success.
    bool open_locker(int32_t inventory_id)
    {
        // ONE WindowManager lookup for the whole operation. This function used
        // to resolve it five separate times (once inside pop_modal_screen, once
        // for modal_before, once for the after-check, each a full walk of every
        // live UObject).
        UObject* const window_manager = find_window_manager();

        if (m_screenOpen) {
            pop_modal_screen(window_manager);
        }
        // Captured before the interact so success can be judged by whether a
        // NEW screen actually landed on the Modal layer, rather than by
        // whether ProcessEvent returned (it always does).
        UObject* const modal_before = active_modal_widget(window_manager);
        auto* target_component = find_component_for_inventory(inventory_id);
        auto* owner = target_component ? target_component->GetTypedOuter<AActor>() : nullptr;
        if (!owner) {
            log_line(L"Open locker: inventory " + std::to_wstring(inventory_id) + L" not found.");
            return false;
        }

        auto* interaction_class = StorageTerminal::ReflectionUtils::find_class_by_name(
            StorageTerminalTargets::kInventoryInteractionComponentClass);
        UObject* interaction_component = nullptr;
        if (interaction_class) {
            auto matches = owner->GetComponentsByClass(interaction_class);
            if (matches.Num() > 0) {
                interaction_component = matches[0];
            }
        }
        if (!interaction_component) {
            log_line(L"Open locker: inventory " + std::to_wstring(inventory_id) + L" has no interaction component.");
            return false;
        }

        // The game's own gate on whether this container may be interacted
        // with at all. Calling interact regardless was reaching past a rule
        // the game enforces on the player.
        if (auto* enabled_field = StorageTerminal::PropertyReflection::find_property(
                interaction_component->GetClassPrivate(), StorageTerminalTargets::kFieldInteractionEnabled)) {
            if (!StorageTerminal::PropertyReflection::read_bool(
                    enabled_field, reinterpret_cast<const uint8_t*>(interaction_component)).value_or(true)) {
                log_line(L"Open locker: inventory " + std::to_wstring(inventory_id) + L" is not interactable right now.");
                return false;
            }
        }

        // One controller lookup, with the pawn derived from it. read_player_pawn()
        // used to scan for the controller all over again.
        auto* player_controller = find_player_controller();
        auto* pawn = read_pawn_of(player_controller);
        auto* interact = StorageTerminal::ReflectionUtils::find_function(
            interaction_component, StorageTerminalTargets::kInteractWithInventoryInteractionComponent);
        auto* controller_param = interact
            ? StorageTerminal::PropertyReflection::find_property(interact, StorageTerminalTargets::kFieldController)
            : nullptr;
        auto* pawn_param = interact
            ? StorageTerminal::PropertyReflection::find_property(interact, StorageTerminalTargets::kFieldPawn)
            : nullptr;
        if (!interact || !controller_param || !pawn_param || !player_controller || !pawn) {
            log(L"Open locker: interact surface unavailable.");
            return false;
        }

        std::vector<uint8_t> params(static_cast<size_t>(interact->GetPropertiesSize()), 0);
        StorageTerminal::PropertyReflection::write_object(controller_param, params.data(), player_controller);
        StorageTerminal::PropertyReflection::write_object(pawn_param, params.data(), pawn);
        interaction_component->ProcessEvent(interact, params.data());

        auto* screen = active_modal_widget(window_manager);
        if (!screen || screen == modal_before) {
            log_line(L"Open locker: interact on inventory " + std::to_wstring(inventory_id)
                     + L" did not put its screen up.");
            return false;
        }

        m_ownedScreen = screen;
        m_openInventoryId = inventory_id;
        m_checksSinceOpen = 0; // start a quiet period
        log_line(L"Opened locker inventory " + std::to_wstring(inventory_id)
                 + L" (" + StorageTerminal::ReflectionUtils::safe_full_name(owner) + L").");
        return true;
    }

    // Asks for a switch to `inventory_id`, popping the current screen now and
    // letting service_pending_locker() do the interact once the Modal layer has
    // actually cleared.
    //
    // This MUST NOT be done inline. Popping and interacting in the same call
    // failed every single time in the 2026-07-29 log: the pop had not taken
    // effect yet, so the game ignored the interact ("did not put its screen
    // up"), and by the time the pop did land there was nothing to replace it.
    // Same shape as the NoA open path, which already defers for the same reason.
    void request_locker(int32_t inventory_id)
    {
        if (inventory_id < 0) {
            return;
        }
        // If a switch is already in flight the Modal layer is already empty --
        // popping again would be a second pop against whatever landed there.
        // Retarget instead, so holding Page Down walks the index and only the
        // locker you stop on is actually opened. Without this, fast browsing
        // pushed a pop plus a native screen open several times a second, which
        // is the UI-stack churn this project has crashed on before.
        const bool already_pending = (m_pendingLockerId >= 0);

        m_pendingLockerId = inventory_id;
        m_pendingLockerChecks = kSwitchAfterPopChecks;

        if (!already_pending) {
            pop_modal_screen(find_window_manager());
        }
    }

    // Completes a pending switch. Returns true while one is in flight, so the
    // caller can leave the rest of the update alone.
    bool service_pending_locker()
    {
        if (m_pendingLockerId < 0) {
            return false;
        }

        auto* window_manager = find_window_manager();
        if (active_modal_widget(window_manager) != nullptr) {
            // The old screen is still going down. Wait for it.
            if (--m_pendingLockerChecks <= 0) {
                log_line(L"Switch to locker " + std::to_wstring(m_pendingLockerId)
                         + L": the previous screen never closed; giving up.");
                m_pendingLockerId = -1;
            }
            return true;
        }

        const int32_t target = m_pendingLockerId;
        m_pendingLockerId = -1;

        if (open_locker(target)) {
            update_screen();
            return true;
        }

        // The Modal layer is empty and we could not put a screen back on it.
        // Release everything rather than holding state for a screen that does
        // not exist -- that mismatch is what left the game with no screen and
        // no way back to normal input.
        log(L"Switch failed and no screen is up; standing down.");
        restore_touched_grid();
        forget_screen();
        return true;
    }

    // The locker to land on for the current query: the nearest one holding a
    // match, or -- with no query yet -- simply the nearest locker with
    // anything in it. It used to be whichever locker held the most stacks,
    // resolved with >= so ties went to whatever was scanned last, which meant
    // F5 dropped the player into an arbitrary container.
    int32_t pick_landing_locker()
    {
        if (!m_browse.empty()) {
            return m_browse.front().inventory_id;
        }

        int32_t best_id = -1;
        double best_distance = 0.0;
        bool best_has_distance = false;

        WorldPoint player{};
        const bool have_player = player_location(player);

        for (const auto& source : m_snapshot.sources()) {
            if (source.is_player || source.items.empty()
                || !StorageTerminal::InventorySnapshot::is_fresh(source)) {
                continue;
            }
            double distance = 0.0;
            bool has_distance = false;
            if (have_player && source.has_location) {
                const double dx = source.x - player.x;
                const double dy = source.y - player.y;
                const double dz = source.z - player.z;
                distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                has_distance = true;
            }
            const bool better = (best_id < 0)
                || (has_distance && !best_has_distance)
                || (has_distance && best_has_distance && distance < best_distance);
            if (better) {
                best_id = source.inventory_id;
                best_distance = distance;
                best_has_distance = has_distance;
            }
        }
        return best_id;
    }

    // Opens the storage network view. Reached only by picking NoA's dialogue
    // option at a terminal -- there is deliberately no hotkey.
    void open_storage_network()
    {
        if (reconcile_screen_state()) {
            close_screen();
            return;
        }

        rebuild_and_log_snapshot();
        m_query.clear();
        rebuild_matches();

        const int32_t landing = pick_landing_locker();
        if (landing < 0) {
            log(L"No communal lockers with anything in them were found.");
            return;
        }
        if (!open_locker(landing)) {
            return;
        }

        m_screenOpen = true;
        update_screen();
        log(L"Storage Network open. Type to search; PAGE UP/DOWN browse the lockers holding it; Escape closes.");
    }

    // Closes the screen: pop it, then release our state.
    //
    // The mod MUST do the pop. Letting the game close its own screen on Escape
    // was tried and confirmed broken in game (2026-07-29, UE4SS.log line 1071):
    // the mod logged "released", stood down, and the screen just stayed on
    // screen -- the game does not route Escape to a storage screen that was
    // opened through the interact hijack. Standing down also cleared
    // m_screenOpen, which silently killed the arrow/Page keys for the rest of
    // the session, because every handler starts with reconcile_screen_state().
    //
    // The earlier "mouse stops working" report was NOT caused by this pop. It
    // was the browse path popping and then failing to open a replacement (see
    // request_locker) -- the player noticed the dead mouse after closing, which
    // is what made the close look guilty.
    void close_screen()
    {
        if (!reconcile_screen_state()) {
            return;
        }
        restore_touched_grid();
        pop_modal_screen(find_window_manager());
        forget_screen();
        log(L"Storage Network closed.");
    }

    // ---- input ----------------------------------------------------------

    void on_search_key(wchar_t character)
    {
        if (!reconcile_screen_state()) {
            return; // keys behave normally when our screen isn't up
        }
        if (character == L'\b') {
            if (m_query.empty()) {
                return;
            }
            m_query.pop_back();
        } else {
            m_query.push_back(character);
        }
        rebuild_matches();
        // Show the first (nearest) locker that has it, so a search lands you
        // somewhere useful immediately. Deferred for the same reason as browse;
        // update_screen runs when the new screen is actually up.
        if (!m_browse.empty() && m_browse[m_browseIndex].inventory_id != m_openInventoryId) {
            request_locker(m_browse[m_browseIndex].inventory_id);
            return;
        }
        update_screen();
    }

    void rebuild_and_log_snapshot()
    {
        const auto result = m_snapshotBuilder.rebuild(m_snapshot);
        if (!result.get_items_function_found) {
            return;
        }

        const std::wstring line = L"Snapshot: " + std::to_wstring(result.sources_discovered) + L" inventor(ies)"
            + (result.player_inventory_found ? L" (including yours)" : L"")
            + (result.reachable_ids_found > 0
                   ? L", scoped to " + std::to_wstring(result.reachable_ids_found) + L" reachable."
                   : L", unscoped.");

        // Only log when this differs from the last line written. Rebuilds fire
        // on every inventory event, and the summary is usually identical --
        // re-writing it every couple of seconds for a whole session is pure IO.
        const uint64_t signature = fnv1a(line);
        if (signature == m_lastSnapshotLogSig) {
            return;
        }
        m_lastSnapshotLogSig = signature;
        log_line(line);
    }

    // Runs on the mod-update thread, never inside the click hook.
    void service_noa_request()
    {
        if (m_noaTerminal.consume_open_request()) {
            log(L"NoA: storage network requested.");
            m_noaTerminal.close_terminal_ui();
            m_pendingOpenChecks = kOpenAfterCloseChecks;
            return;
        }

        if (m_pendingOpenChecks < 0) {
            return;
        }

        // Wait for NoA's own screen to leave the Modal layer before asking a
        // locker to put its screen there.
        if (active_modal_widget() == nullptr) {
            m_pendingOpenChecks = -1;
            open_storage_network();
            return;
        }

        if (--m_pendingOpenChecks <= 0) {
            m_pendingOpenChecks = -1;
            log(L"NoA: terminal screen never closed; not opening the storage network.");
        }
    }

public:
    StorageTerminalMod()
        : CppUserModBase()
    {
        ModName = STR("StorageTerminal");
        // Kept in lockstep with the VERSION file at the repo root --
        // scripts/bump-version.ps1 rewrites both, and publish-release.ps1
        // refuses to upload if they ever disagree.
        ModVersion = STR("1.0.0");
        ModDescription = STR("Search every communal locker at once, then jump to the one holding what you need.");
        ModAuthors = STR("GitHub Copilot");
    }

    auto on_program_start() -> void override {}

    auto on_unreal_init() -> void override
    {
        if (m_bootLogged) {
            return;
        }
        m_bootLogged = true;
        log(L"Storage Network loaded.");

        // No opener hotkey by design. The Storage Network is reached by
        // walking up to a NoA terminal and picking her dialogue option, so the
        // base's storage index is something the base gives you, not something
        // you carry around the world. Escape stays bound purely to close.
        register_keydown_event(Input::Key::ESCAPE, [this]() { close_screen(); });
        for (int32_t offset = 0; offset < 26; ++offset) {
            const auto key = static_cast<Input::Key>(static_cast<uint8_t>(Input::Key::A) + offset);
            const wchar_t character = static_cast<wchar_t>(L'a' + offset);
            register_keydown_event(key, [this, character]() { on_search_key(character); });
        }
        // Digits and a little punctuation: item names contain them (and an
        // item whose name has a digit used to be unreachable entirely).
        for (int32_t offset = 0; offset < 10; ++offset) {
            const wchar_t character = static_cast<wchar_t>(L'0' + offset);
            const auto row_key = static_cast<Input::Key>(static_cast<uint8_t>(Input::Key::ZERO) + offset);
            const auto pad_key = static_cast<Input::Key>(static_cast<uint8_t>(Input::Key::NUM_ZERO) + offset);
            register_keydown_event(row_key, [this, character]() { on_search_key(character); });
            register_keydown_event(pad_key, [this, character]() { on_search_key(character); });
        }
        register_keydown_event(Input::Key::OEM_MINUS, [this]() { on_search_key(L'-'); });
        register_keydown_event(Input::Key::SUBTRACT, [this]() { on_search_key(L'-'); });
        register_keydown_event(Input::Key::OEM_PERIOD, [this]() { on_search_key(L'.'); });
        register_keydown_event(Input::Key::OEM_SEVEN, [this]() { on_search_key(L'\''); });
        register_keydown_event(Input::Key::SPACE, [this]() { on_search_key(L' '); });
        register_keydown_event(Input::Key::BACKSPACE, [this]() { on_search_key(L'\b'); });
        // Browse the lockers holding the searched item. The open inventory
        // widget consumes the ARROW keys for its own grid navigation (it has
        // OnKeyDown/OnPreviewKeyDown handlers, confirmed by trace), so Page
        // Up/Down are the reliable bindings; arrows stay bound for when the
        // widget does not have focus.
        register_keydown_event(Input::Key::PAGE_DOWN, [this]() { browse(+1); });
        register_keydown_event(Input::Key::PAGE_UP, [this]() { browse(-1); });
        register_keydown_event(Input::Key::DOWN_ARROW, [this]() { browse(+1); });
        register_keydown_event(Input::Key::UP_ARROW, [this]() { browse(-1); });

        log(L"Ask NoA at any terminal for the storage network. Type to search, PAGE UP/DOWN to browse lockers holding it, Escape to close.");
    }

    auto on_update() -> void override
    {
        const bool due_this_tick = (m_ticksSinceCheck == 0);
        m_ticksSinceCheck = (m_ticksSinceCheck + 1) % kCheckIntervalTicks;
        if (!due_this_tick || !is_game_world_ready()) {
            return;
        }

        if (!m_worldReadyLogged) {
            m_worldReadyLogged = true;
            log(L"Game world detected.");
        }

        // Refreshed once per check so the search path (which runs per keystroke)
        // reads a cached position instead of scanning for the player controller
        // every time. Only needed while the screen is up -- distance is not
        // used for anything else.
        if (m_screenOpen) {
            refresh_player_location();
        }

        // Retry until it actually installs. This used to give up after one
        // attempt: if no UWEInventoryComponent existed at that instant the
        // hook never installed, and since rebuilds are gated on the flag it
        // sets, the snapshot then stayed frozen at its first scan for the
        // whole session with nothing logged to say so.
        if (!m_inventoryEventHook.is_installed()) {
            m_inventoryEventHook.try_install([this]() { m_inventoryDirty.store(true, std::memory_order_relaxed); });
        }

        // Same retry-until-installed rule for the NoA click watcher, and a
        // periodic rescan so a terminal the player builds later still gets the
        // option.
        if (!m_noaTerminal.is_click_watch_installed()) {
            m_noaTerminal.try_install_click_watch();
        }
        if (m_terminalRescanChecks == 0) {
            m_noaTerminal.refresh_terminals();
        }
        m_terminalRescanChecks = (m_terminalRescanChecks + 1) % kChecksBetweenTerminalScans;

        service_noa_request();

        if (m_checksSinceOpen < kQuietChecksAfterOpen) {
            ++m_checksSinceOpen;
        }
        ++m_checksSinceRebuild;

        // Finish any locker switch the player asked for. While one is in flight
        // the Modal layer is legitimately empty, so nothing below should run and
        // read that as the screen having gone away.
        if (service_pending_locker()) {
            return;
        }

        // Keep our idea of the screen honest even when no key is pressed.
        reconcile_screen_state();

        // QUIET PERIOD AFTER OPENING A SCREEN.
        //
        // This guard was written, documented, and then never actually applied:
        // m_checksSinceOpen was incremented every check and read by nothing, so
        // the protection it describes has never once run. Opening a locker
        // fires inventory events, which set the dirty flag, which rebuilt and
        // called update_screen() while the screen we had just popped was still
        // tearing its widgets down -- the crash-while-browsing case.
        //
        // Checked BEFORE the exchange below so the dirty flag is not consumed
        // and thrown away; the rebuild simply happens once things have settled.
        if (m_checksSinceOpen < kQuietChecksAfterOpen) {
            return;
        }

        // Only rebuild when something actually changed AND enough time has
        // passed.
        const bool changed = m_inventoryDirty.exchange(false, std::memory_order_relaxed);
        if (!changed || m_checksSinceRebuild < kMinChecksBetweenRebuilds) {
            return;
        }
        m_checksSinceRebuild = 0;

        rebuild_and_log_snapshot();

        // Deliberately NO auto-advance. Opening/popping native screens
        // perturbs which components are live when we rescan; auto-advancing
        // on that opened yet another screen and churned the game's UI stack
        // until it crashed (2026-07-28). The mod only ever changes screens
        // when the player presses a key.
        if (m_screenOpen) {
            rebuild_matches();
            update_screen();
        }
    }

    ~StorageTerminalMod() override = default;
};

#define STORAGE_TERMINAL_MOD_API __declspec(dllexport)
extern "C"
{
    STORAGE_TERMINAL_MOD_API RC::CppUserModBase* start_mod()
    {
        return new StorageTerminalMod();
    }

    STORAGE_TERMINAL_MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
