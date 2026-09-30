#pragma once

#include <Windows.h>

#include <cstdint>
#include <string_view>
#include <vector>

// MSVC RTTI lookups in the game executable: the vtable of a class from its decorated type name
// (".?AVFrpgFlipper_60FPS@NS_FRPG@@"), the class of an object from its vtable. The executable keeps
// its RTTI (FromSoftware's classes are all there), so these survive game updates where fixed
// addresses would not.
namespace dsru::game::rtti {

// Vtables (primary, offset 0 in the complete object) of the class named `decoratedName` in `module`
// (nullptr = the executable). Usually one; empty when the class isn't there.
std::vector<uintptr_t> FindVTables(std::string_view decoratedName, HMODULE module = nullptr);

// The decorated type name of the class whose vtable is `vtable`, or empty if `vtable` doesn't look
// like one (no complete object locator in front of it). Reads memory that may be invalid under SEH.
std::string_view ClassOf(uintptr_t vtable, HMODULE module = nullptr);

}  // namespace dsru::game::rtti
