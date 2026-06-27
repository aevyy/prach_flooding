#include "cell_config.h"
#include "ssb_sync.h"
extern "C" {
#include "srsran/phy/common/phy_common_nr.h"
}
#include <cstdio>
#include <cstring>
#include <cstdlib>

// M0 PBCH decode test.
// Usage: test_pbch <capture_file.cf32> [rfn_dl_freq_hz]
// The capture file should be a raw cf32 (interleaved float32 I/Q) at the DL sample rate.
// The first argument is the file path.
// The second optional argument overrides the DL center frequency.

static bool load_capture(const char* path, cf_t** out_buf, uint32_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // cf_t = 2 floats (8 bytes)
    *out_len = sz / sizeof(cf_t);
    *out_buf = (cf_t*)malloc(sz);
    if (!*out_buf) {
        fclose(f);
        return false;
    }
    fread(*out_buf, 1, sz, f);
    fclose(f);
    
    printf("Loaded capture: %u samples from %s (%.2f ms)\n",
           *out_len, path, *out_len / (*out_len > 0 ? 23.04e-3 : 1));
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <capture_file.cf32> [dl_freq_hz]\n", argv[0]);
        printf("Decodes PBCH from a raw cf32 capture.\n");
        return 1;
    }
    
    const char* capture_path = argv[1];
    double dl_freq = 1842.5e6;
    if (argc >= 3) {
        dl_freq = atof(argv[2]);
    }
    
    // Load capture
    cf_t* buf = nullptr;
    uint32_t buf_len = 0;
    if (!load_capture(capture_path, &buf, &buf_len)) {
        return 1;
    }
    
    // Cell config
    cell_config cfg;
    cfg.band           = 3;
    cfg.pci            = 0;
    cfg.dl_freq_hz     = dl_freq;
    cfg.nof_prb        = 106;
    cfg.scs            = srsran_subcarrier_spacing_15kHz;
    cfg.ssb_freq_hz    = 368410 * 5000.0; // SSB ARFCN 368410
    cfg.ssb_scs        = srsran_subcarrier_spacing_15kHz;
    cfg.ssb_pattern    = SRSRAN_SSB_PATTERN_A;
    cfg.duplex_mode    = SRSRAN_DUPLEX_MODE_FDD;
    cfg.ssb_period_ms  = 10;
    cfg.srate_hz       = 23.04e6;
    
    printf("Cell: band=n%d, PCI=%u, DL=%.1f MHz, SSB=%.3f MHz, PRB=%u\n",
           cfg.band, cfg.pci, cfg.dl_freq_hz/1e6, cfg.ssb_freq_hz/1e6, cfg.nof_prb);
    
    // Initialize SSB sync
    ssb_sync sync;
    if (!sync.init(cfg, cfg.srate_hz)) {
        fprintf(stderr, "FATAL: ssb_sync init failed\n");
        free(buf);
        return 1;
    }
    
    // Search for SSB
    uint32_t pci, sfn, ssb_idx, t_offset;
    float snr_db;
    srsran_mib_nr_t mib;
    
    bool found = sync.search(buf, buf_len, &pci, &sfn, &ssb_idx,
                             &t_offset, &snr_db, &mib);
    
    printf("\n=== PBCH Decode Result ===\n");
    if (found) {
        printf("  SUCCESS: PBCH CRC passed!\n");
        printf("  PCI:        %u\n", pci);
        printf("  SFN (4LSB): %u\n", sfn);
        printf("  SSB idx:    %u\n", ssb_idx);
        printf("  t_offset:   %u samples\n", t_offset);
        printf("  SNR:        %.1f dB\n", snr_db);
        printf("  MIB:\n");
        printf("    coreset0_idx:   %u\n", mib.coreset0_idx);
        printf("    ss0_idx:        %u\n", mib.ss0_idx);
        printf("    cell_barred:    %s\n", mib.cell_barred ? "YES" : "no");
        printf("    intra_freq_res: %s\n", mib.intra_freq_reselection ? "yes" : "no");
        printf("    scs_common:     %u kHz\n", 
               (mib.scs_common == srsran_subcarrier_spacing_15kHz) ? 15 : 
               (mib.scs_common == srsran_subcarrier_spacing_30kHz) ? 30 : 0);
        printf("    k_ssb:          %u\n", mib.ssb_offset);
        printf("    dmrs_typeA_pos: %u\n", mib.dmrs_typeA_pos);
        printf("    hrf:            %s\n", mib.hrf ? "second" : "first");
    } else {
        printf("  FAILED: PBCH CRC did not pass.\n");
        printf("  Possible causes:\n");
        printf("    1. SNR too low (try cabled path / higher RX gain)\n");
        printf("    2. SSB frequency offset incorrect\n");
        printf("    3. Sample rate mismatch\n");
        printf("    4. Capture does not contain SSB\n");
    }
    
    free(buf);
    return found ? 0 : 1;
}
