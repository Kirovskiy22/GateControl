#pragma once
#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <map>
#include <set>
#include <cstdlib>
#include <cmath>
#include "config.hpp"
#include "logger.hpp"
#include "motion.hpp"
#include "plate.hpp"
#include "plate_reader.hpp"

class VideoStream {
    std::string url_;
    cv::VideoCapture cap_;
    cv::Mat frame_;
    bool grabbed_ = false;
    std::atomic<bool> stopped_{false};
    std::atomic<double> last_grab_sec_{0.0};
    std::recursive_mutex lock_;
    std::thread thread_;

    static double monotonic_sec() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

public:
    explicit VideoStream(const std::string& url) : url_(url) {}

    ~VideoStream() {
        stop();
    }

    bool start() {
#ifdef _WIN32
        _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS",
                  "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|stimeout;5000000|timeout;5000000");
#else
        setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS",
               "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|stimeout;5000000|timeout;5000000", 1);
#endif
        try {
            cap_.open(url_, cv::CAP_FFMPEG);
            if (!cap_.isOpened()) {
                cap_.open(url_);
            }
            if (!cap_.isOpened()) {
                return false;
            }
            cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);
            stopped_ = false;
            last_grab_sec_ = monotonic_sec();
            thread_ = std::thread(&VideoStream::update, this);
            return true;
        } catch (...) {
            return false;
        }
    }

    void update() {
        double last_decode = 0.0;
        while (!stopped_) {
            try {
                if (!cap_.isOpened() || !cap_.grab()) {
                    stopped_ = true;
                    break;
                }
                double now = monotonic_sec();
                last_grab_sec_ = now;
                if (now - last_decode >= 0.15) {
                    cv::Mat decoded;
                    if (cap_.retrieve(decoded) && !decoded.empty()) {
                        std::lock_guard<std::recursive_mutex> guard(lock_);
                        frame_ = decoded;
                        grabbed_ = true;
                        last_decode = now;
                    }
                }
            } catch (...) {
                stopped_ = true;
                break;
            }
        }
        try {
            if (cap_.isOpened()) cap_.release();
        } catch (...) {}
    }

    bool read(cv::Mat& out_frame) {
        if (monotonic_sec() - last_grab_sec_.load() > 10.0) {
            stopped_ = true;
            return false;
        }
        std::lock_guard<std::recursive_mutex> guard(lock_);
        if (!frame_.empty() && grabbed_) {
            out_frame = frame_.clone();
            return true;
        }
        return false;
    }

    bool is_stopped() const { return stopped_; }

    void stop() {
        stopped_ = true;
        if (thread_.joinable()) {
            try { thread_.join(); } catch (...) {}
        }
    }
};

class AnprWorker {
public:
    using SignalGateFn = std::function<std::pair<bool, std::string>(const std::string&)>;

private:
    struct BBoxRecord {
        int width;
        float cy;
        double timestamp;
    };

    struct PendingAutoClose {
        std::string gate_id;
        std::string gate_name;
        std::string plate;
        double trigger_at;
    };

    struct CamRuntime {
        std::string id;
        std::string name;
        std::string rtsp_url;
        std::string status = "connecting";
        std::unique_ptr<VideoStream> stream;
        cv::Mat prev_gray;
        double last_process = 0.0;
        double next_reconnect = 0.0;
        std::vector<uchar> jpeg;
    };

    Config& cfg_;
    SignalGateFn signal_gate_by_id_;
    PlateReader reader_;
    bool models_loaded_ = false;

    std::map<std::string, CamRuntime> cam_runtimes_;
    double pause_recognition_until_ = 0.0;
    std::unordered_map<std::string, double> recent_opened_;
    std::unordered_map<std::string, BBoxRecord> bbox_history_;
    std::vector<PendingAutoClose> pending_closes_;

    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::recursive_mutex lock_;

    int event_id_ = 0;
    std::deque<json> events_;
    std::string last_plate_;
    std::string last_reason_;
    std::string last_camera_name_;
    std::pair<std::string, std::string> last_logged_;

    static double monotonic_sec() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

public:
    AnprWorker(SignalGateFn signal_gate_by_id, Config& cfg)
        : cfg_(cfg),
          signal_gate_by_id_(std::move(signal_gate_by_id)) {}

    ~AnprWorker() {
        stop();
    }

    void start() {
        stop_ = false;
        thread_ = std::thread(&AnprWorker::run, this);
    }

    void stop() {
        stop_ = true;
        if (thread_.joinable()) {
            try { thread_.join(); } catch (...) {}
        }
        std::lock_guard<std::recursive_mutex> guard(lock_);
        for (auto& [_, rt] : cam_runtimes_) {
            if (rt.stream) rt.stream->stop();
        }
    }

    void push_external_event(const std::string& text, const std::string& kind = "info") {
        push_event(text, kind);
    }

    std::vector<uchar> last_jpeg(const std::string& cam_id = "") const {
        if (!cfg_.preview_enabled()) return {};
        std::lock_guard<std::recursive_mutex> guard(lock_);
        std::string target = cam_id.empty() ? cfg_.active_camera_id() : cam_id;
        auto it = cam_runtimes_.find(target);
        if (it != cam_runtimes_.end()) {
            return it->second.jpeg;
        }
        if (!cam_runtimes_.empty()) {
            return cam_runtimes_.begin()->second.jpeg;
        }
        return {};
    }

    json get_status() const {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        double now = monotonic_sec();
        double pause_left = std::max(0.0, pause_recognition_until_ - now);
        std::string active_cid = cfg_.active_camera_id();

        json cams_json = json::object();
        auto cfg_cams = cfg_.cameras();
        std::string active_cam_status = "disconnected";

        for (const auto& [cid, ccfg] : cfg_cams) {
            std::string st = "connecting";
            auto it = cam_runtimes_.find(cid);
            if (it != cam_runtimes_.end()) {
                st = cfg_.system_enabled() ? it->second.status : "paused";
            }
            if (cid == active_cid) active_cam_status = st;
            cams_json[cid] = {
                {"id", cid},
                {"name", ccfg.name},
                {"rtsp_url", ccfg.rtsp_url},
                {"linked_gates", ccfg.linked_gates},
                {"status", st}
            };
        }

        return {
            {"camera", cfg_.system_enabled() ? active_cam_status : "paused"},
            {"active_camera_id", active_cid},
            {"preview_enabled", cfg_.preview_enabled()},
            {"cameras", cams_json},
            {"last_plate", last_plate_.empty() ? nullptr : json(last_plate_)},
            {"last_plate_display", last_plate_.empty() ? nullptr : json(plate::format_plate(last_plate_))},
            {"last_reason", last_reason_.empty() ? nullptr : json(last_reason_)},
            {"last_camera_name", last_camera_name_.empty() ? nullptr : json(last_camera_name_)},
            {"pause_left_sec", static_cast<int>(std::ceil(pause_left))},
            {"events", events_}
        };
    }

private:
    void push_event(const std::string& text, const std::string& kind) {
        std::lock_guard<std::recursive_mutex> guard(lock_);

        // Получаем точное время на самом ПК (сервере)
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char time_str[16];
        std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

        event_id_++;
        events_.push_front({
            {"id", event_id_},
            {"time", std::string(time_str)},
            {"text", text},
            {"kind", kind}
        });
        if (events_.size() > 60) events_.pop_back();
        FileLogger::instance().write(kind, text);
    }

    void store_preview_for_cam(CamRuntime& rt, const cv::Mat& frame) {
        if (!cfg_.preview_enabled()) return;
        try {
            cv::Mat resized = frame;
            if (frame.cols > 640) {
                double scale = 640.0 / frame.cols;
                cv::resize(frame, resized, cv::Size(640, static_cast<int>(frame.rows * scale)), 0, 0, cv::INTER_AREA);
            }
            std::vector<uchar> buf;
            if (cv::imencode(".jpg", resized, buf, {cv::IMWRITE_JPEG_QUALITY, 70})) {
                std::lock_guard<std::recursive_mutex> guard(lock_);
                rt.jpeg = std::move(buf);
            }
        } catch (...) {}
    }

    void sync_cameras_with_config(double now) {
        auto cfg_cams = cfg_.cameras();
        std::lock_guard<std::recursive_mutex> guard(lock_);

        // Удаляем отключенные камеры
        for (auto it = cam_runtimes_.begin(); it != cam_runtimes_.end();) {
            if (!cfg_cams.count(it->first)) {
                if (it->second.stream) it->second.stream->stop();
                push_event("Камера [" + it->second.name + "] удалена из мониторинга", "info");
                it = cam_runtimes_.erase(it);
            } else {
                ++it;
            }
        }

        // Добавляем новые или перезапускаем камеры с измененным RTSP URL
        for (const auto& [cid, ccfg] : cfg_cams) {
            auto it = cam_runtimes_.find(cid);
            if (it == cam_runtimes_.end()) {
                CamRuntime rt;
                rt.id = cid;
                rt.name = ccfg.name;
                rt.rtsp_url = ccfg.rtsp_url;
                rt.status = "disconnected";
                rt.next_reconnect = now;
                cam_runtimes_.emplace(cid, std::move(rt));
            } else {
                it->second.name = ccfg.name;
                if (it->second.rtsp_url != ccfg.rtsp_url) {
                    push_event("Изменен RTSP-адрес камеры '" + ccfg.name + "' -> переподключение...", "info");
                    if (it->second.stream) it->second.stream->stop();
                    it->second.stream.reset();
                    it->second.rtsp_url = ccfg.rtsp_url;
                    it->second.status = "disconnected";
                    it->second.next_reconnect = now;
                }
            }
        }
    }

    void cleanup_old_records(double now) {
        for (auto it = bbox_history_.begin(); it != bbox_history_.end();) {
            if (now - it->second.timestamp > 60.0) it = bbox_history_.erase(it);
            else ++it;
        }
        for (auto it = recent_opened_.begin(); it != recent_opened_.end();) {
            if (now - it->second > 3600.0) it = recent_opened_.erase(it);
            else ++it;
        }
    }

    void check_auto_close_timers(double now) {
        if (!cfg_.system_enabled()) return;

        auto it = pending_closes_.begin();
        while (it != pending_closes_.end()) {
            if (now >= it->trigger_at) {
                std::string gid = it->gate_id;
                std::string gname = it->gate_name;
                std::string shown = plate::format_plate(it->plate);
                it = pending_closes_.erase(it);

                if (cfg_.auto_close_enabled()) {
                    try {
                        auto [ok, msg] = signal_gate_by_id_(gid);
                        if (ok) {
                            push_event("Автозакрытие по таймеру [" + gname + "] (" + shown + ") — повторный сигнал START отправлен", "info");
                        } else {
                            push_event("Автозакрытие по таймеру [" + gname + "] НЕ выполнено: " + msg, "error");
                        }
                    } catch (const std::exception& e) {
                        push_event(std::string("Исключение автозакрытия: ") + e.what(), "error");
                    }
                }
            } else {
                ++it;
            }
        }
    }

    void run() {
        try {
            push_event("Загрузка нейросетевых моделей ONNX...", "info");
            reader_.load();
            models_loaded_ = true;
            push_event("Модели детекции и OCR успешно загружены", "success");
        } catch (const std::exception& e) {
            push_event(std::string("Критическая ошибка загрузки моделей ONNX: ") + e.what(), "error");
            return;
        } catch (...) {
            push_event("Неизвестная критическая ошибка при загрузке моделей ONNX", "error");
            return;
        }

        double last_cleanup = monotonic_sec();

        while (!stop_) {
            try {
                double now = monotonic_sec();
                check_auto_close_timers(now);

                if (now - last_cleanup > 300.0) {
                    cleanup_old_records(now);
                    last_cleanup = now;
                }

                if (!cfg_.system_enabled()) {
                    std::lock_guard<std::recursive_mutex> guard(lock_);
                    for (auto& [_, rt] : cam_runtimes_) {
                        if (rt.stream) {
                            rt.stream->stop();
                            rt.stream.reset();
                        }
                        rt.status = "paused";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }

                sync_cameras_with_config(now);

                std::vector<std::string> cam_ids;
                {
                    std::lock_guard<std::recursive_mutex> guard(lock_);
                    for (const auto& [cid, _] : cam_runtimes_) cam_ids.push_back(cid);
                }

                std::string active_cid = cfg_.active_camera_id();

                for (const auto& cid : cam_ids) {
                    if (stop_ || !cfg_.system_enabled()) break;

                    VideoStream* stream_ptr = nullptr;
                    std::string cam_name, cam_url;
                    bool need_process = false;

                    {
                        std::lock_guard<std::recursive_mutex> guard(lock_);
                        auto it = cam_runtimes_.find(cid);
                        if (it == cam_runtimes_.end()) continue;
                        CamRuntime& rt = it->second;
                        cam_name = rt.name;
                        cam_url = rt.rtsp_url;

                        if (!rt.stream || rt.stream->is_stopped()) {
                            if (rt.stream) {
                                rt.stream->stop();
                                rt.stream.reset();
                                rt.status = "disconnected";
                                push_event("Поток камеры '" + cam_name + "' оборвался, переподключение...", "warn");
                                rt.next_reconnect = now + 3.0;
                            }
                            if (now >= rt.next_reconnect && !cam_url.empty()) {
                                auto new_stream = std::make_unique<VideoStream>(cam_url);
                                if (new_stream->start()) {
                                    rt.stream = std::move(new_stream);
                                    rt.status = "connected";
                                    push_event("Камера '" + cam_name + "' подключена (" + cam_url + ")", "success");
                                } else {
                                    rt.status = "disconnected";
                                    FileLogger::instance().write("NET", "Не удалось подключиться к RTSP камеры '" + cam_name + "': " + cam_url);
                                    rt.next_reconnect = now + 4.0;
                                }
                            }
                            continue;
                        }

                        if (now - rt.last_process >= 1.0) {
                            need_process = true;
                            stream_ptr = rt.stream.get();
                        }
                    }

                    if (!need_process || !stream_ptr) continue;

                    cv::Mat frame;
                    if (!stream_ptr->read(frame) || frame.empty()) continue;

                    {
                        std::lock_guard<std::recursive_mutex> guard(lock_);
                        auto it = cam_runtimes_.find(cid);
                        if (it != cam_runtimes_.end()) {
                            it->second.last_process = now;
                            // Сжимаем превью только если включен показ потока в окне и эта камера выбрана
                            if (cfg_.preview_enabled() && cid == active_cid) {
                                store_preview_for_cam(it->second, frame);
                            }
                        }
                    }

                    if (now < pause_recognition_until_) continue;

                    try {
                        handle_camera_frame(cid, cam_name, *stream_ptr, frame, now);
                    } catch (const std::exception& e) {
                        FileLogger::instance().write("ERROR", "Ошибка анализа кадра камеры '" + cam_name + "': " + e.what());
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            } catch (const std::exception& e) {
                FileLogger::instance().write("ERROR", std::string("Сбой главного цикла AnprWorker: ") + e.what());
                std::this_thread::sleep_for(std::chrono::seconds(2));
            } catch (...) {
                FileLogger::instance().write("ERROR", "Неизвестный сбой главного цикла AnprWorker");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    bool is_car_departing(VideoStream& stream, const std::string& track_key, const cv::Rect& bbox1,
                          double now, std::string& out_detail) {
        int w1 = bbox1.width;
        float cx1 = bbox1.x + bbox1.width * 0.5f;
        float cy1 = bbox1.y + bbox1.height * 0.5f;

        auto it = bbox_history_.find(track_key);
        if (it != bbox_history_.end() && (now - it->second.timestamp) <= 4.0) {
            int prev_w = it->second.width;
            float prev_cy = it->second.cy;
            it->second = {w1, cy1, now};

            if (cy1 < prev_cy - 3.0f || w1 < static_cast<int>(prev_w * 0.96f)) {
                out_detail = "рамка уходит вверх/сжимается (w: " + std::to_string(prev_w) + "->" + std::to_string(w1) +
                             "px, Y: " + std::to_string(static_cast<int>(prev_cy)) + "->" + std::to_string(static_cast<int>(cy1)) + ")";
                return true;
            }
            return false;
        }

        bbox_history_[track_key] = {w1, cy1, now};
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        cv::Mat frame2;
        if (!stream.read(frame2) || frame2.empty()) {
            out_detail = "поток прервался на контрольном замере (150 мс)";
            return true;
        }

        bool clipped_right_edge = (bbox1.x + bbox1.width >= frame2.cols - 20);
        auto boxes2 = reader_.detect_only(frame2);
        if (boxes2.empty()) {
            out_detail = "номер покинул кадр за 150 мс (уехал за границу обзора)";
            return true;
        }

        const cv::Rect* best_b2 = nullptr;
        float min_dist = 1e9f;
        for (const auto& b : boxes2) {
            float cx2 = b.x + b.width * 0.5f;
            float cy2 = b.y + b.height * 0.5f;
            float dist = std::hypot(cx2 - cx1, cy2 - cy1);
            if (dist < min_dist && dist < frame2.cols * 0.35f) {
                min_dist = dist;
                best_b2 = &b;
            }
        }

        if (!best_b2) {
            out_detail = "рамка автомобиля вышла из зоны отслеживания за 150 мс";
            return true;
        }

        int w2 = best_b2->width;
        float cy2 = best_b2->y + best_b2->height * 0.5f;
        bbox_history_[track_key] = {w2, cy2, monotonic_sec()};

        if (cy2 < cy1 - 3.0f) {
            out_detail = "движение вверх от ворот (Y: " + std::to_string(static_cast<int>(cy1)) + "->" + std::to_string(static_cast<int>(cy2)) + "px)";
            return true;
        }
        if (!clipped_right_edge && w2 < static_cast<int>(w1 * 0.96f)) {
            out_detail = "уменьшение ширины рамки (w: " + std::to_string(w1) + "->" + std::to_string(w2) + "px)";
            return true;
        }
        return false;
    }

    void handle_camera_frame(const std::string& cam_id, const std::string& cam_name,
                             VideoStream& stream, const cv::Mat& frame, double now) {
        cv::Mat curr_gray = motion::downscale_gray(frame, 160);
        {
            std::lock_guard<std::recursive_mutex> guard(lock_);
            auto it = cam_runtimes_.find(cam_id);
            if (it != cam_runtimes_.end()) {
                if (cfg_.motion_filter_enabled()) {
                    bool moving = motion::has_motion(it->second.prev_gray, curr_gray, cfg_.motion_threshold());
                    it->second.prev_gray = curr_gray;
                    if (!moving) return;
                } else {
                    it->second.prev_gray = curr_gray;
                }
            }
        }

        auto reads = reader_.read(frame);
        float min_thresh = std::max(0.1f, cfg_.anpr_min_confidence() - 0.2f);

        const PlateRead* best = nullptr;
        for (const auto& r : reads) {
            if (plate::to_u32(plate::normalize_plate(r.text)).size() >= 4 && r.confidence >= min_thresh) {
                best = &r;
                break;
            }
        }
        if (!best) return;

        float conf = std::clamp(best->confidence > 1.0f ? best->confidence / 100.0f : best->confidence, 0.0f, 1.0f);
        std::string repaired = plate::repair_ocr_plate(best->text);
        std::string norm_plate = !repaired.empty() ? repaired : plate::normalize_plate(best->text);
        std::string shown = plate::format_plate(norm_plate);
        int pct = static_cast<int>(conf * 100.0f);

        if (conf < cfg_.anpr_min_confidence()) {
            log_once(cam_id + ":" + norm_plate, "low_conf",
                     "[" + cam_name + "] " + shown + ": сигнал НЕ отправлен — низкая уверенность OCR (" + std::to_string(pct) + "%)", "warn");
            return;
        }

        if (cfg_.anpr_require_valid_format() && (repaired.empty() || !plate::is_valid_ru_plate(repaired))) {
            log_once(cam_id + ":" + norm_plate, "bad_fmt",
                     "[" + cam_name + "] " + norm_plate + ": сигнал НЕ отправлен — не соответствует формату 6-значного ГРЗ", "warn");
            return;
        }

        // Проверяем привязку камеры к контроллерам ЕСП
        auto all_cams = cfg_.cameras();
        auto all_gates = cfg_.gates();
        auto cam_it = all_cams.find(cam_id);
        if (cam_it == all_cams.end()) return;

        const auto& linked_gates = cam_it->second.linked_gates;
        if (linked_gates.empty()) {
            update_last_state(norm_plate, "камера не привязана к ЕСП", cam_name);
            log_once(cam_id + ":" + norm_plate, "no_links",
                     "[" + cam_name + "] Распознан " + shown + " (" + std::to_string(pct) + "%), но сигнал НЕ отправлен: к камере '" + cam_name + "' не привязан ни один контроллер ЕСП!", "warn");
            return;
        }

        // Ищем, каким из привязанных к этой камере контроллеров ЕСП разрешен этот номер
        std::vector<GateConfig> matching_gates;
        for (const auto& gid : linked_gates) {
            auto git = all_gates.find(gid);
            if (git == all_gates.end()) continue;
            const auto& gcfg = git->second;

            if (!cfg_.anpr_whitelist_only()) {
                matching_gates.push_back(gcfg);
            } else {
                for (const auto& allowed_raw : gcfg.allowed_plates) {
                    std::string allowed_norm = plate::repair_ocr_plate(allowed_raw);
                    if (allowed_norm.empty()) allowed_norm = plate::normalize_plate(allowed_raw);
                    if (allowed_norm == norm_plate) {
                        matching_gates.push_back(gcfg);
                        break;
                    }
                }
            }
        }

        if (matching_gates.empty()) {
            update_last_state(norm_plate, "нет в белом списке привязанных ЕСП", cam_name);
            log_once(cam_id + ":" + norm_plate, "not_in_wl",
                     "[" + cam_name + "] " + shown + " (" + std::to_string(pct) + "%): сигнал НЕ отправлен — номера нет в белом списке привязанных к этой камере контроллеров ЕСП", "warn");
            return;
        }

        // Проверка уезжающей машины
        if (cfg_.departing_check_enabled() && best->bbox.width > 0) {
            std::string dep_detail;
            if (is_car_departing(stream, cam_id + ":" + norm_plate, best->bbox, now, dep_detail)) {
                update_last_state(norm_plate, "машина уезжает (игнор)", cam_name);
                log_once(cam_id + ":" + norm_plate, "departing",
                         "[" + cam_name + "] " + shown + ": сигнал НЕ отправлен — машина уезжает (" + dep_detail + ")", "warn");
                return;
            }
        }

        // Проверка скользящего кулдауна для номера
        auto it_cd = recent_opened_.find(norm_plate);
        if (it_cd != recent_opened_.end() && (now - it_cd->second) < cfg_.anpr_same_plate_cooldown_sec()) {
            double left = cfg_.anpr_same_plate_cooldown_sec() - (now - it_cd->second);
            it_cd->second = now;
            log_once(cam_id + ":" + norm_plate, "cooldown",
                     "[" + cam_name + "] " + shown + ": повторный сигнал пропущен — активен кулдаун номера (ещё " + std::to_string(static_cast<int>(left)) + " сек.)", "info");
            return;
        }

        // Отправляем сигнал на каждый подходящий привязанный контроллер ЕСП
        bool any_opened = false;
        for (const auto& gcfg : matching_gates) {
            if (!gcfg.enabled) {
                update_last_state(norm_plate, "контроллер " + gcfg.name + " ВЫКЛ", cam_name);
                log_once(cam_id + ":" + norm_plate + ":" + gcfg.id, "gate_disabled",
                         "[" + cam_name + "] " + shown + ": номер разрешён, но сигнал на '" + gcfg.name + "' НЕ отправлен — контроллер отключен кнопкой ВЫКЛ в интерфейсе", "warn");
                continue;
            }

            auto [ok, msg] = signal_gate_by_id_(gcfg.id);
            if (ok) {
                any_opened = true;
                push_event("[" + cam_name + " -> " + gcfg.name + "] " + shown + " (" + std::to_string(pct) + "%): ворота открыты! Пауза аналитики 10 сек.", "success");
                if (cfg_.auto_close_enabled()) {
                    double delay = cfg_.auto_close_sec();
                    pending_closes_.push_back({gcfg.id, gcfg.name, norm_plate, now + delay});
                    push_event("[" + gcfg.name + "] Таймер автозакрытия запущен на " + std::to_string(static_cast<int>(delay)) + " сек.", "info");
                }
            } else {
                update_last_state(norm_plate, "ошибка связи с ЕСП", cam_name);
                push_event("[" + cam_name + " -> " + gcfg.name + "] " + shown + ": не удалось открыть — " + msg, "error");
            }
        }

        if (any_opened) {
            recent_opened_[norm_plate] = now;
            pause_recognition_until_ = now + 10.0;
            update_last_state(norm_plate, "открыто (пауза 10 с)", cam_name);
        }
    }

    void update_last_state(const std::string& plate_str, const std::string& reason, const std::string& cam_name) {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        last_plate_ = plate_str;
        last_reason_ = reason;
        last_camera_name_ = cam_name;
    }

    void log_once(const std::string& key1, const std::string& key2, const std::string& msg, const std::string& kind) {
        std::pair<std::string, std::string> k = {key1, key2};
        if (k != last_logged_) {
            last_logged_ = k;
            push_event(msg, kind);
        }
    }
};