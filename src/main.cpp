#include <httplib.h>
#include <nlohmann/json.hpp>
#include "webview.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <exception>
#include <csignal>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "logger.hpp"
#include "config.hpp"
#include "system_tools.hpp"
#include "plate.hpp"
#include "esp32_gate.hpp"
#include "anpr_worker.hpp"

using json = nlohmann::json;

inline std::string safe_dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

#ifdef _WIN32
WNDPROC g_orig_wndproc = nullptr;

// Перехват закрытия окна и выключения Windows с подробной записью причины в лог
LRESULT CALLBACK SafeCloseWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_QUERYENDSESSION || msg == WM_ENDSESSION) {
        FileLogger::instance().write("SYSTEM", "Получен системный сигнал WM_ENDSESSION: Windows выключается или перезагружается");
        sys_tools::notify_intentional_exit();
        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wParam, lParam);
    }
    if (msg == WM_CLOSE) {
        int res = MessageBoxW(
            hwnd,
            L"Сервер обработки номеров сейчас работает в постоянном режиме.\n\n"
            L"Вы действительно хотите принудительно остановить сервер и закрыть программу?",
            L"Подтверждение закрытия — Gate Control v2",
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 | MB_SYSTEMMODAL
        );
        if (res != IDYES) {
            FileLogger::instance().write("INFO", "Попытка закрытия окна отменена пользователем (выбрано 'Нет')");
            return 0;
        }
        FileLogger::instance().write("WARN", "ШТАТНЫЙ ВЫХОД: Пользователь подтвердил закрытие программы вручную (выбрано 'Да')");
        sys_tools::notify_intentional_exit();
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wParam, lParam);
}

BOOL WINAPI ConsoleAndSystemCtrlHandler(DWORD ctrlType) {
    std::string reason = "Неизвестный системный сигнал";
    if (ctrlType == CTRL_CLOSE_EVENT) reason = "Закрытие системного процесса (CTRL_CLOSE_EVENT)";
    else if (ctrlType == CTRL_LOGOFF_EVENT) reason = "Выход пользователя из системы Windows (CTRL_LOGOFF_EVENT)";
    else if (ctrlType == CTRL_SHUTDOWN_EVENT) reason = "Выключение питания / перезагрузка ОС (CTRL_SHUTDOWN_EVENT)";
    FileLogger::instance().write("SYSTEM", "Программа остановлена системой: " + reason);
    return FALSE;
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    std::ostringstream oss;
    DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    oss << "АВАРИЙНОЕ ЗАКРЫТИЕ (SEH Exception Code: 0x" << std::hex << code
        << ", адрес: 0x" << addr << "). Если включен Watchdog, программа перезапустится через 3 сек.";
    FileLogger::instance().write("FATAL", oss.str());
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetUnhandledExceptionFilter(CrashHandler);
    SetConsoleCtrlHandler(ConsoleAndSystemCtrlHandler, TRUE);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif

    std::set_terminate([]() {
        FileLogger::instance().write("FATAL", "Вызван std::terminate() — неперехваченное исключение C++. Перезапуск через Watchdog...");
        std::abort();
    });

    FileLogger::instance().write("SYSTEM", "=== Запуск сервера GateControl v2 (PID процесса активен) ===");
    Config cfg("config.json");

    // Инициализируем скрипт Watchdog и проверяем статус Автозапуска
    sys_tools::ensure_watchdog_scripts_created();
    if (cfg.watchdog_enabled()) {
        sys_tools::set_watchdog_state(true);
    }
    if (cfg.autostart_enabled() != sys_tools::is_autostart_registered()) {
        sys_tools::set_autostart_state(cfg.autostart_enabled());
    }

    ESP32GateController controller(cfg);

    auto signal_gate_by_id = [&](const std::string& gate_id) -> std::pair<bool, std::string> {
        return controller.open_gate(gate_id);
    };

    AnprWorker anpr(signal_gate_by_id, cfg);
    std::mutex api_lock;
    anpr.start();

    std::atomic<bool> app_running{true};
    std::thread esp_poll_thread([&]() {
        while (app_running) {
            if (cfg.system_enabled()) {
                try { controller.refresh(); } catch (...) {}
            }
            for (int i = 0; i < 100 && app_running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    auto full_status = [&]() -> json {
        return {
            {"gates", controller.get_gates_status()},
            {"anpr", anpr.get_status()},
            {"settings", cfg.get_settings_json()},
            {"api_version", 6}
        };
    };

    httplib::Server svr;

    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            FileLogger::instance().write("ERROR", "HTTP ошибка на маршруте " + req.path + ": " + e.what());
        } catch (...) {
            FileLogger::instance().write("ERROR", "Неизвестная HTTP ошибка на маршруте " + req.path);
        }
        res.status = 500;
        res.set_content("{\"detail\":\"Internal Server Error\"}", "application/json; charset=utf-8");
    });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("static/index.html", std::ios::binary);
        if (!f.is_open()) {
            res.status = 404;
            res.set_content("static/index.html not found", "text/plain");
            return;
        }
        std::string html((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res.set_content(html, "text/html; charset=utf-8");
    });

    svr.Get("/api/status", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Get("/api/anpr/snapshot", [&](const httplib::Request& req, httplib::Response& res) {
        std::string cam_id = req.has_param("cam_id") ? req.get_param_value("cam_id") : "";
        auto jpeg = anpr.last_jpeg(cam_id);
        if (jpeg.empty()) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Превью отключено или нет кадра"}})), "application/json");
            return;
        }
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), "image/jpeg");
    });

    // Обновление правого блока настроек (включая Watchdog, Автозапуск и показ потока)
    svr.Post("/api/settings", [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.status = 400;
            res.set_content(safe_dump(json({{"detail", "Некорректный JSON"}})), "application/json");
            return;
        }
        auto cur = cfg.get_settings_json();
        bool sys_en = body.value("system_enabled", cur["system_enabled"].get<bool>());
        bool prev_en = body.value("preview_enabled", cur["preview_enabled"].get<bool>());
        bool auto_en = body.value("auto_close_enabled", cur["auto_close_enabled"].get<bool>());
        double auto_sec = body.value("auto_close_sec", cur["auto_close_sec"].get<double>());
        bool dep_en = body.value("departing_check_enabled", cur["departing_check_enabled"].get<bool>());
        bool mot_en = body.value("motion_filter_enabled", cur["motion_filter_enabled"].get<bool>());
        bool wd_en = body.value("watchdog_enabled", cur["watchdog_enabled"].get<bool>());
        bool as_en = body.value("autostart_enabled", cur["autostart_enabled"].get<bool>());

        bool prev_wd = cur["watchdog_enabled"].get<bool>();
        bool prev_as = cur["autostart_enabled"].get<bool>();

        cfg.update_settings(sys_en, prev_en, auto_en, auto_sec, dep_en, mot_en, wd_en, as_en);

        if (wd_en != prev_wd) sys_tools::set_watchdog_state(wd_en);
        if (as_en != prev_as) sys_tools::set_autostart_state(as_en);

        anpr.push_external_event("Настройки системы обновлены пользователем", "info");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    // --- API УПРАВЛЕНИЯ КАМЕРАМИ И ПРИВЯЗКАМИ К ЕСП ---
    svr.Post("/api/cameras/active", [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (!body.is_discarded() && body.contains("camera_id")) {
            std::string cid = body["camera_id"].get<std::string>();
            cfg.set_active_camera(cid);
            FileLogger::instance().write("INFO", "Переключено отображение потока в интерфейсе на камеру [" + cid + "]");
        }
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Post("/api/cameras", [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("rtsp_url")) {
            res.status = 400;
            res.set_content(safe_dump(json({{"detail", "Укажите RTSP ссылку"}})), "application/json");
            return;
        }
        std::string cid = body.value("id", "");
        std::string name = body.value("name", "Новая камера");
        std::string rtsp = body.value("rtsp_url", "");
        std::vector<std::string> links;
        if (body.contains("linked_gates") && body["linked_gates"].is_array()) {
            for (auto& g : body["linked_gates"]) links.push_back(g.get<std::string>());
        }
        std::string saved_id = cfg.add_or_update_camera(cid, name, rtsp, links);
        anpr.push_external_event("Сохранена камера '" + name + "' [" + saved_id + "], привязано контроллеров ЕСП: " + std::to_string(links.size()), "info");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Delete(R"(/api/cameras/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string cid = req.matches[1];
        if (!cfg.delete_camera(cid)) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Камера не найдена"}})), "application/json");
            return;
        }
        anpr.push_external_event("Удалена камера [" + cid + "]", "warn");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    // --- API УПРАВЛЕНИЯ КОНТРОЛЛЕРАМИ ЕСП ---
    svr.Post("/api/gates", [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("url")) {
            res.status = 400;
            res.set_content(safe_dump(json({{"detail", "Укажите IP / URL контроллера ЕСП"}})), "application/json");
            return;
        }
        std::string gid = body.value("id", "");
        std::string name = body.value("name", "Новый ЕСП");
        std::string url = body.value("url", "");
        std::string token = body.value("token", "123456");
        bool enabled = body.value("enabled", true);

        std::string saved_id = cfg.add_or_update_gate(gid, name, url, token, enabled);
        anpr.push_external_event("Сохранен контроллер ЕСП '" + name + "' (" + url + ")", "info");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Post(R"(/api/gates/([^/]+)/toggle)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string gid = req.matches[1];
        auto body = json::parse(req.body, nullptr, false);
        auto gates = cfg.gates();
        if (!gates.count(gid)) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Контроллер не найден"}})), "application/json");
            return;
        }
        bool next_state = body.is_discarded() ? !gates[gid].enabled : body.value("enabled", !gates[gid].enabled);
        cfg.set_gate_enabled(gid, next_state);
        anpr.push_external_event("Отправка сигналов на контроллер '" + gates[gid].name + "': " + (next_state ? "ВКЛЮЧЕНА" : "ОТКЛЮЧЕНА"), next_state ? "success" : "warn");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Delete(R"(/api/gates/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string gid = req.matches[1];
        if (!cfg.delete_gate(gid)) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Контроллер не найден"}})), "application/json");
            return;
        }
        anpr.push_external_event("Контроллер ЕСП [" + gid + "] удален", "warn");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    // --- ЛОГИ ---
    svr.Post("/api/logs/open", [&](const httplib::Request&, httplib::Response& res) {
        std::string log_path = FileLogger::instance().path();
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        res.set_content(safe_dump(json({{"ok", true}, {"message", "Файл логов открыт"}})), "application/json; charset=utf-8");
    });

    svr.Get("/api/logs/tail", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(safe_dump(json({{"ok", true}, {"log", FileLogger::instance().tail(150)}})), "application/json; charset=utf-8");
    });

    // --- БЕЛЫЕ СПИСКИ НОМЕРОВ КОНТРОЛЛЕРОВ ---
    svr.Get(R"(/api/gates/([^/]+)/plates)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string gate_id = req.matches[1];
        auto gates = cfg.gates();
        if (!gates.count(gate_id)) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Ворота не найдены"}})), "application/json");
            return;
        }
        auto plates = gates[gate_id].allowed_plates;
        std::sort(plates.begin(), plates.end());
        res.set_content(safe_dump(json({{"ok", true}, {"plates", plates}})), "application/json; charset=utf-8");
    });

    svr.Post(R"(/api/gates/([^/]+)/plates)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string gate_id = req.matches[1];
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("plate")) {
            res.status = 400;
            res.set_content(safe_dump(json({{"detail", "Некорректный JSON"}})), "application/json");
            return;
        }

        std::string p = plate::normalize_plate(body["plate"].get<std::string>());
        if (!plate::is_valid_ru_plate(p)) {
            res.status = 400;
            res.set_content(safe_dump(json({{"detail", "Формат: А123ВС (6 символов)"}})), "application/json");
            return;
        }

        std::lock_guard<std::mutex> guard(api_lock);
        auto gates = cfg.gates();
        if (!gates.count(gate_id)) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Ворота не найдены"}})), "application/json");
            return;
        }

        auto current = gates[gate_id].allowed_plates;
        for (const auto& item : current) {
            if (plate::normalize_plate(item) == p) {
                res.status = 409;
                res.set_content(safe_dump(json({{"detail", "Номер уже есть"}})), "application/json");
                return;
            }
        }

        current.push_back(p);
        cfg.set_gate_plates(gate_id, current);
        anpr.push_external_event("Добавлен номер " + p + " в список контроллера '" + gates[gate_id].name + "'", "info");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Delete(R"(/api/gates/([^/]+)/plates/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string gate_id = req.matches[1];
        std::string target = plate::normalize_plate(req.matches[2]);

        std::lock_guard<std::mutex> guard(api_lock);
        auto gates = cfg.gates();
        if (!gates.count(gate_id)) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Ворота не найдены"}})), "application/json");
            return;
        }

        auto current = gates[gate_id].allowed_plates;
        std::vector<std::string> updated;
        for (const auto& item : current) {
            if (plate::normalize_plate(item) != target) updated.push_back(item);
        }

        if (updated.size() == current.size()) {
            res.status = 404;
            res.set_content(safe_dump(json({{"detail", "Номер не найден"}})), "application/json");
            return;
        }

        cfg.set_gate_plates(gate_id, updated);
        anpr.push_external_event("Удален номер " + target + " из контроллера '" + gates[gate_id].name + "'", "info");
        res.set_content(safe_dump(full_status()), "application/json; charset=utf-8");
    });

    svr.Post(R"(/api/gates/([^/]+)/(open|stop|refresh))", [&](const httplib::Request& req, httplib::Response& res) {
        if (!cfg.system_enabled()) {
            res.status = 403;
            res.set_content(safe_dump(json({{"detail", "Программа остановлена кнопкой СТОП"}})), "application/json; charset=utf-8");
            return;
        }
        std::string gate_id = req.matches[1];
        std::string action = req.matches[2];
        std::pair<bool, std::string> result;

        if (action == "open") result = controller.open_gate(gate_id);
        else if (action == "stop") result = controller.stop_gate(gate_id);
        else result = controller.refresh(gate_id);

        if (!result.first) {
            res.status = 503;
            res.set_content(safe_dump(json({{"detail", result.second}})), "application/json; charset=utf-8");
            return;
        }
        res.set_content(safe_dump(json({{"ok", true}, {"message", result.second}})), "application/json; charset=utf-8");
    });

    int port = cfg.web_port();
    // Слушаем 0.0.0.0, чтобы к серверу можно было подключаться с телефона (через Wi-Fi / Tailscale / Туннель)
    std::thread server_thread([&]() {
        svr.listen("0.0.0.0", port);
    });

    webview::webview w(false, nullptr);
    w.set_title("Gate Control v2 — Мультикамерный сервер ANPR");
    w.set_size(1360, 900, WEBVIEW_HINT_NONE);

#ifdef _WIN32
    HWND hwnd = nullptr;
    try {
        hwnd = static_cast<HWND>(w.window().value());
    } catch (...) {
        hwnd = nullptr;
    }
    if (hwnd) {
        g_orig_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SafeCloseWndProc))
        );
    }
#endif

    w.navigate("http://127.0.0.1:" + std::to_string(port));
    w.run();

    app_running = false;
    svr.stop();
    if (server_thread.joinable()) server_thread.join();
    if (esp_poll_thread.joinable()) esp_poll_thread.join();
    anpr.stop();

    FileLogger::instance().write("SYSTEM", "=== Сервер GateControl v2 штатно завершил работу ===");
    return 0;
}