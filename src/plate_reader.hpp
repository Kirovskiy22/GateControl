#pragma once
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <cstring>
#include "plate.hpp"
#include "preprocess.hpp"

struct PlateRead {
    std::string text;
    float confidence;
    cv::Rect bbox;
};

class PlateReader {
    Ort::Env env_;
    Ort::SessionOptions session_opts_;
    std::unique_ptr<Ort::Session> detector_session_;
    std::unique_ptr<Ort::Session> ocr_session_;

    std::string det_in_name_, det_out_name_;
    std::string ocr_in_name_, ocr_out_name_;
    int ocr_h_ = 70;
    int ocr_w_ = 140;
    int ocr_c_ = 1;
    bool ocr_nchw_ = false;
    bool ocr_uint8_input_ = true;

    const std::string vocab_ = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";

public:
    PlateReader() : env_(ORT_LOGGING_LEVEL_ERROR, "GateControlANPR") {
        session_opts_.SetIntraOpNumThreads(2);
        session_opts_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Отключаем накопительный кэш арены ОЗУ для стабильной работы 24/7
        session_opts_.DisableCpuMemArena();
        session_opts_.DisableMemPattern();
    }

    void load(const std::wstring& det_model_path = L"models/yolo_plate.onnx",
              const std::wstring& ocr_model_path = L"models/cct_ocr.onnx") {
        detector_session_ = std::make_unique<Ort::Session>(env_, det_model_path.c_str(), session_opts_);
        ocr_session_ = std::make_unique<Ort::Session>(env_, ocr_model_path.c_str(), session_opts_);

        Ort::AllocatorWithDefaultOptions alloc;
        det_in_name_ = detector_session_->GetInputNameAllocated(0, alloc).get();
        det_out_name_ = detector_session_->GetOutputNameAllocated(0, alloc).get();

        ocr_in_name_ = ocr_session_->GetInputNameAllocated(0, alloc).get();
        ocr_out_name_ = ocr_session_->GetOutputNameAllocated(0, alloc).get();

        auto type_info = ocr_session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        ocr_uint8_input_ = (type_info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8);
        auto shape = type_info.GetShape();
        if (shape.size() == 4) {
            if (shape[3] == 1 || shape[3] == 3) {
                ocr_nchw_ = false;
                ocr_h_ = static_cast<int>(shape[1] > 0 ? shape[1] : 70);
                ocr_w_ = static_cast<int>(shape[2] > 0 ? shape[2] : 140);
                ocr_c_ = static_cast<int>(shape[3]);
            } else {
                ocr_nchw_ = true;
                ocr_c_ = static_cast<int>(shape[1] > 0 ? shape[1] : 1);
                ocr_h_ = static_cast<int>(shape[2] > 0 ? shape[2] : 70);
                ocr_w_ = static_cast<int>(shape[3] > 0 ? shape[3] : 140);
            }
        }
    }

    std::vector<PlateRead> read(const cv::Mat& frame) {
        if (frame.empty() || frame.cols < 32 || frame.rows < 32 || !detector_session_ || !ocr_session_) {
            return {};
        }

        std::vector<PlateRead> raw_reads;
        try {
            std::vector<cv::Rect> boxes = detect_plates(frame);
            cv::Rect frame_bounds(0, 0, frame.cols, frame.rows);

            for (size_t i = 0; i < boxes.size() && i < 3; ++i) {
                cv::Rect b = boxes[i] & frame_bounds;
                if (b.width <= 12 || b.height <= 6) continue;

                cv::Mat direct_crop = frame(b).clone();
                auto r0 = run_ocr(direct_crop);
                r0.bbox = b;
                raw_reads.push_back(r0);

                cv::Rect padded = preprocess::pad_box(
                    static_cast<float>(b.x), static_cast<float>(b.y),
                    static_cast<float>(b.x + b.width), static_cast<float>(b.y + b.height),
                    frame.cols, frame.rows
                ) & frame_bounds;

                if (padded.width > 12 && padded.height > 6) {
                    cv::Mat pad_crop = preprocess::prepare_plate_crop(frame(padded));
                    for (const auto& variant : preprocess::ocr_variants(pad_crop)) {
                        auto rv = run_ocr(variant);
                        rv.bbox = b;
                        raw_reads.push_back(rv);
                    }
                }
            }
        } catch (...) {
            return {};
        }

        return rank_reads(raw_reads);
    }

    std::vector<cv::Rect> detect_only(const cv::Mat& frame) {
        if (frame.empty() || frame.cols < 32 || frame.rows < 32 || !detector_session_) {
            return {};
        }
        try {
            return detect_plates(frame);
        } catch (...) {
            return {};
        }
    }

private:
    std::vector<cv::Rect> detect_plates(const cv::Mat& frame) {
        const int inp_size = 384;
        float r = std::min(static_cast<float>(inp_size) / frame.cols,
                           static_cast<float>(inp_size) / frame.rows);
        int new_w = std::max(1, static_cast<int>(std::round(frame.cols * r)));
        int new_h = std::max(1, static_cast<int>(std::round(frame.rows * r)));
        int dw = (inp_size - new_w) / 2;
        int dh = (inp_size - new_h) / 2;

        cv::Mat resized, padded, rgb;
        cv::resize(frame, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
        cv::copyMakeBorder(resized, padded, dh, inp_size - new_h - dh, dw, inp_size - new_w - dw,
                           cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
        cv::cvtColor(padded, rgb, cv::COLOR_BGR2RGB);

        std::vector<float> input_tensor(1 * 3 * inp_size * inp_size);
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < inp_size; ++y) {
                const cv::Vec3b* row_ptr = rgb.ptr<cv::Vec3b>(y);
                for (int x = 0; x < inp_size; ++x) {
                    input_tensor[c * inp_size * inp_size + y * inp_size + x] =
                        row_ptr[x][c] / 255.0f;
                }
            }
        }

        std::array<int64_t, 4> shape = {1, 3, inp_size, inp_size};
        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
        Ort::Value in_val = Ort::Value::CreateTensor<float>(
            mem_info, input_tensor.data(), input_tensor.size(), shape.data(), shape.size()
        );

        const char* in_names[] = {det_in_name_.c_str()};
        const char* out_names[] = {det_out_name_.c_str()};
        auto out_vals = detector_session_->Run(Ort::RunOptions{nullptr}, in_names, &in_val, 1, out_names, 1);

        auto out_info = out_vals[0].GetTensorTypeAndShapeInfo();
        auto out_shape = out_info.GetShape();
        const float* data = out_vals[0].GetTensorData<float>();

        std::vector<cv::Rect> boxes;
        if (out_shape.size() == 2 && out_shape[1] >= 6) {
            int num_dets = static_cast<int>(out_shape[0]);
            int stride = static_cast<int>(out_shape[1]);
            for (int i = 0; i < num_dets; ++i) {
                const float* row = data + i * stride;
                float score = (stride == 7) ? row[6] : row[4];
                if (!std::isfinite(score) || score < 0.25f) continue;

                float x1 = ((stride == 7 ? row[1] : row[0]) - dw) / r;
                float y1 = ((stride == 7 ? row[2] : row[1]) - dh) / r;
                float x2 = ((stride == 7 ? row[3] : row[2]) - dw) / r;
                float y2 = ((stride == 7 ? row[4] : row[3]) - dh) / r;

                if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) {
                    continue;
                }

                int left = std::clamp(static_cast<int>(x1), 0, frame.cols - 1);
                int top = std::clamp(static_cast<int>(y1), 0, frame.rows - 1);
                int right = std::clamp(static_cast<int>(x2), 0, frame.cols);
                int bottom = std::clamp(static_cast<int>(y2), 0, frame.rows);
                if (right - left > 12 && bottom - top > 6) {
                    boxes.emplace_back(left, top, right - left, bottom - top);
                }
            }
        }
        return boxes;
    }

    PlateRead run_ocr(const cv::Mat& crop) {
        if (crop.empty()) return {"", 0.0f, {}};
        cv::Mat proc;
        if (ocr_c_ == 1) {
            if (crop.channels() == 3) cv::cvtColor(crop, proc, cv::COLOR_BGR2GRAY);
            else proc = crop.clone();
        } else {
            if (crop.channels() == 1) cv::cvtColor(crop, proc, cv::COLOR_GRAY2RGB);
            else cv::cvtColor(crop, proc, cv::COLOR_BGR2RGB);
        }
        cv::resize(proc, proc, cv::Size(ocr_w_, ocr_h_), 0, 0, cv::INTER_LINEAR);
        if (!proc.isContinuous()) {
            proc = proc.clone();
        }

        const size_t expected_bytes = static_cast<size_t>(ocr_h_) * ocr_w_ * ocr_c_;
        if (proc.total() * proc.elemSize() != expected_bytes) {
            return {"", 0.0f, {}};
        }

        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> shape = ocr_nchw_
            ? std::array<int64_t, 4>{1, ocr_c_, ocr_h_, ocr_w_}
            : std::array<int64_t, 4>{1, ocr_h_, ocr_w_, ocr_c_};

        const char* in_names[] = {ocr_in_name_.c_str()};
        const char* out_names[] = {ocr_out_name_.c_str()};

        std::vector<Ort::Value> out_vals;
        if (ocr_uint8_input_) {
            std::vector<uint8_t> buf(expected_bytes);
            std::memcpy(buf.data(), proc.data, expected_bytes);
            Ort::Value in_val = Ort::Value::CreateTensor<uint8_t>(
                mem_info, buf.data(), buf.size(), shape.data(), shape.size()
            );
            out_vals = ocr_session_->Run(Ort::RunOptions{nullptr}, in_names, &in_val, 1, out_names, 1);
        } else {
            std::vector<float> buf(expected_bytes);
            const uint8_t* raw_ptr = proc.data;
            for (size_t i = 0; i < expected_bytes; ++i) {
                buf[i] = static_cast<float>(raw_ptr[i]) / 255.0f;
            }
            Ort::Value in_val = Ort::Value::CreateTensor<float>(
                mem_info, buf.data(), buf.size(), shape.data(), shape.size()
            );
            out_vals = ocr_session_->Run(Ort::RunOptions{nullptr}, in_names, &in_val, 1, out_names, 1);
        }

        auto out_info = out_vals[0].GetTensorTypeAndShapeInfo();
        auto out_shape = out_info.GetShape();
        const float* probs = out_vals[0].GetTensorData<float>();

        int slots = static_cast<int>(out_shape.size() == 3 ? out_shape[1] : out_shape[0]);
        int vocab_len = static_cast<int>(out_shape.back());

        std::string text;
        float conf_sum = 0.0f;
        int char_count = 0;

        for (int s = 0; s < slots; ++s) {
            const float* slot_probs = probs + s * vocab_len;
            int best_idx = 0;
            float best_p = slot_probs[0];
            for (int v = 1; v < vocab_len; ++v) {
                if (slot_probs[v] > best_p) {
                    best_p = slot_probs[v];
                    best_idx = v;
                }
            }
            if (best_idx < static_cast<int>(vocab_.size())) {
                char c = vocab_[best_idx];
                if (c != '_') {
                    text.push_back(c);
                    conf_sum += best_p;
                    char_count++;
                }
            }
        }

        float avg_conf = char_count > 0 ? (conf_sum / char_count) : 0.0f;
        return {text, avg_conf, {}};
    }

    std::vector<PlateRead> rank_reads(const std::vector<PlateRead>& combined) {
        std::vector<PlateRead> valid_list, rest_list;
        std::set<std::string> seen;

        for (const auto& item : combined) {
            std::string repaired = plate::repair_ocr_plate(item.text);
            std::string norm = !repaired.empty() ? repaired : plate::normalize_plate(item.text);
            if (norm.empty() || plate::to_u32(norm).size() < 4 || seen.count(norm)) continue;
            seen.insert(norm);

            if (!repaired.empty() && plate::is_valid_ru_plate(repaired)) {
                valid_list.push_back({repaired, item.confidence, item.bbox});
            } else {
                rest_list.push_back({norm, item.confidence, item.bbox});
            }
        }

        auto by_conf = [](const PlateRead& a, const PlateRead& b) {
            return a.confidence > b.confidence;
        };
        std::sort(valid_list.begin(), valid_list.end(), by_conf);
        std::sort(rest_list.begin(), rest_list.end(), by_conf);

        valid_list.insert(valid_list.end(), rest_list.begin(), rest_list.end());
        return valid_list;
    }
};