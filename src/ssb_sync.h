#pragma once

#include "cell_config.h"
extern "C" {
#include "srsran/phy/sync/ssb.h"
#include "srsran/phy/phch/pbch_msg_nr.h"
}
#include <cstdint>

class ssb_sync {
public:
    ssb_sync() = default;
    ~ssb_sync();
    
    // Initialize the SSB sync object with the cell config
    bool init(const cell_config& cfg, double rx_srate_hz);
    
    // Search for SSB in time-domain samples. Returns true if SSB detected and PBCH decoded.
    // `in` is the RX buffer of samples at rx_srate_hz.
    // `nof_samples` is the number of samples.
    // Resets the object for a new search.
    bool search(const cf_t* in, uint32_t nof_samples, uint32_t* out_pci,
                uint32_t* out_sfn, uint32_t* out_ssb_idx,
                uint32_t* out_t_offset, float* out_snr_db,
                srsran_mib_nr_t* out_mib, bool* out_hrf = nullptr);
    // Track SSB (when we already know PCI and approximate timing).
    bool track(const cf_t* sf_buffer, uint32_t N_id, uint32_t ssb_idx, uint32_t n_hf,
               srsran_mib_nr_t* out_mib, float* out_snr_db);
    
    // Get the SSB object (for advanced usage)
    srsran_ssb_t* get_ssb() { return &m_ssb; }
    
    // Get the last decoded MIB
    const srsran_mib_nr_t& get_mib() const { return m_mib; }
    
    // Get the time offset of last detected SSB in samples
    uint32_t get_t_offset() const { return m_t_offset; }
    
    // Get SSB SNR
    float get_snr_db() const { return m_snr_db; }
    
private:
    srsran_ssb_t     m_ssb = {};
    cell_config      m_cfg;
    srsran_mib_nr_t  m_mib = {};
    uint32_t         m_t_offset = 0;
    float            m_snr_db   = 0;
    bool             m_initialized = false;
};
