#include "inject.h"
#include <cstdio>

bool injector::init(const cell_config& cfg, dl_grid& dl, rar_builder& rar, msg4_builder& m4) {
    m_cfg  = cfg;
    m_dl   = &dl;
    m_rar  = &rar;
    m_msg4 = &m4;
    m_initialized = true;
    printf("Injector initialized (TX gain=%.1f dB)\n", m_tx_gain);
    return true;
}

bool injector::inject_msg2(uint32_t sfn, uint32_t slot, uint16_t tc_rnti, uint16_t ta_cmd) {
    // TODO M2: Schedule and transmit Msg2 with timed RF TX
    fprintf(stderr, "TODO M2: inject_msg2 not yet implemented (needs Radio B TX)\n");
    (void)sfn; (void)slot; (void)tc_rnti; (void)ta_cmd;
    return false;
}

bool injector::inject_msg4(uint32_t sfn, uint32_t slot, uint16_t tc_rnti,
                           const uint8_t* cid, uint32_t cid_len) {
    // TODO M3: Schedule and transmit Msg4
    fprintf(stderr, "TODO M3: inject_msg4 not yet implemented (needs Radio B TX)\n");
    (void)sfn; (void)slot; (void)tc_rnti; (void)cid; (void)cid_len;
    return false;
}
