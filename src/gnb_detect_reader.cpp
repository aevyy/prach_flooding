#include "gnb_detect_reader.h"
#include <iostream>
#include <regex>
#include <chrono>
#include <sys/time.h>

gnb_detect_reader::gnb_detect_reader() {}
gnb_detect_reader::~gnb_detect_reader() {}

bool gnb_detect_reader::init(const std::string& log_path) {
    m_log_path = log_path;
    m_log_stream.open(log_path, std::ios::in);
    if (!m_log_stream.is_open()) return false;
    m_log_stream.seekg(0, std::ios::end);
    m_last_pos = m_log_stream.tellg();
    return true;
}

std::vector<gnb_detection_record> gnb_detect_reader::read_new_detections() {
    std::vector<gnb_detection_record> res;
    if (!m_log_stream.is_open()) {
        m_log_stream.open(m_log_path, std::ios::in);
        if (!m_log_stream.is_open()) return res;
        m_log_stream.seekg(0, std::ios::end);
        m_last_pos = m_log_stream.tellg();
        return res;
    }
    
    m_log_stream.clear(); // clear EOF flags
    m_log_stream.seekg(m_last_pos);
    
    std::string line;
    // We want to match: PRACH: rssi=+0.5dB detected_preambles=[{idx=55 ta=-7.29us power=+85.8dB snr=0.0dB}] t=351.3us
    // Frame and slot might not be consistently in the log format from srsRAN, but often there is a t=... or MAC prefix.
    // However, the anchor line provided is just "PRACH: ...". We will extract all detected_preambles.
    // If the line has [SFN.slot] we will try to extract it, else 0.
    std::regex rx_item(R"(\{idx=(\d+)\s+ta=([+-]?[0-9]*\.?[0-9]+)us\s+power=([+-]?[0-9]*\.?[0-9]+)dB\s+snr=([+-]?[0-9]*\.?[0-9]+)dB\})");
    
    // Some srsRAN builds put [ 123.4] before PRACH:
    std::regex rx_prefix(R"(\[\s*(\d+)\.(\d+)\])"); 

    while (std::getline(m_log_stream, line)) {
        if (line.find("PRACH:") != std::string::npos && line.find("detected_preambles=[") != std::string::npos) {
            uint32_t frame = 0, slot = 0;
            std::smatch sm_prefix;
            if (std::regex_search(line, sm_prefix, rx_prefix)) {
                frame = std::stoul(sm_prefix[1].str());
                slot = std::stoul(sm_prefix[2].str());
            }
            
            struct timeval tv;
            gettimeofday(&tv, NULL);
            double wall_time = tv.tv_sec + tv.tv_usec / 1e6;
            
            std::sregex_iterator currentMatch(line.begin(), line.end(), rx_item);
            std::sregex_iterator lastMatch;
            while (currentMatch != lastMatch) {
                std::smatch match = *currentMatch;
                gnb_detection_record rec;
                rec.frame = frame;
                rec.slot = slot;
                rec.idx = std::stoul(match[1].str());
                rec.ta_us = std::stof(match[2].str());
                rec.power_db = std::stof(match[3].str());
                rec.snr_db = std::stof(match[4].str());
                rec.log_wall_time = wall_time;
                res.push_back(rec);
                currentMatch++;
            }
        }
    }
    m_last_pos = m_log_stream.tellg();
    return res;
}
