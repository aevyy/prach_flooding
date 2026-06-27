#include "ra_rnti.h"

uint16_t compute_ra_rnti(uint32_t s_id, uint32_t t_id, uint32_t f_id, uint32_t ul_carrier_id) {
    return 1 + s_id + 14 * t_id + 14 * 80 * f_id + 14 * 80 * 8 * ul_carrier_id;
}

uint16_t cell_ra_rnti() {
    // format 0: s_id=0, t_id=4, f_id=0, ul_carrier_id=0
    return compute_ra_rnti(0, 4, 0, 0);
}
