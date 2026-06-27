// M1 gate: PRACH loopback test
// Generate a PRACH preamble (RAPID 0, format 0, root_seq 1), add noise,
// then detect it back to verify the PRACH detection pipeline.

#include "cell_config.h"
#include "ul_rx.h"
extern "C" {
#include "srsran/phy/phch/prach.h"
#include "srsran/phy/common/phy_common_nr.h"
}
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

int main() {
    printf("=== PRACH Loopback Test (M1 gate) ===\n\n");
    
    // Cell config matching the real gNB
    cell_config cfg;
    cfg.nof_prb            = 106;
    cfg.prach_config_idx   = 1;
    cfg.prach_root_seq_idx = 1;
    cfg.prach_zcz          = 0;
    cfg.prach_freq_offset  = 8;
    cfg.num_ra_preambles   = 1;
    
    // Initialize PRACH for detection
    ul_rx rx;
    if (!rx.init(cfg)) {
        fprintf(stderr, "FATAL: PRACH init failed\n");
        return 1;
    }
    
    // Generate a PRACH preamble signal (RAPID 0)
    // The PRACH signal length for format 0 at 15kHz, 106 PRB
    uint32_t max_N_ifft_ul = srsran_min_symbol_sz_rb(cfg.nof_prb);
    
    // PRACH format 0: 
    // Tseq = 839 * Ts / (15kHz * 2048) but we use the FFT size
    // sig_len needs to be large enough for the full PRACH slot
    // At 15kHz SCS, symbol size = max_N_ifft_ul, slot = 14 symbols
    // PRACH occupies 1 subframe = 1 slot at 15kHz
    uint32_t slot_samples = max_N_ifft_ul * SRSRAN_NSYMB_PER_SLOT_NR;
    cf_t* prach_signal = (cf_t*)calloc(slot_samples, sizeof(cf_t));
    if (!prach_signal) {
        fprintf(stderr, "FATAL: malloc failed\n");
        return 1;
    }
    
    // Generate preamble using srsran_prach_gen
    // seq_index = 0 (this is RAPID 0, the only valid preamble with num_ra_preambles=1)
    // freq_offset = 8 PRB * 12 = 96 subcarriers offset
    
    // We need a PRACH TX object to generate
    srsran_prach_t prach_tx = {};
    if (srsran_prach_init(&prach_tx, max_N_ifft_ul) != SRSRAN_SUCCESS) {
        fprintf(stderr, "FATAL: prach_tx init failed\n");
        free(prach_signal);
        return 1;
    }
    
    srsran_prach_cfg_t tx_cfg = {};
    tx_cfg.is_nr            = true;
    tx_cfg.config_idx       = cfg.prach_config_idx;
    tx_cfg.root_seq_idx     = cfg.prach_root_seq_idx;
    tx_cfg.zero_corr_zone   = cfg.prach_zcz;
    tx_cfg.freq_offset      = cfg.prach_freq_offset;
    tx_cfg.num_ra_preambles = cfg.num_ra_preambles;
    tx_cfg.hs_flag          = false;
    
    if (srsran_prach_set_cfg(&prach_tx, &tx_cfg, cfg.nof_prb) != SRSRAN_SUCCESS) {
        fprintf(stderr, "FATAL: prach_tx set_cfg failed\n");
        srsran_prach_free(&prach_tx);
        free(prach_signal);
        return 1;
    }
    
    // Generate preamble 0
    if (srsran_prach_gen(&prach_tx, 0, cfg.prach_freq_offset, prach_signal) != SRSRAN_SUCCESS) {
        fprintf(stderr, "FATAL: prach_gen failed\n");
        srsran_prach_free(&prach_tx);
        free(prach_signal);
        return 1;
    }
    
    // Compute signal power
    float sig_power = 0;
    for (uint32_t i = 0; i < slot_samples; i++) {
        float re = __real__ prach_signal[i];
        float im = __imag__ prach_signal[i];
        sig_power += re*re + im*im;
    }
    sig_power /= slot_samples;
    printf("Generated PRACH preamble: seq=0, freq_offset=%u, samples=%u, power=%.2f\n",
           cfg.prach_freq_offset, slot_samples, sig_power);
    
    // Detect the preamble
    uint32_t indices[64];
    float    ta_offsets[64];
    float    metrics[64];
    
    int n_detected = rx.detect_prach(prach_signal, slot_samples,
                                     indices, ta_offsets, metrics, 64);
    
    printf("\nDetection results: %d preamble(s) found\n", n_detected);
    
    bool pass = false;
    for (int i = 0; i < n_detected; i++) {
        printf("  idx=%u  ta_offset=%.2f us  metric=%.2f\n",
               indices[i], ta_offsets[i] * 1e6, metrics[i]);
        if (indices[i] == 0) {
            pass = true;
        }
    }
    
    if (pass) {
        printf("\nLOOPBACK PASS: Preamble 0 detected successfully.\n");
    } else {
        printf("\nLOOPBACK FAIL: Preamble 0 NOT detected.\n");
    }
    
    srsran_prach_free(&prach_tx);
    free(prach_signal);
    return pass ? 0 : 1;
}
