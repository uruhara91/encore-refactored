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
        std::string output = ShellUtility::Execute("cmd power is-power-save-mode");
        return (output.find("true") != std::string::npos);
    } catch (...) {
        return false;
    }
}

void encore_main_daemon(void) {
    constexpr auto INGAME_LOOP_INTERVAL = std::chrono::milliseconds(1500);
    constexpr auto NORMAL_LOOP_INTERVAL = std::chrono::seconds(5);
    const EncoreProfileMode SCREEN_OFF_PROFILE = static_cast<EncoreProfileMode>(99);

    EncoreProfileMode cur_mode = PERFCOMMON;
    DumpsysWindowDisplays window_displays;

    std::string active_package;
    std::string last_game_package = "";
    auto last_full_check = std::chrono::steady_clock::now();
    
    bool in_game_session = false;
    bool battery_saver_state = false;
    bool dnd_enabled_by_us = false;
    PIDTracker pid_tracker;
    
    // Counter init 100 agar langsung cek saat boot
    int idle_battery_check_counter = 100;

    auto GetActiveGame = [&](const std::vector<RecentAppList> &app_list) -> std::string {
        for (const auto &app : app_list) {
            if (app.visible && game_registry.is_game_registered(app.package_name)) {
                return app.package_name;
            }
        }
        return "";
    };

    run_perfcommon();
    pthread_setname_np(pthread_self(), "EncoreLoop");
    InitCpuGovernorPaths();

    while (true) {
        // Fast Check: Module Update / Disable
        if (access(MODULE_UPDATE, F_OK) == 0) [[unlikely]] {
            LOGI("Module update detected, exiting...");
            break;
        }

        auto now = std::chrono::steady_clock::now();
        
        // 1. WINDOW SCAN
        auto interval = in_game_session ? INGAME_LOOP_INTERVAL : NORMAL_LOOP_INTERVAL;
        auto elapsed = now - last_full_check;

        if (elapsed < interval) {
            auto time_to_sleep = interval - elapsed;
            
            if (time_to_sleep > std::chrono::seconds(1)) {
                time_to_sleep = std::chrono::seconds(1);
            }
            std::this_thread::sleep_for(time_to_sleep);
            continue;
        }
        
        try {
            Dumpsys::WindowDisplays(window_displays);
            last_full_check = std::chrono::steady_clock::now(); // Update timer
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // 2. SCREEN OFF LOGIC (Deep Sleep)
        if (!window_displays.screen_awake) {
            if (cur_mode != SCREEN_OFF_PROFILE) {
                LOGI("Screen OFF -> Force Powersave");
                SetCpuGovernor("powersave"); 
                cur_mode = SCREEN_OFF_PROFILE;
            }
            // Sleep panjang biar masuk Doze
            std::this_thread::sleep_for(NORMAL_LOOP_INTERVAL);
            
            // WAKEUP SIGNAL: Saat bangun, paksa cek profile segera
            idle_battery_check_counter = 100; 
            continue;
        }

        // 3. GAME VALIDATION
        if (in_game_session && !active_package.empty()) {
             if (!pid_tracker.is_valid()) {
                LOGI("Game PID dead: %s", active_package.c_str());
                goto handle_game_exit;
             }
             
             bool still_visible = false;
             for(const auto& app : window_displays.recent_app) {
                 if(app.package_name == active_package) { still_visible = true; break; }
             }
             if (!still_visible) {
                LOGI("Game hidden: %s", active_package.c_str());
                goto handle_game_exit;
             }
        }
        
        // 4. DETECT NEW GAME
        if (active_package.empty()) {
            active_package = GetActiveGame(window_displays.recent_app);
            if (!active_package.empty()) {
                in_game_session = true;
            }
        }

        // ===========================
        // STATE: GAMING
        // ===========================
        if (!active_package.empty() && window_displays.screen_awake) {
            if (active_package != last_game_package) {
                LOGI("Enter Game: %s", active_package.c_str());
                
                // 1. Ambil state game SATU KALI saja
                auto active_game = game_registry.find_game(active_package); // Asumsi sudah fix jadi std::optional atau copy
                bool lite_mode = (active_game && active_game->lite_mode) || config_store.get_preferences().enforce_lite_mode;
                bool enable_dnd = (active_game && active_game->enable_dnd);

                ResolutionManager::GetInstance().ApplyGameMode(active_package);
                
                // 2. LOGIC BARU: Bypass charge HANYA jika bukan Lite Mode
                if (!lite_mode) {
                    BypassManager::GetInstance().SetBypass(true);
                    LOGI("Bypass Charge: ON (Full Performance)");
                } else {
                    // Pastikan mati jika masuk game dengan profil Lite
                    BypassManager::GetInstance().SetBypass(false);
                    LOGI("Bypass Charge: OFF (Lite Mode)");
                }
                
                if (enable_dnd) {
                    set_do_not_disturb(true);
                    dnd_enabled_by_us = true;
                    LOGI("DND Mode: ON");
                }

                last_game_package = active_package;
            }

            if (cur_mode != PERFORMANCE_PROFILE) {
                pid_t game_pid = Dumpsys::GetAppPID(active_package);
                if (game_pid > 0) {
                    cur_mode = PERFORMANCE_PROFILE;
                    
                    // Kita kueri ulang di sini aman karena sangat cepat (zero overhead)
                    auto active_game = game_registry.find_game(active_package);
                    bool lite_mode = (active_game && active_game->lite_mode) || config_store.get_preferences().enforce_lite_mode;
                    
                    apply_performance_profile(lite_mode, active_package, game_pid);
                    pid_tracker.set_pid(game_pid);
                    LOGI("Profile: Performance (PID: %d)", game_pid);
                }
            }
            continue; 
        }

        // ===========================
        // STATE: IDLE
        // ===========================
        if (!last_game_package.empty()) {
        handle_game_exit:
            LOGI("Exit Game: %s", last_game_package.c_str());
            ResolutionManager::GetInstance().ResetGameMode(last_game_package);
            BypassManager::GetInstance().SetBypass(false);
            
            if (dnd_enabled_by_us) {
                set_do_not_disturb(false);
                dnd_enabled_by_us = false;
                LOGI("DND Mode: OFF (Restored)");
            }
            
            last_game_package = "";
            active_package.clear();
            pid_tracker.invalidate();
            in_game_session = false;
            
            idle_battery_check_counter = 100; 
        }

        // Battery & Profile Check
        if (++idle_battery_check_counter >= 6) {
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
