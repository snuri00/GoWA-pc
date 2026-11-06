#include "edge_glue.h"

namespace edge::glue {

int32_t attr_map_find(const uint8_t* ls, uint32_t base, uint32_t id) {
    uint32_t n = ls[((base & 0x3fff0) + 2) & kLsMask];
    if (n == 0) return -1;
    if (ls[((base & 0x3fff0) + 11) & kLsMask] == (id & 0xff)) return 0;
    for (uint32_t k = 1; k < n; k++) {
        uint32_t a = base + 11 + 8 * k;
        if (ls[((a & 0x3fff0) + (a & 15)) & kLsMask] == (id & 0xff)) return int32_t(k);
    }
    return -1;
}

int32_t slot_find(const uint8_t* ls, uint32_t ctx, uint32_t id) {
    uint32_t c0 = ls[ctx & kLsMask];
    uint32_t count = (c0 & 15) + ((c0 & 0x80) ? 1 : 0);
    uint32_t base = (ctx + 352) & 0x3fff0;
    uint32_t idx = 16;
    for (uint32_t i = 0; i < 16; i++)
        if (ls[base + i] == (id & 0xff)) {
            idx = i;
            break;
        }
    if (int32_t(count) > int32_t(idx)) return int32_t(idx);
    return -1;
}

uint32_t slot_table(const uint8_t* ls, uint32_t ctx, uint32_t id) {
    int32_t s = slot_find(ls, ctx, id);
    if (s < 0) return 0;
    return rd32(ls, ctx + 16 + 4 * uint32_t(s));
}

uint32_t slot_set(uint8_t* ls, uint32_t ctx, uint32_t id, uint32_t value) {
    int32_t s = slot_find(ls, ctx, id);
    if (s < 0) return 0xffffffffu;
    ls[((ctx + 352) & 0x3fff0) + (uint32_t(s) & 15)] = uint8_t(value);
    return uint32_t(s);
}

uint32_t slot_alloc(uint8_t* ls, uint32_t ctx, uint32_t id) { return slot_set(ls, ctx, 0, id); }

}
