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

#include "../src/CustomLogic/BypassManager.hpp"
#include "../src/CustomLogic/ResolutionManager.hpp"

GameRegistry game_registry;

void encore_main_daemon(void) {
    constexpr auto NORMAL_LOOP_INTERVAL_MS = 5000;
    constexpr auto INGAME_LOOP_INTERVAL_MS = 1000;

    EncoreProfileMode cur_mode = PERFCOMMON;

    std::string active_package;
    std::string last_game_package = "";
    
    bool in_game_session = false;
    bool battery_saver_state = false;
    PIDTracker pid_tracker;
    
    int idle_battery_check_counter = 100;

    run_perfcommon();
    pthread_setname_np(pthread_self(), "EncoreLoop");
    InitCpuGovernorPaths();

    FILE* log_pipe = popen("/system/bin/logcat -b events -v raw -s wm_set_resumed_activity am_set_resumed_activity", "r");
    if (!log_pipe) {
        LOGE("Failed to open logcat pipe!");
        return;
    }

    int log_fd = fileno(log_pipe);
    fcntl(log_fd, F_SETFL, fcntl(log_fd, F_GETFL) | O_NONBLOCK);

    struct pollfd pfd;
    pfd.fd = log_fd;
    pfd.events = POLLIN;
    char log_buf[1024];

    while (true) {
        if (access(MODULE_UPDATE, F_OK) == 0) [[unlikely]] {
            LOGI("Module update detected, exiting...");
            break;
        }

        if (!log_pipe) {
            log_pipe = popen("/system/bin/logcat -b events -v raw -s wm_set_resumed_activity am_set_resumed_activity", "r");
            if (log_pipe) {
                log_fd = fileno(log_pipe);
                fcntl(log_fd, F_SETFL, fcntl(log_fd, F_GETFL) | O_NONBLOCK);
                pfd.fd = log_fd;
            } else {
                pfd.fd = -1;
            }
        }

        int timeout_ms = in_game_session ? INGAME_LOOP_INTERVAL_MS : NORMAL_LOOP_INTERVAL_MS;
        int ret = poll(&pfd, 1, timeout_ms);

        if (ret < 0 && errno == EINTR) continue; 

        bool app_changed = false;
        std::string new_fg_app = active_package;

        if (ret > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOGE("Logcat pipe error/hup! Restarting...");
                if (log_pipe) { pclose(log_pipe); log_pipe = nullptr; }
                pfd.fd = -1;
                continue;
            }

            if (pfd.revents & POLLIN) {
                if (fgets(log_buf, sizeof(log_buf), log_pipe) == nullptr) {
                    if (feof(log_pipe)) {
                        LOGE("Logcat pipe EOF! Restarting...");
                        pclose(log_pipe);
                        log_pipe = nullptr;
                        pfd.fd = -1;
                    } else {
                        clearerr(log_pipe);
                    }
                    continue;
                }

                do {
                    std::string_view line(log_buf); 
                    size_t start = line.find(',');
                    size_t end = line.find('/');
                    
                    if (start != std::string_view::npos && end != std::string_view::npos && end > start) {
                        std::string_view pkg_view = line.substr(start + 1, end - start - 1);
                        
                        size_t first = pkg_view.find_first_not_of(" \t\r\n[");
                        if (first != std::string_view::npos) {
                            pkg_view.remove_prefix(first);
                            
                            size_t last = pkg_view.find_last_not_of(" \t\r\n]");
                            if (last != std::string_view::npos) {
                                pkg_view.remove_suffix(pkg_view.length() - last - 1);
                            }
                            
                            if (!pkg_view.empty()) {
                                new_fg_app = std::string(pkg_view);
                                app_changed = true;
                            }
                        }
                    }
                } while (fgets(log_buf, sizeof(log_buf), log_pipe) != nullptr);

                clearerr(log_pipe);
            }
        }

        if (app_changed) {
            if (game_registry.is_game_registered(new_fg_app)) {
                active_package = new_fg_app;
                in_game_session = true;
            } else {
                active_package.clear(); 
                in_game_session = false;
            }
        }

        bool force_exit = false;

        if (!app_changed && in_game_session && !active_package.empty()) {
            if (!pid_tracker.is_valid()) {
                LOGI("Game PID dead (Force Close): {}", active_package);
                force_exit = true;
            }
        }
        
        if (app_changed && active_package.empty() && !last_game_package.empty()) {
            force_exit = true;
        }

        // ===========================
        // EXIT GAME
        // ===========================
        if (force_exit) {
            if (!last_game_package.empty()) {
                LOGI("Exit Game: {}", last_game_package);
                ResolutionManager::GetInstance().ResetGameMode(last_game_package);
                BypassManager::GetInstance().SetBypass(false);
                
                LOGI("[TRACE-MAIN] Memaksa DND OFF karena keluar dari game.");
                set_do_not_disturb(false); 
                
                last_game_package = "";
            }
            
            active_package.clear();
            pid_tracker.invalidate();
            in_game_session = false;
            
            idle_battery_check_counter = 100;
            cur_mode = PERFCOMMON; 
        }

        // ===========================
        // ENTER GAME
        // ===========================
        if (in_game_session && !active_package.empty() && active_package != last_game_package) {
            
            LOGI("[TRACE-MAIN] Logcat terpicu untuk: {}", active_package);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            std::string real_focused_app = GetFocusedPackage();
            LOGI("[TRACE-MAIN] Dumpsys melapor fokus pada: {}", real_focused_app.empty() ? "UNKNOWN" : real_focused_app);

            if (real_focused_app != active_package) {
                LOGW("[TRACE-MAIN] Validasi gagal. Fokus asli: {}. Resetting state...", real_focused_app);
                active_package.clear();
                in_game_session = false;
            } else {
                pid_t game_pid = GetAppPID_Fast(active_package);
                
                for (int i = 0; i < 5 && game_pid <= 0; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    game_pid = GetAppPID_Fast(active_package);
                }

                if (game_pid > 0) {
                    LOGI("[TRACE-MAIN] PID {} Valid. Menerapkan Profil & DND.", game_pid);
                    
                    if (!last_game_package.empty()) {
                        ResolutionManager::GetInstance().ResetGameMode(last_game_package);
                        set_do_not_disturb(false);
                    }

                    auto active_game = game_registry.find_game(active_package);
                    bool lite_mode = (active_game && active_game->lite_mode) || config_store.get_preferences().enforce_lite_mode;
                    bool enable_dnd = (active_game && active_game->enable_dnd);
                    bool enable_bypass = active_game ? active_game->enable_bypass : false;
                    std::string downscale = active_game ? active_game->downscale_ratio : "1.0";

                    ResolutionManager::GetInstance().ApplyGameMode(active_package, downscale);
                    BypassManager::GetInstance().SetBypass(enable_bypass);
                    
                    if (enable_dnd) {
                        set_do_not_disturb(true);
                    }

                    cur_mode = PERFORMANCE_PROFILE;
                    apply_performance_profile(lite_mode, active_package, game_pid);
                    pid_tracker.set_pid(game_pid);
                    last_game_package = active_package;
                } else {
                    LOGW("[TRACE-MAIN] PID tidak ditemukan untuk {}, membatalkan session.", active_package);
                    active_package.clear();
                    in_game_session = false;
                }
            }
        }

        // ===========================
        // IDLE CHECK
        // ===========================
        if (!in_game_session && ++idle_battery_check_counter >= 6) {
            battery_saver_state = CheckBatterySaver();
            idle_battery_check_counter = 0;
            
            if (battery_saver_state) {
                if (cur_mode != POWERSAVE_PROFILE) {
                    cur_mode = POWERSAVE_PROFILE;
                    apply_powersave_profile();
                }
            } else {
                if (cur_mode != BALANCE_PROFILE) {
                    cur_mode = BALANCE_PROFILE;
                    apply_balance_profile();
                }
            }
        }
    }

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
