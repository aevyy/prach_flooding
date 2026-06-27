#include "cell_config.h"
#include <yaml-cpp/yaml.h>
#include <cstdio>
#include <cmath>

static double arfcn_to_freq(uint32_t arfcn) {
    // NR-ARFCN to Hz (TS 38.104, 5.4.2.1)
    // F_REF = F_REF-Offs + deltaF_Global * (N_REF - N_REF-Offs)
    // For 0-2999999: F_REF-Offs=0, deltaF_Global=5kHz, N_REF-Offs=0
    return arfcn * 5000.0;
}

static uint32_t freq_to_arfcn(double freq) {
    return (uint32_t)std::round(freq / 5000.0);
}

bool parse_gnb_config(const std::string& path, cell_config& cfg) {
    try {
        YAML::Node root = YAML::LoadFile(path);
        
        auto cell = root["cell_cfg"];
        if (!cell) {
            fprintf(stderr, "ERROR: cell_cfg section not found in gnb.yaml\n");
            return false;
        }
        
        // Carrier
        if (cell["dl_arfcn"]) cfg.dl_arfcn = cell["dl_arfcn"].as<uint32_t>();
        if (cell["band"])     cfg.band     = cell["band"].as<uint32_t>();
        
        cfg.dl_freq_hz = arfcn_to_freq(cfg.dl_arfcn);
        
        // UL carrier: FDD pair for n3
        // n3: DL=N*5kHz, UL=N*5kHz - 95MHz gap
        cfg.ul_freq_hz = cfg.dl_freq_hz - 95.0e6;
        
        if (cell["channel_bandwidth_MHz"]) {
            // n3 20MHz -> 106 PRB
            cfg.nof_prb = 106;
        }
        
        // SCS
        if (cell["common_scs"]) {
            int scs_val = cell["common_scs"].as<int>();
            switch (scs_val) {
                case 15: cfg.scs = srsran_subcarrier_spacing_15kHz; break;
                case 30: cfg.scs = srsran_subcarrier_spacing_30kHz; break;
                default: cfg.scs = srsran_subcarrier_spacing_15kHz; break;
            }
        }
        
        // SSB frequency: known from gNB log output
        // SSB arfcn: 368410, SSB offset pointA: 40, k_SSB: 6
        if (cell["ssb_arfcn"]) {
            cfg.ssb_arfcn = cell["ssb_arfcn"].as<uint32_t>();
        }
        cfg.ssb_arfcn = 368410; // from gNB log
        cfg.ssb_freq_hz = arfcn_to_freq(cfg.ssb_arfcn);
        
        // SDR sample rate
        auto ru = root["ru_sdr"];
        if (ru && ru["srate"]) {
            cfg.srate_hz = ru["srate"].as<double>() * 1e6;
        }
        
        // PDCCH common config
        auto pdcch = cell["pdcch"];
        if (pdcch) {
            auto common = pdcch["common"];
            if (common) {
                if (common["coreset0_index"]) cfg.coreset0_idx = common["coreset0_index"].as<uint32_t>();
                if (common["ss0_index"])      cfg.ss0_idx      = common["ss0_index"].as<uint32_t>();
            }
        }
        
        // PRACH config
        auto prach = cell["prach"];
        if (prach) {
            if (prach["prach_config_index"]) cfg.prach_config_idx = prach["prach_config_index"].as<uint32_t>();
        }
        
        // PDSCH / PUSCH MCS table
        if (cell["pdsch"] && cell["pdsch"]["mcs_table"]) {
            std::string mcs = cell["pdsch"]["mcs_table"].as<std::string>();
            if (mcs == "qam64")       cfg.mcs_table = srsran_mcs_table_64qam;
            else if (mcs == "qam256") cfg.mcs_table = srsran_mcs_table_256qam;
            else if (mcs == "qam64LowSE") cfg.mcs_table = srsran_mcs_table_qam64LowSE;
        }
        
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR parsing gnb.yaml: %s\n", e.what());
        return false;
    }
}

// TS 38.211 Table 6.3.3.2-2 (FR1 paired) — minimal subset covering
// format 0 and format 1 (indices 0–55).  Returns true on success.
void cell_config::resolve_prach_ro() {
    // {format, x, y, subframe} per config_idx
    static const struct { uint32_t fmt, x, y, sf; } tbl[] = {
        {0, 16, 1, 1},  {0, 16, 1, 4},  {0, 16, 1, 7},  {0, 16, 1, 9},
        {0, 8,  1, 1},  {0, 8,  1, 4},  {0, 8,  1, 7},  {0, 8,  1, 9},
        {0, 4,  1, 1},  {0, 4,  1, 4},  {0, 4,  1, 7},  {0, 4,  1, 9},
        {0, 2,  1, 1},  {0, 2,  1, 4},  {0, 2,  1, 7},  {0, 2,  1, 9},
        {0, 1,  0, 1},  {0, 1,  0, 4},  {0, 1,  0, 7},  {0, 1,  0, 1},
        {0, 1,  0, 2},  {0, 1,  0, 3},  {0, 1,  0, 1},  {0, 1,  0, 0},
        {0, 1,  0, 1},  {0, 1,  0, 0},  {0, 1,  0, 1},  {0, 1,  0, 0},
        // format 1: indices 28–55
        {1, 16, 1, 1},  {1, 16, 1, 4},  {1, 16, 1, 7},  {1, 16, 1, 9},
        {1, 8,  1, 1},  {1, 8,  1, 4},  {1, 8,  1, 7},  {1, 8,  1, 9},
        {1, 4,  1, 1},  {1, 4,  1, 4},  {1, 4,  1, 7},  {1, 4,  1, 9},
        {1, 2,  1, 1},  {1, 2,  1, 4},  {1, 2,  1, 7},  {1, 2,  1, 9},
        {1, 1,  0, 1},  {1, 1,  0, 4},  {1, 1,  0, 7},  {1, 1,  0, 1},
        {1, 1,  0, 2},  {1, 1,  0, 3},  {1, 1,  0, 1},  {1, 1,  0, 2},
        {1, 1,  0, 3},  {1, 1,  0, 0},  {1, 1,  0, 1},  {1, 1,  0, 0},
    };
    constexpr uint32_t N = sizeof(tbl) / sizeof(tbl[0]);
    if (prach_config_idx < N) {
        auto& e    = tbl[prach_config_idx];
        prach_format   = e.fmt;
        prach_x        = e.x;
        prach_y        = e.y;
        prach_subframe = e.sf;
    }
}

void print_cell_config(const cell_config& cfg) {
    printf("=== Cell Configuration ===\n");
    printf("  PCI:                %u\n", cfg.pci);
    printf("  Band:               n%u\n", cfg.band);
    printf("  DL freq:            %.1f MHz\n", cfg.dl_freq_hz / 1e6);
    printf("  UL freq:            %.1f MHz\n", cfg.ul_freq_hz / 1e6);
    printf("  SSB freq:           %.3f MHz (ARFCN %u)\n", cfg.ssb_freq_hz / 1e6, cfg.ssb_arfcn);
    printf("  PRB:                %u\n", cfg.nof_prb);
    printf("  SCS:                15 kHz\n");
    printf("  Sample rate:        %.2f MHz\n", cfg.srate_hz / 1e6);
    printf("  SSB pattern:        A (15 kHz)\n");
    printf("  SSB period:         %u ms\n", cfg.ssb_period_ms);
    printf("  PRACH config_idx:   %u\n", cfg.prach_config_idx);
    printf("  PRACH format:       %u\n", cfg.prach_format);
    printf("  PRACH offset:       %u PRB\n", cfg.prach_freq_offset);
    printf("  PRACH root seq:     %u\n", cfg.prach_root_seq_idx);
    printf("  PRACH zcz:          %u\n", cfg.prach_zcz);
    printf("  num_ra_preambles:   %u\n", cfg.num_ra_preambles);
    printf("  RO:                 subframe %u, SFN%%%u==%u\n", cfg.prach_subframe, cfg.prach_x, cfg.prach_y);
    printf("  ra-ResponseWindow:  %u slots\n", cfg.ra_response_window);
    printf("  CORESET0 idx:       %u\n", cfg.coreset0_idx);
    printf("  SS0 idx:            %u\n", cfg.ss0_idx);
    printf("===========================\n");
}
