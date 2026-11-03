#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct GateConfig {
    std::string id;
    std::string name;
    std::string url;
    std::string token;
    bool enabled = true; // ВКЛ/ВЫКЛ отправки сигнала на этот контроллер из программы
    std::vector<std::string> allowed_plates;
};

struct CameraConfig {
    std::string id;
    std::string name;
    std::string rtsp_url;
    std::vector<std::string> linked_gates; // Список ID контроллеров ЕСП, привязанных к этой камере
};

class Config {
public:
    std::string config_path;
    json data;
    mutable std::mutex mtx;

    explicit Config(const std::string& path = "config.json") : config_path(path) {
        load();
    }

    void load() {
        std::lock_guard<std::mutex> lock(mtx);
        std::ifstream f(config_path);
        if (f.is_open()) {
            try {
                f >> data;
            } catch (...) {
                data = json::object();
            }
        } else {
            data = json::object();
        }
        migrate_if_needed_unlocked();
    }

    void save_unlocked() {
        std::ofstream f(config_path);
        if (f.is_open()) {
            f << data.dump(2);
        }
    }

private:
    // Автоматическая миграция старого config.json в новую структуру камер и контроллеров
    void migrate_if_needed_unlocked() {
        bool changed = false;
        if (!data.contains("gates") || !data["gates"].is_object()) {
            data["gates"] = json::object();
            changed = true;
        }
        std::vector<std::string> all_gate_ids;
        for (auto& [gid, gobj] : data["gates"].items()) {
            all_gate_ids.push_back(gid);
            if (!gobj.contains("enabled")) {
                gobj["enabled"] = true;
                changed = true;
            }
        }

        if (!data.contains("cameras") || !data["cameras"].is_object() || data["cameras"].empty()) {
            std::string old_rtsp = data.value("rtsp_url", "rtsp://admin:admin@192.168.0.229:554/live/main");
            data["cameras"] = {
                {"cam_1", {
                    {"name", "Камера 1 (Основная)"},
                    {"rtsp_url", old_rtsp},
                    {"linked_gates", all_gate_ids}
                }}
            };
            data["active_camera_id"] = "cam_1";
            changed = true;
        }

        if (!data.contains("active_camera_id") || data["active_camera_id"].get<std::string>().empty()) {
            for (auto& [cid, _] : data["cameras"].items()) {
                data["active_camera_id"] = cid;
                changed = true;
                break;
            }
        }

        if (!data.contains("preview_enabled")) { data["preview_enabled"] = true; changed = true; }
        if (!data.contains("watchdog_enabled")) { data["watchdog_enabled"] = true; changed = true; }
        if (!data.contains("autostart_enabled")) { data["autostart_enabled"] = false; changed = true; }

        if (changed) save_unlocked();
    }

public:
    std::string web_host() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("web_host", "0.0.0.0");
    }
    int web_port() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("web_port", 8080);
    }

    bool system_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("system_enabled", true);
    }
    bool preview_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("preview_enabled", true);
    }
    std::string active_camera_id() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("active_camera_id", "cam_1");
    }
    bool auto_close_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_auto_close", false);
    }
    double auto_close_sec() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_close_after_sec", 15.0);
    }
    bool departing_check_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("departing_check_enabled", true);
    }
    bool motion_filter_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("motion_filter_enabled", true);
    }
    bool watchdog_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("watchdog_enabled", true);
    }
    bool autostart_enabled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("autostart_enabled", false);
    }
    float motion_threshold() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("motion_threshold", 0.012f);
    }

    void update_settings(bool sys_en, bool prev_en, bool auto_close_en, double auto_close_s,
                         bool departing_en, bool motion_en, bool watchdog_en, bool autostart_en) {
        std::lock_guard<std::mutex> lock(mtx);
        data["system_enabled"] = sys_en;
        data["preview_enabled"] = prev_en;
        data["anpr_auto_close"] = auto_close_en;
        data["anpr_close_after_sec"] = std::max(1.0, auto_close_s);
        data["departing_check_enabled"] = departing_en;
        data["motion_filter_enabled"] = motion_en;
        data["watchdog_enabled"] = watchdog_en;
        data["autostart_enabled"] = autostart_en;
        save_unlocked();
    }

    json get_settings_json() const {
        std::lock_guard<std::mutex> lock(mtx);
        return {
            {"system_enabled", data.value("system_enabled", true)},
            {"preview_enabled", data.value("preview_enabled", true)},
            {"active_camera_id", data.value("active_camera_id", "cam_1")},
            {"auto_close_enabled", data.value("anpr_auto_close", false)},
            {"auto_close_sec", data.value("anpr_close_after_sec", 15.0)},
            {"departing_check_enabled", data.value("departing_check_enabled", true)},
            {"motion_filter_enabled", data.value("motion_filter_enabled", true)},
            {"watchdog_enabled", data.value("watchdog_enabled", true)},
            {"autostart_enabled", data.value("autostart_enabled", false)}
        };
    }

    float anpr_min_confidence() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_min_confidence", 0.55f);
    }
    float anpr_open_min_confidence() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_open_min_confidence", 0.60f);
    }
    double anpr_same_plate_cooldown_sec() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_same_plate_cooldown_sec", 60.0);
    }
    bool anpr_whitelist_only() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_whitelist_only", true);
    }
    bool anpr_require_valid_format() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_require_valid_format", true);
    }
    bool anpr_open_on_detect() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("anpr_open_on_detect", true);
    }
    double esp32_timeout_sec() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("esp32_timeout_sec", 3.0);
    }
    double esp32_command_cooldown_sec() const {
        std::lock_guard<std::mutex> lock(mtx);
        return data.value("esp32_command_cooldown_sec", 1.5);
    }

    // Управление камерами (RTSP-потоками) и их привязками к ЕСП
    std::map<std::string, CameraConfig> cameras() const {
        std::lock_guard<std::mutex> lock(mtx);
        std::map<std::string, CameraConfig> result;
        if (data.contains("cameras") && data["cameras"].is_object()) {
            for (auto& [cid, cobj] : data["cameras"].items()) {
                CameraConfig cc;
                cc.id = cid;
                cc.name = cobj.value("name", cid);
                cc.rtsp_url = cobj.value("rtsp_url", "");
                if (cobj.contains("linked_gates") && cobj["linked_gates"].is_array()) {
                    for (auto& g : cobj["linked_gates"]) {
                        cc.linked_gates.push_back(g.get<std::string>());
                    }
                }
                result[cid] = cc;
            }
        }
        return result;
    }

    void set_active_camera(const std::string& cam_id) {
        std::lock_guard<std::mutex> lock(mtx);
        if (data.contains("cameras") && data["cameras"].contains(cam_id)) {
            data["active_camera_id"] = cam_id;
            save_unlocked();
        }
    }

    std::string add_or_update_camera(const std::string& cam_id, const std::string& name,
                                     const std::string& rtsp_url, const std::vector<std::string>& linked_gates) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string cid = cam_id;
        if (cid.empty()) {
            int idx = 1;
            while (data["cameras"].contains("cam_" + std::to_string(idx))) idx++;
            cid = "cam_" + std::to_string(idx);
        }
        data["cameras"][cid] = {
            {"name", name.empty() ? cid : name},
            {"rtsp_url", rtsp_url},
            {"linked_gates", linked_gates}
        };
        if (!data.contains("active_camera_id") || data["active_camera_id"].get<std::string>().empty()) {
            data["active_camera_id"] = cid;
        }
        save_unlocked();
        return cid;
    }

    bool delete_camera(const std::string& cam_id) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!data.contains("cameras") || !data["cameras"].contains(cam_id)) return false;
        data["cameras"].erase(cam_id);
        if (data.value("active_camera_id", "") == cam_id) {
            std::string next_id = "";
            for (auto& [cid, _] : data["cameras"].items()) { next_id = cid; break; }
            data["active_camera_id"] = next_id;
        }
        save_unlocked();
        return true;
    }

    // Управление контроллерами ЕСП
    std::map<std::string, GateConfig> gates() const {
        std::lock_guard<std::mutex> lock(mtx);
        std::map<std::string, GateConfig> result;
        if (data.contains("gates") && data["gates"].is_object()) {
            for (auto& [gid, gobj] : data["gates"].items()) {
                GateConfig gc;
                gc.id = gid;
                gc.name = gobj.value("name", gid);
                gc.url = gobj.value("url", "");
                gc.token = gobj.value("token", "");
                gc.enabled = gobj.value("enabled", true);
                if (gobj.contains("allowed_plates") && gobj["allowed_plates"].is_array()) {
                    for (auto& p : gobj["allowed_plates"]) {
                        gc.allowed_plates.push_back(p.get<std::string>());
                    }
                }
                result[gid] = gc;
            }
        }
        return result;
    }

    std::string add_or_update_gate(const std::string& gate_id, const std::string& name,
                                   const std::string& url, const std::string& token, bool enabled) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string gid = gate_id;
        if (gid.empty()) {
            int idx = 1;
            while (data["gates"].contains("gate_" + std::to_string(idx))) idx++;
            gid = "gate_" + std::to_string(idx);
        }
        json existing_plates = json::array();
        if (data["gates"].contains(gid) && data["gates"][gid].contains("allowed_plates")) {
            existing_plates = data["gates"][gid]["allowed_plates"];
        }
        data["gates"][gid] = {
            {"name", name.empty() ? gid : name},
            {"url", url},
            {"token", token},
            {"enabled", enabled},
            {"allowed_plates", existing_plates}
        };
        save_unlocked();
        return gid;
    }

    bool set_gate_enabled(const std::string& gate_id, bool enabled) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!data.contains("gates") || !data["gates"].contains(gate_id)) return false;
        data["gates"][gate_id]["enabled"] = enabled;
        save_unlocked();
        return true;
    }

    bool delete_gate(const std::string& gate_id) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!data.contains("gates") || !data["gates"].contains(gate_id)) return false;
        data["gates"].erase(gate_id);
        // Удаляем контроллер из привязок камер
        if (data.contains("cameras") && data["cameras"].is_object()) {
            for (auto& [cid, cobj] : data["cameras"].items()) {
                if (cobj.contains("linked_gates") && cobj["linked_gates"].is_array()) {
                    json new_links = json::array();
                    for (auto& g : cobj["linked_gates"]) {
                        if (g.get<std::string>() != gate_id) new_links.push_back(g);
                    }
                    cobj["linked_gates"] = new_links;
                }
            }
        }
        save_unlocked();
        return true;
    }

    void set_gate_plates(const std::string& gate_id, const std::vector<std::string>& plates) {
        std::lock_guard<std::mutex> lock(mtx);
        if (data.contains("gates") && data["gates"].contains(gate_id)) {
            data["gates"][gate_id]["allowed_plates"] = plates;
            save_unlocked();
        }
    }
};