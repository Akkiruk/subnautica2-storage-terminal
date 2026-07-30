#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Every game class/function/field name this mod touches, and why it is safe.
//
// The mod is read-only apart from opening a locker's native screen (and
// writing that screen's title/description text, which is cosmetic). Anything
// here that would move, create, or delete items was removed on 2026-07-28
// along with the abandoned merged-inventory designs -- see
// docs/archive/ARCHITECTURE_OPTIONS.md.
//
// Before adding a new name: check docs/UE4SS_API_REFERENCE.md for whether a
// real compiled UE4SS function already does the job.
namespace StorageTerminalTargets {

// --- inventories (read-only) ---------------------------------------------
// Every locker's items come from UWEInventoryComponent::GetItems(), a const
// function the game itself calls constantly. Discovery scans live components
// directly rather than trusting any subsystem registration array -- three
// registration-based designs failed, see docs/REVERSE_ENGINEERING_NOTES.md.
inline constexpr std::wstring_view kInventoryComponentClass = L"UWEInventoryComponent";
inline constexpr std::wstring_view kGetItems = L"GetItems";
inline constexpr std::wstring_view kFieldInventoryId = L"InventoryId";
inline constexpr std::wstring_view kFieldIsCommunal = L"bIsCommunal";
inline constexpr std::wstring_view kFieldItemType = L"ItemType";
inline constexpr std::wstring_view kFieldCount = L"Count";

// The locker's own label (an FText the player can edit in game -- the game
// ships WBP_LockerLabelScreen for it). This is how a search result names the
// container it found something in; without it the player only ever got an
// ordinal, which is exactly the thing they were trying not to memorise.
inline constexpr std::wstring_view kFieldInventoryName = L"InventoryName";

// UUWEItemType's localized display name. Dumper-7 renames it "Name_0"
// because it collides with UObject::Name in generated C++, but the runtime
// reflected name is usually plain "Name" -- both are tried, and whichever
// resolves as an FText wins. Searching this instead of the asset name is what
// makes the search match what the player actually reads on screen.
inline constexpr std::array<std::wstring_view, 2> kItemDisplayNameCandidates{
    std::wstring_view(L"Name"),
    std::wstring_view(L"Name_0"),
};

// --- change notifications (observe only) ---------------------------------
// Hooked through UE4SS's shared ProcessEvent stream, never a per-function
// trampoline (that crashed; see feedback_ue4ss_reflection_bugs). These are
// per-component handlers of a subsystem-wide broadcast, so one real change
// fires them on every component -- we only use them as a "something moved"
// signal, which makes that harmless.
inline constexpr std::wstring_view kOnInventoryUpdated = L"OnInventoryUpdated";
inline constexpr std::wstring_view kOnItemAddedToInventory = L"OnItemAddedToInventory";
inline constexpr std::wstring_view kOnItemRemovedFromInventory = L"OnItemRemovedFromInventory";

// --- opening a locker: the mod's ONLY non-read action ---------------------
// The exact function that runs when a player walks up to a storage and
// presses interact, so the game does all the work and the result is
// server-authoritative and multiplayer-safe.
//
// InventoryInteractionEnabled is the component's own replicated gate for
// whether that interaction is currently allowed (a locker can be sealed,
// powered down, or otherwise refused). The mod reads it and declines rather
// than calling interact anyway -- calling it regardless was reaching past a
// rule the game enforces on the player.
inline constexpr std::wstring_view kInventoryInteractionComponentClass = L"UWEInventoryInteractionComponent";
inline constexpr std::wstring_view kInteractWithInventoryInteractionComponent = L"InteractWithInventoryInteractionComponent";
inline constexpr std::wstring_view kFieldInteractionEnabled = L"InventoryInteractionEnabled";
inline constexpr std::wstring_view kFieldController = L"Controller";
inline constexpr std::wstring_view kFieldPawn = L"Pawn";
inline constexpr std::wstring_view kPlayerControllerClass = L"SN2PlayerController";

// --- closing / switching screens -----------------------------------------
// UWindowManager owns every screen, one stack per layer. It lives in the
// engine transient package, so it MUST be found with a raw unfiltered
// lookup -- ReflectionUtils::find_first hides it and the close silently did
// nothing for two sessions because of that.
// Only the Modal layer is ever touched: popping other layers removed the
// HUD and broke input, and popping the player's own PDA crashed the game.
// GetActiveWidget also serves as the mod's source of truth for whether its
// screen is still up, so its own bookkeeping can never drift from reality.
inline constexpr std::wstring_view kWindowManagerClass = L"WindowManager";
inline constexpr std::wstring_view kGetActiveWidget = L"GetActiveWidget";
inline constexpr std::wstring_view kWindowManagerPop = L"Pop";
inline constexpr std::wstring_view kFieldLayerId = L"LayerId";
inline constexpr std::wstring_view kFieldWidget = L"Widget";
inline constexpr std::wstring_view kFieldReturnValue = L"ReturnValue";
inline constexpr uint8_t kStorageScreenLayer = 3; // EUWEWindowManagerLayer::Modal

// --- the open screen's text (cosmetic writes) -----------------------------
// Used to show the search state and the match list. The widget lives in the
// transient package too, hence the unfiltered lookup. Writes only ever go to
// the grid that belongs to the screen currently on the Modal layer: stale
// WBP_Inventory_C instances from previous opens stay resolvable for a while
// and used to receive the same write (observed growing 1 -> 2 -> 3 -> 4
// across four opens in the 2026-07-28 log).
//
// Name_0 is the title. DescriptionText is a second, larger block on the same
// screen -- it is what makes a multi-line result list possible at all, rather
// than cramming everything into one title string. Dumper-7 renames colliding
// members, so "Name_0" may be plain "Name" at runtime; both are tried.
inline constexpr std::wstring_view kInventoryWidgetClass = L"WBP_Inventory_C";
inline constexpr std::wstring_view kFieldViewModel = L"ViewModel";
inline constexpr std::wstring_view kFieldViewModelInventoryComponent = L"InventoryComponent";
inline constexpr std::wstring_view kFieldShowInventoryTitle = L"ShowInventoryTitle";
inline constexpr std::wstring_view kFieldDescriptionText = L"DescriptionText";
inline constexpr std::wstring_view kFieldDescriptionRoot = L"DescriptionRoot";
inline constexpr std::wstring_view kSetText = L"SetText";
inline constexpr std::wstring_view kSetVisibility = L"SetVisibility";
inline constexpr std::wstring_view kFieldInVisibility = L"InVisibility";
inline constexpr std::wstring_view kFieldInText = L"InText";
inline constexpr uint8_t kVisibilityVisible = 0; // ESlateVisibility::Visible

inline constexpr std::array<std::wstring_view, 2> kTitleWidgetCandidates{
    std::wstring_view(L"Name_0"),
    std::wstring_view(L"Name"),
};

// --- the NoA terminal: the mod's only way in ------------------------------
// The Storage Network is reached by walking up to a NoA computer terminal and
// picking a dialogue option, not by a global hotkey. That is the point: the
// base's storage should be readable from the base's terminal, not from
// anywhere in the world.
//
// UWEComputerTextInterfaceComponent is the terminal's dialogue component (the
// live instances are Blueprint subclasses -- BPC_ComputerTextInterface_Component_C
// -- but FindAllOf matches up the superclass chain, so searching for the base
// name finds them).
inline constexpr std::wstring_view kComputerTextInterfaceComponentClass = L"UWEComputerTextInterfaceComponent";
inline constexpr std::wstring_view kDialogueDataClass = L"UWEComputerTextInterfaceDialogueData";

// The root menu is built from DefaultRootDialogueData + ExtraRootDialogueData.
// Append straight to Extra: the obvious-looking AddRootDialogueOption() was
// confirmed in-game NOT to touch it (it only added to MergedRootDialogueData,
// and the option never appeared). See PropertyReflection::append_object_to_array.
inline constexpr std::wstring_view kFieldExtraRootDialogueData = L"ExtraRootDialogueData";

// Clicking a dialogue option routes through both of these; whichever fires
// first wins and the other is ignored for that press.
inline constexpr std::wstring_view kOnDialogueClicked = L"OnDialogueClicked";
inline constexpr std::wstring_view kHandleDialogueClicked = L"HandleDialogueClicked";
inline constexpr std::wstring_view kFieldDialogueData = L"DialogueData";
inline constexpr std::wstring_view kFieldClickedDialogueData = L"ClickedDialogueData";
inline constexpr std::wstring_view kCloseUI = L"CloseUI";

// Fields set on our own dialogue-option data object.
inline constexpr std::wstring_view kFieldInputText = L"InputText";
inline constexpr std::wstring_view kFieldResponseText = L"ResponseText";

// --- scoping the search to reachable storage ------------------------------
// UWECraftingComponent lives on the player character and its RegisteredSourceIds
// is the set of communal inventories the GAME itself considers reachable from
// where the player is standing -- the exact set the fabricator draws from. Using
// it means the terminal shows the same storage the base's own crafting does,
// with no invented radius. Falls back to every communal inventory if it is
// empty (see SnapshotBuilder).
inline constexpr std::wstring_view kCraftingComponentClass = L"UWECraftingComponent";
inline constexpr std::wstring_view kFieldRegisteredSourceIds = L"RegisteredSourceIds";

}
