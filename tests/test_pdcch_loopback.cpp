// M2 gate: PDCCH loopback test
// Encode a DCI 1_0 for RAR (RA-RNTI=57), run DMRS channel estimation,
// then decode it back using srsRAN's PDCCH receiver.

#include "cell_config.h"
#include "dl_grid.h"
#include "ra_rnti.h"
extern "C" {
#include "srsran/phy/phch/pdcch_nr.h"
#include "srsran/phy/phch/dci_nr.h"
#include "srsran/phy/ch_estimation/dmrs_pdcch.h"
#include "srsran/phy/common/phy_common_nr.h"
}
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>

int main() {
    printf("=== PDCCH Loopback Test (M2 gate) ===\n\n");

    cell_config cfg;
    cfg.pci              = 0;
    cfg.dl_freq_hz       = 1842.5e6;
    cfg.nof_prb          = 106;
    cfg.scs              = srsran_subcarrier_spacing_15kHz;
    cfg.ssb_scs          = srsran_subcarrier_spacing_15kHz;
    cfg.coreset0_idx     = 12;
    cfg.ss0_idx          = 0;

    dl_grid grid;
    if (!grid.init(cfg)) {
        fprintf(stderr, "FATAL: dl_grid init failed\n");
        return 1;
    }

    uint16_t ra_rnti = cell_ra_rnti();
    printf("Encoding PDCCH DCI 1_0 (RA-RNTI=0x%04x)...\n", ra_rnti);
    
    if (!grid.encode_pdcch_rar(ra_rnti, 0)) {
        fprintf(stderr, "FAIL: PDCCH encode failed\n");
        return 1;
    }
    printf("  PDCCH encode: OK\n");

    // Run the DMRS channel estimator
    printf("\nRunning PDCCH DMRS channel estimator...\n");
    
    srsran_dmrs_pdcch_estimator_t est = {};
    int ret = srsran_dmrs_pdcch_estimator_init(&est, &grid.get_carrier(), &grid.get_coreset0());
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "FAIL: estimator init failed\n");
        return 1;
    }
    
    srsran_slot_cfg_t slot_cfg = {};
    slot_cfg.idx = 0;
    ret = srsran_dmrs_pdcch_estimate(&est, &slot_cfg, grid.get_slot_grid());
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "FAIL: DMRS estimate failed\n");
        srsran_dmrs_pdcch_estimator_free(&est);
        return 1;
    }
    printf("  DMRS estimate: OK\n");
    
    // Get channel estimates for our candidate (L=4, nCCE=0)
    srsran_dci_location_t loc = {};
    loc.ncce = 0;
    loc.L    = 2;  // log2(4)
    
    srsran_dmrs_pdcch_ce_t ce = {};
    ret = srsran_dmrs_pdcch_get_ce(&est, &loc, &ce);
    srsran_dmrs_pdcch_estimator_free(&est);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "FAIL: get CE failed\n");
        return 1;
    }
    printf("  Channel estimate: %u RE\n", ce.nof_re);
    
    // Decode
    printf("\nDecoding PDCCH...\n");
    
    srsran_pdcch_nr_t pdcch_rx = {};
    srsran_pdcch_nr_args_t rx_args = {};
    rx_args.disable_simd = false;
    rx_args.measure_evm  = true;
    rx_args.measure_time = false;

    ret = srsran_pdcch_nr_init_rx(&pdcch_rx, &rx_args);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "FAIL: pdcch init_rx failed\n");
        return 1;
    }
    ret = srsran_pdcch_nr_set_carrier(&pdcch_rx, &grid.get_carrier(), &grid.get_coreset0());
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "FAIL: pdcch set_carrier failed\n");
        srsran_pdcch_nr_free(&pdcch_rx);
        return 1;
    }
    
    srsran_dci_msg_nr_t dci_msg_rx = {};
    dci_msg_rx.nof_bits            = 41;  // MUST set: decode uses this for K = nof_bits+24
    dci_msg_rx.ctx.ss_type          = srsran_search_space_type_common_1;
    dci_msg_rx.ctx.coreset_id       = 0;
    dci_msg_rx.ctx.coreset_start_rb = grid.get_coreset0().offset_rb;
    dci_msg_rx.ctx.rnti_type        = srsran_rnti_type_ra;
    dci_msg_rx.ctx.format           = srsran_dci_format_nr_1_0;
    dci_msg_rx.ctx.rnti             = ra_rnti;
    dci_msg_rx.ctx.location.ncce    = 0;
    dci_msg_rx.ctx.location.L       = 2;  // log2(4) — logarithmic AL

    srsran_pdcch_nr_res_t result = {};
    ret = srsran_pdcch_nr_decode(&pdcch_rx, grid.get_slot_grid(), &ce, &dci_msg_rx, &result);
    srsran_pdcch_nr_free(&pdcch_rx);

    printf("  Decode ret=%d crc=%s evm=%.2f%%\n", ret, result.crc ? "PASS" : "FAIL", result.evm);

    if (ret == SRSRAN_SUCCESS && result.crc) {
        printf("\nLOOPBACK PASS: PDCCH DCI 1_0 (RA-RNTI=0x%04x) encoded and decoded.\n", ra_rnti);
        return 0;
    } else {
        printf("\nLOOPBACK FAIL: PDCCH CRC=%s\n", result.crc ? "pass" : "fail");
        return 1;
    }
}
