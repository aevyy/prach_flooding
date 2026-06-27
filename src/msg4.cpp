#include "msg4.h"
#include <cstdio>

uint32_t msg4_builder::build(const uint8_t* cid, uint32_t cid_len,
                             uint8_t* out_pdu, uint32_t max_len) {
    // TODO M3: Build contention resolution PDU
    fprintf(stderr, "TODO M3: msg4_builder::build not yet implemented\n");
    (void)cid; (void)cid_len; (void)out_pdu; (void)max_len;
    return 0;
}
