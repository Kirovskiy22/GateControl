#pragma once
#include <opencv2/opencv.hpp>
#include <algorithm>

namespace motion {

// Сжатие кадра в маленькую Ч/Б матрицу 160px для мгновенного сравнения
inline cv::Mat downscale_gray(const cv::Mat& frame, int width = 160) {
    cv::Mat gray;
    if (frame.channels() == 3) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = frame;
    }
    if (gray.cols > width) {
        double scale = static_cast<double>(width) / gray.cols;
        int new_h = std::max(1, static_cast<int>(gray.rows * scale));
        cv::resize(gray, gray, cv::Size(width, new_h), 0, 0, cv::INTER_AREA);
    }
    return gray;
}

// Средняя абсолютная разница пикселей (0..1)
inline float frame_motion_score(const cv::Mat& prev_gray, const cv::Mat& curr_gray) {
    if (prev_gray.empty() || curr_gray.empty() || prev_gray.size() != curr_gray.size()) {
        return 1.0f;
    }
    cv::Mat diff;
    cv::absdiff(prev_gray, curr_gray, diff);
    return static_cast<float>(cv::mean(diff)[0] / 255.0);
}

inline bool has_motion(const cv::Mat& prev_gray, const cv::Mat& curr_gray, float threshold) {
    return frame_motion_score(prev_gray, curr_gray) >= threshold;
}

} // namespace motion