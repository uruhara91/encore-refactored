/*
 * Copyright (C) 2026 Rem01Gaming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <csignal>

#include <DeviceMitigationStore.hpp>
#include <Dumpsys.hpp>
#include <Encore.hpp>
#include <EncoreCLI.hpp>
#include <EncoreConfig.hpp>
#include <EncoreConfigStore.hpp>
#include <EncoreLog.hpp>
#include <EncoreUtility.hpp>
#include <GameRegistry.hpp>
#include <InotifyWatcher.hpp>
#include <ModuleProperty.hpp>
#include <PIDTracker.hpp>
#include <ShellUtility.hpp>
#include <SignalHandler.hpp>

// Custom Logic Managers
#include "../src/CustomLogic/BypassManager.hpp"
#include "../src/CustomLogic/ResolutionManager.hpp"

GameRegistry game_registry;

// --- HELPERS ---

static std::vector<std::string> cpu_governor_paths;

void InitCpuGovernorPaths() {
    DIR* dir = opendir("/sys/devices/system/cpu");
    if (!dir) return;
    
    struct dirent* ent;
    char path[128];
    
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cpu", 3) == 0 && isdigit(ent->d_name[3])) {
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", ent->d_name);

            if (access(path, W_OK) == 0) {
                cpu_governor_paths.push_back(path);
            }
        }
    }
    closedir(dir);
    LOGI("Cached %zu CPU governor paths", cpu_governor_paths.size());
}

void SetCpuGovernor(const std::string& governor) {
    if (cpu_governor_paths.empty()) return; // Safety guard

    // Langsung buka path yang sudah di-cache
    for (const auto& path : cpu_governor_paths) {
        int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            write(fd, governor.c_str(), governor.length());
            close(fd);
        }
    }
}

bool IsCharging() {
    static int fd = open("/sys/class/power_supply/battery/status", O_RDONLY | O_CLOEXEC);
    
    // Fail-safe jika awal boot sysfs belum ready
    if (fd == -1) {
        fd = open("/sys/class/power_supply/battery/status", O_RDONLY | O_CLOEXEC);
        if (fd == -1) return false;
    }

    char buf[16];
    ssize_t len = pread(fd, buf, sizeof(buf) - 1, 0);

    if (len > 0) {
        buf[len] = '\0';
        if (strcasestr(buf, "Charging") || strcasestr(buf, "Full")) return true;
    }
    return false;
}

bool CheckBatterySaver() {
    if (IsCharging()) return false;
    
    try {
        std::vector<std::string> args = {"/system/bin/cmd", "power", "is-power-save-mode"};
        PipeResult pipe_res = popen_direct(args);
        
        if (pipe_res.stream != nullptr) {
            char buf[32];
            if (fgets(buf, sizeof(buf), pipe_res.stream) != nullptr) {
                return (strcasestr(buf, "true") != nullptr);
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

pid_t GetAppPID_Fast(const std::string& targetPkg) {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;

    struct dirent* ent;
    char cmdlinePath[64];
    char cmdlineBuf[256];
    pid_t found_pid = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;

        snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%s/cmdline", ent->d_name);
        int fd = open(cmdlinePath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t len = read(fd, cmdlineBuf, sizeof(cmdlineBuf) - 1);
            close(fd);
            if (len > 0) {
                // Di Linux, spasi di cmdline diganti dengan null terminator '\0'
                if (strcmp(cmdlineBuf, targetPkg.c_str()) == 0) {
                    found_pid = atoi(ent->d_name);
                    break;
                }
            }
        }
    }
    closedir(dir);
    return found_pid;
}

void encore_main_daemon(void) {
    constexpr auto NORMAL_LOOP_INTERVAL_MS = 5000;
    // Interval game bisa kita perbesar karena event buka-tutup app kini terdeteksi INSTAN (<5ms)
    constexpr auto INGAME_LOOP_INTERVAL_MS = 3000; 

    EncoreProfileMode cur_mode = PERFCOMMON;

    std::string active_package;
    std::string last_game_package = "";
    
    bool in_game_session = false;
    bool battery_saver_state = false;
    bool dnd_enabled_by_us = false;
    PIDTracker pid_tracker;
    
    int idle_battery_check_counter = 100;

    run_perfcommon();
    pthread_setname_np(pthread_self(), "EncoreLoop");
    InitCpuGovernorPaths();

    // 1. BUKA STREAM KE KERNEL LOGCAT (EVENT BUFFER)
    FILE* log_pipe = popen("/system/bin/logcat -b events -v raw -s wm_set_resumed_activity", "r");
    if (!log_pipe) {
        LOGE("Failed to open logcat pipe!");
        return;
    }

    int log_fd = fileno(log_pipe);
    // Jadikan non-blocking agar tidak tersangkut di fgets()
    fcntl(log_fd, F_SETFL, fcntl(log_fd, F_GETFL) | O_NONBLOCK);

    struct pollfd pfd;
    pfd.fd = log_fd;
    pfd.events = POLLIN;
    char log_buf[512];

    while (true) {
        if (access(MODULE_UPDATE, F_OK) == 0) [[unlikely]] {
            LOGI("Module update detected, exiting...");
            break;
        }

        int timeout_ms = in_game_session ? INGAME_LOOP_INTERVAL_MS : NORMAL_LOOP_INTERVAL_MS;
        
        // 2. DAEMON TERTIDUR PULAS DI SINI (0% CPU STUTTER-FREE)
        int ret = poll(&pfd, 1, timeout_ms);

        bool app_changed = false;
        std::string new_fg_app = active_package;

        // --- EVENT: ADA PERPINDAHAN APLIKASI DI LAYAR ---
        if (ret > 0 && (pfd.revents & POLLIN)) {
            while (fgets(log_buf, sizeof(log_buf), log_pipe)) {
                std::string line(log_buf);
                
                size_t start = line.find(',');
                size_t end = line.find('/');
                
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    std::string pkg = line.substr(start + 1, end - start - 1);
                    // Bersihkan spasi/karakter sisa
                    pkg.erase(0, pkg.find_first_not_of(" \t\r\n["));
                    pkg.erase(pkg.find_last_not_of(" \t\r\n]") + 1);
                    
                    if (!pkg.empty()) {
                        new_fg_app = pkg;
                        app_changed = true;
                    }
                }
            }
        }

        // --- LOGIC: VALIDASI GAME BARU/KELUAR ---
        if (app_changed) {
            if (game_registry.is_game_registered(new_fg_app)) {
                active_package = new_fg_app;
                in_game_session = true;
            } else {
                // User buka Launcher (XOSLauncher) atau App biasa, kosongkan active_package
                active_package.clear(); 
            }
        }

        bool force_exit = false;

        // --- PENGGANTI DUMPSYS: CEK PID MATI VIA KERNEL ---
        if (in_game_session && !active_package.empty() && pid_tracker.is_valid()) {
            // kill(pid, 0) adalah trik OS untuk mengecek apakah PID masih ada
            // Trik ini jauh lebih ringan daripada Dumpsys
            if (kill(pid_tracker.get_pid(), 0) != 0) {
                LOGI("Game PID dead (Force Close): %s", active_package.c_str());
                force_exit = true;
            }
        }
        
        // Jika statusnya pindah ke app non-game, exit game mode
        if (app_changed && active_package != last_game_package && active_package.empty()) {
            force_exit = true;
        }

        // ===========================
        // STATE: EXIT GAME
        // ===========================
        if (force_exit && !last_game_package.empty()) {
            LOGI("Exit Game: %s", last_game_package.c_str());
            ResolutionManager::GetInstance().ResetGameMode(last_game_package);
            BypassManager::GetInstance().SetBypass(false);
            
            if (dnd_enabled_by_us) {
                dnd_enabled_by_us = false;
                set_do_not_disturb(false);
            }
            
            last_game_package = "";
            active_package.clear();
            pid_tracker.invalidate();
            in_game_session = false;
            
            idle_battery_check_counter = 100; // Paksa idle check selanjutnya
            cur_mode = BALANCE_PROFILE; // Fallback profile 
        }

        // ===========================
        // STATE: ENTER GAME
        // ===========================
        if (in_game_session && !active_package.empty()) {
            if (active_package != last_game_package) {
                LOGI("Enter Game: %s", active_package.c_str());
                
                auto active_game = game_registry.find_game(active_package); 
                bool lite_mode = (active_game && active_game->lite_mode) || config_store.get_preferences().enforce_lite_mode;
                bool enable_dnd = (active_game && active_game->enable_dnd);

                ResolutionManager::GetInstance().ApplyGameMode(active_package);
                
                if (!lite_mode) {
                    BypassManager::GetInstance().SetBypass(true);
                    LOGI("Bypass Charge: ON");
                } else {
                    BypassManager::GetInstance().SetBypass(false);
                    LOGI("Bypass Charge: OFF (Lite Mode)");
                }
                
                if (enable_dnd) {
                    set_do_not_disturb(true);
                    dnd_enabled_by_us = true;
                }

                // Ambil PID HANYA 1x saat pertama buka game
                pid_t game_pid = GetAppPID_Fast(active_package);
                if (game_pid > 0) {
                    cur_mode = PERFORMANCE_PROFILE;
                    apply_performance_profile(lite_mode, active_package, game_pid);
                    pid_tracker.set_pid(game_pid);
                    LOGI("Profile: Performance (PID: %d)", game_pid);
                }

                last_game_package = active_package;
            }
            continue; // Jangan jalankan idle check saat main game
        }

        // ===========================
        // STATE: IDLE CHECK
        // ===========================
        if (!in_game_session && ++idle_battery_check_counter >= 6) {
            battery_saver_state = CheckBatterySaver();
            idle_battery_check_counter = 0;
            
            if (battery_saver_state) {
                if (cur_mode != POWERSAVE_PROFILE) {
                    LOGI("Profile: PowerSave");
                    cur_mode = POWERSAVE_PROFILE;
                    apply_powersave_profile();
                }
            } else {
                if (cur_mode != BALANCE_PROFILE) {
                    LOGI("Profile: Balance");
                    cur_mode = BALANCE_PROFILE;
                    apply_balance_profile();
                }
            }
        }
    }

    // Cleanup saat Daemon di-stop
    if (log_pipe) pclose(log_pipe);
}

int run_daemon() {
    auto SetModule_DescriptionStatus = [](const std::string &status) {
        static const std::string description_base = "Special performance module for your Device.";
        std::string description_new = "[" + status + "] " + description_base;

        std::vector<ModuleProperties> module_properties{{"description", description_new}};

        try {
            ModuleProperty::Change(MODULE_PROP, module_properties);
        } catch (const std::runtime_error &e) {
            LOGE("Failed to apply module properties: {}", e.what());
        }
    };

    auto NotifyFatalError = [&SetModule_DescriptionStatus](const std::string &error_msg) {
        notify(("ERROR: " + error_msg).c_str());
        SetModule_DescriptionStatus("\xE2\x9D\x8C " + error_msg);
    };

    std::atexit([]() { SignalHandler::cleanup_before_exit(); });

    SignalHandler::setup_signal_handlers();

    if (!create_lock_file()) {
        std::cerr << "\033[31mERROR:\033[0m Another instance of Encore Daemon is already running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (!check_dumpsys_sanity()) {
        std::cerr << "\033[31mERROR:\033[0m Dumpsys sanity check failed" << std::endl;
        NotifyFatalError("Dumpsys sanity check failed");
        LOGC("Dumpsys sanity check failed");
        return EXIT_FAILURE;
    }

    if (access(ENCORE_GAMELIST, F_OK) != 0) {
        std::cerr << "\033[31mERROR:\033[0m " << ENCORE_GAMELIST << " is missing" << std::endl;
        NotifyFatalError("gamelist.json is missing");
        LOGC("{} is missing", ENCORE_GAMELIST);
        return EXIT_FAILURE;
    }

    if (!game_registry.load_from_json(ENCORE_GAMELIST)) {
        std::cerr << "\033[31mERROR:\033[0m Failed to parse " << ENCORE_GAMELIST << std::endl;
        NotifyFatalError("Failed to parse gamelist.json");
        LOGC("Failed to parse {}", ENCORE_GAMELIST);
        return EXIT_FAILURE;
    }

    if (!device_mitigation_store.load_config()) {
        std::cerr << "\033[31mERROR:\033[0m Failed to parse " << DEVICE_MITIGATION_FILE << std::endl;
        NotifyFatalError("Failed to parse device_mitigation.json");
        LOGC("Failed to parse {}", DEVICE_MITIGATION_FILE);
        return EXIT_FAILURE;
    }

    if (daemon(0, 0) != 0) {
        LOGC("Failed to daemonize service");
        NotifyFatalError("Failed to daemonize service");
        return EXIT_FAILURE;
    }

    InotifyWatcher file_watcher;
    if (!init_file_watcher(file_watcher)) {
        LOGC("Failed to initialize file watcher");
        NotifyFatalError("Failed to initialize file watcher");
        return EXIT_FAILURE;
    }

    LOGI("Initializing Custom Logic Managers...");
    BypassManager::GetInstance().Init();
    ResolutionManager::GetInstance().LoadGameMap("/data/adb/.config/encore/games.txt");

    LOGI("Encore Tweaks daemon started");
    SetModule_DescriptionStatus("\xF0\x9F\x98\x8B Tweaks applied successfully");
    encore_main_daemon();

    // If we reach this, the daemon is dead
    LOGW("Encore Tweaks daemon exited");
    SignalHandler::cleanup_before_exit();
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    if (getuid() != 0) {
        std::cerr << "Run as root!" << std::endl;
        return EXIT_FAILURE;
    }
    return encore_cli(argc, argv);
}
