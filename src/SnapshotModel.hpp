#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace RC::Unreal {
class UObject;
}

namespace StorageTerminal {

// One item type held in one inventory, already merged across stacks (the game
// has no stacking -- every FUWEInventoryItem::Count is 1 -- so a locker with
// twelve titanium yields twelve one-count entries that SnapshotBuilder folds
// into a single ItemCount with count 12).
struct ItemCount {
    // The player-facing, localized name from UUWEItemType::Name (FText). This
    // is what the player searches and what is displayed. The asset name was
    // used for both until 2026-07-28; it only worked because the two happen to
    // be similar in English.
    std::wstring display_name;

    // UObject::GetName() of the UUWEItemType asset. Kept only as a secondary
    // search key, so a player who knows the internal name (or an item whose
    // FText is empty) still finds it.
    std::wstring asset_name;

    // Both names pre-lowercased for searching. The search runs on EVERY
    // KEYSTROKE over every item in every locker, and it used to lowercase both
    // names inline -- two string allocations per item per keypress. Folding it
    // into the snapshot does the work once per rebuild instead.
    std::wstring display_lower;
    std::wstring asset_lower;

    int32_t count = 0;
};

// Lowercases a string for case-insensitive search. Shared so the snapshot and
// the query are always folded the same way -- if these ever diverged, searches
// would silently miss.
inline std::wstring to_search_key(std::wstring_view value)
{
    std::wstring lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lowered;
}

struct InventorySourceSnapshot {
    int32_t inventory_id = -1;

    // UWEInventoryComponent::InventoryName (FText) -- the locker's own label,
    // which the player can edit in game (the game ships WBP_LockerLabelScreen
    // for exactly this). Falls back to a tidied owning-actor class name when
    // the locker has never been named.
    std::wstring display_name;

    // Owning actor's world location, for distance/direction to the player.
    // Zero when the owner could not be resolved (see has_location).
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool has_location = false;

    // The player's own carried inventory is included as a source so "do I
    // have it" covers pockets too, but it is never a navigation target -- you
    // are already standing in it.
    bool is_player = false;

    std::vector<ItemCount> items;

    // The live UUWEInventoryComponent. Held so opening a locker can resolve
    // its component from the snapshot instead of rescanning every UObject in
    // the world. Only dereferenced during the same frame as a rebuild, or
    // after re-validating against a fresh scan.
    RC::Unreal::UObject* component = nullptr;

    // How many consecutive rebuilds have failed to see this source. Opening
    // and popping native screens perturbs which components are live during a
    // rescan, so a locker can vanish for one scan and come back -- which used
    // to blink search results out from under the player mid-search
    // (confirmed in the 2026-07-28 log: "'copper': 1 locker" became
    // "0 lockers" with nothing moved). A source is only really gone once it
    // has been missing kMissesBeforeDrop times in a row.
    int32_t consecutive_misses = 0;
};

class InventorySnapshot {
public:
    static constexpr int32_t kMissesBeforeDrop = 3;

    // Marks every known source as unseen. Call before a rebuild pass, then
    // upsert_source for each source actually found, then drop_unseen_sources.
    void begin_scan()
    {
        for (auto& source : m_sources) {
            ++source.consecutive_misses;
        }
    }

    void upsert_source(InventorySourceSnapshot source)
    {
        source.consecutive_misses = 0;

        auto existing = find_source(source.inventory_id);
        if (existing != m_sources.end()) {
            *existing = std::move(source);
            return;
        }

        m_sources.push_back(std::move(source));
    }

    void drop_unseen_sources()
    {
        m_sources.erase(
            std::remove_if(m_sources.begin(), m_sources.end(), [](const InventorySourceSnapshot& source) {
                return source.consecutive_misses >= kMissesBeforeDrop;
            }),
            m_sources.end());
    }

    // True for a source seen by the most recent scan. Callers should ignore
    // stale sources for anything that touches the world (opening a locker),
    // but may still display them -- a locker that missed one scan almost
    // certainly still exists.
    [[nodiscard]] static bool is_fresh(const InventorySourceSnapshot& source)
    {
        return source.consecutive_misses == 0;
    }

    [[nodiscard]] const std::vector<InventorySourceSnapshot>& sources() const
    {
        return m_sources;
    }

    void clear()
    {
        m_sources.clear();
    }

private:
    std::vector<InventorySourceSnapshot>::iterator find_source(int32_t inventory_id)
    {
        for (auto it = m_sources.begin(); it != m_sources.end(); ++it) {
            if (it->inventory_id == inventory_id) {
                return it;
            }
        }

        return m_sources.end();
    }

    std::vector<InventorySourceSnapshot> m_sources;
};

}
