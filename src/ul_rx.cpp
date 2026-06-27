#include "ul_rx.h"
#include <cstdio>
#include <cstring>

ul_rx::~ul_rx() {
    if (m_initialized) {
        srsran_prach_free(&m_prach);
    }
}

bool ul_rx::init(const cell_config& cfg) {
    m_cfg = cfg;
    
    // Initialize PRACH object
    uint32_t max_N_ifft_ul = srsran_min_symbol_sz_rb(cfg.nof_prb);
    if (srsran_prach_init(&m_prach, max_N_ifft_ul) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_prach_init failed\n");
        return false;
    }
    
    // Configure PRACH
    srsran_prach_cfg_t prach_cfg = {};
    prach_cfg.is_nr              = true;
    prach_cfg.config_idx         = cfg.prach_config_idx;
    prach_cfg.root_seq_idx       = cfg.prach_root_seq_idx;
    prach_cfg.zero_corr_zone     = cfg.prach_zcz;
    prach_cfg.freq_offset        = cfg.prach_freq_offset;
    prach_cfg.num_ra_preambles   = cfg.num_ra_preambles;
    prach_cfg.hs_flag            = false;
    prach_cfg.enable_successive_cancellation = false;
    prach_cfg.enable_freq_domain_offset_calc = false;
    
    if (srsran_prach_set_cfg(&m_prach, &prach_cfg, cfg.nof_prb) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_prach_set_cfg failed\n");
        return false;
    }
    
    m_initialized = true;
    printf("UL RX (PRACH) initialized: config_idx=%u root_seq=%u zcz=%u freq_offset=%u\n",
           cfg.prach_config_idx, cfg.prach_root_seq_idx, cfg.prach_zcz, cfg.prach_freq_offset);
    return true;
}

int ul_rx::detect_prach(const cf_t* signal, uint32_t sig_len,
                        uint32_t* out_indices, float* out_ta_offsets,
                        float* out_metrics, uint32_t max_detect) {
    if (!m_initialized) return 0;
    
    uint32_t n_detected = 0;
    float peak_to_avg = 0;
    
    int ret = srsran_prach_detect_offset(&m_prach,
                                         m_cfg.prach_freq_offset,
                                         (cf_t*)signal,
                                         sig_len,
                                         out_indices,
                                         out_ta_offsets,
                                         &peak_to_avg,
                                         &n_detected);
    
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "PRACH detect: srsran_prach_detect_offset returned %d\n", ret);
        return 0;
    }
    
    // Limit to max_detect
    if (n_detected > max_detect) n_detected = max_detect;
    
    // Note: srsran_prach_detect_offset returns indices but not detection metrics
    // Set metrics to peak_to_avg for each detection
    if (out_metrics) {
        for (uint32_t i = 0; i < n_detected; i++) {
            out_metrics[i] = peak_to_avg;
        }
    }
    
    return (int)n_detected;
}
