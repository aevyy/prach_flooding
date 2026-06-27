#include "ro.h"
#include <cstdio>

namespace ro {

bool is_occasion(const cell_config& cfg, uint32_t sfn, uint32_t slot) {
    // config_idx=1: subframe 4, SFN % 16 == 1, FDD
    return (sfn % 16 == 1) && (slot == 4);
}

uint32_t system_slot(uint32_t sfn, uint32_t slot) {
    return sfn * 10 + slot;
}

void from_system_slot(uint32_t sys_slot, uint32_t& sfn, uint32_t& slot) {
    sfn  = sys_slot / 10;
    slot = sys_slot % 10;
}

uint32_t next_occasion_slot(const cell_config& cfg, uint32_t after_slot) {
    // Find the next (sfn,slot) with sfn%16==1 and slot==4, strictly AFTER after_slot
    uint32_t sfn, slot;
    from_system_slot(after_slot + 1, sfn, slot);
    
    // Find next SFN where sfn%16==1
    uint32_t remainder = sfn % 16;
    uint32_t target_sfn;
    if (remainder <= 1) {
        target_sfn = sfn - remainder + 1;
    } else {
        target_sfn = sfn - remainder + 17;
    }
    
    uint32_t candidate = system_slot(target_sfn, 4);
    if (candidate <= after_slot) {
        target_sfn += 16;
        candidate = system_slot(target_sfn, 4);
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
