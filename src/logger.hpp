#pragma once
#include <fstream>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>

class FileLogger {
    std::mutex mtx_;
    std::string filename_ = "gatecontrol.log";
    static constexpr std::uintmax_t MAX_LOG_SIZE = 5 * 1024 * 1024; // 5 МБ

    static std::string now_str() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    void rotate_if_needed() {
        std::error_code ec;
        if (std::filesystem::exists(filename_, ec)) {
            auto sz = std::filesystem::file_size(filename_, ec);
            if (!ec && sz >= MAX_LOG_SIZE) {
                std::filesystem::rename(filename_, "gatecontrol.old.log", ec);
            }
        }
    }

public:
    static FileLogger& instance() {
        static FileLogger inst;
        return inst;
    }

    void write(const std::string& level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        rotate_if_needed();
        std::string line = "[" + now_str() + "] [" + level + "] " + msg;
        std::ofstream f(filename_, std::ios::app);
        if (f.is_open()) {
            f << line << "\n";
            f.flush();
        }
        std::cout << line << std::endl;
    }

    std::string tail(size_t max_lines = 120) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::ifstream f(filename_);
        if (!f.is_open()) return "Лог-файл пока пуст.";
        std::deque<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
            if (lines.size() > max_lines) lines.pop_front();
        }
        std::ostringstream oss;
        for (const auto& l : lines) oss << l << "\n";
        return oss.str();
    }

    std::string path() const { return filename_; }
};