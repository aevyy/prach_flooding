#pragma once

extern "C" {
#include "srsran/phy/common/phy_common_nr.h"
#include "srsran/phy/sync/ssb.h"
}
#include <cstdint>
#include <string>

struct cell_config {
    // gNB identity
    uint32_t pci = 0;
    
    // Band and carrier
    uint32_t      band               = 3;
    double        dl_freq_hz         = 1842.5e6;
    double        ul_freq_hz         = 1747.5e6;
    double        ssb_freq_hz        = 0.0;
    uint32_t      ssb_arfcn          = 0;
    uint32_t      dl_arfcn           = 368500;
    uint32_t      nof_prb            = 106;
    srsran_subcarrier_spacing_t scs  = srsran_subcarrier_spacing_15kHz;
    double        srate_hz           = 23.04e6;
    
    // SSB
    srsran_ssb_pattern_t    ssb_pattern     = SRSRAN_SSB_PATTERN_A;
    srsran_duplex_mode_t    duplex_mode     = SRSRAN_DUPLEX_MODE_FDD;
    uint32_t                ssb_period_ms   = 10;
    srsran_subcarrier_spacing_t ssb_scs     = srsran_subcarrier_spacing_15kHz;
    
    // PRACH
    uint32_t prach_config_idx   = 1;
    uint32_t prach_root_seq_idx = 1;
    uint32_t prach_zcz          = 0;
    uint32_t prach_freq_offset  = 8;
    uint32_t num_ra_preambles   = 1;
    uint32_t prach_format       = 0; // format 0
    uint32_t prach_x            = 16; // SFN period
    uint32_t prach_y            = 1;  // SFN offset
    uint32_t prach_subframe     = 4;  // subframe/slot containing PRACH
    
    // RACH response
    uint32_t ra_response_window   = 10; // slots
    uint32_t contention_resolution_timer = 64; // subframes (sf64)
    
    // CORESET0 / SearchSpace
    uint32_t coreset0_idx = 12;
    uint32_t ss0_idx      = 0;
    
    // PDSCH / PUSCH config
    srsran_mcs_table_t mcs_table = srsran_mcs_table_64qam;
    uint32_t pdsch_tda_start_symbol_and_length = 40; // startSymbol=2, length=8
    uint32_t pusch_tda_start_symbol_and_length = 27; // startSymbol=1, length=5
    uint32_t pusch_k2 = 4; // slots between UL grant and PUSCH
    
    // SSB positions
    uint64_t ssb_positions_in_burst = 0x80; // "10000000" -> only SSB 0
    
    // Timing
    int32_t n_timing_advance_offset = 25600;
    
    // Derived: sample rate in samples per slot
    uint32_t samples_per_slot() const {
        return srsran_min_symbol_sz_rb(nof_prb) * SRSRAN_NSYMB_PER_SLOT_NR;
    }
    
    uint32_t samples_per_frame() const {
        return samples_per_slot() * SRSRAN_NSLOTS_PER_FRAME_NR(srsran_subcarrier_spacing_15kHz);
    }
    
    // Fill srsran_carrier_nr_t
    void fill_carrier(srsran_carrier_nr_t* carrier) const {
        carrier->pci                   = pci;
        carrier->dl_center_frequency_hz = dl_freq_hz;
        carrier->ul_center_frequency_hz = ul_freq_hz;
        carrier->ssb_center_freq_hz     = ssb_freq_hz;
        carrier->offset_to_carrier      = 0;
        carrier->scs                    = scs;
        carrier->nof_prb                = nof_prb;
        carrier->start                  = 0;
        carrier->max_mimo_layers        = 1;
    }
};

// Parse the gNB config file (gnb.yaml) to populate cell_config.
// Returns true on success.
bool parse_gnb_config(const std::string& path, cell_config& cfg);

// Print cell config summary
void print_cell_config(const cell_config& cfg);
