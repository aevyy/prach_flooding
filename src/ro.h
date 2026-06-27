#pragma once

#include "cell_config.h"
#include <cstdint>

namespace ro {

// Check if a given (sfn, slot) is a PRACH occasion for this cell.
// config_idx=1: format 0, subframe 4, SFN % 16 == 1
bool is_occasion(const cell_config& cfg, uint32_t sfn, uint32_t slot);

// Get the system slot number for a given (sfn, slot_in_frame).
// With 15 kHz SCS (mu=0): system_slot = sfn * 10 + slot
uint32_t system_slot(uint32_t sfn, uint32_t slot);

// Calculate SFN and slot from a system slot number.
void from_system_slot(uint32_t sys_slot, uint32_t& sfn, uint32_t& slot);

// Get the next PRACH occasion's system slot number on or after `after_slot`.
uint32_t next_occasion_slot(const cell_config& cfg, uint32_t after_slot);

// Print all PRACH occasions in a given range of system slots.
void print_occasions_in_range(const cell_config& cfg, uint32_t start_slot, uint32_t end_slot);

} // namespace ro
