#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <cstdint>

struct gnb_detection_record {
    uint32_t frame;
    uint32_t slot;
    uint32_t idx;
    float ta_us;
    float power_db;
    float snr_db;
    double log_wall_time;
};

class gnb_detect_reader {
public:
    gnb_detect_reader();
    ~gnb_detect_reader();

    bool init(const std::string& log_path);
    
    // Read newly appended lines and parse them.
    std::vector<gnb_detection_record> read_new_detections();

private:
    std::string m_log_path;
    std::ifstream m_log_stream;
    std::streampos m_last_pos = 0;
};
