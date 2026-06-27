#include "dl_grid.h"
extern "C" {
#include "srsran/phy/phch/dci_nr.h"
#include "srsran/phy/phch/sch_cfg_nr.h"
#include "srsran/phy/scrambling/scrambling.h"
#include "srsran/phy/ch_estimation/dmrs_pdcch.h"
#include "srsran/phy/ch_estimation/dmrs_sch.h"
#include "srsran/phy/fec/softbuffer.h"
}
#include "ra_rnti.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

dl_grid::~dl_grid() {
    if (m_grid) {
        free(m_grid);
        m_grid = nullptr;
    }
    if (m_pdcch_initialized) srsran_pdcch_nr_free(&m_pdcch);
    if (m_pdsch_initialized) srsran_pdsch_nr_free(&m_pdsch);
    if (m_ofdm_initialized)  srsran_ofdm_tx_free(&m_ofdm);
}

bool dl_grid::init(const cell_config& cfg) {
    m_cfg = cfg;
    cfg.fill_carrier(&m_carrier);

    // Allocate resource grid for one slot
    m_grid_sz = SRSRAN_SLOT_LEN_RE_NR(cfg.nof_prb);
    m_grid = (cf_t*)calloc(m_grid_sz, sizeof(cf_t));
    if (!m_grid) {
        fprintf(stderr, "ERROR: failed to allocate resource grid (%u RE)\n", m_grid_sz);
        return false;
    }

    // Set up CORESET0 from config index
    int ret = srsran_coreset_zero(m_carrier.pci,
                                  0, // ssb_pointA_freq_offset_Hz
                                  cfg.ssb_scs,
                                  cfg.scs,
                                  cfg.coreset0_idx,
                                  &m_coreset0);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_coreset_zero failed for idx=%u\n", cfg.coreset0_idx);
        return false;
    }

    // Initialize PDCCH for TX
    srsran_pdcch_nr_args_t pdcch_args = {};
    pdcch_args.disable_simd    = false;
    pdcch_args.measure_evm     = false;
    pdcch_args.measure_time    = false;

    ret = srsran_pdcch_nr_init_tx(&m_pdcch, &pdcch_args);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_pdcch_nr_init_tx failed\n");
        return false;
    }
    m_pdcch_initialized = true;

    ret = srsran_pdcch_nr_set_carrier(&m_pdcch, &m_carrier, &m_coreset0);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_pdcch_nr_set_carrier failed\n");
        return false;
    }

    // Initialize PDSCH for TX (gNB mode)
    srsran_pdsch_nr_args_t pdsch_args = {};
    pdsch_args.sch.max_nof_iter        = 6;
    pdsch_args.measure_evm             = false;
    pdsch_args.measure_time            = false;
    pdsch_args.max_prb                 = cfg.nof_prb;
    pdsch_args.max_layers              = 1;

    ret = srsran_pdsch_nr_init_enb(&m_pdsch, &pdsch_args);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_pdsch_nr_init_enb failed\n");
        return false;
    }
    m_pdsch_initialized = true;

    ret = srsran_pdsch_nr_set_carrier(&m_pdsch, &m_carrier);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_pdsch_nr_set_carrier failed\n");
        return false;
    }

    // Initialize OFDM modulator
    ret = srsran_ofdm_tx_init(&m_ofdm, SRSRAN_CP_NORM, m_grid, nullptr, cfg.nof_prb);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_ofdm_tx_init failed\n");
        return false;
    }
    m_ofdm_initialized = true;

    printf("DL grid initialized: %u PRB, CORESET0=%u (bw=%u PRB, dur=%u sym, offset=%u RB)\n",
           cfg.nof_prb, cfg.coreset0_idx,
           srsran_coreset_get_bw(&m_coreset0),
           m_coreset0.duration,
           m_coreset0.offset_rb);

    m_initialized = true;
    return true;
}

bool dl_grid::encode_pdcch_rar(uint16_t ra_rnti, uint32_t slot_idx) {
    if (!m_initialized) return false;

    // Build DCI message
    srsran_dci_msg_nr_t dci_msg = {};

    // DCI format 1_0 for RAR (fallback DCI, CRC scrambled by RA-RNTI)
    // Need to pack a DL DCI manually since we don't have the full DCI config set up
    // For RAR, the DCI 1_0 in common SS has a specific size
    // We'll use the srsran_dci_nr_t to compute the size first
    
    // Set up DCI config for the carrier
    srsran_dci_nr_t dci_cfg = {};
    srsran_dci_cfg_nr_t dci_cfg_params = {};
    dci_cfg_params.coreset0_bw       = srsran_coreset_get_bw(&m_coreset0);
    dci_cfg_params.bwp_dl_initial_bw  = m_cfg.nof_prb;
    dci_cfg_params.bwp_dl_active_bw   = m_cfg.nof_prb;
    dci_cfg_params.bwp_ul_initial_bw  = m_cfg.nof_prb;
    dci_cfg_params.bwp_ul_active_bw   = m_cfg.nof_prb;
    dci_cfg_params.monitor_common_0_0 = true;
    dci_cfg_params.monitor_0_0_and_1_0 = false;

    if (srsran_dci_nr_set_cfg(&dci_cfg, &dci_cfg_params) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_dci_nr_set_cfg failed\n");
        return false;
    }

    // DCI size for format 1_0 in common SS
    uint32_t dci_size = srsran_dci_nr_size(&dci_cfg, srsran_search_space_type_common_1,
                                           srsran_dci_format_nr_1_0);
    printf("  PDCCH: DCI 1_0 (RA-RNTI=0x%04x) size=%u bits\n", ra_rnti, dci_size);

    // Build DCI 1_0 using srsRAN's DCI packer
    srsran_dci_dl_nr_t dci_dl = {};
    dci_dl.ctx.ss_type          = srsran_search_space_type_common_1;
    dci_dl.ctx.coreset_id       = 0;
    dci_dl.ctx.coreset_start_rb = m_coreset0.offset_rb;
    dci_dl.ctx.rnti_type        = srsran_rnti_type_ra;
    dci_dl.ctx.format           = srsran_dci_format_nr_1_0;
    dci_dl.ctx.rnti             = ra_rnti;
    dci_dl.ctx.location.ncce    = 0;
    dci_dl.ctx.location.L       = 2;  // log2(4) - logarithmic aggregation level!
    dci_dl.coreset0_bw          = dci_cfg_params.coreset0_bw;

    // Frequency domain: RIV for S=0, L=4, N_RB_DL_BWP = coreset0_bw or bwp_dl_initial_bw?
    // DCI 1_0 in common SS uses the size of CORESET0 bandwidth for frequency allocation
    // when DCI is transmitted in CORESET0 (TS 38.212 7.3.1.2.1)
    uint32_t N_bw = dci_cfg_params.coreset0_bw; // use CORESET0 BW for freq alloc
    // RIV = N*(L-1)+S = 96*(4-1)+0 = 288
    dci_dl.freq_domain_assigment = N_bw * (4 - 1) + 0; // RIV = SIZE_BW_DL*(L-1)+S
    
    // Time domain: index 0 of default table
    dci_dl.time_domain_assigment = 0;
    
    // VRB-to-PRB mapping: non-interleaved
    dci_dl.vrb_to_prb_mapping = 0;
    
    // MCS: 0
    dci_dl.mcs = 0;
    
    // RV: 0
    dci_dl.rv = 0;
    
    // NDI (for TC-RNTI, not applicable for RA-RNTI but set to 0)
    dci_dl.ndi = 0;
    
    // HARQ process number: not used for broadcast, set to 0
    dci_dl.pid = 0;
    
    // DAI: per TS 38.212, only present if more than 1 serving cell, set to 0
    dci_dl.dai = 0;
    
    // TPC for PUCCH: first value = 0 (not used for RAR as no HARQ feedback)
    dci_dl.tpc = 0;
    
    // PUCCH resource indicator: 0 (not used for RAR)
    dci_dl.pucch_resource = 0;
    
    // PDSCH-to-HARQ_feedback timing: 0 (not applicable for RAR)
    dci_dl.harq_feedback = 0;
    
    // reserved bits: 0
    dci_dl.reserved = 0;

    // Pack DCI using srsRAN
    int pack_ret = srsran_dci_nr_dl_pack(&dci_cfg, &dci_dl, &dci_msg);
    if (pack_ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_dci_nr_dl_pack failed (ret=%d)\n", pack_ret);
        return false;
    }

    printf("  DCI 1_0 packed: %u bits\n", dci_msg.nof_bits);

    // Clear grid for this slot
    memset(m_grid, 0, m_grid_sz * sizeof(cf_t));

    // Place PDCCH DMRS in the grid (mandatory: without this, receiver can't do channel estimation)
    srsran_slot_cfg_t slot_cfg = {};
    slot_cfg.idx = slot_idx;
    int dmrs_ret = srsran_dmrs_pdcch_put(&m_carrier, &m_coreset0, &slot_cfg,
                                          &dci_msg.ctx.location, m_grid);
    if (dmrs_ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_dmrs_pdcch_put failed (ret=%d)\n", dmrs_ret);
        return false;
    }

    // Encode PDCCH
    int ret = srsran_pdcch_nr_encode(&m_pdcch, &dci_msg, m_grid);
    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_pdcch_nr_encode failed (ret=%d)\n", ret);
        return false;
    }

    m_last_pdcch_slot = slot_idx;
    m_last_ra_rnti    = ra_rnti;
    m_has_pdcch       = true;
    
    printf("  PDCCH encoded successfully\n");
    return true;
}

bool dl_grid::encode_pdsch_rar(const uint8_t* rar_pdu, uint32_t pdu_len, uint32_t slot_idx) {
    if (!m_initialized) return false;

    // Set up PDSCH grant for RAR
    srsran_sch_cfg_nr_t sch_cfg = {};

    // Grant parameters matching the gNB's RAR: rb=[0..3) tbs=10 (from gNB log)
    sch_cfg.grant.rnti      = m_last_ra_rnti;
    sch_cfg.grant.rnti_type = srsran_rnti_type_ra;
    sch_cfg.grant.k         = 0;  // k0=0 (same slot as PDCCH)
    sch_cfg.grant.S         = 2;  // start symbol 2 (from TDA SLIV=40 → S=2, L=12)
    sch_cfg.grant.L         = 12; // length 12 symbols
    sch_cfg.grant.mapping   = srsran_sch_mapping_type_A;
    sch_cfg.grant.nof_layers = 1;
    sch_cfg.grant.dci_format = srsran_dci_format_nr_1_0;

    // Frequency domain: RBs 0-3
    for (uint32_t i = 0; i < 4; i++) {
        sch_cfg.grant.prb_idx[i] = true;
    }
    sch_cfg.grant.nof_prb = 4;

    // Transport block
    sch_cfg.grant.tb[0].mod      = SRSRAN_MOD_QPSK; // RAR uses QPSK
    sch_cfg.grant.tb[0].mcs      = 0;  // MCS 0
    sch_cfg.grant.tb[0].tbs      = pdu_len; // 10 bytes per gNB log
    sch_cfg.grant.tb[0].R        = 0.12; // Very low code rate for RAR
    sch_cfg.grant.tb[0].rv       = 0;
    sch_cfg.grant.tb[0].ndi      = 0;
    sch_cfg.grant.tb[0].N_L      = 1;
    sch_cfg.grant.tb[0].enabled  = true;
    sch_cfg.grant.tb[0].cw_idx   = 0;

    // DMRS config
    sch_cfg.dmrs.type           = srsran_dmrs_sch_type_1;
    sch_cfg.dmrs.typeA_pos      = srsran_dmrs_sch_typeA_pos_2;
    sch_cfg.dmrs.length         = srsran_dmrs_sch_len_1;
    sch_cfg.dmrs.additional_pos = srsran_dmrs_sch_add_pos_2;
    sch_cfg.grant.nof_dmrs_cdm_groups_without_data = 0; // default: no DMRS CDM group reservation for RAR

    // nof_re for 4 RB, 12 symbols, no DMRS CDM group reservation
    // = 4 RBs * 12 symbols * 12 RE = 576
    sch_cfg.grant.tb[0].nof_re   = 576;
    sch_cfg.grant.tb[0].nof_bits = 576 * 2; // QPSK = 1152 bits

    // Initialize TX softbuffer (required by SCH encoder)
    srsran_softbuffer_tx_t softbuffer_tx = {};
    if (srsran_softbuffer_tx_init(&softbuffer_tx, sch_cfg.grant.nof_prb) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: softbuffer_tx init failed\n");
        return false;
    }
    sch_cfg.grant.tb[0].softbuffer.tx = &softbuffer_tx;

    // SCH config
    sch_cfg.sch_cfg.mcs_table    = m_cfg.mcs_table;
    sch_cfg.sch_cfg.xoverhead    = srsran_xoverhead_0;
    sch_cfg.sch_cfg.limited_buffer_rm = false;

    // No reserved RE patterns

    // Prepare data buffer
    uint8_t* data[SRSRAN_MAX_TB] = {};
    uint8_t* tb_data = (uint8_t*)calloc(pdu_len, 1);
    if (!tb_data) return false;
    memcpy(tb_data, rar_pdu, pdu_len);
    data[0] = tb_data;

    // Allocate slot symbol grid
    cf_t* sf_symbols[SRSRAN_MAX_PORTS] = {};
    sf_symbols[0] = m_grid;

    // Place PDSCH DMRS in the grid
    srsran_slot_cfg_t slot_cfg_ps = {};
    slot_cfg_ps.idx = slot_idx;
    srsran_dmrs_sch_t dmrs_sch = {};
    if (srsran_dmrs_sch_init(&dmrs_sch, false) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: dmrs_sch init failed\n");
        free(tb_data);
        return false;
    }
    // Set carrier for DMRS generation
    if (srsran_dmrs_sch_set_carrier(&dmrs_sch, &m_carrier) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: dmrs_sch set_carrier failed\n");
        srsran_dmrs_sch_free(&dmrs_sch);
        free(tb_data);
        return false;
    }
    if (srsran_dmrs_sch_put_sf(&dmrs_sch, &slot_cfg_ps, &sch_cfg, &sch_cfg.grant, sf_symbols[0]) != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: dmrs_sch put_sf failed\n");
        srsran_dmrs_sch_free(&dmrs_sch);
        free(tb_data);
        return false;
    }
    srsran_dmrs_sch_free(&dmrs_sch);

    int ret = srsran_pdsch_nr_encode(&m_pdsch, &sch_cfg, &sch_cfg.grant, data, sf_symbols);
    srsran_softbuffer_tx_free(&softbuffer_tx);
    free(tb_data);

    if (ret != SRSRAN_SUCCESS) {
        fprintf(stderr, "ERROR: srsran_pdsch_nr_encode failed (ret=%d)\n", ret);
        return false;
    }

    m_last_pdsch_slot = slot_idx;
    m_has_pdsch       = true;

    printf("  PDSCH RAR encoded: %u bytes, RB 0-3, syms %u-%u\n",
           pdu_len, sch_cfg.grant.S, sch_cfg.grant.S + sch_cfg.grant.L - 1);
    return true;
}

bool dl_grid::modulate(cf_t* out_samples) {
    if (!m_initialized || !m_ofdm_initialized) return false;

    // OFDM modulate the slot grid
    // Note: srsran_ofdm_tx_sf produces m_ofdm.cfg.symbol_sz * SRSRAN_NSYMB_PER_SLOT_NR samples
    // We need to provide the output buffer
    // The OFDM object was initialized with nof_prb, so symbol_sz = min_symbol_sz_rb(nof_prb) = 128 for 6 PRB or 256 for 106 PRB
    // For 106 PRB: symbol_sz = 256 (since 128*12=1536 < 106*12=1272, so 256 is needed)
    
    // Re-init OFDM with output buffer
    // The srsran_ofdm_tx_sf writes to out_buffer set during init
    // We need to reset the output pointer
    m_ofdm.cfg.out_buffer = out_samples;
    
    srsran_ofdm_tx_sf(&m_ofdm);

    return true;
}

uint32_t dl_grid::num_samples_per_slot() const {
    return srsran_min_symbol_sz_rb(m_cfg.nof_prb) * SRSRAN_NSYMB_PER_SLOT_NR;
}
