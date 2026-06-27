#include "ro.h"
#include <cstdio>
#include <cstdint>

namespace ro {

bool is_occasion(const cell_config& cfg, uint32_t sfn, uint32_t slot) {
    return (sfn % cfg.prach_x == cfg.prach_y) && (slot == cfg.prach_subframe);
}

uint32_t system_slot(uint32_t sfn, uint32_t slot) {
    return sfn * 10 + slot;
}

void from_system_slot(uint32_t sys_slot, uint32_t& sfn, uint32_t& slot) {
    sfn  = sys_slot / 10;
    slot = sys_slot % 10;
}

uint32_t next_occasion_slot(const cell_config& cfg, uint32_t after_slot) {
    uint32_t sfn, slot;
    from_system_slot(after_slot + 1, sfn, slot);

    uint32_t x     = cfg.prach_x;
    uint32_t y     = cfg.prach_y;
    uint32_t target_sf = cfg.prach_subframe;

    // Find next SFN where sfn % x == y
    uint32_t remainder = sfn % x;
    uint32_t target_sfn;
    if (remainder <= y) {
        target_sfn = sfn - remainder + y;
    } else {
        target_sfn = sfn - remainder + x + y;
    }

    uint32_t candidate = system_slot(target_sfn, target_sf);
    if (candidate <= after_slot) {
        target_sfn += x;
        candidate = system_slot(target_sfn, target_sf);
    }

    return candidate;
}

void print_occasions_in_range(const cell_config& cfg, uint32_t start_sf_slot, uint32_t end_sf_slot) {
    uint32_t sfn_start, slot_start;
    from_system_slot(start_sf_slot, sfn_start, slot_start);
    uint32_t sfn_end, slot_end;
    from_system_slot(end_sf_slot, sfn_end, slot_end);

    printf("PRACH occasions between slot %u (SFN %u.%u) and slot %u (SFN %u.%u):\n",
           start_sf_slot, sfn_start, slot_start, end_sf_slot, sfn_end, slot_end);

    int count = 0;
    for (uint32_t s = start_sf_slot; s <= end_sf_slot; s++) {
        uint32_t sf, sl;
        from_system_slot(s, sf, sl);
        if (is_occasion(cfg, sf, sl)) {
            printf("  %u.  system_slot=%u  SFN=%u  slot=%u\n", ++count, s, sf, sl);
        }
    }
    printf("Total: %d occasions\n", count);
}

} // namespace ro
