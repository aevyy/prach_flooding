#include "cell_config.h"
#include <cstdio>
#include <cmath>

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
