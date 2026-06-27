#pragma once

#include <cstdint>

// Compute RA-RNTI per TS 38.321 §5.1.3:
// RA-RNTI = 1 + s_id + 14*t_id + 14*80*f_id + 14*80*8*ul_carrier_id
//
// For format 0 with SCS=15kHz on FDD:
//   s_id = 0 (first symbol index of PRACH)
//   t_id = slot index within SFN (0..79 for 15kHz, 1ms slots)
//          For PRACH subframe 4: t_id = 4
//   f_id = 0 (msg1-FDM=1)
//   ul_carrier_id = 0 (NUL)
//
// Expected value: 1 + 0 + 14*4 = 57 = 0x39
uint16_t compute_ra_rnti(uint32_t s_id, uint32_t t_id, uint32_t f_id, uint32_t ul_carrier_id);

// Convenience: compute RA-RNTI for this cell's RACH occasion
// s_id=0, t_id=4, f_id=0, ul_carrier_id=0 -> 57
uint16_t cell_ra_rnti();
