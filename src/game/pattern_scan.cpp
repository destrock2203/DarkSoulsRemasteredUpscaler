#include "game/pattern_scan.h"

#include <cctype>
#include <span>
#include <string>
#include <vector>

namespace dsru::game {

namespace {

// -1 = wildcard
std::vector<int> ParsePattern(std::string_view pattern) {
    std::vector<int> bytes;
    size_t i = 0;
    while (i < pattern.size()) {
        if (std::isspace(static_cast<unsigned char>(pattern[i]))) {
            ++i;
        } else if (pattern[i] == '?') {
            bytes.push_back(-1);
            while (i < pattern.size() && pattern[i] == '?') ++i;
        } else {
            bytes.push_back(std::stoi(std::string(pattern.substr(i, 2)), nullptr, 16));
            i += 2;
        }
    }
    return bytes;
}

std::optional<uintptr_t> Scan(std::span<const uint8_t> memory, const std::vector<int>& pattern) {
    if (pattern.empty() || memory.size() < pattern.size()) return std::nullopt;
    const size_t last = memory.size() - pattern.size();
    for (size_t i = 0; i <= last; ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] != -1 && memory[i + j] != static_cast<uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) return reinterpret_cast<uintptr_t>(memory.data() + i);
    }
    return std::nullopt;
}

}  // namespace

std::optional<uintptr_t> FindPattern(std::string_view pattern, HMODULE module) {
    if (!module) module = GetModuleHandleW(nullptr);
    const auto base = reinterpret_cast<const uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    const std::vector<int> bytes = ParsePattern(pattern);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::span<const uint8_t> memory(base + section->VirtualAddress, section->Misc.VirtualSize);
        if (auto hit = Scan(memory, bytes)) return hit;
    }
    return std::nullopt;
}

}  // namespace dsru::game
