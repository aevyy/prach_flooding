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
    if (m_last_pos != std::streampos(-1)) {
        m_last_valid_pos = m_last_pos;
    } else {
        m_last_pos = 0;
        m_last_valid_pos = 0;
    }
    return true;
}

std::vector<gnb_detection_record> gnb_detect_reader::read_new_detections() {
    std::vector<gnb_detection_record> res;
    if (!m_log_stream.is_open()) {
        m_log_stream.open(m_log_path, std::ios::in);
        if (!m_log_stream.is_open()) return res;
        m_last_pos = 0;
        m_last_valid_pos = 0;
    }

    // rotation/truncation guard
    m_log_stream.clear();
    m_log_stream.seekg(0, std::ios::end);
    std::streampos end_pos = m_log_stream.tellg();
    if (end_pos == std::streampos(-1)) {
        // file inaccessible, restore last valid position
        m_log_stream.clear();
        m_log_stream.seekg(m_last_valid_pos);
        return res;
    }
    if (end_pos < m_last_pos) {
        m_last_pos = 0;
        m_last_valid_pos = 0;
    }

    m_log_stream.clear();
    m_log_stream.seekg(m_last_pos);

    std::string line;
    std::regex rx_item(R"(\{idx=(\d+)\s+ta=([+-]?[0-9]*\.?[0-9]+)us\s+power=([+-]?[0-9]*\.?[0-9]+)dB\s+snr=([+-]?[0-9]*\.?[0-9]+)dB\})");
    std::regex rx_prefix(R"(\[\s*(\d+)\.(\d+)\])");

    while (std::getline(m_log_stream, line)) {
        // Track position BEFORE processing the line (start of next line for restoration)
        std::streampos pos_before = m_log_stream.tellg(); // this is AFTER getline consumed the line

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

        // Only store positions that are valid (not -1)
        if (pos_before != std::streampos(-1)) {
            m_last_valid_pos = pos_before;
            m_last_pos = pos_before;
        }
    }

    // C4: After hitting EOF, don't poison m_last_pos with -1
    if (m_log_stream.eof()) {
        m_log_stream.clear();                    // clear EOF/fail bits
        // Restore to last valid position — never -1
        if (m_last_valid_pos != std::streampos(-1)) {
            m_log_stream.seekg(m_last_valid_pos);
            m_last_pos = m_last_valid_pos;
        }
    }

    return res;
}
