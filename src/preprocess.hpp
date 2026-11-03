#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <algorithm>

namespace preprocess {

inline cv::Rect pad_box(float x1, float y1, float x2, float y2, int width, int height,
                        float pad = 0.18f, float pad_left = 0.30f) {
    float bw = std::max(1.0f, x2 - x1);
    float bh = std::max(1.0f, y2 - y1);
    int left = std::max(0, static_cast<int>(x1 - bw * pad_left));
    int top = std::max(0, static_cast<int>(y1 - bh * pad));
    int right = std::min(width, static_cast<int>(x2 + bw * pad));
    int bottom = std::min(height, static_cast<int>(y2 + bh * pad));
    if (right <= left || bottom <= top) return cv::Rect(0, 0, width, height);
    return cv::Rect(left, top, right - left, bottom - top);
}

inline cv::Mat upscale_min_height(const cv::Mat& frame, int min_height = 96) {
    if (frame.empty() || frame.rows >= min_height) return frame;
    double scale = static_cast<double>(min_height) / frame.rows;
    cv::Mat out;
    cv::resize(frame, out, cv::Size(std::max(1, static_cast<int>(frame.cols * scale)), min_height), 0, 0, cv::INTER_CUBIC);
    return out;
}

inline cv::Mat prepare_plate_crop(const cv::Mat& crop, int min_width = 480, int min_height = 100) {
    if (crop.empty()) return crop;
    cv::Mat scaled = upscale_min_height(crop, min_height);
    if (scaled.cols < min_width) {
        double scale = static_cast<double>(min_width) / std::max(1, scaled.cols);
        cv::resize(scaled, scaled, cv::Size(min_width, std::max(1, static_cast<int>(scaled.rows * scale))), 0, 0, cv::INTER_CUBIC);
    }
    cv::Mat bordered;
    cv::copyMakeBorder(scaled, bordered, 4, 4, 14, 6, cv::BORDER_REPLICATE);
    return bordered;
}

inline cv::Mat clahe_bgr(const cv::Mat& frame) {
    cv::Mat lab;
    cv::cvtColor(frame, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(channels[0], channels[0]);
    cv::merge(channels, lab);
    cv::Mat out;
    cv::cvtColor(lab, out, cv::COLOR_Lab2BGR);
    return out;
}

inline cv::Mat sharpen(const cv::Mat& frame) {
    cv::Mat kernel = (cv::Mat_<float>(3, 3) <<
         0, -1,  0,
        -1,  5, -1,
         0, -1,  0);
    cv::Mat out;
    cv::filter2D(frame, out, -1, kernel);
    return out;
}

inline std::vector<cv::Mat> ocr_variants(const cv::Mat& crop) {
    if (crop.empty()) return {};
    cv::Mat base = upscale_min_height(crop);
    cv::Mat enhanced = sharpen(clahe_bgr(base));
    cv::Mat gray, gray_bgr;
    cv::cvtColor(enhanced, gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray, gray_bgr, cv::COLOR_GRAY2BGR);
    return {base, enhanced, gray_bgr};
}

} // namespace preprocess