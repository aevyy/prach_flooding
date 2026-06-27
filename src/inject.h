#pragma once

#include "cell_config.h"
#include "dl_grid.h"
#include "rar.h"
#include "msg4.h"
#include <cstdint>

// Injection scheduler: times and transmits Msg2/Msg4
class injector {
public:
    injector() = default;
    
    bool init(const cell_config& cfg, dl_grid& dl, rar_builder& rar, msg4_builder& m4);
    
    // TODO M2: Schedule and transmit Msg2 at the next RACH occasion
    bool inject_msg2(uint32_t sfn, uint32_t slot, uint16_t tc_rnti, uint16_t ta_cmd);
    
    // TODO M3: Schedule and transmit Msg4
    bool inject_msg4(uint32_t sfn, uint32_t slot, uint16_t tc_rnti,
                     const uint8_t* cid, uint32_t cid_len);
    
    void set_tx_gain(double gain_db) { m_tx_gain = gain_db; }
    double get_tx_gain() const { return m_tx_gain; }

private:
    cell_config  m_cfg;
    dl_grid*     m_dl    = nullptr;
    rar_builder* m_rar   = nullptr;
    msg4_builder* m_msg4 = nullptr;
    double       m_tx_gain = 30.0;
    bool         m_initialized = false;
};
