#include "ssb_sync.h"
#include "srsran/common/band_helper.h"
#include <cstdio>
#include <cstring>

ssb_sync::~ssb_sync() {
    if (m_initialized) {
        srsran_ssb_free(&m_ssb);
    }
}

bool ssb_sync::init(const cell_config& cfg, double rx_srate_hz) {
    m_cfg = cfg;
    
    // Initialize SSB object
    srsran_ssb_args_t args = {};
    args.max_srate_hz     = rx_srate_hz;
    args.min_scs          = srsran_subcarrier_spacing_15kHz;
    args.enable_search    = true;
    args.enable_measure   = true;
    args.enable_encode    = false;
    args.enable_decode    = true;
    args.disable_polar_simd = false;
    args.pbch_dmrs_thr    = 0; // default
    
    if (srsran_ssb_init(&m_ssb, &args) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_ssb_init failed\n");
        return false;
    }
    
    // Configure SSB
    srsran_ssb_cfg_t ssb_cfg = {};
    ssb_cfg.srate_hz        = rx_srate_hz;
    ssb_cfg.center_freq_hz  = cfg.dl_freq_hz;
    ssb_cfg.ssb_freq_hz     = cfg.ssb_freq_hz;
    ssb_cfg.scs             = cfg.ssb_scs;
    ssb_cfg.pattern         = cfg.ssb_pattern;
    ssb_cfg.duplex_mode     = cfg.duplex_mode;
    ssb_cfg.periodicity_ms  = cfg.ssb_period_ms;
    ssb_cfg.beta_pss        = SRSRAN_SSB_DEFAULT_BETA;
    ssb_cfg.beta_sss        = SRSRAN_SSB_DEFAULT_BETA;
    ssb_cfg.beta_pbch       = SRSRAN_SSB_DEFAULT_BETA;
    ssb_cfg.beta_pbch_dmrs  = SRSRAN_SSB_DEFAULT_BETA;
    ssb_cfg.scaling         = 0; // default
    
    if (srsran_ssb_set_cfg(&m_ssb, &ssb_cfg) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_ssb_set_cfg failed\n");
        return false;
    }
    
    m_initialized = true;
    return true;
}

bool ssb_sync::search(const cf_t* in, uint32_t nof_samples, uint32_t* out_pci,
                      uint32_t* out_sfn, uint32_t* out_ssb_idx,
                      uint32_t* out_t_offset, float* out_snr_db,
                      srsran_mib_nr_t* out_mib, bool* out_hrf,
                      float* out_cfo_hz) {
    if (!m_initialized) {
        fprintf(stderr, "ERROR: ssb_sync not initialized\n");
        return false;
    }
    
    srsran_ssb_search_res_t result = {};
    
    int ret = srsran_ssb_search(&m_ssb, in, nof_samples, &result);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_ssb_search returned %d\n", ret);
        return false;
    }
    
    // Check PBCH CRC
    if (!result.pbch_msg.crc) {
        fprintf(stderr, "SSB: PSS/SSS found (N_id=%u) but PBCH CRC failed. SNR=%.1f dB\n",
                result.N_id, result.measurements.snr_dB);
        if (out_snr_db) *out_snr_db = result.measurements.snr_dB;
        return false;
    }
    
    uint32_t N_id = result.N_id;
    m_t_offset    = result.t_offset;
    m_snr_db      = result.measurements.snr_dB;
    
    // Unpack MIB from PBCH message
    memset(&m_mib, 0, sizeof(m_mib));
    srsran_pbch_msg_nr_mib_unpack(&result.pbch_msg, &m_mib);
    
    // Compute SFN: The PBCH payload contains SFN 4 LSBs, 
    // with additional bits from PBCH DMRS. The SSB search recovers
    // the full SFN through the SSB index and half-frame bit.
    uint32_t sfn_4lsb = result.pbch_msg.sfn_4lsb;
    uint32_t ssb_idx  = result.pbch_msg.ssb_idx; // 0-7 for Lmax=8, or 3 LSB only
    bool     hrf      = result.pbch_msg.hrf;
    
    // For Lmax=8 (3-6 GHz): SSB index occupies bits 0-2 from PBCH payload
    // Higher bits from DMRS
    // SFN: 6 MSB from PBCH TTI, 4 LSB from PBCH payload
    // For Pattern A / 15 kHz / FDD / Lmax=4: SSB index bits 0-1
    // We don't know full SFN from a single PBCH decode alone without context.
    // The SSB search in srsRAN provides the SFN through TTI tracking.
    // Here we report what we have.
    
    // With periodicity 10ms, SSB in first or second half-frame
    // The SFN 4 LSBs are extracted from the PBCH payload
    uint32_t sfn = sfn_4lsb; // partial - need context for MSBs
    
    *out_pci      = N_id;
    *out_sfn      = sfn;
    *out_ssb_idx  = ssb_idx;
    *out_t_offset = m_t_offset;
    if (out_snr_db) *out_snr_db = m_snr_db;
    if (out_mib)    *out_mib    = m_mib;
    if (out_hrf)    *out_hrf    = hrf;
    if (out_cfo_hz) *out_cfo_hz = result.measurements.cfo_hz;
    
    char info_str[256];
    srsran_pbch_msg_info(&result.pbch_msg, info_str, sizeof(info_str));
    printf("SSB found: N_id=%u SFN_4lsb=%u ssb_idx=%u hrf=%d snr=%.1f dB\n",
           N_id, sfn_4lsb, ssb_idx, hrf, m_snr_db);
    printf("  PBCH: %s\n", info_str);
    printf("  MIB: coreset0_idx=%u ss0_idx=%u cell_barred=%d\n",
           m_mib.coreset0_idx, m_mib.ss0_idx, m_mib.cell_barred);
    
    return true;
}

bool ssb_sync::track(const cf_t* sf_buffer, uint32_t N_id, uint32_t ssb_idx,
                     uint32_t n_hf, srsran_mib_nr_t* out_mib, float* out_snr_db) {
    if (!m_initialized) return false;
    
    srsran_csi_trs_measurements_t meas = {};
    srsran_pbch_msg_nr_t pbch_msg = {};
    
    int ret = srsran_ssb_track(&m_ssb, sf_buffer, N_id, ssb_idx, n_hf, &meas, &pbch_msg);
    if (ret != SRSRAN_SUCCESS || !pbch_msg.crc) {
        fprintf(stderr, "SSB track: failed (ret=%d, crc=%d)\n", ret, pbch_msg.crc);
        return false;
    }
    
    m_snr_db = meas.snr_dB;
    memset(&m_mib, 0, sizeof(m_mib));
    srsran_pbch_msg_nr_mib_unpack(&pbch_msg, &m_mib);
    
    if (out_mib)    *out_mib     = m_mib;
    if (out_snr_db) *out_snr_db  = m_snr_db;
    
    return true;
}
