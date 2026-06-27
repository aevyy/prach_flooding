#pragma once

#include "cell_config.h"
extern "C" {
#include "srsran/phy/phch/pdcch_nr.h"
#include "srsran/phy/phch/pdsch_nr.h"
#include "srsran/phy/dft/ofdm.h"
}
#include <cstdint>

// DL resource grid builder: carrier, CORESET0, PDCCH/PDSCH encode, OFDM modulate
class dl_grid {
public:
    dl_grid() = default;
    ~dl_grid();
    
    bool init(const cell_config& cfg);
    
    // Encode PDCCH with DCI format 1_0 for RAR (CRC scrambled by RA-RNTI)
    // The PDCCH is placed in the internal slot grid.
    bool encode_pdcch_rar(uint16_t ra_rnti, uint32_t slot_idx);
    
    // Encode PDSCH carrying a RAR PDU into the same grid (overlaid)
    bool encode_pdsch_rar(const uint8_t* rar_pdu, uint32_t pdu_len, uint32_t slot_idx);
    
    // OFDM-modulate the slot grid to time-domain samples.
    // out_samples must point to a buffer of num_samples_per_slot() complex floats.
    bool modulate(cf_t* out_samples);
    
    // Access grid
    cf_t* get_slot_grid() { return m_grid; }
    uint32_t grid_size() const { return m_grid_sz; }
    uint32_t num_samples_per_slot() const;
    
    // Get carrier / coreset
    const srsran_carrier_nr_t& get_carrier() const { return m_carrier; }
    const srsran_coreset_t& get_coreset0() const { return m_coreset0; }
    
    // TODO M3: encode_pdcch_msg4 (TC-RNTI)
    // TODO M3: encode_pdsch_msg4

private:
    cell_config         m_cfg;
    srsran_carrier_nr_t m_carrier = {};
    srsran_coreset_t    m_coreset0 = {};
    srsran_pdcch_nr_t   m_pdcch = {};
    srsran_pdsch_nr_t   m_pdsch = {};
    srsran_ofdm_t       m_ofdm = {};
    cf_t*               m_grid = nullptr;
    uint32_t            m_grid_sz = 0;
    bool                m_initialized = false;
    bool                m_pdcch_initialized = false;
    bool                m_pdsch_initialized = false;
    bool                m_ofdm_initialized = false;
    
    // Last state
    bool     m_has_pdcch = false;
    bool     m_has_pdsch = false;
    uint32_t m_last_pdcch_slot = 0;
    uint32_t m_last_pdsch_slot = 0;
    uint16_t m_last_ra_rnti    = 0;
};
