#include "NoaTerminal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "HookTargets.hpp"
#include "PropertyReflection.hpp"
#include "ReflectionUtils.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace StorageTerminal {

namespace {

// What NoA offers, and what she says when you pick it. The response is shown
// for an instant before the storage screen takes over.
constexpr const wchar_t* kOptionLabel = L"Access storage network";
constexpr const wchar_t* kOptionResponse =
    L"Indexing every container linked to this base. Stand by.";

void log_line(const std::wstring& message)
{
    Output::send<LogLevel::Verbose>(STR("[StorageTerminal] {}\n"), message);
}

}

// Writes InputText/ResponseText and VERIFIES the write by reading it back.
//
// Why this is not just "write it once": UE4SS copies FText shallowly and, by
// its own admission in FText.cpp, "we were treating FText as a POD type
// anyway" -- the assignment never takes a reference on the underlying shared
// text data. A mod-built FText that is consumed immediately (the screen title,
// handed straight to ProcessEvent) is fine; one parked in a long-lived UObject
// field for minutes before the UI reads it is not. In game the option button
// rendered, and rendered BLANK (2026-07-29).
//
// So: write, read back, and report. Callers re-run this on every terminal
// rescan, so a field that comes back empty gets rewritten rather than staying
// blank forever.
bool NoaTerminal::ensure_option_text(UObject* data)
{
    if (ReflectionUtils::is_dead(data)) {
        return false;
    }

    auto* class_private = data->GetClassPrivate();
    auto* prompt_field = PropertyReflection::find_property(class_private, StorageTerminalTargets::kFieldInputPrompt);
    auto* input_field = PropertyReflection::find_property(class_private, StorageTerminalTargets::kFieldInputText);
    auto* response_field = PropertyReflection::find_property(class_private, StorageTerminalTargets::kFieldResponseText);
    if (!input_field && !prompt_field) {
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(data);

    const auto current_input = input_field
        ? PropertyReflection::read_text(input_field, base).value_or(std::wstring{})
        : std::wstring{};
    const auto current_prompt = prompt_field
        ? PropertyReflection::read_text(prompt_field, base).value_or(std::wstring{})
        : std::wstring{};

    const bool input_ok = !input_field || current_input == kOptionLabel;
    const bool prompt_ok = !prompt_field || current_prompt == kOptionLabel;
    if (input_ok && prompt_ok) {
        return true;
    }

    // Write BOTH label fields. Only one of them is what WBP_CTI_Button_C
    // actually renders, and which one is decided in Blueprint bytecode that the
    // SDK dump does not show. Writing InputText alone produced a blank button
    // in game more than once, so the label goes in both; whichever the widget
    // reads, it now finds the right string.
    if (input_field) {
        PropertyReflection::write_text(input_field, base, kOptionLabel);
    }
    if (prompt_field) {
        PropertyReflection::write_text(prompt_field, base, kOptionLabel);
    }
    if (response_field) {
        PropertyReflection::write_text(response_field, base, kOptionResponse);
    }

    const auto after = input_field
        ? PropertyReflection::read_text(input_field, base).value_or(std::wstring{})
        : PropertyReflection::read_text(prompt_field, base).value_or(std::wstring{});
    if (after != kOptionLabel) {
        log_line(L"NoA: option label did not stick (read back '" + after + L"').");
        return false;
    }
    return true;
}

// Logs what the GAME's own working dialogue options hold in each of the three
// FText fields, once per session.
//
// This exists because the option button kept rendering blank while our write
// verifiably landed in InputText. Rather than guess again at which field the
// button binds, this prints the real values from options that DO render
// ("Missing Colonists", "Survival Guide", ...) so the field can be identified
// from evidence. Pure reads, first terminal only, one time.
void NoaTerminal::log_reference_option_fields(UObject* component)
{
    if (m_loggedReferenceFields || ReflectionUtils::is_dead(component)) {
        return;
    }
    // NOT latched here. The array can still be unpopulated on the first rescan
    // after world load, and latching on that would mean the diagnostic never
    // runs -- the same "gave up after one attempt" mistake this project has
    // made three times. The flag is set only once something is actually logged.

    // Sample ExtraRootDialogueData, NOT DefaultRootDialogueData.
    //
    // Confirmed in game (2026-07-30, UE4SS.log line 1052): DefaultRootDialogueData
    // is EMPTY on these components. The options the player actually sees --
    // "Missing Colonists", "Survival Guide" and the rest -- live in
    // ExtraRootDialogueData, which is the very array the mod appends to. So the
    // game's own working options sit right beside ours, in the same array, and
    // are the correct reference for which FText field carries the label.
    auto* extra_field = PropertyReflection::find_property(
        component->GetClassPrivate(), StorageTerminalTargets::kFieldExtraRootDialogueData);
    if (!extra_field) {
        log_line(L"NoA: no ExtraRootDialogueData to sample.");
        return;
    }

    const auto view = PropertyReflection::read_array(extra_field, reinterpret_cast<uint8_t*>(component));
    if (!view || !view->data || view->count <= 0) {
        log_line(L"NoA: ExtraRootDialogueData is empty; nothing to sample.");
        return;
    }

    log_line(L"NoA: sampling the game's own options (of " + std::to_wstring(view->count)
             + L") to find which FText field is the label:");

    int32_t sampled = 0;
    for (int32_t index = 0; index < view->count && sampled < 4; ++index) {
        UObject* option = nullptr;
        std::memcpy(&option, view->element_at(index), sizeof(option));
        if (ReflectionUtils::is_dead(option)) {
            continue;
        }
        // Skip our own options -- they are the ones rendering blank, so they
        // are the question, not the reference.
        const bool ours = std::any_of(m_options.begin(), m_options.end(), [&](const TerminalOption& o) {
            return o.data == option;
        });
        if (ours) {
            continue;
        }
        ++sampled;
        auto* option_class = option->GetClassPrivate();
        auto* base = reinterpret_cast<uint8_t*>(option);

        const auto read_field = [&](std::wstring_view name) -> std::wstring {
            auto* field = PropertyReflection::find_property(option_class, name);
            return field ? PropertyReflection::read_text(field, base).value_or(std::wstring{}) : std::wstring{L"<no field>"};
        };

        log_line(L"  [" + std::to_wstring(index) + L"] InputPrompt='" + read_field(StorageTerminalTargets::kFieldInputPrompt)
                 + L"' InputText='" + read_field(StorageTerminalTargets::kFieldInputText)
                 + L"' ResponseText='" + read_field(StorageTerminalTargets::kFieldResponseText) + L"'");
    }

    // Latch only on success, so an empty array early in world load is retried.
    if (sampled > 0) {
        m_loggedReferenceFields = true;
    }
}

UObject* NoaTerminal::create_option_for(UObject* component)
{
    // Outered to the terminal's own component, so the option's lifetime is
    // tied to the terminal it belongs to and nothing else has to keep it
    // alive. One data object per terminal rather than one shared object
    // avoids any question of a dead outer taking down an option that other
    // terminals still reference.
    auto* data = ReflectionUtils::construct_object(StorageTerminalTargets::kDialogueDataClass, component);
    if (!data) {
        return nullptr;
    }

    if (!PropertyReflection::find_property(data->GetClassPrivate(), StorageTerminalTargets::kFieldInputText)) {
        log_line(L"NoA: dialogue data has no InputText; option not created.");
        return nullptr;
    }

    ensure_option_text(data);
    return data;
}

int32_t NoaTerminal::refresh_terminals()
{
    // The live components, enumerated ONCE and used both as the work list and
    // as the liveness oracle for existing bookkeeping.
    const auto components = ReflectionUtils::find_all(StorageTerminalTargets::kComputerTextInterfaceComponentClass);

    const auto is_live = [&components](UObject* component) {
        return component != nullptr
            && std::find(components.begin(), components.end(), component) != components.end();
    };

    // ---- 1. Drop bookkeeping for components that no longer exist ----------
    //
    // Nothing used to remove entries here, and that caused two confirmed bugs:
    //
    //  (a) Going to the main menu and reloading a save destroys every component
    //      from the previous world. The stale entries stayed, and because the
    //      "already handled this terminal?" test below compared POINTERS, a new
    //      component that happened to reuse a dead one's address was treated as
    //      already done -- so the option was never installed and vanished for
    //      the rest of the session.
    //  (b) ensure_option_text() was re-asserting the label on the `data` object
    //      of every remembered entry, including freed ones -- a read and a write
    //      into released memory every rescan.
    //
    // A stale slot is also a correctness risk for the click hook: if a freed
    // data pointer is reused by an unrelated new object, a click on that object
    // would match by pointer and open the storage network.
    const size_t before_prune = m_options.size();
    m_options.erase(
        std::remove_if(m_options.begin(), m_options.end(), [&](const TerminalOption& option) {
            return !is_live(option.component);
        }),
        m_options.end());
    const size_t pruned = before_prune - m_options.size();

    if (pruned > 0) {
        // Republish the whole slot array so the hook never sees a dead pointer.
        for (size_t index = 0; index < kMaxTerminals; ++index) {
            UObject* const value = (index < m_options.size()) ? m_options[index].data : nullptr;
            m_optionSlots[index].store(value, std::memory_order_release);
        }
        log_line(L"NoA: dropped " + std::to_wstring(pruned)
                 + L" terminal(s) that no longer exist; " + std::to_wstring(m_options.size()) + L" remain.");
    }

    // ---- 2. Install or repair the option on every live terminal -----------
    int32_t added = 0;

    for (auto* component : components) {
        if (!component) {
            continue;
        }

        auto* extra_field = PropertyReflection::find_property(
            component->GetClassPrivate(), StorageTerminalTargets::kFieldExtraRootDialogueData);
        if (!extra_field) {
            continue;
        }
        auto* base = reinterpret_cast<uint8_t*>(component);

        log_reference_option_fields(component);

        // Is this terminal already carrying OUR option? The authoritative test
        // is the component's own array, not our bookkeeping -- the array cannot
        // be stale, and checking it makes this pass self-healing if the game
        // ever rebuilds ExtraRootDialogueData behind us. (array_contains_object
        // only compares pointers, so a dead `data` is never dereferenced.)
        const auto known = std::find_if(m_options.begin(), m_options.end(), [&](const TerminalOption& option) {
            return option.component == component;
        });
        if (known != m_options.end()) {
            if (PropertyReflection::array_contains_object(extra_field, base, known->data)) {
                // Re-assert the label: a mod-written FText in a long-lived
                // field is not guaranteed to survive, and a blank button is the
                // visible symptom. See ensure_option_text.
                ensure_option_text(known->data);
                continue;
            }
            // Our option is gone from the array but the component is alive --
            // the entry is worthless, so drop it and reinstall below.
            const size_t slot = static_cast<size_t>(std::distance(m_options.begin(), known));
            m_options.erase(known);
            for (size_t index = slot; index < kMaxTerminals; ++index) {
                UObject* const value = (index < m_options.size()) ? m_options[index].data : nullptr;
                m_optionSlots[index].store(value, std::memory_order_release);
            }
        }

        if (m_options.size() >= kMaxTerminals) {
            log_line(L"NoA: terminal limit reached; not adding the option to any more.");
            break;
        }

        auto* data = create_option_for(component);
        if (!data) {
            continue;
        }

        if (!PropertyReflection::append_object_to_array(extra_field, base, data)) {
            log_line(L"NoA: could not append the option to ExtraRootDialogueData.");
            continue;
        }

        // Publish to the hook's view BEFORE recording it locally, so a click
        // can never arrive for an option the hook does not yet recognise.
        m_optionSlots[m_options.size()].store(data, std::memory_order_release);
        m_options.push_back(TerminalOption{component, data});
        ++added;
    }

    if (added > 0) {
        log_line(L"NoA: storage option added to " + std::to_wstring(added)
                 + L" terminal(s); " + std::to_wstring(m_options.size()) + L" total.");
    }
    return added;
}

bool NoaTerminal::try_install_click_watch()
{
    if (m_clickWatchInstalled) {
        return true;
    }

    auto* component = ReflectionUtils::find_first(StorageTerminalTargets::kComputerTextInterfaceComponentClass);
    if (!component) {
        return false;
    }

    // A click routes through both of these. Watch both and let the first one
    // to arrive win; the flag makes the second a no-op.
    auto* on_clicked = ReflectionUtils::find_function(component, StorageTerminalTargets::kOnDialogueClicked);
    auto* handle_clicked = ReflectionUtils::find_function(component, StorageTerminalTargets::kHandleDialogueClicked);
    if (!on_clicked && !handle_clicked) {
        return false;
    }

    auto* on_clicked_param = on_clicked
        ? PropertyReflection::find_property(on_clicked, StorageTerminalTargets::kFieldDialogueData)
        : nullptr;
    auto* handle_clicked_param = handle_clicked
        ? PropertyReflection::find_property(handle_clicked, StorageTerminalTargets::kFieldClickedDialogueData)
        : nullptr;
    if (!on_clicked_param && !handle_clicked_param) {
        return false;
    }

    const Hook::FCallbackOptions options{false, true, STR("StorageTerminal"), STR("NoaDialogueWatch")};

    Hook::RegisterProcessEventPostCallback(
        [this, on_clicked, handle_clicked, on_clicked_param, handle_clicked_param](
            auto&, UObject* context, UFunction* function, void* params) {
            if (!params) {
                return;
            }

            FProperty* data_param = nullptr;
            if (function == on_clicked && on_clicked_param) {
                data_param = on_clicked_param;
            } else if (function == handle_clicked && handle_clicked_param) {
                data_param = handle_clicked_param;
            } else {
                return;
            }

            auto* clicked = PropertyReflection::read_object(data_param, static_cast<const uint8_t*>(params));
            if (!clicked) {
                return;
            }

            // Pointer identity against the published slots. Nothing here
            // dereferences a game object beyond the params buffer already
            // materialized for this call, and nothing calls back into the
            // game -- the actual work happens on the next mod update.
            for (auto& slot : m_optionSlots) {
                auto* ours = slot.load(std::memory_order_acquire);
                if (!ours) {
                    continue;
                }
                if (ours == clicked) {
                    m_pendingComponent.store(context, std::memory_order_relaxed);
                    m_openRequested.store(true, std::memory_order_release);
                    return;
                }
            }
        },
        options);

    m_clickWatchInstalled = true;
    log_line(L"NoA: watching for the storage option to be picked.");
    return true;
}

bool NoaTerminal::consume_open_request()
{
    return m_openRequested.exchange(false, std::memory_order_acquire);
}

void NoaTerminal::close_terminal_ui()
{
    auto* component = m_pendingComponent.exchange(nullptr, std::memory_order_relaxed);
    if (!component) {
        return;
    }

    // Only ever call CloseUI on a component we know is one of the terminals we
    // installed onto -- the hook's `context` is whatever object ProcessEvent
    // was dispatching on, and it should not be trusted blindly.
    const bool ours = std::any_of(m_options.begin(), m_options.end(), [&](const TerminalOption& option) {
        return option.component == component;
    });
    if (!ours) {
        return;
    }
    // The pointer was captured on the game thread when the option was clicked
    // and is dispatched on here a check later. A terminal destroyed in between
    // (the player deconstructing it, or a level transition) would otherwise be
    // ProcessEvent'd after teardown.
    if (ReflectionUtils::is_dead(component)) {
        return;
    }

    auto* close = ReflectionUtils::find_function(component, StorageTerminalTargets::kCloseUI);
    if (!close) {
        return;
    }
    std::vector<uint8_t> params(static_cast<size_t>(close->GetPropertiesSize()), 0);
    component->ProcessEvent(close, params.empty() ? nullptr : params.data());
    log_line(L"NoA: closed the terminal dialogue.");
}

}
