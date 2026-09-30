#include "game/shader_db.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace dsru::game::shaders {

namespace {

constexpr Info kTable[] = {
#include "game/shader_table.inc"
};

constexpr bool Sorted() {
    for (size_t i = 1; i < std::size(kTable); ++i)
        if (!(kTable[i - 1].id < kTable[i].id)) return false;
    return true;
}
static_assert(Sorted(), "shader_table.inc must be sorted by id (regenerate it with tools/gen_shader_table.py)");

}  // namespace

uint64_t IdOf(const void* bytecode, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    if (bytes && size >= 32 && std::memcmp(bytes, "DXBC", 4) == 0) {
        uint64_t id = 0;
        std::memcpy(&id, bytes + 4, sizeof(id));
        return id;
    }
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; bytes && i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash | (1ull << 63);
}

const Info* Find(uint64_t id) {
    const auto it = std::lower_bound(std::begin(kTable), std::end(kTable), id,
                                     [](const Info& info, uint64_t value) { return info.id < value; });
    return it != std::end(kTable) && it->id == id ? &*it : nullptr;
}

std::string_view FirstName(const Info* info) {
    if (!info) return "?";
    const std::string_view names = info->names;
    return names.substr(0, names.find('|'));
}

size_t Count() { return std::size(kTable); }

}  // namespace dsru::game::shaders
