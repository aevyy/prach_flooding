#pragma once

#include "cell_config.h"
extern "C" {
#include "srsran/phy/phch/prach.h"
}
#include <cstdint>

// UL receiver: PRACH detection (Msg1) and PUSCH decode (Msg3)
class ul_rx {
public:
    ul_rx() = default;
    ~ul_rx();
    
    bool init(const cell_config& cfg);
    
    // Detect PRACH preambles in received samples
    // Returns number of detected preambles. Sets out_indices and out_offsets.
    int detect_prach(const cf_t* signal, uint32_t sig_len,
                     uint32_t* out_indices, float* out_ta_offsets,
                     float* out_metrics, uint32_t max_detect);
    
    // TODO M3: PUSCH decode for Msg3

private:
    srsran_prach_t m_prach = {};
    cell_config    m_cfg;
    bool           m_initialized = false;
};
