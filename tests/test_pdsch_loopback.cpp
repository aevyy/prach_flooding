// M2 gate: PDSCH loopback test
// Encode a MAC RAR PDU, then decode it back.

#include "cell_config.h"
#include "dl_grid.h"
#include "rar.h"
#include "ra_rnti.h"
extern "C" {
#include "srsran/phy/phch/pdsch_nr.h"
#include "srsran/phy/phch/sch_cfg_nr.h"
#include "srsran/phy/common/phy_common_nr.h"
#include "srsran/phy/ch_estimation/chest_dl.h"
#include "srsran/phy/fec/softbuffer.h"
}
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <array>
#include <array>

int main() {
    printf("=== PDSCH Loopback Test (M2 gate) ===\n\n");

    cell_config cfg;
    cfg.pci              = 0;
    cfg.dl_freq_hz       = 1842.5e6;
    cfg.nof_prb          = 106;
    cfg.scs              = srsran_subcarrier_spacing_15kHz;
    cfg.ssb_scs          = srsran_subcarrier_spacing_15kHz;
    cfg.coreset0_idx     = 12;
    cfg.ss0_idx          = 0;
    cfg.mcs_table        = srsran_mcs_table_64qam;

    dl_grid grid;
    if (!grid.init(cfg)) {
        fprintf(stderr, "FATAL: dl_grid init failed\n");
        return 1;
    }

    // Build RAR
    rar_builder rar;
    uint16_t ta_cmd = 4;
    uint16_t tc_rnti = 0x4601;
    std::array<uint8_t, 27> ul_grant = {};
    rar_builder::build_ul_grant(8, 4, 0, 0, ul_grant);
    uint8_t rar_pdu[16] = {};
    uint32_t rar_len = rar.build(ta_cmd, tc_rnti, ul_grant, rar_pdu, sizeof(rar_pdu));
    printf("RAR PDU: %u bytes (TA=%u TC-RNTI=0x%04x)\n", rar_len, ta_cmd, tc_rnti);

    // PDCCH encode (sets ra_rnti context)
    uint16_t ra_rnti = cell_ra_rnti();
    if (!grid.encode_pdcch_rar(ra_rnti, 0)) {
        fprintf(stderr, "FAIL: PDCCH encode prerequisite\n");
        return 1;
    }

    // PDSCH encode
    printf("Encoding PDSCH RAR...\n");
    if (!grid.encode_pdsch_rar(rar_pdu, rar_len, 0)) {
        fprintf(stderr, "FAIL: PDSCH encode failed\n");
        return 1;
    }
    printf("  PDSCH encode: OK\n");

    // Decode
    printf("\nDecoding PDSCH...\n");

    srsran_pdsch_nr_t pdsch_rx = {};
    srsran_pdsch_nr_args_t rx_args = {};
    rx_args.sch.max_nof_iter = 6;
    rx_args.measure_evm      = true;
    rx_args.measure_time     = false;
    rx_args.max_prb          = cfg.nof_prb;
    rx_args.max_layers       = 1;

    int ret = srsran_pdsch_nr_init_ue(&pdsch_rx, &rx_args);
    assert(ret == SRSRAN_SUCCESS);
    ret = srsran_pdsch_nr_set_carrier(&pdsch_rx, &grid.get_carrier());
    assert(ret == SRSRAN_SUCCESS);

    // Config for decode
    srsran_sch_cfg_nr_t sch_cfg_rx = {};
    sch_cfg_rx.grant.rnti      = ra_rnti;
    sch_cfg_rx.grant.rnti_type = srsran_rnti_type_ra;
    sch_cfg_rx.grant.k         = 0;
    sch_cfg_rx.grant.S         = 2;
    sch_cfg_rx.grant.L         = 12;
    sch_cfg_rx.grant.mapping   = srsran_sch_mapping_type_A;
    sch_cfg_rx.grant.nof_layers = 1;
    sch_cfg_rx.grant.dci_format = srsran_dci_format_nr_1_0;
    sch_cfg_rx.grant.nof_dmrs_cdm_groups_without_data = 0;
    sch_cfg_rx.grant.beta_dmrs  = 0;
    for (uint32_t i = 0; i < 4; i++) sch_cfg_rx.grant.prb_idx[i] = true;
    sch_cfg_rx.grant.nof_prb = 4;

    sch_cfg_rx.grant.tb[0].mod      = SRSRAN_MOD_QPSK;
    sch_cfg_rx.grant.tb[0].mcs      = 0;
    sch_cfg_rx.grant.tb[0].tbs      = rar_len;
    sch_cfg_rx.grant.tb[0].R        = 0.12;
    sch_cfg_rx.grant.tb[0].rv       = 0;
    sch_cfg_rx.grant.tb[0].ndi      = 0;
    sch_cfg_rx.grant.tb[0].N_L      = 1;
    sch_cfg_rx.grant.tb[0].enabled  = true;
    sch_cfg_rx.grant.tb[0].cw_idx   = 0;
    sch_cfg_rx.grant.tb[0].nof_re   = 576; // MUST match encoder
    sch_cfg_rx.grant.tb[0].nof_bits = 1152;

    sch_cfg_rx.dmrs.type           = srsran_dmrs_sch_type_1;
    sch_cfg_rx.dmrs.typeA_pos      = srsran_dmrs_sch_typeA_pos_2;
    sch_cfg_rx.dmrs.length         = srsran_dmrs_sch_len_1;
    sch_cfg_rx.dmrs.additional_pos = srsran_dmrs_sch_add_pos_2;
    sch_cfg_rx.sch_cfg.mcs_table       = cfg.mcs_table;
    sch_cfg_rx.sch_cfg.xoverhead       = srsran_xoverhead_0;
    sch_cfg_rx.sch_cfg.limited_buffer_rm = false;

    // Initialize channel estimate properly
    srsran_chest_dl_res_t channel = {};
    if (srsran_chest_dl_res_init_re(&channel, sch_cfg_rx.grant.tb[0].nof_re) != SRSRAN_SUCCESS) {
        fprintf(stderr, "FAIL: chest_dl_res_init_re failed\n");
        return 1;
    }
    srsran_chest_dl_res_set_identity(&channel);
    channel.noise_estimate = 0.001f;

    cf_t* sf_symbols[SRSRAN_MAX_PORTS] = {};
    sf_symbols[0] = grid.get_slot_grid();

    // Softbuffer
    srsran_softbuffer_rx_t sb = {};
    srsran_softbuffer_rx_init(&sb, 1);
    sch_cfg_rx.grant.tb[0].softbuffer.rx = &sb;

    srsran_pdsch_res_nr_t res = {};
    // Pre-allocate payload buffer for decoded TB
    uint8_t dec_payload[512] = {};
    res.tb[0].payload = dec_payload;
    ret = srsran_pdsch_nr_decode(&pdsch_rx, &sch_cfg_rx, &sch_cfg_rx.grant, &channel, sf_symbols, &res);

    // Clean up CE
    srsran_chest_dl_res_free(&channel);
    srsran_softbuffer_rx_free(&sb);
    srsran_pdsch_nr_free(&pdsch_rx);

    printf("  Decode ret=%d\n", ret);
    printf("  TB0 CRC=%s avg_iter=%.1f\n", res.tb[0].crc ? "PASS" : "FAIL", res.tb[0].avg_iter);
    // Debug: print first bytes even if CRC failed
    printf("  Decoded: ");
    for (int i = 0; i < (int)rar_len && i < 10; i++) printf("%02x ", dec_payload[i]);
    printf("\n  Expected:");
    for (uint32_t i = 0; i < rar_len && i < 10; i++) printf("%02x ", rar_pdu[i]);
    printf("\n");

    if (ret == SRSRAN_SUCCESS && res.tb[0].crc) {
        bool match = (memcmp(dec_payload, rar_pdu, rar_len) == 0);
        if (match) {
            printf("\nLOOPBACK PASS: PDSCH RAR encoded and decoded successfully.\n");
            return 0;
        }
        printf("\nLOOPBACK FAIL: Payload mismatch.\n");
    } else {
        printf("  TB0 CRC=FAIL\n");
        printf("LOOPBACK FAIL: PDSCH CRC failed.\n");
    }
    return 1;
}
