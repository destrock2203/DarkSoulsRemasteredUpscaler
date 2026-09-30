#include "game/rtti.h"

#include <algorithm>
#include <cstring>

namespace dsru::game::rtti {

namespace {

// x64 RTTI: the qword before a vtable points to its RTTICompleteObjectLocator, which holds
// image-relative offsets (signature 1).
struct CompleteObjectLocator {
    uint32_t signature;  // 1 on x64
    uint32_t offset;     // of this vtable in the complete object
    uint32_t cdOffset;
    uint32_t typeDescriptor;  // RVA of the TypeDescriptor
    uint32_t classDescriptor;
    uint32_t self;  // RVA of this locator
};

struct Image {
    uintptr_t base = 0;
    size_t size = 0;
};

Image ImageOf(HMODULE module) {
    if (!module) module = GetModuleHandleW(nullptr);
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return {base, nt->OptionalHeader.SizeOfImage};
}

// Searches the image's non-executable, initialized sections (RTTI and vtables live in .rdata / .data).
template <class Fn>
void ForEachDataSection(const Image& image, Fn&& fn) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_UNINITIALIZED_DATA)) continue;
        const size_t size = (std::min)(static_cast<size_t>(section->Misc.VirtualSize),
                                       static_cast<size_t>(section->SizeOfRawData));
        fn(image.base + section->VirtualAddress, size);
    }
}

}  // namespace

std::vector<uintptr_t> FindVTables(std::string_view decoratedName, HMODULE module) {
    std::vector<uintptr_t> result;
    const Image image = ImageOf(module);

    // 1. The TypeDescriptor: {vftable ptr, spare ptr, char name[]}: the name at +0x10.
    std::vector<uint32_t> typeDescriptors;
    ForEachDataSection(image, [&](uintptr_t begin, size_t size) {
        const auto* bytes = reinterpret_cast<const char*>(begin);
        for (size_t i = 0x10; i + decoratedName.size() + 1 <= size; ++i) {
            if (bytes[i] != decoratedName[0] || std::memcmp(bytes + i, decoratedName.data(), decoratedName.size()) != 0 ||
                bytes[i + decoratedName.size()] != '\0')
                continue;
            typeDescriptors.push_back(static_cast<uint32_t>(begin + i - 0x10 - image.base));
        }
    });
    if (typeDescriptors.empty()) return result;

    // 2. Complete object locators referring to it (signature 1, offset 0, self RVA matches).
    std::vector<uintptr_t> locators;
    ForEachDataSection(image, [&](uintptr_t begin, size_t size) {
        for (size_t i = 0; i + sizeof(CompleteObjectLocator) <= size; i += 4) {
            const auto* col = reinterpret_cast<const CompleteObjectLocator*>(begin + i);
            if (col->signature != 1 || col->offset != 0) continue;
            if (col->self != static_cast<uint32_t>(begin + i - image.base)) continue;
            for (uint32_t td : typeDescriptors)
                if (col->typeDescriptor == td) locators.push_back(begin + i);
        }
    });

    // 3. The vtable follows the pointer to its locator.
    ForEachDataSection(image, [&](uintptr_t begin, size_t size) {
        for (size_t i = 0; i + 16 <= size; i += 8) {
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(begin + i);
            for (uintptr_t col : locators)
                if (value == col) result.push_back(begin + i + 8);
        }
    });
    return result;
}

std::string_view ClassOf(uintptr_t vtable, HMODULE module) {
    const Image image = ImageOf(module);
    if (vtable < image.base + 8 || vtable >= image.base + image.size) return {};
    __try {
        const uintptr_t colAddress = *reinterpret_cast<const uintptr_t*>(vtable - 8);
        if (colAddress < image.base || colAddress + sizeof(CompleteObjectLocator) > image.base + image.size) return {};
        const auto* col = reinterpret_cast<const CompleteObjectLocator*>(colAddress);
        if (col->signature != 1 || col->self != colAddress - image.base) return {};
        const uintptr_t td = image.base + col->typeDescriptor;
        if (td + 0x10 >= image.base + image.size) return {};
        return std::string_view(reinterpret_cast<const char*>(td + 0x10));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
}

}  // namespace dsru::game::rtti
