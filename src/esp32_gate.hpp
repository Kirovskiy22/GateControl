#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include "config.hpp"
#include "logger.hpp"

using json = nlohmann::json;

class ESP32GateController {
    Config& cfg_;
    std::mutex lock_;
    double last_command_at_ = 0.0;
    std::map<std::string, json> device_;
    std::map<std::string, std::string> last_error_;

    static double monotonic_sec() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    static std::string normalize_base_url(std::string base) {
        while (!base.empty() && (base.back() == '/' || base.back() == ' ')) base.pop_back();
        while (!base.empty() && base.front() == ' ') base.erase(base.begin());
        if (!base.empty() && base.find("http://") != 0 && base.find("https://") != 0) {
            base = "http://" + base;
        }
        return base;
    }

    json request(const std::string& endpoint, const std::string& gate_id) {
        auto gates = cfg_.gates();
        auto it = gates.find(gate_id);
        if (it == gates.end()) {
            throw std::runtime_error("Контроллер ЕСП [" + gate_id + "] не существует в конфигурации");
        }
        std::string base = normalize_base_url(it->second.url);
        if (base.empty()) {
            throw std::runtime_error("Не указан IP/URL для контроллера '" + it->second.name + "'");
        }

        httplib::Client cli(base);
        int timeout_s = std::max(1, static_cast<int>(cfg_.esp32_timeout_sec()));
        cli.set_connection_timeout(timeout_s, 0);
        cli.set_read_timeout(timeout_s, 0);

        httplib::Headers headers = {
            {"User-Agent", "curl/7.81.0"},
            {"Accept", "*/*"}
        };
        if (!it->second.token.empty()) {
            headers.emplace("X-Token", it->second.token);
        }

        std::string path = "/" + endpoint;
        auto res = cli.Get(path, headers);
        if (!res) {
            auto err = res.error();
            throw std::runtime_error("Нет ответа от " + base + path + " (ошибка сокета: " + httplib::to_string(err) + ")");
        }
        if (res->status == 401) {
            throw std::runtime_error("Контроллер " + base + " отклонил X-Token (HTTP 401 Unauthorized)");
        }
        if (res->status < 200 || res->status >= 300) {
            throw std::runtime_error("Контроллер " + base + " вернул ошибку HTTP " + std::to_string(res->status));
        }

        try {
            json data = res->body.empty() ? json::object() : json::parse(res->body);
            if (data.is_object() && data.value("ok", true) == false) {
                throw std::runtime_error(data.value("error", "ESP32 отклонил команду (ok=false)"));
            }
            return data;
        } catch (const json::parse_error&) {
            return {{"ok", true}, {"raw", res->body}};
        }
    }

    std::pair<bool, std::string> command(const std::string& endpoint, const std::string& label, const std::string& gate_id) {
        std::lock_guard<std::mutex> guard(lock_);
        auto gates = cfg_.gates();
        auto it = gates.find(gate_id);
        if (it == gates.end()) {
            std::string msg = "Сигнал '" + label + "' НЕ отправлен: контроллер [" + gate_id + "] не найден";
            FileLogger::instance().write("SIGNAL", msg);
            return {false, msg};
        }

        const auto& gcfg = it->second;
        // Проверяем, включена ли отправка сигналов на этот контроллер
        if (!gcfg.enabled) {
            std::string msg = "Сигнал '" + label + "' НЕ отправлен на '" + gcfg.name + "' (" + gcfg.url + "): отправка сигналов на этот контроллер ВЫКЛЮЧЕНА в интерфейсе";
            FileLogger::instance().write("SIGNAL", msg);
            return {false, msg};
        }

        double now = monotonic_sec();
        double wait = cfg_.esp32_command_cooldown_sec() - (now - last_command_at_);
        if (wait > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        }
        try {
            json data = request(endpoint, gate_id);
            last_command_at_ = monotonic_sec();
            device_[gate_id] = data;
            last_error_[gate_id] = "";
            std::string ok_msg = gcfg.name + " (" + gcfg.url + "): успешно принял сигнал '" + label + "'";
            FileLogger::instance().write("SIGNAL", ok_msg);
            return {true, ok_msg};
        } catch (const std::exception& e) {
            last_error_[gate_id] = e.what();
            std::string err_msg = "Сигнал '" + label + "' НЕ дошел до '" + gcfg.name + "' (" + gcfg.url + ") — Причина: " + e.what();
            FileLogger::instance().write("ERROR", err_msg);
            return {false, err_msg};
        }
    }

public:
    explicit ESP32GateController(Config& cfg) : cfg_(cfg) {}

    std::pair<bool, std::string> open_gate(const std::string& gate_id) {
        return command("open", "open", gate_id);
    }

    std::pair<bool, std::string> stop_gate(const std::string& gate_id) {
        return command("stop", "stop", gate_id);
    }

    std::pair<bool, std::string> refresh(const std::string& gate_id = "") {
        std::lock_guard<std::mutex> guard(lock_);
        if (!gate_id.empty()) {
            try {
                device_[gate_id] = request("status", gate_id);
                last_error_[gate_id] = "";
                return {true, "Связь с контроллером проверена"};
            } catch (const std::exception& e) {
                device_.erase(gate_id);
                last_error_[gate_id] = e.what();
                FileLogger::instance().write("NET", std::string("Опрос статуса [") + gate_id + "] не удался: " + e.what());
                return {false, e.what()};
            }
        }
        std::string errors;
        for (const auto& [gid, _] : cfg_.gates()) {
            try {
                device_[gid] = request("status", gid);
                last_error_[gid] = "";
            } catch (const std::exception& e) {
                device_.erase(gid);
                last_error_[gid] = e.what();
                if (!errors.empty()) errors += "; ";
                errors += gid + ": " + e.what();
            }
        }
        if (!errors.empty()) return {false, errors};
        return {true, "Статусы всех контроллеров обновлены"};
    }

    json get_gates_status() {
        std::lock_guard<std::mutex> guard(lock_);
        json result = json::object();
        for (const auto& [gid, gcfg] : cfg_.gates()) {
            bool reachable = device_.count(gid) > 0 && !device_[gid].empty();
            std::string state = reachable ? device_[gid].value("gate_status", "") : "";
            std::string label = "неизвестно";
            if (!reachable) label = "нет связи";
            else if (state == "OPEN") label = "открыты";
            else if (state == "NOT_OPEN") label = "закрыты или закрываются";

            result[gid] = {
                {"id", gid},
                {"name", gcfg.name},
                {"url", gcfg.url},
                {"token", gcfg.token},
                {"enabled", gcfg.enabled},
                {"state", state.empty() ? nullptr : json(state)},
                {"label", label},
                {"reachable", reachable},
                {"last_error", last_error_[gid]},
                {"allowed_plates", gcfg.allowed_plates},
                {"ip", reachable && device_[gid].contains("ip") ? device_[gid]["ip"] : nullptr}
            };
        }
        return result;
    }
};