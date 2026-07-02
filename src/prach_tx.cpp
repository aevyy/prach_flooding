// prach_tx.cpp — PRACH preamble generator and RF transmitter
//
// Uses srsran_rf_t (srsRAN's RF abstraction) instead of raw UHD to avoid
// Boost.DateTime compatibility issues with newer GCC.

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
#include <sstream>
#include <sys/time.h>

// ---------------------------------------------------------------------------
// UHD async error callback — registered with srsran_rf_register_error_handler.
// Counts late, underflow, overflow events for telemetry.
// ---------------------------------------------------------------------------
static void rf_error_callback(void* arg, srsran_rf_error_t error) {
    auto* self = static_cast<prach_tx*>(arg);
    switch (error.type) {
        case srsran_rf_error_t::SRSRAN_RF_ERROR_LATE:
            self->m_async_late.fetch_add(1);      break;
        case srsran_rf_error_t::SRSRAN_RF_ERROR_UNDERFLOW:
            self->m_async_underflow.fetch_add(1);  break;
        case srsran_rf_error_t::SRSRAN_RF_ERROR_OVERFLOW:
            self->m_async_overflow.fetch_add(1);   break;
        default:
            self->m_async_seq_error.fetch_add(1);  break;
    }
}

// ---------------------------------------------------------------------------
// TS 38.211 Table 6.3.3.1-3: physical root index u from logical root index i
// L_RA = 839. 838 entries total. Sourced from 3GPP spec / srsRAN PRACH LUT.
// ---------------------------------------------------------------------------
static constexpr uint16_t PRACH_ROOT_U_839[838] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,
   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
   20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
   30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
   40,  41,  42,  43,  44,  45,  46,  47,  48,  49,
   50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
   60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
   70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
   80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
   90,  91,  92,  93,  94,  95,  96,  97,  98,  99,
  100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
  110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
  120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
  130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
  140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
  150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
  160, 161, 162, 163, 164, 165, 166, 167, 168, 169,
  170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
  180, 181, 182, 183, 184, 185, 186, 187, 188, 189,
  190, 191, 192, 193, 194, 195, 196, 197, 198, 199,
  200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
  210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
  220, 221, 222, 223, 224, 225, 226, 227, 228, 229,
  230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
  240, 241, 242, 243, 244, 245, 246, 247, 248, 249,
  250, 251, 252, 253, 254, 255, 256, 257, 258, 259,
  260, 261, 262, 263, 264, 265, 266, 267, 268, 269,
  270, 271, 272, 273, 274, 275, 276, 277, 278, 279,
  280, 281, 282, 283, 284, 285, 286, 287, 288, 289,
  290, 291, 292, 293, 294, 295, 296, 297, 298, 299,
  300, 301, 302, 303, 304, 305, 306, 307, 308, 309,
  310, 311, 312, 313, 314, 315, 316, 317, 318, 319,
  320, 321, 322, 323, 324, 325, 326, 327, 328, 329,
  330, 331, 332, 333, 334, 335, 336, 337, 338, 339,
  340, 341, 342, 343, 344, 345, 346, 347, 348, 349,
  350, 351, 352, 353, 354, 355, 356, 357, 358, 359,
  360, 361, 362, 363, 364, 365, 366, 367, 368, 369,
  370, 371, 372, 373, 374, 375, 376, 377, 378, 379,
  380, 381, 382, 383, 384, 385, 386, 387, 388, 389,
  390, 391, 392, 393, 394, 395, 396, 397, 398, 399,
  400, 401, 402, 403, 404, 405, 406, 407, 408, 409,
  410, 411, 412, 413, 414, 415, 416, 417, 418, 419,
  420, 421, 422, 423, 424, 425, 426, 427, 428, 429,
  430, 431, 432, 433, 434, 435, 436, 437, 438, 439,
  440, 441, 442, 443, 444, 445, 446, 447, 448, 449,
  450, 451, 452, 453, 454, 455, 456, 457, 458, 459,
  460, 461, 462, 463, 464, 465, 466, 467, 468, 469,
  470, 471, 472, 473, 474, 475, 476, 477, 478, 479,
  480, 481, 482, 483, 484, 485, 486, 487, 488, 489,
  490, 491, 492, 493, 494, 495, 496, 497, 498, 499,
  500, 501, 502, 503, 504, 505, 506, 507, 508, 509,
  510, 511, 512, 513, 514, 515, 516, 517, 518, 519,
  520, 521, 522, 523, 524, 525, 526, 527, 528, 529,
  530, 531, 532, 533, 534, 535, 536, 537, 538, 539,
  540, 541, 542, 543, 544, 545, 546, 547, 548, 549,
  550, 551, 552, 553, 554, 555, 556, 557, 558, 559,
  560, 561, 562, 563, 564, 565, 566, 567, 568, 569,
  570, 571, 572, 573, 574, 575, 576, 577, 578, 579,
  580, 581, 582, 583, 584, 585, 586, 587, 588, 589,
  590, 591, 592, 593, 594, 595, 596, 597, 598, 599,
  600, 601, 602, 603, 604, 605, 606, 607, 608, 609,
  610, 611, 612, 613, 614, 615, 616, 617, 618, 619,
  620, 621, 622, 623, 624, 625, 626, 627, 628, 629,
  630, 631, 632, 633, 634, 635, 636, 637, 638, 639,
  640, 641, 642, 643, 644, 645, 646, 647, 648, 649,
  650, 651, 652, 653, 654, 655, 656, 657, 658, 659,
  660, 661, 662, 663, 664, 665, 666, 667, 668, 669,
  670, 671, 672, 673, 674, 675, 676, 677, 678, 679,
  680, 681, 682, 683, 684, 685, 686, 687, 688, 689,
  690, 691, 692, 693, 694, 695, 696, 697, 698, 699,
  700, 701, 702, 703, 704, 705, 706, 707, 708, 709,
  710, 711, 712, 713, 714, 715, 716, 717, 718, 719,
  720, 721, 722, 723, 724, 725, 726, 727, 728, 729,
  730, 731, 732, 733, 734, 735, 736, 737, 738, 739,
  740, 741, 742, 743, 744, 745, 746, 747, 748, 749,
  750, 751, 752, 753, 754, 755, 756, 757, 758, 759,
  760, 761, 762, 763, 764, 765, 766, 767, 768, 769,
  770, 771, 772, 773, 774, 775, 776, 777, 778, 779,
  780, 781, 782, 783, 784, 785, 786, 787, 788, 789,
  790, 791, 792, 793, 794, 795, 796, 797, 798, 799,
  800, 801, 802, 803, 804, 805, 806, 807, 808, 809,
  810, 811, 812, 813, 814, 815, 816, 817, 818, 819,
  820, 821, 822, 823, 824, 825, 826, 827, 828, 829,
  830, 831, 832, 833, 834, 835, 836, 837
};

// ---------------------------------------------------------------------------
// TS 38.211 Table 6.3.3.1-5, unrestricted set, L_RA=839, 1.25 kHz (long fmt 0)
// ---------------------------------------------------------------------------
static constexpr int NCS_LONG_125_UNRESTRICTED[16] =
    {0, 13, 15, 18, 22, 26, 32, 38, 46, 59, 76, 93, 119, 167, 279, 419};

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
prach_tx::~prach_tx() {
  stop_async_monitor();
  if (m_initialized) {
    srsran_prach_free(&m_prach);
  }
  if (m_rf_open) {
    srsran_rf_close(&m_rf);
  }
}

// ---------------------------------------------------------------------------
// start_async_monitor — register UHD async callback via srsran_rf
// ---------------------------------------------------------------------------
void prach_tx::start_async_monitor() {
  if (!m_rf_open || m_dry_run) return;
  srsran_rf_register_error_handler(&m_rf, rf_error_callback, this);
  m_async_thread_running = true;
  printf("[prach_tx] UHD async error callback registered\n");
}

void prach_tx::stop_async_monitor() {
  m_async_thread_running = false;
}

// ---------------------------------------------------------------------------
// build_preamble_list — per TS 38.211 §6.3.3.1
// ---------------------------------------------------------------------------
std::vector<PreambleId> prach_tx::build_preamble_list(int total, int per_root,
                                                       int ncs, int root0) const {
    std::vector<PreambleId> out;
    int root = root0, v = 0;
    for (int i = 0; i < total; ++i) {
        if (per_root > 0 && v >= per_root) { v = 0; root = (root + 1) % 838; }
        out.push_back({root, v, v * ncs});
        if (per_root > 0) ++v; else { root = (root + 1) % 838; }
    }
    return out;
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
  m_max_preambles_per_occasion = tc.flood.max_preambles_per_occasion;
  m_papr_backoff_db = tc.flood.papr_backoff_db;

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

  // --- Multi-freq-position config ---
  m_multi_freq_pos_count = tc.multi_ro.freq_pos_count;

  if (m_multi_freq_pos_count > 1) {
    if (cfg.msg1_fdm == 1) {
      printf("\n[prach_tx] ERROR: Cannot enable multi-freq-pos (requested %u) because msg1_fdm == 1.\n", m_multi_freq_pos_count);
      printf("           The gNB has only allocated ONE frequency-domain PRACH occasion per time slot.\n");
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
    printf("[prach_tx] freq_offset overridden: %u -> %u\n",
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
  m_rx_to_tx_cal_s = tc.timing.rx_to_tx_cal_s;
  m_resync_cp_fraction = tc.timing.resync_cp_fraction;
  m_max_time_since_sync_ms = tc.timing.max_time_since_sync_ms;
  m_ssb_fit_window_m = tc.ssb_fit.fit_window_m;
  m_regen_period_occasions = tc.ssb_fit.regen_period_occasions;

  // Initialize srsRAN PRACH object
  uint32_t max_N_ifft_ul = srsran_min_symbol_sz_rb(cfg.nof_prb);
  printf("[prach_tx] max_N_ifft_ul = %u\n", max_N_ifft_ul);

  // Validate sample rate
  uint32_t scs_hz = SRSRAN_SUBC_SPACING_NR(cfg.scs);
  double expected_srate = (double)max_N_ifft_ul * scs_hz;

  if (std::abs(cfg.srate_hz - expected_srate) > 1.0) {
    fprintf(stderr, "[prach_tx] FATAL: Sample rate mismatch\n");
    fprintf(stderr, "  srate_hz from config = %.0f Hz\n", cfg.srate_hz);
    fprintf(stderr, "  expected (FFT*SCS)   = %u * %u = %.0f Hz\n",
            max_N_ifft_ul, scs_hz, expected_srate);
    return false;
  }

  printf("[prach_tx] Sample rate validated: %.2f MHz = %u * %u Hz (FFT * SCS)\n",
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

  if (m_flood_enabled) {
    prach_cfg.num_ra_preambles = m_flood_num_preambles;
    printf("[prach_tx] FLOOD: overriding num_ra_preambles %u -> %u\n",
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

  // --- A1: Build spec-correct preamble ID list ---
  {
    uint32_t ncs = (cfg.prach_zcz < 16) ? (uint32_t)NCS_LONG_125_UNRESTRICTED[cfg.prach_zcz] : 0;
    uint32_t per_root = (ncs == 0) ? 1 : (839 / ncs);

    uint32_t total_preambles = cfg.num_ra_preambles;
    bool assumed = false;
    if (total_preambles <= 1) {
      total_preambles = 64;
      assumed = true;
    }

    m_preamble_list = build_preamble_list((int)total_preambles, (int)per_root,
                                           (int)ncs, (int)cfg.prach_root_seq_idx);

    uint32_t roots_searched = (total_preambles + per_root - 1) / per_root;
    uint32_t estimated_ceiling = per_root * roots_searched;

    printf("\n[prach_tx] --- PRACH Configuration & Capacity Estimate ---\n");
    printf("  N_CS (zcz=%u)            = %u (TS 38.211 Table 6.3.3.1-5)\n", cfg.prach_zcz, ncs);
    printf("  Cyclic shifts / root       = %u\n", per_root);
    printf("  Logical root start         = %u (phys u=%u)\n",
           cfg.prach_root_seq_idx, (cfg.prach_root_seq_idx < 838) ? PRACH_ROOT_U_839[cfg.prach_root_seq_idx] : 0);
    if (assumed) {
      printf("  Roots searched             = %u (ASSUMING total_preambles=%u)\n", roots_searched, total_preambles);
    } else {
      printf("  Roots searched             = %u (based on num_ra_preambles=%u)\n", roots_searched, total_preambles);
    }
    printf("  Preamble list built: %zu entries (totalNumberOfRA-Preambles=%u)\n",
           m_preamble_list.size(), total_preambles);
    printf("  ESTIMATED per-RO detection ceiling = %u preambles\n", estimated_ceiling);
    printf("  NOTE: prach_zcz and prach_root_seq_idx came from InfluxDB. Verify against gNB SIB1!\n");
    printf("----------------------------------------------------------\n\n");

    if (m_flood_enabled && m_flood_num_preambles > estimated_ceiling) {
      printf("[prach_tx] WARNING: flood.num_preambles (%u) > estimated per-RO detection ceiling (%u).\n",
             m_flood_num_preambles, estimated_ceiling);
      printf("           Excess preambles raise the noise floor and reduce reliable detections.\n\n");
    }
  }

  m_initialized = true;

  if (m_flood_enabled) {
    printf("[prach_tx] FLOOD MODE: %s strategy, %u preambles, max_per_occ=%u, backoff=%.1f dB, PAPR_backoff=%.1f dB\n",
           m_flood_strategy.c_str(), m_flood_num_preambles,
           m_max_preambles_per_occasion, m_flood_power_backoff_db, m_papr_backoff_db);
  }
  if (m_multi_freq_pos_count > 1) {
    printf("[prach_tx] MULTI-FREQ-POS: %u positions @ 7 PRB spacing\n",
           m_multi_freq_pos_count);
  }
  printf("[prach_tx] Initialized:\n");
  printf("  config_idx   = %u\n", m_cfg.config_idx);
  printf("  root_seq     = %u\n", m_cfg.root_seq);
  printf("  zcz          = %u\n", m_cfg.zcz);
  printf("  freq_offset  = %u PRBs\n", m_cfg.freq_offset);
  printf("  tx_freq_hz   = %.3f MHz\n", m_cfg.tx_freq_hz / 1e6);
  printf("  srate_hz     = %.2f MHz\n", m_cfg.srate_hz / 1e6);
  printf("  tx_gain_db   = %.1f dB\n", m_cfg.tx_gain_db);
  printf("  dry_run      = %s\n", m_dry_run ? "YES" : "NO");

  // Buffer generation moved to AFTER SSB sync (regenerate_tx_buffers is called from sync_to_ssb)
  return true;
}

// ---------------------------------------------------------------------------
// correct_freq_offset
// ---------------------------------------------------------------------------
uint32_t prach_tx::correct_freq_offset(uint32_t raw_offset) const {
  int delta_prb = (int)m_cell_cfg.nof_prb -
                  (int)srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb));
  return (uint32_t)((int)raw_offset - delta_prb / 2);
}

// ---------------------------------------------------------------------------
// generate_preamble_buffer — generate a single preamble waveform via srsran
// ---------------------------------------------------------------------------
bool prach_tx::generate_preamble_buffer(std::vector<std::complex<float>>& buf,
                                         uint32_t rapid, uint32_t corrected_offset,
                                         bool apply_cfo) {
  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;
  buf.assign(preamble_len, {0.0f, 0.0f});
  cf_t *raw = reinterpret_cast<cf_t *>(buf.data());

  if (srsran_prach_gen(&m_prach, rapid, corrected_offset, raw) != SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] srsran_prach_gen failed for RAPID=%u offset=%u\n",
            rapid, corrected_offset);
    return false;
  }

  // Apply CFO correction to individual buffer if requested
  if (apply_cfo && m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w = -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < preamble_len; n++) {
      float ph = (float)(w * (double)n);
      buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// superimpose_preambles_at_offset
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
    std::fill(tmp.begin(), tmp.end(), std::complex<float>(0.0f, 0.0f));
    uint32_t rapid = m_flood_enabled ? m_flood_indices[p] : m_cfg.rapid;
    if (srsran_prach_gen(&m_prach, rapid, corrected_offset, raw) !=
        SRSRAN_SUCCESS) {
      fprintf(stderr, "[prach_tx] superimpose: gen failed RAPID=%u offset=%u\n",
              rapid, corrected_offset);
      return false;
    }

    float phase = (m_flood_enabled && p < m_flood_phases.size()) ? m_flood_phases[p] : 0.0f;
    std::complex<float> rot(std::cos(phase), std::sin(phase));

    for (uint32_t n = 0; n < preamble_len; n++) {
      pos_buf[n] += tmp[n] * rot;
    }
  }

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
// generate_occasion_composite — per-occasion subset with SLM + PAPR backoff
// ---------------------------------------------------------------------------
bool prach_tx::generate_occasion_composite(const std::vector<PreambleId>& subset) {
  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;
  uint32_t freq_offset_corrected = correct_freq_offset(m_cfg.freq_offset);

  uint32_t cap = m_max_preambles_per_occasion;
  uint32_t n_preambles = (uint32_t)std::min(subset.size(), (size_t)cap);

  // 1. Generate individual preamble buffers for this subset
  std::vector<std::vector<std::complex<float>>> bufs(n_preambles);
  for (uint32_t p = 0; p < n_preambles; p++) {
    uint32_t rapid = (p < m_flood_indices.size()) ? m_flood_indices[p] : 0;
    if (!generate_preamble_buffer(bufs[p], rapid, freq_offset_corrected, false)) {
      return false;
    }
  }

  // 2. SLM phase optimization
  m_last_slm_phases.assign(n_preambles, 0.0f);
  m_flood_phases.assign(n_preambles, 0.0f);

  if (m_flood_slm_candidates == 0) {
    for (uint32_t p = 0; p < n_preambles; p++) {
      m_flood_phases[p] = M_PI * (float)(p * p) / (float)n_preambles;
    }
    printf("[prach_tx] Newman phase optimization applied\n");
  } else {
    std::mt19937 rng(42 + m_flood_tx_count); // vary seed per occasion
    std::uniform_int_distribution<int> dist(0, 3);

    float best_papr = 1e9f;
    std::vector<std::complex<float>> temp_buf(preamble_len);
    std::vector<float> candidate_phases(n_preambles, 0.0f);

    for (uint32_t c = 0; c < m_flood_slm_candidates; c++) {
      candidate_phases[0] = 0.0f;
      for (uint32_t p = 1; p < n_preambles; p++) {
        candidate_phases[p] = (float)dist(rng) * M_PI / 2.0f;
      }

      std::fill(temp_buf.begin(), temp_buf.end(), std::complex<float>(0.0f, 0.0f));
      float peak_mag = 0.0f;
      float power = 0.0f;

      for (uint32_t p = 0; p < n_preambles; p++) {
        std::complex<float> rot(std::cos(candidate_phases[p]), std::sin(candidate_phases[p]));
        for (uint32_t n = 0; n < preamble_len; n++) {
          temp_buf[n] += bufs[p][n] * rot;
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
    printf("[prach_tx] SLM: %u candidates, best PAPR=%.2f dB\n",
           m_flood_slm_candidates, 20.0f * std::log10(best_papr));
  }

  // 3. Superimpose with best phases
  m_flood_tx_buf.assign(preamble_len, {0.0f, 0.0f});
  for (uint32_t p = 0; p < n_preambles; p++) {
    std::complex<float> rot(std::cos(m_flood_phases[p]), std::sin(m_flood_phases[p]));
    for (uint32_t n = 0; n < preamble_len; n++) {
      m_flood_tx_buf[n] += bufs[p][n] * rot;
    }
  }

  // 4. Apply CFO correction to composite
  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w = -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < preamble_len; n++) {
      float ph = (float)(w * (double)n);
      m_flood_tx_buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
  }

  // 5. Peak-normalize with PAPR backoff — NEVER hard-clip
  {
    float max_mag = 0.0f;
    float power = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      max_mag = std::max(max_mag, std::abs(m_flood_tx_buf[i]));
      power += std::norm(m_flood_tx_buf[i]);
    }
    // PAPR backoff: peak sits papr_backoff_db below full scale (0.95)
    float backoff_linear = std::pow(10.0f, m_papr_backoff_db / 20.0f);
    if (backoff_linear > 1.0f) backoff_linear = 1.0f;
    float target_peak = 0.95f * backoff_linear;
    float scale = target_peak / (max_mag + 1e-12f);

    float final_power = 0.0f, final_max_mag = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
      m_flood_tx_buf[i] *= scale;
      final_power += std::norm(m_flood_tx_buf[i]);
      final_max_mag = std::max(final_max_mag, std::abs(m_flood_tx_buf[i]));
    }
    float final_rms = std::sqrt(final_power / preamble_len);
    m_composite_papr_db = 20.0f * std::log10(final_max_mag / (final_rms + 1e-12f));
    printf("[prach_tx] Occasion composite: peak=%.4f RMS=%.4f PAPR=%.2f dB (backoff=%.1f dB, scale=%.3f)\n",
           final_max_mag, final_rms, m_composite_papr_db, m_papr_backoff_db, scale);
  }

  // Also normalize and store individual buffers for cycle mode
  for (uint32_t p = 0; p < n_preambles && p < m_flood_bufs.size(); p++) {
    float power = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) power += std::norm(m_flood_bufs[p][i]);
    power /= preamble_len;
    float target_amp = 0.7f;
    float scale = target_amp / (std::sqrt(power) + 1e-12f);
    for (auto &s : m_flood_bufs[p]) s *= scale;
  }

  return true;
}

// ---------------------------------------------------------------------------
// generate_preamble — single-preamble TX buffer
// ---------------------------------------------------------------------------
bool prach_tx::generate_preamble() {
  if (!m_initialized) return false;

  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;

  printf("[prach_tx] PRACH format params: N_cp=%u, N_seq=%u, total=%u samples\n",
         m_prach.N_cp, m_prach.N_seq, preamble_len);

  m_tx_buf.assign(preamble_len, {0.0f, 0.0f});
  cf_t *raw_buf = reinterpret_cast<cf_t *>(m_tx_buf.data());

  uint32_t freq_offset_corrected = correct_freq_offset(m_cfg.freq_offset);
  printf("[prach_tx] freq_offset: raw=%u corrected=%u\n",
         m_cfg.freq_offset, freq_offset_corrected);

  if (srsran_prach_gen(&m_prach, m_cfg.rapid, freq_offset_corrected, raw_buf) !=
      SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_prach_gen failed\n");
    return false;
  }

  m_preamble_len_samples = preamble_len;

  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w = -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < m_preamble_len_samples; n++) {
      float ph = (float)(w * (double)n);
      m_tx_buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
    printf("[prach_tx] CFO correction applied: %.1f Hz UL (sign=%+d)\n",
           m_cfo_ul_hz, m_cfo_sign);
  }

  float power = 0.0f;
  for (uint32_t i = 0; i < preamble_len; i++)
    power += std::norm(m_tx_buf[i]);
  power /= preamble_len;

  if (power < 1e-12f) {
    fprintf(stderr, "[prach_tx] WARNING: Preamble has near-zero power\n");
  }

  float target_amp = 0.7f;
  float scale = target_amp / (std::sqrt(power) + 1e-12f);
  for (auto &s : m_tx_buf) s *= scale;

  printf("[prach_tx] Preamble: RAPID=%u, len=%u, RMS=%.4f\n",
         m_cfg.rapid, m_preamble_len_samples, std::sqrt(power));

  return true;
}

// ---------------------------------------------------------------------------
// generate_flood_preambles — pre-compute individual preamble buffers
// ---------------------------------------------------------------------------
bool prach_tx::generate_flood_preambles() {
  if (!m_initialized) return false;

  uint32_t freq_offset_corrected = correct_freq_offset(m_cfg.freq_offset);

  printf("[prach_tx] FLOOD: generating %u individual preamble buffers...\n",
         m_flood_num_preambles);

  m_flood_bufs.resize(m_flood_num_preambles);
  for (uint32_t p = 0; p < m_flood_num_preambles; p++) {
    uint32_t idx = m_flood_indices[p];
    if (!generate_preamble_buffer(m_flood_bufs[p], idx, freq_offset_corrected, false)) {
      return false;
    }
  }

  printf("[prach_tx] FLOOD: %u individual buffers ready\n", m_flood_num_preambles);
  return true;
}

// ---------------------------------------------------------------------------
// generate_multi_freq_preambles
// ---------------------------------------------------------------------------
bool prach_tx::generate_multi_freq_preambles() {
  if (!m_initialized) return false;

  uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;
  uint32_t srsran_nrb = srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb));
  uint32_t max_corrected_offset = (srsran_nrb >= 6) ? (srsran_nrb - 6) : 0;

  uint32_t actual_positions = 0;
  m_freq_offsets_used.clear();
  uint32_t prach_bw_rb = 6;

  for (uint32_t k = 0; k < m_multi_freq_pos_count; k++) {
    uint32_t current_offset = m_cfg.freq_offset + k * prach_bw_rb;
    uint32_t off_k = correct_freq_offset(current_offset);
    if (off_k > max_corrected_offset) {
      printf("[prach_tx] MULTI-FREQ-POS: position %u offset=%u exceeds max=%u -> clamping to %u positions\n",
             k, off_k, max_corrected_offset, actual_positions);
      break;
    }
    m_freq_offsets_used.push_back(off_k);
    actual_positions++;
  }

  if (actual_positions == 0) {
    fprintf(stderr, "[prach_tx] FATAL: no valid freq positions\n");
    return false;
  }
  if (actual_positions < m_multi_freq_pos_count) {
    m_multi_freq_pos_count = actual_positions;
  }

  printf("[prach_tx] MULTI-FREQ-POS: %u positions, base_corrected=%u\n",
         m_multi_freq_pos_count, m_freq_offsets_used[0]);

  float per_pos_target = 0.7f / std::sqrt((float)m_multi_freq_pos_count);
  m_multi_freq_tx_buf.assign(preamble_len, {0.0f, 0.0f});

  for (uint32_t k = 0; k < m_multi_freq_pos_count; k++) {
    if (!superimpose_preambles_at_offset(m_freq_offsets_used[k],
                                         m_multi_freq_tx_buf, per_pos_target)) {
      return false;
    }
  }

  if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
    double w = -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
    for (uint32_t n = 0; n < preamble_len; n++) {
      float ph = (float)(w * (double)n);
      m_multi_freq_tx_buf[n] *= std::complex<float>(std::cos(ph), std::sin(ph));
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// regenerate_tx_buffers — call after SSB sync / CFO update
// ---------------------------------------------------------------------------
bool prach_tx::regenerate_tx_buffers() {
  printf("[prach_tx] Regenerating TX buffers with current CFO (%.1f Hz UL)\n",
         m_cfo_ul_hz);

  if (!generate_preamble()) {
    fprintf(stderr, "[prach_tx] FATAL: preamble regen failed\n");
    return false;
  }
  if (m_flood_enabled && !generate_flood_preambles()) {
    fprintf(stderr, "[prach_tx] FATAL: flood regen failed\n");
    return false;
  }
  if (m_multi_freq_pos_count > 1 && !generate_multi_freq_preambles()) {
    fprintf(stderr, "[prach_tx] FATAL: multi-freq regen failed\n");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// get_multi_ro_ra_rntis
// ---------------------------------------------------------------------------
std::vector<uint16_t> prach_tx::get_multi_ro_ra_rntis(uint32_t ro_slot) const {
  std::vector<uint16_t> out;
  uint32_t n_pos = (m_multi_freq_pos_count > 0) ? m_multi_freq_pos_count : 1;
  for (uint32_t k = 0; k < n_pos; k++) {
    uint32_t f_id = k;
    out.push_back(compute_ra_rnti(0, ro_slot, f_id, 0));
  }
  return out;
}

// ---------------------------------------------------------------------------
// get_current_rapid_list
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
  uint32_t current = m_flood_indices[m_flood_tx_count % m_flood_num_preambles];
  return std::to_string(current);
}

// ---------------------------------------------------------------------------
// get_tx_buffer
// ---------------------------------------------------------------------------
void *prach_tx::get_tx_buffer() {
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

  double rx_gain_db = 40.0;
  srsran_rf_set_rx_gain(&m_rf, rx_gain_db);
  double actual_rx_gain = srsran_rf_get_rx_gain(&m_rf);
  printf("[prach_tx] RX gain: requested=%.1f dB, actual=%.1f dB\n",
         rx_gain_db, actual_rx_gain);

  double actual_rx_rate = srsran_rf_set_rx_srate(&m_rf, m_cfg.srate_hz);
  printf("[prach_tx] RX rate: requested=%.2f MHz, actual=%.2f MHz\n",
         m_cfg.srate_hz / 1e6, actual_rx_rate / 1e6);

  double actual_tx_rate = srsran_rf_set_tx_srate(&m_rf, m_cfg.srate_hz);
  printf("[prach_tx] TX rate: requested=%.2f MHz, actual=%.2f MHz\n",
         m_cfg.srate_hz / 1e6, actual_tx_rate / 1e6);

  if (std::abs(actual_tx_rate - m_cfg.srate_hz) / m_cfg.srate_hz > 0.01) {
    fprintf(stderr, "[prach_tx] WARNING: TX sample rate deviation > 1%%\n");
  }

  double actual_rx_freq =
      srsran_rf_set_rx_freq(&m_rf, 0, m_cell_cfg.dl_freq_hz);
  printf("[prach_tx] RX freq: requested=%.3f MHz, actual=%.3f MHz\n",
         m_cell_cfg.dl_freq_hz / 1e6, actual_rx_freq / 1e6);

  if (std::abs(actual_rx_freq - m_cell_cfg.dl_freq_hz) > 100e3) {
    fprintf(stderr, "[prach_tx] FATAL: RX freq deviates >100 kHz\n");
    srsran_rf_close(&m_rf);
    return false;
  }

  double actual_tx_freq = srsran_rf_set_tx_freq(&m_rf, 0, m_cfg.tx_freq_hz);
  printf("[prach_tx] TX freq: requested=%.3f MHz, actual=%.3f MHz\n",
         m_cfg.tx_freq_hz / 1e6, actual_tx_freq / 1e6);

  if (std::abs(actual_tx_freq - m_cfg.tx_freq_hz) > 100e3) {
    fprintf(stderr, "[prach_tx] FATAL: TX freq deviates >100 kHz\n");
    srsran_rf_close(&m_rf);
    return false;
  }

  if (srsran_rf_set_tx_gain(&m_rf, m_cfg.tx_gain_db) != SRSRAN_SUCCESS) {
    fprintf(stderr, "[prach_tx] WARNING: srsran_rf_set_tx_gain returned error\n");
  }
  double actual_tx_gain = srsran_rf_get_tx_gain(&m_rf);
  printf("[prach_tx] TX gain: requested=%.1f dB, actual=%.1f dB\n",
         m_cfg.tx_gain_db, actual_tx_gain);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Register UHD async error callback
  start_async_monitor();

  printf("[prach_tx] RF device ready\n");
  m_rf_open = true;
  return true;
}

// ---------------------------------------------------------------------------
// update_ssb_anchor — LS fit of clock drift over last M SSBs
// ---------------------------------------------------------------------------
void prach_tx::update_ssb_anchor(double ssb_device_time, int sfn, int hrf,
                                  double cfo_hz, double device_time_now) {
  std::lock_guard<std::mutex> lock(m_anchor_mutex);

  SsbAnchor a;
  a.ssb_device_time  = ssb_device_time;
  a.sfn              = sfn;
  a.half_frame       = hrf;
  a.cfo_hz           = cfo_hz;
  a.last_update_time  = device_time_now;

  m_anchor_history.push_back(a);

  // Keep at most M entries
  while (m_anchor_history.size() > m_ssb_fit_window_m) {
    m_anchor_history.pop_front();
  }

  // LS fit: gNB-time (SFN in seconds) vs USRP-device-time
  if (m_anchor_history.size() >= 2) {
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    size_t n = m_anchor_history.size();

    // Use the first entry as reference
    double ref_dev = m_anchor_history.front().ssb_device_time;
    double ref_sfn = m_anchor_history.front().sfn;

    for (const auto& entry : m_anchor_history) {
      double x = entry.ssb_device_time - ref_dev;           // USRP device time delta
      double y = (entry.sfn - ref_sfn) * 10e-3;             // gNB frame time delta (10ms per frame)
      sum_x += x;
      sum_y += y;
      sum_xy += x * y;
      sum_xx += x * x;
    }

    double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) > 1e-18) {
      double slope = (n * sum_xy - sum_x * sum_y) / denom;
      // slope is gNB-time-per-USRP-device-time ratio
      // clock_drift_ppm = (slope - 1.0) * 1e6
      a.clock_drift_ppm = (slope - 1.0) * 1e6;

      printf("[prach_tx] Clock fit: slope=%.9f drift=%.3f ppm (N=%zu samples)\n",
             slope, a.clock_drift_ppm, n);
    }
  }

  m_current_anchor = a;
}

// ---------------------------------------------------------------------------
// compute_occasion_device_time — drift-corrected absolute TX time
// ---------------------------------------------------------------------------
double prach_tx::compute_occasion_device_time(uint32_t sfn, uint32_t ro_slot) const {
  std::lock_guard<std::mutex> lock(m_anchor_mutex);

  const SsbAnchor& anchor = m_current_anchor;

  uint32_t sync_sfn = (uint32_t)anchor.sfn;
  uint32_t sync_slot = m_sync_slot;  // slot within that frame

  int delta_frames = (int)sfn - (int)sync_sfn;
  if (delta_frames < 0) delta_frames += 1024; // SFN wraps at 1024

  int delta_sf = (int)ro_slot - (int)sync_slot;

  // Start symbol offset for format 0: symbol 0
  double start_symbol_offset_s = 0.0;

  double drift_correction = (1.0 + anchor.clock_drift_ppm / 1e6);

  double occ_device_time =
      anchor.ssb_device_time
    + (double)delta_frames * 10e-3 * drift_correction
    + (double)delta_sf      * 1e-3  * drift_correction
    + start_symbol_offset_s
    + m_rx_to_tx_cal_s;

  return occ_device_time;
}

// ---------------------------------------------------------------------------
// compute_timing_error — seconds since last anchor update
// ---------------------------------------------------------------------------
double prach_tx::compute_timing_error(double target_device_time) const {
  std::lock_guard<std::mutex> lock(m_anchor_mutex);
  double dt = target_device_time - m_current_anchor.last_update_time;
  return dt;
}

// ---------------------------------------------------------------------------
// sync_to_ssb
// ---------------------------------------------------------------------------
bool prach_tx::sync_to_ssb() {
  if (m_dry_run) {
    printf("[prach_tx] DRY RUN: skipping SSB sync\n");
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

  if (!m_ssb_sync.init(m_cell_cfg, m_cfg.srate_hz)) {
    fprintf(stderr, "[prach_tx] WARNING: ssb_sync init failed\n");
    if (m_has_fallback && attempt_sync_from_fallback()) {
      return true;
    }
    fprintf(stderr, "[prach_tx] FATAL: ssb_sync init failed and no fallback\n");
    return false;
  }

  uint32_t samples_per_frame = m_cell_cfg.samples_per_frame();
  m_rx_buf.assign(samples_per_frame, {0.0f, 0.0f});

  printf("[prach_tx] Searching for SSB (PCI=%u)...\n", m_cell_cfg.pci);

  int ret = srsran_rf_start_rx_stream(&m_rf, false);
  if (ret != 0) {
    fprintf(stderr, "[prach_tx] WARNING: srsran_rf_start_rx_stream returned %d\n", ret);
    if (m_has_fallback && attempt_sync_from_fallback()) {
      return true;
    }
    fprintf(stderr, "[prach_tx] FATAL: Cannot start RX stream\n");
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::vector<std::complex<float>> flush_buf(m_cell_cfg.samples_per_slot());
  for (int i = 0; i < 10; i++) {
    srsran_rf_recv(&m_rf, flush_buf.data(), flush_buf.size(), true);
  }

  cf_t *rx_data = reinterpret_cast<cf_t *>(m_rx_buf.data());
  uint32_t chunk_size = m_cell_cfg.samples_per_slot();
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
      fprintf(stderr, "[prach_tx] WARNING: RX chunk failed (nrecv=%d)\n", n);
      break;
    }
    total_recv += (uint32_t)n;
    rx_ptr += (uint32_t)n;
    remaining -= (uint32_t)n;
  }

  srsran_rf_stop_rx_stream(&m_rf);

  if (total_recv == 0) {
    if (m_has_fallback) {
      printf("[prach_tx] Attempting sniffer-based SSB sync fallback...\n");
      if (attempt_sync_from_fallback()) return true;
    }
    fprintf(stderr, "[prach_tx] FATAL: SSB sync failed (no RF RX)\n");
    return false;
  }

  uint32_t pci = 0, sfn = 0, ssb_idx = 0, t_offset = 0;
  float snr_db = 0.0f, cfo_hz = 0.0f;
  srsran_mib_nr_t mib = {};
  bool hrf = false;

  if (!m_ssb_sync.search(rx_data, total_recv, &pci, &sfn, &ssb_idx, &t_offset,
                         &snr_db, &mib, &hrf, &cfo_hz)) {
    fprintf(stderr, "[prach_tx] WARNING: SSB not found / PBCH decode failed\n");
    if (m_has_fallback) {
      printf("[prach_tx] Attempting sniffer-based SSB sync fallback...\n");
      if (attempt_sync_from_fallback()) return true;
    }
    return false;
  }

  if (pci != m_cell_cfg.pci && m_cell_cfg.pci != 0) {
    fprintf(stderr, "[prach_tx] WARNING: Detected PCI %u != expected %u\n", pci,
            m_cell_cfg.pci);
  }

  double ssb_start_device_time =
      rx_device_time + ((double)t_offset / m_cfg.srate_hz);

  // Pattern A, 15 kHz SCS, Lmax=4:
  uint32_t half_slot = (ssb_idx < 2) ? 0 : 1;
  uint32_t hf_offset = hrf ? 5 : 0;
  uint32_t ssb_slot = hf_offset + half_slot;

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

  m_cfo_dl_hz = cfo_hz;
  m_cfo_ul_hz = m_cfo_dl_hz * (m_cfg.tx_freq_hz / m_cell_cfg.dl_freq_hz);

  printf("[prach_tx] SSB sync acquired:\n");
  printf("  PCI         = %u\n", pci);
  printf("  SFN (4 LSB) = %u\n", sfn);
  printf("  SSB index   = %u\n", ssb_idx);
  printf("  SNR         = %.1f dB\n", snr_db);
  printf("  CFO (DL)    = %.1f Hz\n", m_cfo_dl_hz);
  printf("  CFO (UL)    = %.1f Hz\n", m_cfo_ul_hz);
  printf("  PSS dev_time= %.6f s\n", ssb_start_device_time);
  printf("  Slot        = %u (ssb_idx=%u, hrf=%d)\n", ssb_slot, ssb_idx, hrf);

  // B1: Update SSB anchor for timing tracker
  {
    time_t full_secs = 0;
    double frac_secs = 0.0;
    srsran_rf_get_time(&m_rf, &full_secs, &frac_secs);
    double dev_now = (double)full_secs + frac_secs;
    update_ssb_anchor(ssb_start_device_time, (int)sfn, (int)hrf,
                      (double)cfo_hz, dev_now);
    printf("[prach_tx] SSB anchor updated: drift=%.3f ppm\n",
           m_current_anchor.clock_drift_ppm);
  }

  // B4: Regenerate TX buffers with measured CFO (after sync, not before)
  return regenerate_tx_buffers();
}

// ---------------------------------------------------------------------------
// set_sync_fallback
// ---------------------------------------------------------------------------
void prach_tx::set_sync_fallback(double unix_time, uint32_t sfn,
                                 uint32_t ssb_slot, uint32_t ssb_idx) {
  m_has_fallback = true;
  m_fallback_unix_time = unix_time;
  m_fallback_sfn = sfn;
  m_fallback_ssb_slot = ssb_slot;
  m_fallback_ssb_idx = ssb_idx;
  printf("[prach_tx] Sync fallback registered: SFN=%u slot=%u ssb_idx=%u @ unix=%.3f\n",
         sfn, ssb_slot, ssb_idx, unix_time);
}

// ---------------------------------------------------------------------------
// attempt_sync_from_fallback
// ---------------------------------------------------------------------------
bool prach_tx::attempt_sync_from_fallback() {
  if (!m_has_fallback) return false;

  time_t full_secs = 0;
  double frac_secs = 0.0;
  srsran_rf_get_time(&m_rf, &full_secs, &frac_secs);
  double dev_now = (double)full_secs + frac_secs;

  auto now = std::chrono::system_clock::now();
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch()).count();
  double unix_now = (double)now_us / 1e6;

  m_dev_epoch_offset = unix_now - dev_now;
  double dev_at_mib = m_fallback_unix_time - m_dev_epoch_offset;

  uint32_t ssb_first_symbol =
      ((m_fallback_ssb_idx == 0) || (m_fallback_ssb_idx == 2)) ? 2 : 8;
  if (m_ssb_first_symbol_override >= 0) {
    ssb_first_symbol = (uint32_t)m_ssb_first_symbol_override;
  }
  double ssb_intra_slot_s = 1e-3 * (double)ssb_first_symbol / 14.0;

  m_sync_device_time = dev_at_mib - ssb_intra_slot_s;
  m_sync_sfn = m_fallback_sfn;
  m_sync_slot = m_fallback_ssb_slot;
  m_sync_t_offset = 0;
  m_synced = true;
  m_using_fallback = false;
  m_sync_ssb_idx = m_fallback_ssb_idx;

  // Update anchor for timing tracker
  update_ssb_anchor(dev_at_mib, (int)m_fallback_sfn, 0,
                    0.0, dev_now);

  printf("[prach_tx] Fallback sync (device-time calibrated):\n");
  printf("  Epoch offset   = %.3f s\n", m_dev_epoch_offset);
  printf("  anchor: SFN=%u slot=%u ssb_idx=%u\n", m_sync_sfn, m_sync_slot,
         m_sync_ssb_idx);

  return regenerate_tx_buffers();
}

// ---------------------------------------------------------------------------
// transmit_preamble
// ---------------------------------------------------------------------------
bool prach_tx::transmit_preamble(uint32_t sfn, uint32_t ro_slot) {
  if (!m_initialized) {
    fprintf(stderr, "[prach_tx] FATAL: not initialized\n");
    return false;
  }

  m_ra_rnti = compute_ra_rnti(0, ro_slot, 0, 0);

  printf("\n[prach_tx] ==================== PRACH TX ====================\n");
  printf("[prach_tx] RAPID         = %s\n", get_current_rapid_list().c_str());
  printf("[prach_tx] Target RO     = SFN %u, slot %u\n", sfn, ro_slot);
  printf("[prach_tx] RA-RNTI       = 0x%04x (%u)\n", m_ra_rnti, m_ra_rnti);
  printf("[prach_tx] TX freq       = %.3f MHz\n", m_cfg.tx_freq_hz / 1e6);
  printf("[prach_tx] TX gain       = %.1f dB\n", m_cfg.tx_gain_db);

  if (m_dry_run) {
    printf("[prach_tx] DRY RUN: skipping actual RF transmission\n");
    if (m_flood_enabled) m_flood_tx_count++;
    return true;
  }

  if (!m_rf_open) {
    fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
    return false;
  }

  if (!m_synced) {
    printf("[prach_tx] No SSB sync — acquiring reference timing...\n");
    if (!sync_to_ssb()) {
      fprintf(stderr, "[prach_tx] FATAL: SSB sync failed, cannot align TX\n");
      return false;
    }
  }

  // B2: Compute drift-corrected occasion time
  double t_tx = compute_occasion_device_time(sfn, ro_slot);
  t_tx += m_tx_offset_us * 1e-6;

  time_t full_secs_now = 0;
  double frac_secs_now = 0.0;
  srsran_rf_get_time(&m_rf, &full_secs_now, &frac_secs_now);
  double t_now = (double)full_secs_now + frac_secs_now;

  // B5: Watchdog check
  double cp_duration = 103e-6; // format 0 CP ~103 us
  double timing_error = compute_timing_error(t_tx);
  bool need_resync = std::abs(timing_error) > m_resync_cp_fraction * cp_duration;

  if (m_max_time_since_sync_ms > 0.0) {
    double elapsed_ms = timing_error * 1e3;
    if (elapsed_ms > m_max_time_since_sync_ms) need_resync = true;
  }

  if (need_resync && m_synced) {
    printf("[prach_tx] WATCHDOG: timing error %.3f ms > %.1f%% CP (%.0f us), re-anchoring\n",
           timing_error * 1e3, m_resync_cp_fraction * 100.0, cp_duration * 1e6);
    if (!sync_to_ssb()) {
      fprintf(stderr, "[prach_tx] WARNING: re-anchor failed, continuing with old timing\n");
    } else {
      t_tx = compute_occasion_device_time(sfn, ro_slot);
      t_tx += m_tx_offset_us * 1e-6;
    }
  }

  // A3: Per-occasion subset + round-robin
  uint32_t cap = m_max_preambles_per_occasion;
  size_t start = (m_preamble_occasion_counter * cap) % m_preamble_list.size();
  std::vector<PreambleId> subset;
  for (uint32_t i = 0; i < cap && (start + i) < m_preamble_list.size(); i++) {
    subset.push_back(m_preamble_list[start + i]);
  }
  // Wrap around
  for (uint32_t i = 0; i < cap && subset.size() < cap && i < (size_t)start; i++) {
    subset.push_back(m_preamble_list[i]);
  }

  if (m_flood_enabled && !subset.empty()) {
    if (!generate_occasion_composite(subset)) {
      fprintf(stderr, "[prach_tx] FATAL: occasion composite generation failed\n");
      return false;
    }
  }

  // Advance if target is in the past
  double ro_period_dur = (double)m_cell_cfg.prach_x * 10e-3;
  if (t_tx < t_now - 0.001) {
    double periods_needed = std::ceil((t_now - t_tx) / ro_period_dur);
    t_tx += periods_needed * ro_period_dur;
  }

  if (t_tx <= t_now) {
    fprintf(stderr, "[prach_tx] FATAL: target time %.6f s is in the past (now=%.6f s)\n",
            t_tx, t_now);
    return false;
  }

  printf("[prach_tx] Target RO: SFN=%u slot=%u -> dev_time=%.6f s (delta=%+.3f ms)\n",
         sfn, ro_slot, t_tx, (t_tx - t_now) * 1e3);
  printf("[prach_tx] Anchor drift=%.3f ppm, CFO=%.1f Hz, PAPR=%.2f dB\n",
         m_current_anchor.clock_drift_ppm, m_cfo_ul_hz, m_composite_papr_db);

  void *buffers[1];
  buffers[0] = get_tx_buffer();

  int nsent = srsran_rf_send_timed_multi(&m_rf, buffers, m_preamble_len_samples,
                                         (time_t)t_tx, t_tx - (time_t)t_tx,
                                         true, true, true);

  if (nsent < 0) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_rf_send_timed_multi failed (ret=%d)\n", nsent);
    return false;
  }

  if ((uint32_t)nsent != m_preamble_len_samples) {
    fprintf(stderr, "[prach_tx] WARNING: sent %d/%u samples\n", nsent,
            m_preamble_len_samples);
  }

  // C1: Populate telemetry record
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    m_last_occasion.wall_clock = tv.tv_sec + tv.tv_usec / 1e6;
  }
  m_last_occasion.gnb_sfn               = (int)sfn;
  m_last_occasion.gnb_subframe          = (int)ro_slot;
  m_last_occasion.target_tx_device_time  = t_tx;
  m_last_occasion.actual_tx_device_time  = t_tx; // UHD async would give exact time
  m_last_occasion.delta_t_us            = 0.0;
  m_last_occasion.est_cfo_hz            = m_cfo_ul_hz;
  m_last_occasion.est_clock_drift_ppm    = m_current_anchor.clock_drift_ppm;
  m_last_occasion.time_since_last_resync_ms = timing_error * 1e3;
  m_last_occasion.n_preambles_tx         = (int)(m_flood_enabled ? subset.size() : 1);
  m_last_occasion.tx_gain               = m_cfg.tx_gain_db;
  m_last_occasion.composite_papr_db     = m_composite_papr_db;
  m_last_occasion.n_underflow           = m_async_underflow.load();
  m_last_occasion.n_overflow            = m_async_overflow.load();
  m_last_occasion.n_late                = m_async_late.load();
  m_last_occasion.n_seq_error           = m_async_seq_error.load();

  printf("[prach_tx] Preamble transmitted: %d samples\n", nsent);
  if (m_flood_enabled) {
    printf("[prach_tx] FLOOD: strategy=%s, preambles=%s, subset=%zu/%zu, burst #%u\n",
           m_flood_strategy.c_str(), get_current_rapid_list().c_str(),
           subset.size(), m_preamble_list.size(), m_flood_tx_count);
    m_flood_tx_count++;
  }

  m_preamble_occasion_counter++;
  m_last_tx_time = t_tx;
  return nsent > 0;
}

// ---------------------------------------------------------------------------
// transmit_at_time
// ---------------------------------------------------------------------------
bool prach_tx::transmit_at_time(double dev_time, uint32_t sfn,
                                uint32_t ro_slot) {
  if (!m_initialized) {
    fprintf(stderr, "[prach_tx] FATAL: not initialized\n");
    return false;
  }

  m_ra_rnti = compute_ra_rnti(0, ro_slot, 0, 0);

  if (m_dry_run) {
    printf("[prach_tx] DRY RUN: skipping actual RF transmission\n");
    if (m_flood_enabled) m_flood_tx_count++;
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

  dev_time += m_tx_offset_us * 1e-6;

  // B5: Watchdog check
  double cp_duration = 103e-6;
  double timing_error = compute_timing_error(dev_time);
  bool need_resync = std::abs(timing_error) > m_resync_cp_fraction * cp_duration;

  if (m_max_time_since_sync_ms > 0.0) {
    double elapsed_ms = timing_error * 1e3;
    if (elapsed_ms > m_max_time_since_sync_ms) need_resync = true;
  }

  if (need_resync && m_synced) {
    printf("[prach_tx] WATCHDOG: timing error %.3f ms, re-anchoring\n",
           timing_error * 1e3);
    if (sync_to_ssb()) {
      dev_time = compute_occasion_device_time(sfn, ro_slot);
      dev_time += m_tx_offset_us * 1e-6;
    }
  }

  double ro_period_dur = (double)m_cell_cfg.prach_x * 10e-3;
  if (dev_time <= t_now) {
    double periods_needed = std::ceil((t_now - dev_time) / ro_period_dur);
    dev_time += periods_needed * ro_period_dur;
  }

  if (dev_time <= t_now) {
    fprintf(stderr, "[prach_tx] FATAL: target time %.6f s is in the past (now=%.6f)\n",
            dev_time, t_now);
    return false;
  }

  printf("[prach_tx] TxAtTime: SFN=%u slot=%u -> dev_time=%.6f s (delta=%+.3f ms)\n",
         sfn, ro_slot, dev_time, (dev_time - t_now) * 1e3);

  // A3: Per-occasion subset + round-robin
  uint32_t cap = m_max_preambles_per_occasion;
  size_t start = (m_preamble_occasion_counter * cap) % m_preamble_list.size();
  std::vector<PreambleId> subset;
  for (uint32_t i = 0; i < cap && (start + i) < m_preamble_list.size(); i++) {
    subset.push_back(m_preamble_list[start + i]);
  }
  for (uint32_t i = 0; i < cap && subset.size() < cap; i++) {
    subset.push_back(m_preamble_list[i]);
  }

  if (m_flood_enabled && !subset.empty()) {
    if (!generate_occasion_composite(subset)) {
      fprintf(stderr, "[prach_tx] FATAL: occasion composite generation failed\n");
      return false;
    }
  }

  void *buffers[1];
  buffers[0] = get_tx_buffer();

  int nsent = srsran_rf_send_timed_multi(
      &m_rf, buffers, m_preamble_len_samples, (time_t)dev_time,
      dev_time - (time_t)dev_time, true, true, true);

  if (nsent < 0) {
    fprintf(stderr, "[prach_tx] FATAL: srsran_rf_send_timed_multi failed (ret=%d)\n", nsent);
    return false;
  }
  if ((uint32_t)nsent != m_preamble_len_samples) {
    fprintf(stderr, "[prach_tx] WARNING: sent %d/%u samples\n", nsent,
            m_preamble_len_samples);
  }

  // C1: Telemetry
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    m_last_occasion.wall_clock = tv.tv_sec + tv.tv_usec / 1e6;
  }
  m_last_occasion.gnb_sfn               = (int)sfn;
  m_last_occasion.gnb_subframe          = (int)ro_slot;
  m_last_occasion.target_tx_device_time  = dev_time;
  m_last_occasion.actual_tx_device_time  = dev_time;
  m_last_occasion.delta_t_us            = 0.0;
  m_last_occasion.est_cfo_hz            = m_cfo_ul_hz;
  m_last_occasion.est_clock_drift_ppm    = m_current_anchor.clock_drift_ppm;
  m_last_occasion.time_since_last_resync_ms = timing_error * 1e3;
  m_last_occasion.n_preambles_tx         = (int)(m_flood_enabled ? subset.size() : 1);
  m_last_occasion.tx_gain               = m_cfg.tx_gain_db;
  m_last_occasion.composite_papr_db     = m_composite_papr_db;
  m_last_occasion.n_underflow           = m_async_underflow.load();
  m_last_occasion.n_overflow            = m_async_overflow.load();
  m_last_occasion.n_late                = m_async_late.load();
  m_last_occasion.n_seq_error           = m_async_seq_error.load();

  printf("[prach_tx] Preamble transmitted: %d samples\n", nsent);
  if (m_flood_enabled) {
    printf("[prach_tx] FLOOD: strategy=%s, subset=%zu, burst #%u\n",
           m_flood_strategy.c_str(), subset.size(), m_flood_tx_count);
    m_flood_tx_count++;
  }

  m_preamble_occasion_counter++;
  m_last_tx_time = dev_time;
  return nsent > 0;
}

// dummy main for link test
int prach_tx_link_check() { return 0; }
