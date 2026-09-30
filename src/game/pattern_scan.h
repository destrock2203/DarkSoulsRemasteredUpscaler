#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace dsru::game {

// Finds an IDA-style byte pattern ("48 8B 05 ?? ?? ?? ?? 48 85 C0") in the executable sections of
// `module` (nullptr = main executable). Returns the address of the first match.
std::optional<uintptr_t> FindPattern(std::string_view pattern, HMODULE module = nullptr);

// Resolves a RIP-relative operand: `instruction` points at the instruction, the 32-bit
// displacement is at `instruction + offset`, and the instruction is `length` bytes long.
inline uintptr_t ResolveRelative(uintptr_t instruction, size_t offset, size_t length) {
    const int32_t disp = *reinterpret_cast<const int32_t*>(instruction + offset);
    return instruction + length + disp;
}

}  // namespace dsru::game
