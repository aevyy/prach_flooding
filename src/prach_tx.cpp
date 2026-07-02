// prach_tx.cpp — PRACH preamble generator and RF transmitter
//
// Uses srsran_rf_t (srsRAN's RF abstraction) instead of raw UHD to avoid
// Boost.DateTime compatibility issues with newer GCC.
//
// Key design decisions:
//
// 1. FREQUENCY CORRECTNESS
//    RF TX center frequency = ul_freq_hz (1747.5 MHz for n3).
//    srsran_prach_gen() places the preamble at freq_offset PRBs *within*
//    the UL carrier; this is a baseband shift only — NOT added to HW freq.
//
// 2. TIMING
//    srsran_rf_send_timed: schedules burst with end_of_burst flag.
//    The gNB PRACH detector only correlates within the configured RACH
//    occasion. The usable timing-error budget ≈ CP − round-trip ≈ 103 us for
//    format 0 (co-located → ~full CP). The device-time anchor must therefore be
//    accurate to well within one OFDM symbol (~71 us at 15 kHz SCS).
//
// 3. RA-RNTI LOGGING
//    Compute per TS 38.321 §5.1.3 and log for gNB correlation.

#include "prach_tx.h"
#include "ra_rnti.h"
extern "C" {
#include "srsran/phy/common/phy_common_nr.h"
#include "srsran/phy/rf/rf.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <random>

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
prach_tx::~prach_tx() {
  if (m_initialized) {
    srsran_prach_free(&m_prach);
  }
  if (m_rf_open) {
    srsran_rf_close(&m_rf);
  }
}

// ---------------------------------------------------------------------------
// init — configure from cell_config
// ---------------------------------------------------------------------------
bool prach_tx::init(const cell_config &cfg, const tool_config &tc,
                    bool dry_run) {
  m_dry_run = dry_run;
  m_cell_cfg = cfg;
  m_cfo_correct = tc.cfo.correct;
  m_cfo_sign = tc.cfo.sign;

  // --- Flood mode config ---
  m_flood_enabled = tc.flood.enabled;
  m_flood_strategy = tc.flood.strategy;
  m_flood_power_backoff_db = tc.flood.power_backoff_db;
  m_flood_slm_candidates = tc.flood.slm_candidates;
  m_flood_tx_count = 0;
  
  m_flood_indices.clear();
  std::string list_str = tc.flood.preamble_list;
  if (list_str.find(',') == std::string::npos && list_str.find('-') == std::string::npos) {
      uint32_t n = 0;
      try { n = std::stoul(list_str); } catch (...) {}
      for (uint32_t i = 0; i < n && i < 64; i++) m_flood_indices.push_back(i);
  } else {
      std::stringstream ss(list_str);
      std::string token;
      while (std::getline(ss, token, ',')) {
          auto dash = token.find('-');
          if (dash != std::string::npos) {
              try {
                  uint32_t start = std::stoul(token.substr(0, dash));
                  uint32_t end = std::stoul(token.substr(dash + 1));
                  for (uint32_t i = start; i <= end && i < 64; i++) {
                      if (std::find(m_flood_indices.begin(), m_flood_indices.end(), i) == m_flood_indices.end()) m_flood_indices.push_back(i);
                  }
              } catch (...) {}
          } else {
              try {
                  uint32_t v = std::stoul(token);
                  if (v < 64 && std::find(m_flood_indices.begin(), m_flood_indices.end(), v) == m_flood_indices.end()) m_flood_indices.push_back(v);
              } catch (...) {}
          }
      }
      std::sort(m_flood_indices.begin(), m_flood_indices.end());
  }
  
  m_flood_num_preambles = m_flood_indices.size();
  if (m_flood_num_preambles < 1) {
    m_flood_num_preambles = 1;
    m_flood_indices.push_back(0);
  }

  // --- Multi-freq-position / RA-RNTI sweep config ---
  m_multi_freq_pos_count = tc.multi_ro.freq_pos_count;
  
  if (m_multi_freq_pos_count > 1) {
    if (cfg.msg1_fdm == 1) {
      printf("\n[prach_tx] ERROR: Cannot enable multi-freq-pos (requested %u) because msg1_fdm == 1.\n", m_multi_freq_pos_count);
      printf("           The gNB has only allocated ONE frequency-domain PRACH occasion per time slot.\n");
      printf("           Injecting preambles into unallocated PRBs wastes power and will NOT be detected.\n");
      printf("           Falling back to single frequency position.\n\n");
      m_multi_freq_pos_count = 1;
    } else if (m_multi_freq_pos_count > cfg.msg1_fdm) {
      printf("\n[prach_tx] WARNING: freq_pos_count (%u) > msg1_fdm (%u). Clamping to %u.\n\n", 
             m_multi_freq_pos_count, cfg.msg1_fdm, cfg.msg1_fdm);
      m_multi_freq_pos_count = cfg.msg1_fdm;
    }
  }

  // Apply msg1-FrequencyStart override if set
  uint32_t freq_offset_raw = cfg.prach_freq_offset;
  if (tc.freq.msg1_freq_start_override >= 0) {
    freq_offset_raw = (uint32_t)tc.freq.msg1_freq_start_override;
    printf("[prach_tx] freq_offset overridden: %u → %u (from yaml/CLI)\n",
           cfg.prach_freq_offset, freq_offset_raw);
  }

  m_cfg.tx_freq_hz = cfg.ul_freq_hz;
  m_cfg.srate_hz = cfg.srate_hz;
  m_cfg.rapid = tc.tx.preamble_index;
  m_cfg.nof_prb = cfg.nof_prb;
  m_cfg.config_idx = cfg.prach_config_idx;
  m_cfg.root_seq = cfg.prach_root_seq_idx;
  m_cfg.zcz = cfg.prach_zcz;
  m_cfg.freq_offset = freq_offset_raw;
  m_cfg.tx_gain_db = tc.tx.gain_db;
  m_cfg.dry_run = dry_run;
  m_cfg.cfo_correct = tc.cfo.correct;
  m_cfg.cfo_sign = tc.cfo.sign;
  m_ssb_first_symbol_override = tc.timing.ssb_first_symbol_override;
  m_tx_offset_us = tc.timing.tx_offset_us;

  // Initialize srsRAN PRACH object
  uint32_t max_N_ifft_ul = srsran_min_symbol_sz_rb(cfg.nof_prb);
  printf("[prach_tx] max_N_ifft_ul = %u\n", max_N_ifft_ul);

  // Validate sample rate matches FFT size × SCS
  // If these disagree, the DAC plays the preamble at the wrong rate,
  // stretching it in time and scaling the ZC sequence in frequency.
  uint32_t scs_hz = SRSRAN_SUBC_SPACING_NR(cfg.scs);
  double expected_srate = (double)max_N_ifft_ul * scs_hz;

  if (std::abs(cfg.srate_hz - expected_srate) > 1.0) {
    fprintf(stderr, "[prach_tx] FATAL: Sample rate mismatch\n");
    fprintf(stderr, "  srate_hz from config = %.0f Hz\n", cfg.srate_hz);
    fprintf(stderr, "  expected (FFT×SCS)   = %u × %u = %.0f Hz\n",
            max_N_ifft_ul, scs_hz, expected_srate);
    return false;
  }

  printf(
      "[prach_tx] Sample rate validated: %.2f MHz = %u × %u Hz (FFT × SCS)\n",
      cfg.srate_hz / 1e6, max_N_ifft_ul, scs_hz);

  if (srsran_prach_init(&m_prach, max_N_ifft_ul) != SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_prach_init failed\n");
    return false;
  }

  srsran_prach_cfg_t prach_cfg = {};
  prach_cfg.is_nr = true;
  prach_cfg.config_idx = cfg.prach_config_idx;
  prach_cfg.root_seq_idx = cfg.prach_root_seq_idx;
  prach_cfg.zero_corr_zone = cfg.prach_zcz;
  prach_cfg.freq_offset = cfg.prach_freq_offset;
  prach_cfg.hs_flag = false;
  prach_cfg.enable_successive_cancellation = false;
  prach_cfg.enable_freq_domain_offset_calc = false;

  // CRITICAL: In flood mode, override num_ra_preambles so srsran_prach_gen
  // builds the correct cyclic shift table for ALL requested indices.
  // Without this, srsran_prach_gen fails for indices >= cfg.num_ra_preambles.
  if (m_flood_enabled) {
    prach_cfg.num_ra_preambles = m_flood_num_preambles;
    printf("[prach_tx] FLOOD: overriding num_ra_preambles %u → %u\n",
           cfg.num_ra_preambles, m_flood_num_preambles);
  } else {
    prach_cfg.num_ra_preambles = cfg.num_ra_preambles;
  }

  if (srsran_prach_set_cfg(&m_prach, &prach_cfg, cfg.nof_prb) !=
      SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_prach_set_cfg failed\n");
    srsran_prach_free(&m_prach);
    return false;
  }
  
  // Item 2: Init-time detection-ceiling estimate
  {
    // N_CS table — TS 38.211 Table 6.3.3.1-5, unrestricted set, L_RA = 839 (format 0)
    uint32_t ncs_table[16] = {0, 13, 15, 18, 22, 26, 32, 38, 46, 59, 76, 93, 119, 167, 279, 419};
    uint32_t ncs = (cfg.prach_zcz < 16) ? ncs_table[cfg.prach_zcz] : 0;
    
    // Cyclic-shift windows per root = floor(839 / N_CS); if N_CS == 0, windows_per_root = 1
    uint32_t windows_per_root = (ncs == 0) ? 1 : (839 / ncs);
    
    uint32_t total_preambles = cfg.num_ra_preambles;
    bool assumed = false;
    // If num_ra_preambles is 1 (the default initialization), we probably didn't pull a real 
    // totalNumberOfRA-Preambles from InfluxDB, so assume 64.
    if (total_preambles <= 1) {
      total_preambles = 64;
      assumed = true;
    }
    
    uint32_t roots_searched = (total_preambles + windows_per_root - 1) / windows_per_root; // ceil(total / windows_per_root)
    uint32_t estimated_ceiling = windows_per_root * roots_searched;
    
    printf("\n[prach_tx] --- PRACH Configuration & Capacity Estimate ---\n");
    printf("  N_CS (zcz=%u)            = %u (TS 38.211 Table 6.3.3.1-5)\n", cfg.prach_zcz, ncs);
    printf("  Cyclic shifts / root       = %u\n", windows_per_root);
    if (assumed) {
      printf("  Roots searched             = %u (ASSUMING total_preambles=%u)\n", roots_searched, total_preambles);
    } else {
      printf("  Roots searched             = %u (based on num_ra_preambles=%u)\n", roots_searched, total_preambles);
    }
    printf("  ESTIMATED per-RO detection ceiling = %u preambles\n", estimated_ceiling);
    printf("  NOTE: prach_zcz and prach_root_seq_idx came from InfluxDB. Verify against gNB SIB1!\n");
    printf("        (A wrong zcz silently misaligns every generated cyclic shift.)\n");
    printf("----------------------------------------------------------\n\n");
    
    if (m_flood_enabled && m_flood_num_preambles > estimated_ceiling) {
      printf("[prach_tx] WARNING: flood.num_preambles (%u) > estimated per-RO detection ceiling (%u).\n", 
             m_flood_num_preambles, estimated_ceiling);
      printf("           Excess preambles raise the noise floor and reduce reliable detections.\n\n");
    }
  }

  m_initialized = true;

  if (m_flood_enabled) {
    printf(
        "[prach_tx] FLOOD MODE: %s strategy, %u preambles, backoff=%.1f dB\n",
        m_flood_strategy.c_str(), m_flood_num_preambles,
        m_flood_power_backoff_db);
  }
  if (m_multi_freq_pos_count > 1) {
    printf("[prach_tx] MULTI-FREQ-POS: %u positions @ 7 PRB spacing%s\n",
           m_multi_freq_pos_count,
           m_multi_sweep_fid ? ", sweep_fid=ON (distinct RA-RNTIs)"
                             : ", sweep_fid=OFF");
  }
  printf("[prach_tx] Initialized:\n");
  printf("  config_idx   = %u\n", m_cfg.config_idx);
  printf("  root_seq     = %u\n", m_cfg.root_seq);
  printf("  zcz          = %u\n", m_cfg.zcz);
  printf("  freq_offset  = %u PRBs (baseband shift — NOT added to HW freq)\n",
         m_cfg.freq_offset);
  printf("  tx_freq_hz   = %.3f MHz (UL center, set once)\n",
         m_cfg.tx_freq_hz / 1e6);
  printf("  srate_hz     = %.2f MHz\n", m_cfg.srate_hz / 1e6);
  printf("  tx_gain_db   = %.1f dB\n", m_cfg.tx_gain_db);
  printf("  dry_run      = %s\n", m_dry_run ? "YES" : "NO");

  if (!generate_preamble())
    return false;

  // Generate flood preamble set if flood mode is active
  if (m_flood_enabled) {
    if (!generate_flood_preambles()) {
      fprintf(stderr, "[prach_tx] FATAL: flood preamble generation failed\n");
      return false;
    }
  }

  // Generate multi-frequency-position superimposed buffer when freq_pos_count
  // > 1. This runs AFTER flood generation so it can leverage
  // m_flood_num_preambles.
  if (m_multi_freq_pos_count > 1) {
    if (!generate_multi_freq_preambles()) {
      fprintf(stderr,
              "[prach_tx] FATAL: multi-freq preamble generation failed\n");
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// correct_freq_offset — compensate for srsran's internal PRB count mismatch.
//
// srsran_prach_gen() uses srsran_nof_prb(N_ifft_ul) internally, which for a
// 1536-pt FFT returns 100 PRB even though the actual cell has 106 PRB.  This
// shifts k_0 by ΔN_rb/2 PRBs, so we subtract that from the caller's offset.
// ---------------------------------------------------------------------------
uint32_t prach_tx::correct_freq_offset(uint32_t raw_offset) const {
  int delta_prb = (int)m_cell_cfg.nof_prb -
                  (int)srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb));
  return (uint32_t)((int)raw_offset - delta_prb / 2);
}

// ---------------------------------------------------------------------------
// superimpose_preambles_at_offset — generate all flood RAPIDs at one freq
// position and add them (normalized) into `result`.
//
// Each position's contribution is normalized to `pos_target_amplitude` before
// accumulation.  This decouples per-position power from the number of RAPIDs,
// so the combined multi-freq buffer has predictable power regardless of
// how many preambles or positions are superimposed.
// ---------------------------------------------------------------------------
bool prach_tx::superimpose_preambles_at_offset(
    uint32_t corrected_offset, std::vector<std::complex<float>> &result,
    float pos_target_amplitude) {

  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;
  std::vector<std::complex<float>> pos_buf(preamble_len, {0.0f, 0.0f});
  std::vector<std::complex<float>> tmp(preamble_len, {0.0f, 0.0f});
  cf_t *raw = reinterpret_cast<cf_t *>(tmp.data());

  uint32_t n_preambles = m_flood_enabled ? m_flood_num_preambles : 1;
  for (uint32_t p = 0; p < n_preambles; p++) {
    // Reset tmp before each gen call (srsran_prach_gen does overwrite it,
    // but be explicit for clarity)
    std::fill(tmp.begin(), tmp.end(), std::complex<float>(0.0f, 0.0f));
    uint32_t rapid = m_flood_enabled ? m_flood_indices[p] : m_cfg.rapid;
    if (srsran_prach_gen(&m_prach, rapid, corrected_offset, raw) !=
        SRSRAN_SUCCESS) {
      fprintf(stderr,
              "[prach_tx] superimpose_preambles_at_offset: gen failed RAPID=%u "
              "offset=%u\n",
              rapid, corrected_offset);
      return false;
    }
    
    // Apply phase from m_flood_phases (computed in generate_flood_preambles)
    float phase = (m_flood_enabled && p < m_flood_phases.size()) ? m_flood_phases[p] : 0.0f;
    std::complex<float> rot(std::cos(phase), std::sin(phase));
    
    for (uint32_t n = 0; n < preamble_len; n++) {
      pos_buf[n] += tmp[n] * rot;
    }
  }

  // Normalize this position's superimposed buffer to pos_target_amplitude
  // (RMS). This ensures each freq position contributes equal power regardless
  // of N_preambles.
  float power = 0.0f;
  for (uint32_t i = 0; i < preamble_len; i++)
    power += std::norm(pos_buf[i]);
  float rms = std::sqrt(power / preamble_len);
  float scale = (rms > 1e-12f) ? (pos_target_amplitude / rms) : 0.0f;
  for (uint32_t n = 0; n < preamble_len; n++) {
    result[n] += scale * pos_buf[n];
  }
  return true;
}

// ---------------------------------------------------------------------------
// generate_preamble — pre-compute the TX buffer
// ---------------------------------------------------------------------------
bool prach_tx::generate_preamble() {
  if (!m_initialized)
    return false;

  // Read actual preamble length: N_cp + N_seq samples
  // This is what srsran_prach_gen actually writes.
  // The old code allocated slot_samples, causing:
  //   1. RMS normalization error (averaging over zero tail)
  //   2. Heap overrun for longer formats (N_cp+N_seq > slot_samples)
  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;

  printf(
      "[prach_tx] PRACH format params: N_cp=%u, N_seq=%u, total=%u samples\n",
      m_prach.N_cp, m_prach.N_seq, preamble_len);

  m_tx_buf.assign(preamble_len, {0.0f, 0.0f});

  cf_t *raw_buf = reinterpret_cast<cf_t *>(m_tx_buf.data());

  // Compensate for srsran PRB-count mismatch (see correct_freq_offset())
  uint32_t freq_offset_corrected = correct_freq_offset(m_cfg.freq_offset);
  printf("[prach_tx] freq_offset: raw=%u  srsran_Nrb=%u  actual_Nrb=%u  "
         "corrected=%u\n",
         m_cfg.freq_offset,
         srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb)),
         m_cell_cfg.nof_prb, freq_offset_corrected);

  // Generate preamble: seq_index=RAPID, freq_offset as corrected
  if (srsran_prach_gen(&m_prach, m_cfg.rapid, freq_offset_corrected, raw_buf) !=
      SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_prach_gen failed\n");
    return false;
  }

  m_preamble_len_samples = preamble_len;

  // Apply CFO correction (pre-rotate TX buffer before normalization)
  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w =
        -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < m_preamble_len_samples; n++) {
      float ph = (float)(w * (double)n);
      m_tx_buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
    printf("[prach_tx] CFO correction applied: %.1f Hz UL (sign=%+d, w=%.6e "
           "rad/sample)\n",
           m_cfo_ul_hz, m_cfo_sign, w);
  }

  // Compute signal RMS power over the actual preamble (no zero tail)
  float power = 0.0f;
  for (uint32_t i = 0; i < preamble_len; i++) {
    power += std::norm(m_tx_buf[i]);
  }
  power /= preamble_len;

  printf("[prach_tx] Preamble generated: RAPID=%u, len=%u samples, RMS=%.4f\n",
         m_cfg.rapid, m_preamble_len_samples, std::sqrt(power));

  if (power < 1e-12f) {
    fprintf(
        stderr,
        "[prach_tx] WARNING: Preamble has near-zero power — check config\n");
  }

  // Normalize to 0.7 target amplitude
  float target_amp = 0.7f;
  float scale = target_amp / (std::sqrt(power) + 1e-12f);
  for (auto &s : m_tx_buf)
    s *= scale;

  printf("[prach_tx] Amplitude normalized: scale=%.3f (target=%.2f)\n", scale,
         target_amp);

  return true;
}

// ---------------------------------------------------------------------------
// generate_flood_preambles — pre-compute N preamble buffers + superimposed TX
// ---------------------------------------------------------------------------
bool prach_tx::generate_flood_preambles() {
  if (!m_initialized)
    return false;

  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;
  uint32_t freq_offset_corrected = correct_freq_offset(m_cfg.freq_offset);

  printf("[prach_tx] FLOOD: generating %u preamble buffers (len=%u samples "
         "each)...\n",
         m_flood_num_preambles, preamble_len);

  // 1. Allocate and generate individual preamble buffers
  m_flood_bufs.resize(m_flood_num_preambles);
  for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
    m_flood_bufs[p].assign(preamble_len, {0.0f, 0.0f});
    cf_t *raw = reinterpret_cast<cf_t *>(m_flood_bufs[p].data());

    uint32_t idx = m_flood_indices[p];
    if (srsran_prach_gen(&m_prach, idx, freq_offset_corrected, raw) !=
        SRSRAN_SUCCESS) {
      fprintf(stderr,
              "[prach_tx] FATAL: srsran_prach_gen failed for RAPID=%u\n", idx);
      fprintf(stderr,
              "  This usually means num_ra_preambles was not overridden.\n");
      return false;
    }
  }

  // 2. Phase optimization and Superposition
  //    Apply a per-preamble global phase to minimize PAPR of the superimposed buffer.
  m_flood_phases.assign(m_flood_num_preambles, 0.0f);
  
  if (m_flood_slm_candidates == 0) {
    // Newman phases
    for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
      m_flood_phases[p] = M_PI * (float)(p * p) / (float)m_flood_num_preambles;
    }
    printf("[prach_tx] FLOOD: Newman phase optimization applied (0 search candidates)\n");
  } else {
    // SLM search
    std::mt19937 rng(42); // fixed seed for reproducibility
    std::uniform_int_distribution<int> dist(0, 3);
    
    float best_papr = 1e9f;
    std::vector<std::complex<float>> temp_buf(preamble_len);
    std::vector<float> candidate_phases(m_flood_num_preambles, 0.0f);
    
    for (uint32_t c = 0; c < m_flood_slm_candidates; c++) {
      candidate_phases[0] = 0.0f; // phi_0 = 0 fixed
      for (uint32_t p = 1; p < m_flood_num_preambles; p++) {
        candidate_phases[p] = (float)dist(rng) * M_PI / 2.0f;
      }
      
      std::fill(temp_buf.begin(), temp_buf.end(), std::complex<float>(0.0f, 0.0f));
      float peak_mag = 0.0f;
      float power = 0.0f;
      
      for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
        std::complex<float> rot(std::cos(candidate_phases[p]), std::sin(candidate_phases[p]));
        for (uint32_t n = 0; n < preamble_len; n++) {
          temp_buf[n] += m_flood_bufs[p][n] * rot;
        }
      }
      for (uint32_t n = 0; n < preamble_len; n++) {
        float mag = std::abs(temp_buf[n]);
        if (mag > peak_mag) peak_mag = mag;
        power += std::norm(temp_buf[n]);
      }
      float rms = std::sqrt(power / preamble_len);
      float papr = (rms > 1e-12f) ? (peak_mag / rms) : 1e9f;
      
      if (papr < best_papr) {
        best_papr = papr;
        m_flood_phases = candidate_phases;
      }
    }
    printf("[prach_tx] FLOOD: SLM phase optimization applied (%u candidates), lowest PAPR=%.2f dB\n",
           m_flood_slm_candidates, 20.0f * std::log10(best_papr));
  }

  // 3. Superimpose: complex-add all N buffers sample-by-sample with best phase
  m_flood_tx_buf.assign(preamble_len, {0.0f, 0.0f});
  for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
    std::complex<float> rot(std::cos(m_flood_phases[p]), std::sin(m_flood_phases[p]));
    for (uint32_t n = 0; n < preamble_len; n++) {
      m_flood_tx_buf[n] += m_flood_bufs[p][n] * rot;
    }
  }

  // 4. CFO correction on the COMBINED buffer (one rotation, not N)
  //    CFO is a property of the radio, not the preamble.
  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w =
        -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < preamble_len; n++) {
      float ph = (float)(w * (double)n);
      m_flood_tx_buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
    printf("[prach_tx] FLOOD: CFO correction applied to superimposed buffer\n");
  }

  // Also apply CFO to individual buffers (for cycle mode)
  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w =
        -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
      for (uint32_t n = 0; n < preamble_len; n++) {
        float ph = (float)(w * (double)n);
        m_flood_bufs[p][n] *= std::complex<float>(std::cos(ph), std::sin(ph));
      }
    }
  }

  // 5. Normalize superimposed buffer (CFR + Peak Normalization)
  {
    // 5a. Compute RMS before clipping
    float power = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++)
      power += std::norm(m_flood_tx_buf[i]);
    float rms = std::sqrt(power / preamble_len);

    // 5b. Soft-clip at 1.5 × RMS to reduce PAPR (Crest Factor Reduction).
    //     This causes slight EVM distortion but prevents catastrophic peak scaling,
    //     restoring the average SNR for high N that was lost when CFR was removed
    //     in the 'major additions' commit alongside SLM phase optimization.
    float clip_level = rms * 1.5f;
    float max_mag_after_clip = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      float mag = std::abs(m_flood_tx_buf[i]);
      if (mag > clip_level) {
        m_flood_tx_buf[i] *= (clip_level / mag);
        mag = clip_level;
      }
      if (mag > max_mag_after_clip)
        max_mag_after_clip = mag;
    }

    // 5c. Normalize peak to target (e.g., 0.95 * backoff)
    float backoff_linear = std::pow(10.0f, m_flood_power_backoff_db / 20.0f);
    float target_peak = 0.95f * backoff_linear;
    float scale = target_peak / (max_mag_after_clip + 1e-12f);

    float final_power = 0.0f;
    float final_max_mag = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      m_flood_tx_buf[i] *= scale;
      final_power += std::norm(m_flood_tx_buf[i]);
      float mag = std::abs(m_flood_tx_buf[i]);
      if (mag > final_max_mag) final_max_mag = mag;
    }
    float final_rms = std::sqrt(final_power / preamble_len);
    float final_papr = 20.0f * std::log10(final_max_mag / (final_rms + 1e-12f));

    printf("[prach_tx] FLOOD superimposed (CFR): peak_target=%.2f, final_peak=%.4f, "
           "final_RMS=%.4f, PAPR=%.2f dB "
           "(scale=%.3f, backoff=%.1f dB)\n",
           target_peak, final_max_mag, final_rms, final_papr, scale, m_flood_power_backoff_db);
           
    // Dry-run self-check: unit-phase vs phase-opt
    if (m_dry_run && m_flood_num_preambles >= 1) {
      std::vector<std::complex<float>> unit_buf(preamble_len, {0.0f, 0.0f});
      for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
        for (uint32_t n = 0; n < preamble_len; n++) {
          unit_buf[n] += m_flood_bufs[p][n];
        }
      }
      
      float unit_max_mag = 0.0f;
      for (uint32_t n = 0; n < preamble_len; n++) {
        float mag = std::abs(unit_buf[n]);
        if (mag > unit_max_mag) unit_max_mag = mag;
      }
      float unit_scale = target_peak / (unit_max_mag + 1e-12f);
      for (uint32_t n = 0; n < preamble_len; n++) {
        unit_buf[n] *= unit_scale;
      }
      
      printf("[prach_tx] FLOOD DRY-RUN CHECK: Unit-phase vs Phase-opt correlation\n");
      for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
        std::complex<float> corr_opt(0.0f, 0.0f);
        std::complex<float> corr_unit(0.0f, 0.0f);
        for (uint32_t n = 0; n < preamble_len; n++) {
          corr_opt += m_flood_tx_buf[n] * std::conj(m_flood_bufs[p][n]);
          corr_unit += unit_buf[n] * std::conj(m_flood_bufs[p][n]);
        }
        float mag_opt = std::abs(corr_opt);
        float mag_unit = std::abs(corr_unit);
        printf("[prach_tx]   p=%u: opt_mag=%.2f, unit_mag=%.2f -> %s\n", 
               p, mag_opt, mag_unit, (mag_opt >= mag_unit * 0.99f) ? "PASS" : "FAIL");
      }
    }
  }

  // 6. Normalize individual buffers (for cycle mode)
  for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
    float power = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      power += std::norm(m_flood_bufs[p][i]);
    }
    power /= preamble_len;
    float target_amp = 0.7f;
    float scale = target_amp / (std::sqrt(power) + 1e-12f);
    for (auto &s : m_flood_bufs[p])
      s *= scale;
  }

  printf("[prach_tx] FLOOD: all %u preamble buffers ready\n",
         m_flood_num_preambles);
  return true;
}

// ---------------------------------------------------------------------------
// generate_multi_freq_preambles — superimpose K freq-domain positions into one
// wideband TX buffer.  Each position uses a corrected freq_offset spaced 7 PRBs
// from the previous (6 PRACH PRBs + 1 guard band PRB).
//
// When m_multi_sweep_fid=true each position k is notionally assigned f_id=k in
// the RA-RNTI formula, so the gNB treats them as K distinct RA-RNTIs and spins
// up K independent RAR MAC processes.  The waveform is identical regardless of
// sweep_fid — it is purely an accounting change in the RA-RNTI logged /
// expected.
//
// CFR is applied to the combined buffer to keep peak amplitude ≤ 0.95.
// ---------------------------------------------------------------------------
bool prach_tx::generate_multi_freq_preambles() {
  if (!m_initialized)
    return false;

  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;

  // Determine the maximum legal corrected offset.
  // srsran uses srsran_nof_prb(N_ifft_ul) as its internal N_rb_ul.
  uint32_t srsran_nrb = srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb));
  // PRACH occupies 6 PRBs; max corrected offset = srsran_nrb - 6
  uint32_t max_corrected_offset = (srsran_nrb >= 6) ? (srsran_nrb - 6) : 0;

  // Clamp the number of positions that actually fit in the UL carrier.
  uint32_t actual_positions = 0;
  m_freq_offsets_used.clear();
  
  uint32_t prach_bw_rb = 6; // Format 0 PRACH bandwidth is 6 PRBs

  for (uint32_t k = 0; k < m_multi_freq_pos_count; k++) {
    // True gNB allocation: freq_start + n * PRACH_BW_RB
    uint32_t current_offset = m_cfg.freq_offset + k * prach_bw_rb;
    uint32_t off_k = correct_freq_offset(current_offset);
    if (off_k > max_corrected_offset) {
      printf("[prach_tx] MULTI-FREQ-POS: position %u offset=%u exceeds max=%u "
             "— clamping to %u positions\n",
             k, off_k, max_corrected_offset, actual_positions);
      break;
    }
    m_freq_offsets_used.push_back(off_k);
    actual_positions++;
  }

  if (actual_positions == 0) {
    fprintf(stderr, "[prach_tx] FATAL: no valid freq positions computed\n");
    return false;
  }
  if (actual_positions < m_multi_freq_pos_count) {
    printf("[prach_tx] MULTI-FREQ-POS: reduced from %u to %u positions "
           "(carrier limit)\n",
           m_multi_freq_pos_count, actual_positions);
    m_multi_freq_pos_count = actual_positions;
  }

  printf("[prach_tx] MULTI-FREQ-POS: generating %u positions, "
         "base_corrected=%u, spacing=7 PRBs\n",
         m_multi_freq_pos_count, m_freq_offsets_used[0]);
  for (uint32_t k = 0; k < m_multi_freq_pos_count; k++) {
    uint32_t f_id = k;
    printf("[prach_tx]   pos[%u]: corrected_offset=%u, f_id=%u, RA-RNTI varies "
           "per burst\n",
           k, m_freq_offsets_used[k], f_id);
  }

  // Accumulate all positions into the combined buffer.
  // Each position is pre-normalized to the same RMS amplitude (0.7 / sqrt(K)),
  // so the total RMS ≈ 0.7 regardless of how many positions are superimposed.
  // This preserves the per-preamble SNR budget relative to single-position
  // flood mode.
  float per_pos_target = 0.7f / std::sqrt((float)m_multi_freq_pos_count);
  printf("[prach_tx] MULTI-FREQ-POS: per-position target amplitude = %.4f (%u "
         "positions)\n",
         per_pos_target, m_multi_freq_pos_count);

  m_multi_freq_tx_buf.assign(preamble_len, {0.0f, 0.0f});
  for (uint32_t k = 0; k < m_multi_freq_pos_count; k++) {
    if (!superimpose_preambles_at_offset(m_freq_offsets_used[k],
                                         m_multi_freq_tx_buf, per_pos_target)) {
      fprintf(stderr,
              "[prach_tx] FATAL: multi-freq position %u generation failed\n",
              k);
      return false;
    }
  }

  // Apply CFO correction (one rotation over the combined buffer)
  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w =
        -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < preamble_len; n++) {
      float ph = (float)(w * (double)n);
      m_multi_freq_tx_buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
    printf("[prach_tx] MULTI-FREQ-POS: CFO correction applied (%.1f Hz UL)\n",
           m_cfo_ul_hz);
  }

  // CFR + Peak normalization
  {
    float power = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++)
      power += std::norm(m_multi_freq_tx_buf[i]);
    float rms = std::sqrt(power / preamble_len);
    float clip_level = rms * 1.5f;

    float max_mag = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      float mag = std::abs(m_multi_freq_tx_buf[i]);
      if (mag > clip_level) {
        m_multi_freq_tx_buf[i] *= (clip_level / mag);
        mag = clip_level;
      }
      if (mag > max_mag)
        max_mag = mag;
    }
    float backoff_linear = std::pow(10.0f, m_flood_power_backoff_db / 20.0f);
    float target_peak = 0.95f * backoff_linear;
    float scale = target_peak / (max_mag + 1e-12f);
    float final_power = 0.0f;
    float final_max_mag = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      m_multi_freq_tx_buf[i] *= scale;
      final_power += std::norm(m_multi_freq_tx_buf[i]);
      float mag = std::abs(m_multi_freq_tx_buf[i]);
      if (mag > final_max_mag) final_max_mag = mag;
    }
    float final_rms = std::sqrt(final_power / preamble_len);
    float final_papr = 20.0f * std::log10(final_max_mag / (final_rms + 1e-12f));
    printf(
        "[prach_tx] MULTI-FREQ-POS combined (CFR): peak_target=%.2f, final_peak=%.4f, RMS=%.4f, PAPR=%.2f dB, "
        "scale=%.3f (%u positions × %u preambles each)\n",
        target_peak, final_max_mag, final_rms, final_papr, scale, m_multi_freq_pos_count,
        m_flood_enabled ? m_flood_num_preambles : 1u);
  }

  return true;
}

// ---------------------------------------------------------------------------
// get_multi_ro_ra_rntis — return all RA-RNTIs this burst will allocate at gNB.
// Caller uses this for CSV logging and console output.
// ---------------------------------------------------------------------------
std::vector<uint16_t> prach_tx::get_multi_ro_ra_rntis(uint32_t ro_slot) const {
  std::vector<uint16_t> out;
  uint32_t n_pos = (m_multi_freq_pos_count > 0) ? m_multi_freq_pos_count : 1;
  for (uint32_t k = 0; k < n_pos; k++) {
    uint32_t f_id = k; // True occasion index is k
    // RA-RNTI = 1 + s_id(0) + 14*t_id(ro_slot) + 14*80*f_id + ...
    out.push_back(compute_ra_rnti(0, ro_slot, f_id, 0));
  }
  return out;
}

// ---------------------------------------------------------------------------
// get_current_rapid_list — return human-readable RAPID description for logging
// ---------------------------------------------------------------------------
std::string prach_tx::get_current_rapid_list() const {
  if (!m_flood_enabled) {
    return std::to_string(m_cfg.rapid);
  }
  if (m_flood_strategy == "superimpose") {
    std::string s;
    for (size_t i = 0; i < m_flood_indices.size(); i++) {
        s += std::to_string(m_flood_indices[i]);
        if (i < m_flood_indices.size() - 1) s += "-";
        if (s.length() > 30) { s += "..."; break; }
    }
    return s;
  }
  // cycle mode
  uint32_t current = m_flood_indices[m_flood_tx_count % m_flood_num_preambles];
  return std::to_string(current);
}

// ---------------------------------------------------------------------------
// get_tx_buffer — return the correct TX buffer for current mode
// ---------------------------------------------------------------------------
void *prach_tx::get_tx_buffer() {
  // Multi-freq-position mode supersedes all others: the combined buffer already
  // incorporates every flood preamble superimposed across all freq positions.
  if (m_multi_freq_pos_count > 1 && !m_multi_freq_tx_buf.empty()) {
    return reinterpret_cast<void *>(m_multi_freq_tx_buf.data());
  }
  if (m_flood_enabled && m_flood_strategy == "superimpose") {
    return reinterpret_cast<void *>(m_flood_tx_buf.data());
  } else if (m_flood_enabled && m_flood_strategy == "cycle") {
    uint32_t idx = m_flood_tx_count % m_flood_num_preambles;
    return reinterpret_cast<void *>(m_flood_bufs[idx].data());
  }
  return reinterpret_cast<void *>(m_tx_buf.data());
}

// ---------------------------------------------------------------------------
// open_rf
// ---------------------------------------------------------------------------
bool prach_tx::open_rf(const std::string &device_args) {
  if (m_dry_run) {
    printf("[prach_tx] dry_run=true — skipping RF device open\n");
    m_rf_open = true;
    return true;
  }

  memset(&m_rf, 0, sizeof(m_rf));

  printf("[prach_tx] Opening RF device (args='%s')...\n", device_args.c_str());

  srsran_rf_load_plugins();

  char args_cstr[256];
  snprintf(args_cstr, sizeof(args_cstr), "%s", device_args.c_str());

  if (srsran_rf_open(&m_rf, args_cstr) != SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_rf_open failed\n");
    return false;
  }

  // Set RX gain FIRST (order matters for B210 RX chain init)
  // NOTE: RX is configured even though we don't use it for streaming.
  // This is required because the B210's TX path depends on RX PLL lock.
  double rx_gain_db = 40.0;
  srsran_rf_set_rx_gain(&m_rf, rx_gain_db);
  double actual_rx_gain = srsran_rf_get_rx_gain(&m_rf);
  printf("[prach_tx] RX gain: requested=%.1f dB, actual=%.1f dB (configured "
         "for PLL lock)\n",
         rx_gain_db, actual_rx_gain);

  // Set RX sample rate (needed for RX chain PLL)
  double actual_rx_rate = srsran_rf_set_rx_srate(&m_rf, m_cfg.srate_hz);
  printf("[prach_tx] RX rate: requested=%.2f MHz, actual=%.2f MHz\n",
         m_cfg.srate_hz / 1e6, actual_rx_rate / 1e6);

  // Set TX sample rate
  double actual_tx_rate = srsran_rf_set_tx_srate(&m_rf, m_cfg.srate_hz);
  printf("[prach_tx] TX rate: requested=%.2f MHz, actual=%.2f MHz\n",
         m_cfg.srate_hz / 1e6, actual_tx_rate / 1e6);

  if (std::abs(actual_tx_rate - m_cfg.srate_hz) / m_cfg.srate_hz > 0.01) {
    fprintf(stderr, "[prach_tx] WARNING: TX sample rate deviation > 1%%\n");
  }

  // Set RX center frequency — DL freq for SSB sync
  double actual_rx_freq =
      srsran_rf_set_rx_freq(&m_rf, 0, m_cell_cfg.dl_freq_hz);
  printf("[prach_tx] RX freq: requested=%.3f MHz, actual=%.3f MHz\n",
         m_cell_cfg.dl_freq_hz / 1e6, actual_rx_freq / 1e6);

  if (std::abs(actual_rx_freq - m_cell_cfg.dl_freq_hz) > 100e3) {
    fprintf(stderr,
            "[prach_tx] FATAL: RX freq %.3f MHz deviates from DL %.3f MHz by > "
            "100 kHz\n",
            actual_rx_freq / 1e6, m_cell_cfg.dl_freq_hz / 1e6);
    srsran_rf_close(&m_rf);
    return false;
  }

  // Set TX center frequency — UL freq for PRACH
  double actual_tx_freq = srsran_rf_set_tx_freq(&m_rf, 0, m_cfg.tx_freq_hz);
  printf("[prach_tx] TX freq: requested=%.3f MHz, actual=%.3f MHz\n",
         m_cfg.tx_freq_hz / 1e6, actual_tx_freq / 1e6);

  if (std::abs(actual_tx_freq - m_cfg.tx_freq_hz) > 100e3) {
    fprintf(stderr,
            "[prach_tx] FATAL: TX freq %.3f MHz deviates from UL %.3f MHz by > "
            "100 kHz\n",
            actual_tx_freq / 1e6, m_cfg.tx_freq_hz / 1e6);
    srsran_rf_close(&m_rf);
    return false;
  }

  // Set TX gain (after all freq/srate are configured)
  if (srsran_rf_set_tx_gain(&m_rf, m_cfg.tx_gain_db) != SRSRAN_SUCCESS) {
    fprintf(stderr,
            "[prach_tx] WARNING: srsran_rf_set_tx_gain returned error\n");
  }
  double actual_tx_gain = srsran_rf_get_tx_gain(&m_rf);
  printf("[prach_tx] TX gain: requested=%.1f dB, actual=%.1f dB\n",
         m_cfg.tx_gain_db, actual_tx_gain);

  // Settling delay
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  printf("[prach_tx] RF device ready\n");
  m_rf_open = true;
  return true;
}

// ---------------------------------------------------------------------------
// sync_to_ssb — synchronize to SSB to get device-time ↔ SFN/slot mapping
// ---------------------------------------------------------------------------
bool prach_tx::sync_to_ssb() {
  if (m_dry_run) {
    printf("[prach_tx] DRY RUN: skipping SSB sync\n");
    // Use fake timing for dry run
    m_synced = true;
    m_sync_device_time = 0.0;
    m_sync_sfn = 0;
    m_sync_slot = 0;
    m_sync_t_offset = 0;
    return true;
  }

  if (!m_rf_open) {
    fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
    return false;
  }

  // Initialize SSB sync
  if (!m_ssb_sync.init(m_cell_cfg, m_cfg.srate_hz)) {
    fprintf(stderr, "[prach_tx] WARNING: ssb_sync init failed\n");
    if (m_has_fallback && attempt_sync_from_fallback()) {
      return true;
    }
    fprintf(stderr, "[prach_tx] FATAL: ssb_sync init failed and no fallback\n");
    return false;
  }

  // Allocate RX buffer: capture enough samples for SSB search
  uint32_t samples_per_frame = m_cell_cfg.samples_per_frame();
  m_rx_buf.assign(samples_per_frame, {0.0f, 0.0f});

  printf("[prach_tx] Searching for SSB (PCI=%u)...\n", m_cell_cfg.pci);
  printf("[prach_tx] Capturing %.1f ms of samples\n",
         (double)samples_per_frame / m_cfg.srate_hz * 1000.0);

  // Start RX stream
  int ret = srsran_rf_start_rx_stream(&m_rf, false);
  if (ret != 0) {
    fprintf(stderr,
            "[prach_tx] WARNING: srsran_rf_start_rx_stream returned %d\n", ret);
    if (m_has_fallback && attempt_sync_from_fallback()) {
      return true;
    }
    fprintf(stderr,
            "[prach_tx] FATAL: Cannot start RX stream and no fallback\n");
    return false;
  }

  // Stabilization delay (B210 needs time to settle after stream start)
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Flush stale samples (~10 ms worth)
  std::vector<std::complex<float>> flush_buf(m_cell_cfg.samples_per_slot());
  for (int i = 0; i < 10; i++) {
    srsran_rf_recv(&m_rf, flush_buf.data(), flush_buf.size(), true);
  }

  // Capture one frame of samples in chunks to avoid srsRAN's
  // RX trial limit (RF_UHD_IMP_MAX_RX_TRIALS=100, chunk_size~2048,
  // so max per call ≈ 204800 samples).
  // FIRST chunk uses recv_with_time to get the device timestamp
  // pinned to the sample boundary — no post-capture jitter.
  cf_t *rx_data = reinterpret_cast<cf_t *>(m_rx_buf.data());
  uint32_t chunk_size = m_cell_cfg.samples_per_slot(); // ~21504
  uint32_t remaining = samples_per_frame;
  uint32_t total_recv = 0;
  cf_t *rx_ptr = rx_data;
  time_t first_full_secs = 0;
  double first_frac_secs = 0.0;
  double rx_device_time = 0.0;
  bool have_timestamp = false;

  while (remaining > 0) {
    uint32_t to_recv = (remaining > chunk_size) ? chunk_size : remaining;
    int n;
    if (!have_timestamp) {
      // First chunk: capture WITH device timestamp pinned to sample 0
      n = srsran_rf_recv_with_time(&m_rf, rx_ptr, to_recv, true,
                                   &first_full_secs, &first_frac_secs);
      if (n > 0) {
        rx_device_time = (double)first_full_secs + first_frac_secs;
        have_timestamp = true;
        printf("[prach_tx] First chunk timestamp: %.6f s\n", rx_device_time);
      }
    } else {
      n = srsran_rf_recv(&m_rf, rx_ptr, to_recv, true);
    }
    if (n < 0) {
      fprintf(stderr,
              "[prach_tx] WARNING: RX chunk failed (nrecv=%d, "
              "requested=%u)\n",
              n, to_recv);
      break;
    }
    total_recv += (uint32_t)n;
    rx_ptr += (uint32_t)n;
    remaining -= (uint32_t)n;
  }

  if (total_recv < samples_per_frame) {
    if (total_recv > 0) {
      printf("[prach_tx] Partial capture: %u/%u samples, "
             "using what we have\n",
             total_recv, samples_per_frame);
    } else {
      fprintf(stderr, "[prach_tx] WARNING: RX failed (0 samples)\n");
    }
  }

  // Stop RX stream
  srsran_rf_stop_rx_stream(&m_rf);

  // If we got no samples at all, try fallback
  if (total_recv == 0) {
    if (m_has_fallback) {
      printf("[prach_tx] Attempting sniffer-based SSB sync fallback...\n");
      if (attempt_sync_from_fallback()) {
        return true;
      }
    }
    fprintf(stderr,
            "[prach_tx] FATAL: SSB sync failed (no RF RX and no fallback)\n");
    return false;
  }

  // Search for SSB using the captured samples
  uint32_t pci = 0, sfn = 0, ssb_idx = 0, t_offset = 0;
  float snr_db = 0.0f, cfo_hz = 0.0f;
  srsran_mib_nr_t mib = {};
  bool hrf = false;

  if (!m_ssb_sync.search(rx_data, total_recv, &pci, &sfn, &ssb_idx, &t_offset,
                         &snr_db, &mib, &hrf, &cfo_hz)) {
    fprintf(stderr,
            "[prach_tx] WARNING: SSB not found or PBCH decode failed\n");

    // Try sniffer-based fallback
    if (m_has_fallback) {
      printf("[prach_tx] Attempting sniffer-based SSB sync fallback...\n");
      if (attempt_sync_from_fallback()) {
        return true;
      }
    }

    return false;
  }

  // Verify PCI matches
  if (pci != m_cell_cfg.pci && m_cell_cfg.pci != 0) {
    fprintf(stderr, "[prach_tx] WARNING: Detected PCI %u != expected %u\n", pci,
            m_cell_cfg.pci);
  }

  // Compute the device time when the SSB started.
  // rx_device_time is the timestamp of the FIRST captured sample
  // (captured with srsran_rf_recv_with_time on the first chunk).
  // t_offset is the sample offset of the SSB within the buffer.
  double ssb_start_device_time =
      rx_device_time + ((double)t_offset / m_cfg.srate_hz);

  // Compute the SSB slot from ssb_idx and half-frame flag.
  // Pattern A, 15 kHz SCS, Lmax=4:
  //   hrf=0: ssb_idx 0,1 → slot 0;  ssb_idx 2,3 → slot 1
  //   hrf=1: ssb_idx 0,1 → slot 5;  ssb_idx 2,3 → slot 6
  uint32_t half_slot = (ssb_idx < 2) ? 0 : 1;
  uint32_t hf_offset = hrf ? 5 : 0; // 5 slots per half-frame at 15 kHz
  uint32_t ssb_slot = hf_offset + half_slot;

  // SSB PSS is at symbol 2 or 8 within its slot (Pattern A, 15 kHz).
  // TS 38.213 §4.1 Case A (15 kHz, carrier <=3 GHz): candidate SSB first
  // symbols are {2, 8, 16, 22}. Within-slot first symbol is 2 (idx 0,2) or
  // 8 (idx 1,3). ssb_slot already accounts for the slot; this is intra-slot.
  // ssb_start_device_time marks the PSS, not the slot boundary.
  // Subtract the intra-slot offset so the anchor aligns to slot start.
  uint32_t ssb_first_symbol = ((ssb_idx == 0) || (ssb_idx == 2)) ? 2 : 8;
  if (m_ssb_first_symbol_override >= 0) {
    ssb_first_symbol = (uint32_t)m_ssb_first_symbol_override;
    printf("[prach_tx] SSB first symbol OVERRIDE: %u\n", ssb_first_symbol);
  }
  double ssb_intra_slot_s = 1e-3 * (double)ssb_first_symbol / 14.0;

  m_sync_device_time = ssb_start_device_time - ssb_intra_slot_s;
  m_sync_sfn = sfn;
  m_sync_slot = ssb_slot;
  m_sync_t_offset = t_offset;
  m_synced = true;
  m_sync_ssb_idx = ssb_idx;

  // Store CFO and scale DL→UL (LO ppm error is constant, so Hz offset scales
  // with carrier)
  m_cfo_dl_hz = cfo_hz;
  m_cfo_ul_hz = m_cfo_dl_hz * (m_cfg.tx_freq_hz / m_cell_cfg.dl_freq_hz);

  printf("[prach_tx] SSB sync acquired:\n");
  printf("  PCI         = %u\n", pci);
  printf("  SFN (4 LSB) = %u\n", sfn);
  printf("  SSB index   = %u\n", ssb_idx);
  printf("  SNR         = %.1f dB\n", snr_db);
  printf("  CFO (DL)    = %.1f Hz\n", m_cfo_dl_hz);
  printf("  CFO (UL)    = %.1f Hz (scaled by %.3f)\n", m_cfo_ul_hz,
         m_cfg.tx_freq_hz / m_cell_cfg.dl_freq_hz);
  printf("  PSS dev_time= %.6f s  (slot-start = %.6f s, -%.0f us)\n",
         ssb_start_device_time, m_sync_device_time, ssb_intra_slot_s * 1e6);
  printf("  Slot        = %u (from ssb_idx=%u, hrf=%d)\n", ssb_slot, ssb_idx,
         hrf);

  // Regenerate preamble with measured CFO correction applied
  if (m_cfo_correct) {
    printf("[prach_tx] Regenerating preamble with measured CFO (%.1f Hz UL)\n",
           m_cfo_ul_hz);
    if (!generate_preamble()) {
      fprintf(stderr, "[prach_tx] FATAL: preamble regen after CFO failed\n");
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// set_sync_fallback — store sniffer-based timing for fallback use
// ---------------------------------------------------------------------------
void prach_tx::set_sync_fallback(double unix_time, uint32_t sfn,
                                 uint32_t ssb_slot, uint32_t ssb_idx) {
  m_has_fallback = true;
  m_fallback_unix_time = unix_time;
  m_fallback_sfn = sfn;
  m_fallback_ssb_slot = ssb_slot;
  m_fallback_ssb_idx = ssb_idx;
  printf("[prach_tx] Sync fallback registered: SFN=%u slot=%u ssb_idx=%u @ "
         "unix=%.3f\n",
         sfn, ssb_slot, ssb_idx, unix_time);
}

// ---------------------------------------------------------------------------
// attempt_sync_from_fallback — use sniffer InfluxDB MIB + device-time epoch
// offset
//
// The B210 device time is NOT Unix time. We calibrate the offset:
//   unix_time = device_time + m_dev_epoch_offset
// Then compute the SFN-to-device-time mapping from the sniffer's MIB.
// TX uses srsran_rf_send_timed_multi for hardware-accurate timing.
// ---------------------------------------------------------------------------
bool prach_tx::attempt_sync_from_fallback() {
  if (!m_has_fallback)
    return false;

  // Record current device time and wall-clock time simultaneously
  time_t full_secs = 0;
  double frac_secs = 0.0;
  srsran_rf_get_time(&m_rf, &full_secs, &frac_secs);
  double dev_now = (double)full_secs + frac_secs;

  auto now = std::chrono::system_clock::now();
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch())
                    .count();
  double unix_now = (double)now_us / 1e6;

  // Calibrate: unix = dev + offset
  m_dev_epoch_offset = unix_now - dev_now;

  // Convert the sniffer's MIB Unix timestamp to device time
  double dev_at_mib = m_fallback_unix_time - m_dev_epoch_offset;

  // SSB PSS intra-slot offset (Pattern A, 15 kHz, as in sync_to_ssb)
  // TS 38.213 §4.1 Case A (15 kHz, carrier <=3 GHz): candidate SSB first
  // symbols are {2, 8, 16, 22}. Within-slot first symbol is 2 (idx 0,2) or
  // 8 (idx 1,3). ssb_slot already accounts for the slot; this is intra-slot.
  uint32_t ssb_first_symbol =
      ((m_fallback_ssb_idx == 0) || (m_fallback_ssb_idx == 2)) ? 2 : 8;
  if (m_ssb_first_symbol_override >= 0) {
    ssb_first_symbol = (uint32_t)m_ssb_first_symbol_override;
  }
  double ssb_intra_slot_s = 1e-3 * (double)ssb_first_symbol / 14.0;

  // Set sync anchor in device-time coordinates (aligned to slot start)
  m_sync_device_time = dev_at_mib - ssb_intra_slot_s;
  m_sync_sfn = m_fallback_sfn; // 4-LSB SFN
  m_sync_slot = m_fallback_ssb_slot;
  m_sync_t_offset = 0;
  m_synced = true;
  m_using_fallback = false; // use normal device-time TX path now
  m_sync_ssb_idx = m_fallback_ssb_idx;

  printf("[prach_tx] Fallback sync (device-time calibrated):\n");
  printf("  Epoch offset   = %.3f s (unix = dev + offset)\n",
         m_dev_epoch_offset);
  printf("  dev_now        = %.6f s\n", dev_now);
  printf("  dev_at_mib     = %.6f s  (slot-start = %.6f s, -%.0f us)\n",
         dev_at_mib, m_sync_device_time, ssb_intra_slot_s * 1e6);
  printf("  anchor: SFN=%u slot=%u ssb_idx=%u\n", m_sync_sfn, m_sync_slot,
         m_sync_ssb_idx);

  return true;
}

// ---------------------------------------------------------------------------
// transmit_preamble
// ---------------------------------------------------------------------------
bool prach_tx::transmit_preamble(uint32_t sfn, uint32_t ro_slot) {
  if (!m_initialized) {
    fprintf(stderr, "[prach_tx] FATAL: not initialized\n");
    return false;
  }

  // Compute RA-RNTI per TS 38.321 §5.1.3
  m_ra_rnti = compute_ra_rnti(0, ro_slot, 0, 0);

  printf("\n[prach_tx] ==================== PRACH TX ====================\n");
  printf("[prach_tx] RAPID         = %s\n", get_current_rapid_list().c_str());
  printf("[prach_tx] Target RO     = SFN %u, slot %u\n", sfn, ro_slot);
  printf("[prach_tx] RA-RNTI       = 0x%04x (%u)\n", m_ra_rnti, m_ra_rnti);
  printf("[prach_tx]   = 1 + s_id(0) + 14*t_id(%u) + 14*80*f_id(0) + "
         "14*80*8*ul_carrier(0)\n",
         ro_slot);
  printf("[prach_tx] TX freq       = %.3f MHz\n", m_cfg.tx_freq_hz / 1e6);
  printf("[prach_tx] TX gain       = %.1f dB\n", m_cfg.tx_gain_db);
  printf("[prach_tx] Preamble len  = %u samples\n", m_preamble_len_samples);
  printf(
      "[prach_tx] =======================================================\n\n");

  if (m_dry_run) {
    printf("[prach_tx] DRY RUN: skipping actual RF transmission\n");
    if (m_flood_enabled) {
      m_flood_tx_count++;
    }
    return true;
  }

  if (!m_rf_open) {
    fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
    return false;
  }

  // Auto-sync to SSB for device-time ↔ SFN/slot mapping
  if (!m_synced) {
    printf("[prach_tx] No SSB sync — acquiring reference timing...\n");
    if (!sync_to_ssb()) {
      fprintf(stderr, "[prach_tx] FATAL: SSB sync failed, cannot align TX\n");
      return false;
    }
  }

  double t_now;
  time_t full_secs_now = 0;
  double frac_secs_now = 0.0;

  // Compute the target TX device time using the sync anchor.
  uint32_t sync_rfn = m_sync_sfn & 0xF;
  uint32_t target_rfn = sfn & 0xF;

  int32_t frames_to_ro;
  if (sync_rfn < target_rfn) {
    frames_to_ro = (int32_t)(target_rfn - sync_rfn);
  } else if (sync_rfn == target_rfn) {
    if ((int32_t)m_sync_slot <= (int32_t)ro_slot) {
      frames_to_ro = 0;
    } else {
      frames_to_ro = 16;
    }
  } else {
    frames_to_ro = (int32_t)(16 - sync_rfn + target_rfn);
  }

  // Frame/slot durations for 15 kHz SCS:
  //   1 slot  = 1 ms
  //   1 frame = 10 ms
  // Use these constants rather than samples_per_slot()/srate which
  // misses CP overhead and gives ~0.933 ms instead of 1 ms.
  const double slot_dur = 1e-3;   // 1 ms
  const double frame_dur = 10e-3; // 10 ms
  // NOTE: ro_period_dur is the actual PRACH occasion period (prach_x frames),
  // NOT the 16-frame 4-LSB SFN rollover used for frames_to_ro above.
  // config_idx=13 → prach_x=2 → RO every 20 ms.
  const double ro_period_dur = (double)m_cell_cfg.prach_x * frame_dur;

  srsran_rf_get_time(&m_rf, &full_secs_now, &frac_secs_now);
  t_now = (double)full_secs_now + frac_secs_now;

  int64_t slots_from_sync =
      (int64_t)frames_to_ro * 10 + (int64_t)ro_slot - (int64_t)m_sync_slot;
  double t_tx = m_sync_device_time + (double)slots_from_sync * slot_dur;

  // Manual timing nudge (sweep parameter from tool_config)
  t_tx += m_tx_offset_us * 1e-6;

  // Advance if target is in the past (one RO period = 16 frames = 160ms)
  if (t_tx < t_now - 0.001) {
    double periods_needed = std::ceil((t_now - t_tx) / ro_period_dur);
    t_tx += periods_needed * ro_period_dur;
  }

  if (t_tx <= t_now) {
    fprintf(
        stderr,
        "[prach_tx] FATAL: target time %.6f s is in the past (now=%.6f s)\n",
        t_tx, t_now);
    return false;
  }

  printf("[prach_tx] Sync anchor: SFN=%u (4LSB) slot=%u @ dev_time=%.6f s\n",
         m_sync_sfn, m_sync_slot, m_sync_device_time);
  printf("[prach_tx] Target RO:    SFN=%u slot=%u → dev_time=%.6f s (Δ=%+.3f "
         "ms)\n",
         sfn, ro_slot, t_tx, (t_tx - t_now) * 1e3);

  void *buffers[1];
  buffers[0] = get_tx_buffer();

  int nsent = srsran_rf_send_timed_multi(&m_rf, buffers, m_preamble_len_samples,
                                         (time_t)t_tx, t_tx - (time_t)t_tx,
                                         true, true, true);

  if (nsent < 0) {
    fprintf(stderr,
            "[prach_tx] FATAL: srsran_rf_send_timed_multi failed (ret=%d)\n",
            nsent);
    return false;
  }

  if ((uint32_t)nsent != m_preamble_len_samples) {
    fprintf(stderr, "[prach_tx] WARNING: sent %d/%u samples\n", nsent,
            m_preamble_len_samples);
  }

  printf("[prach_tx] Preamble transmitted: %d samples\n", nsent);
  if (m_flood_enabled) {
    printf("[prach_tx] FLOOD: strategy=%s, preambles=%s, burst #%u\n",
           m_flood_strategy.c_str(), get_current_rapid_list().c_str(),
           m_flood_tx_count);
    m_flood_tx_count++;
  }
  printf("[prach_tx] Expected gNB response:\n");
  printf("  RAR (Msg2) on RA-RNTI=0x%04x (%u) within 10 slots of RO end\n",
         m_ra_rnti, m_ra_rnti);
  printf(
      "  Check: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra.rnti'\n");

  m_last_tx_time = t_tx;
  return nsent > 0;
}

// ---------------------------------------------------------------------------
// transmit_at_time — transmit preamble at a specific device time
// (for continuous mode: each subsequent RO is 160ms after the previous)
// ---------------------------------------------------------------------------
bool prach_tx::transmit_at_time(double dev_time, uint32_t sfn,
                                uint32_t ro_slot) {
  if (!m_initialized) {
    fprintf(stderr, "[prach_tx] FATAL: not initialized\n");
    return false;
  }

  m_ra_rnti = compute_ra_rnti(0, ro_slot, 0, 0);

  printf("[prach_tx] ==================== PRACH TX ====================\n");
  printf("[prach_tx] RAPID         = %s\n", get_current_rapid_list().c_str());
  printf("[prach_tx] Target RO     = SFN %u, slot %u\n", sfn, ro_slot);
  printf("[prach_tx] RA-RNTI       = 0x%04x (%u)\n", m_ra_rnti, m_ra_rnti);
  printf("[prach_tx] TX freq       = %.3f MHz\n", m_cfg.tx_freq_hz / 1e6);
  printf("[prach_tx] TX gain       = %.1f dB\n", m_cfg.tx_gain_db);
  printf("[prach_tx] Preamble len  = %u samples\n", m_preamble_len_samples);
  printf(
      "[prach_tx] =======================================================\n\n");

  if (m_dry_run) {
    printf("[prach_tx] DRY RUN: skipping actual RF transmission\n");
    if (m_flood_enabled) {
      m_flood_tx_count++;
    }
    return true;
  }
  if (!m_rf_open) {
    fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
    return false;
  }

  time_t full_secs_now = 0;
  double frac_secs_now = 0.0;
  srsran_rf_get_time(&m_rf, &full_secs_now, &frac_secs_now);
  double t_now = (double)full_secs_now + frac_secs_now;

  // Manual timing nudge
  dev_time += m_tx_offset_us * 1e-6;

  // Advance if target is in the past (one RO period = prach_x frames)
  double ro_period_dur = (double)m_cell_cfg.prach_x * 10e-3;
  if (dev_time <= t_now) {
    double periods_needed = std::ceil((t_now - dev_time) / ro_period_dur);
    dev_time += periods_needed * ro_period_dur;
  }

  if (dev_time <= t_now) {
    fprintf(stderr,
            "[prach_tx] FATAL: target time %.6f s is in the past (now=%.6f)\n",
            dev_time, t_now);
    return false;
  }

  printf(
      "[prach_tx] Target RO: SFN=%u slot=%u → dev_time=%.6f s (Δ=%+.3f ms)\n",
      sfn, ro_slot, dev_time, (dev_time - t_now) * 1e3);

  void *buffers[1];
  buffers[0] = get_tx_buffer();

  int nsent = srsran_rf_send_timed_multi(
      &m_rf, buffers, m_preamble_len_samples, (time_t)dev_time,
      dev_time - (time_t)dev_time, true, true, true);

  if (nsent < 0) {
    fprintf(stderr,
            "[prach_tx] FATAL: srsran_rf_send_timed_multi failed (ret=%d)\n",
            nsent);
    return false;
  }
  if ((uint32_t)nsent != m_preamble_len_samples) {
    fprintf(stderr, "[prach_tx] WARNING: sent %d/%u samples\n", nsent,
            m_preamble_len_samples);
  }

  printf("[prach_tx] Preamble transmitted: %d samples\n", nsent);
  if (m_flood_enabled) {
    printf("[prach_tx] FLOOD: strategy=%s, preambles=%s, burst #%u\n",
           m_flood_strategy.c_str(), get_current_rapid_list().c_str(),
           m_flood_tx_count);
    m_flood_tx_count++;
  }
  printf("[prach_tx] Expected gNB response:\n");
  printf("  RAR (Msg2) on RA-RNTI=0x%04x (%u) within 10 slots of RO end\n",
         m_ra_rnti, m_ra_rnti);
  printf(
      "  Check: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra.rnti'\n");

  m_last_tx_time = dev_time;
  return nsent > 0;
}

// dummy main for link test
int prach_tx_link_check() { return 0; }
