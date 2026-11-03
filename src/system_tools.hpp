#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "advapi32.lib")
#endif
#include "logger.hpp"

namespace sys_tools {

inline std::string get_exe_path() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf);
#else
    return "./GateControl";
#endif
}

inline std::string get_exe_dir() {
    std::filesystem::path p(get_exe_path());
    return p.parent_path().string();
}

// 1. Создание и управление скриптом мониторинга (Watchdog)
inline void ensure_watchdog_scripts_created() {
    std::string dir = get_exe_dir();
    std::string bat_path = dir + "\\watchdog_gatecontrol.bat";
    std::string vbs_path = dir + "\\watchdog_gatecontrol.vbs";

    // Батник проверяет процесс каждые 3 секунды и перезапускает при закрытии, если нет флага штатной остановки
    std::ofstream bat(bat_path);
    if (bat.is_open()) {
        bat << "@echo off\r\n"
            << "chcp 65001 >nul\r\n"
            << "cd /d \"" << dir << "\"\r\n"
            << ":loop\r\n"
            << "if exist \"watchdog_stop.flag\" (\r\n"
            << "    del /f /q \"watchdog_stop.flag\" >nul 2>&1\r\n"
            << "    exit /b 0\r\n"
            << ")\r\n"
            << "tasklist /FI \"IMAGENAME eq GateControl.exe\" 2>NUL | find /I \"GateControl.exe\" >NUL\r\n"
            << "if errorlevel 1 (\r\n"
            << "    if exist \"watchdog_stop.flag\" (\r\n"
            << "        del /f /q \"watchdog_stop.flag\" >nul 2>&1\r\n"
            << "        exit /b 0\r\n"
            << "    )\r\n"
            << "    echo [%date% %time%] [WATCHDOG] Обнаружено закрытие GateControl.exe! Автоматический перезапуск... >> gatecontrol.log\r\n"
            << "    start \"\" \"" << get_exe_path() << "\"\r\n"
            << "    timeout /t 5 /nobreak >nul\r\n"
            << ")\r\n"
            << "timeout /t 3 /nobreak >nul\r\n"
            << "goto loop\r\n";
    }

    // VBS-обертка запускает батник полностью скрытно (без черного окна консоли)
    std::ofstream vbs(vbs_path);
    if (vbs.is_open()) {
        vbs << "Set WshShell = CreateObject(\"WScript.Shell\")\r\n"
            << "WshShell.Run chr(34) & \"" << bat_path << "\" & chr(34), 0, False\r\n";
    }
}

inline void set_watchdog_state(bool enable) {
    std::string dir = get_exe_dir();
    std::string flag_path = dir + "\\watchdog_stop.flag";
    std::string vbs_path = dir + "\\watchdog_gatecontrol.vbs";

    ensure_watchdog_scripts_created();

    if (enable) {
        std::error_code ec;
        std::filesystem::remove(flag_path, ec);
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", "wscript.exe", ("\"" + vbs_path + "\"").c_str(), dir.c_str(), SW_HIDE);
#endif
        FileLogger::instance().write("WATCHDOG", "Скрипт мониторинга и автоперезапуска (Watchdog) АКТИВИРОВАН");
    } else {
        std::ofstream flag(flag_path);
        flag << "stop";
        flag.close();
        FileLogger::instance().write("WATCHDOG", "Скрипт мониторинга (Watchdog) ОТКЛЮЧЕН пользователем");
    }
}

// Вызывается при штатном закрытии программы пользователем через окно подтверждения
inline void notify_intentional_exit() {
    std::string flag_path = get_exe_dir() + "\\watchdog_stop.flag";
    std::ofstream flag(flag_path);
    flag << "intentional_exit";
    flag.close();
}

// 2. Управление автозапуском при включении Windows (Реестр HKCU\...\Run + скрипт)
inline bool is_autostart_registered() {
#ifdef _WIN32
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char val[MAX_PATH] = {0};
        DWORD sz = sizeof(val);
        LONG ret = RegQueryValueExA(hKey, "GateControlV2", nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &sz);
        RegCloseKey(hKey);
        return (ret == ERROR_SUCCESS && sz > 1);
    }
#endif
    return false;
}

inline bool set_autostart_state(bool enable) {
    std::string dir = get_exe_dir();
    std::string autostart_bat = dir + "\\autostart_gatecontrol.bat";

    std::ofstream bat(autostart_bat);
    if (bat.is_open()) {
        bat << "@echo off\r\n"
            << "cd /d \"" << dir << "\"\r\n"
            << "start \"\" \"" << get_exe_path() << "\"\r\n";
    }

#ifdef _WIN32
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        FileLogger::instance().write("ERROR", "Не удалось открыть ветку реестра автозагрузки Windows");
        return false;
    }

    if (enable) {
        std::string cmd = "\"" + get_exe_path() + "\"";
        LONG res = RegSetValueExA(hKey, "GateControlV2", 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(cmd.c_str()),
                                  static_cast<DWORD>(cmd.size() + 1));
        RegCloseKey(hKey);
        FileLogger::instance().write("SYSTEM", "Автозапуск при старте системы ВКЛЮЧЕН в реестре Windows");
        return res == ERROR_SUCCESS;
    } else {
        RegDeleteValueA(hKey, "GateControlV2");
        RegCloseKey(hKey);
        FileLogger::instance().write("SYSTEM", "Автозапуск при старте системы ВЫКЛЮЧЕН");
        return true;
    }
#else
    return false;
#endif
}

} // namespace sys_tools