#include "PropertyReflection.hpp"

#include <cstring>

#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>

using namespace RC::Unreal;

namespace StorageTerminal {

namespace {

// Resolves a field name to an FName WITHOUT interning it.
//
// FName's default ctor mode is FNAME_Add, which would add every name we look
// up to the engine's global name pool -- including the speculative ones (the
// "Name" / "Name_0" candidate pairs) that are expected to miss. FNAME_Find
// only consults the existing pool, exactly as UE4SS's own BPMacros.hpp does.
//
// A name that is not in the pool comes back as NAME_None. That must be
// reported as "not found" rather than compared, or a lookup for a field that
// does not exist would match any nameless property instead of failing.
// Returns false for that case, which preserves the old loop's behaviour.
bool resolve_field_name(std::wstring_view name, FName& out_name)
{
    out_name = FName(name, FNAME_Find);
    return !out_name.IsNone();
}

}

FProperty* PropertyReflection::find_property(UStruct* owner, std::wstring_view name)
{
    if (!owner) {
        return nullptr;
    }

    // UStruct::FindProperty is a real compiled UE4SS function that already
    // does exactly what this used to hand-roll, and does it better:
    //
    //  - it iterates with EFieldIterationFlags::IncludeSuper, so it still
    //    finds fields declared on a base class when the live object's class is
    //    a Blueprint subclass. That is the ninth-lesson bug (ForEachProperty()
    //    walks only the struct's OWN properties and silently reported real
    //    fields as missing); the fix is preserved, not lost.
    //  - it compares FNames -- one integer compare -- where the old loop
    //    called FProperty::GetName(), which is GetFName().ToString() and so
    //    allocated a std::wstring for EVERY property it inspected. On a
    //    Blueprint widget class that was hundreds of allocations per lookup.
    //  - it memoizes results in a static map keyed by {UStruct*, FName}, so
    //    repeat lookups (this mod does the same handful constantly) cost a
    //    hash probe instead of a chain walk.
    //
    // See docs/UE4SS_API_REFERENCE.md.
    FName field_name{};
    if (!resolve_field_name(name, field_name)) {
        return nullptr;
    }

    return owner->FindProperty(field_name);
}

FProperty* PropertyReflection::find_struct_field(FProperty* struct_property, std::wstring_view field_name)
{
    auto* as_struct = CastField<FStructProperty>(struct_property);
    if (!as_struct) {
        return nullptr;
    }

    UStruct* script_struct = as_struct->GetStruct().Get();
    if (!script_struct) {
        return nullptr;
    }

    FName resolved{};
    if (!resolve_field_name(field_name, resolved)) {
        return nullptr;
    }

    return script_struct->FindProperty(resolved);
}

std::optional<int32_t> PropertyReflection::read_int(FProperty* property, const uint8_t* container_base)
{
    auto* as_int = CastField<FIntProperty>(property);
    if (!as_int || !container_base) {
        return std::nullopt;
    }

    return *as_int->ContainerPtrToValuePtr<int32_t>(const_cast<uint8_t*>(container_base));
}

bool PropertyReflection::write_byte(FProperty* property, uint8_t* container_base, uint8_t value)
{
    if (!property || !container_base) {
        return false;
    }

    if (auto* as_byte = CastField<FByteProperty>(property)) {
        *as_byte->ContainerPtrToValuePtr<uint8_t>(container_base) = value;
        return true;
    }

    // FEnumProperty stores the value through its underlying numeric property
    // at the same offset; every enum this project touches is uint8-backed
    // (confirmed against the Dumper-7 param structs before use).
    if (auto* as_enum = CastField<FEnumProperty>(property)) {
        *as_enum->ContainerPtrToValuePtr<uint8_t>(container_base) = value;
        return true;
    }

    return false;
}

std::optional<std::wstring> PropertyReflection::read_text(FProperty* property, const uint8_t* container_base)
{
    auto* as_text = CastField<FTextProperty>(property);
    if (!as_text || !container_base) {
        return std::nullopt;
    }

    auto* text = as_text->ContainerPtrToValuePtr<FText>(const_cast<uint8_t*>(container_base));
    if (!text) {
        return std::nullopt;
    }
    return std::wstring(text->ToString());
}

bool PropertyReflection::write_text(FProperty* property, uint8_t* container_base, const std::wstring& value)
{
    auto* as_text = CastField<FTextProperty>(property);
    if (!as_text || !container_base) {
        return false;
    }

    *as_text->ContainerPtrToValuePtr<FText>(container_base) = FText(value.c_str());
    return true;
}

std::optional<bool> PropertyReflection::read_bool(FProperty* property, const uint8_t* container_base)
{
    auto* as_bool = CastField<FBoolProperty>(property);
    if (!as_bool || !container_base) {
        return std::nullopt;
    }

    return as_bool->GetPropertyValueInContainer(const_cast<uint8_t*>(container_base));
}

bool PropertyReflection::write_bool(FProperty* property, uint8_t* container_base, bool value)
{
    auto* as_bool = CastField<FBoolProperty>(property);
    if (!as_bool || !container_base) {
        return false;
    }

    as_bool->SetPropertyValueInContainer(container_base, value);
    return true;
}

UObject* PropertyReflection::read_object(FProperty* property, const uint8_t* container_base)
{
    auto* as_object = CastField<FObjectProperty>(property);
    if (!as_object || !container_base) {
        return nullptr;
    }

    return *as_object->ContainerPtrToValuePtr<UObject*>(const_cast<uint8_t*>(container_base));
}

bool PropertyReflection::write_object(FProperty* property, uint8_t* container_base, UObject* value)
{
    auto* as_object = CastField<FObjectProperty>(property);
    if (!as_object || !container_base) {
        return false;
    }

    *as_object->ContainerPtrToValuePtr<UObject*>(container_base) = value;
    return true;
}

std::optional<PropertyReflection::ArrayView> PropertyReflection::read_array(FProperty* property, uint8_t* container_base)
{
    auto* as_array = CastField<FArrayProperty>(property);
    if (!as_array || !container_base) {
        return std::nullopt;
    }

    auto* inner = as_array->GetInner();
    if (!inner) {
        return std::nullopt;
    }

    auto* script_array = as_array->ContainerPtrToValuePtr<FScriptArray>(container_base);
    if (!script_array) {
        return std::nullopt;
    }

    ArrayView view{};
    view.inner = inner;
    view.data = static_cast<uint8_t*>(script_array->GetData());
    view.count = script_array->Num();
    view.element_size = inner->GetSize();
    return view;
}

std::vector<int32_t> PropertyReflection::read_int_array(FProperty* property, uint8_t* container_base)
{
    std::vector<int32_t> values;

    const auto view = read_array(property, container_base);
    if (!view || !CastField<FIntProperty>(view->inner) || !view->data) {
        return values;
    }

    values.reserve(static_cast<size_t>(view->count));
    for (int32_t index = 0; index < view->count; ++index) {
        int32_t value = 0;
        std::memcpy(&value, view->element_at(index), sizeof(value));
        values.push_back(value);
    }
    return values;
}

bool PropertyReflection::array_contains_object(FProperty* property, uint8_t* container_base, UObject* value)
{
    const auto view = read_array(property, container_base);
    if (!view || !CastField<FObjectProperty>(view->inner) || !view->data) {
        return false;
    }

    for (int32_t index = 0; index < view->count; ++index) {
        UObject* element = nullptr;
        std::memcpy(&element, view->element_at(index), sizeof(element));
        if (element == value) {
            return true;
        }
    }
    return false;
}

bool PropertyReflection::append_object_to_array(FProperty* property, uint8_t* container_base, UObject* value)
{
    auto* as_array = CastField<FArrayProperty>(property);
    if (!as_array || !container_base) {
        return false;
    }

    auto* inner = as_array->GetInner();
    if (!inner || !CastField<FObjectProperty>(inner)) {
        return false;
    }

    auto* script_array = as_array->ContainerPtrToValuePtr<FScriptArray>(container_base);
    if (!script_array) {
        return false;
    }

    // The container's own real growth logic -- the same code path the engine
    // uses -- not a hand-rolled reallocation.
    constexpr int32_t element_size = sizeof(UObject*);
    constexpr uint32_t element_alignment = alignof(UObject*);
    const int32_t index = script_array->AddZeroed(1, element_size, element_alignment);

    auto* slot = reinterpret_cast<UObject**>(
        static_cast<uint8_t*>(script_array->GetData()) + static_cast<size_t>(index) * element_size);
    *slot = value;
    return true;
}

}
