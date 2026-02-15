/*
 * Copyright (C) 2024-2026 Rem01Gaming
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

#include "Dumpsys.hpp"
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <string_view>
#include <algorithm>

namespace Dumpsys {

void WindowDisplays(DumpsysWindowDisplays &result) {
    result.screen_awake = false;
    result.recent_app.clear();
    // Optimasi memori
    result.recent_app.reserve(10); 

    auto pipe = popen_direct({"/system/bin/dumpsys", "window", "visible-apps"});
    if (!pipe.stream) return;

    char buffer[4096]; 
    
    bool found_task_section = false;
    bool exited_task_section = false;
    bool found_awake = false;
    
    constexpr std::string_view KEY_AWAKE = "mAwake=";
    constexpr std::string_view KEY_AWAKE_TRUE = "mAwake=true";
    constexpr std::string_view KEY_TASK_START = "Application tokens in top down Z order:";
    constexpr std::string_view KEY_TASK_HEADER = "* Task{";
    constexpr std::string_view KEY_ACT_RECORD = "* ActivityRecord{";
    constexpr std::string_view KEY_VISIBLE_TRUE = "visible=true";
    constexpr std::string_view KEY_TYPE_STANDARD = "type=standard";

    std::string_view current_task_line_view;
    char saved_task_line[1024];

    while (fgets(buffer, sizeof(buffer), pipe.stream) != nullptr) {
        std::string_view line(buffer);
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);

        if (exited_task_section && found_awake) break;

        if (!found_awake && line.find(KEY_AWAKE) != std::string_view::npos) {
            result.screen_awake = line.find(KEY_AWAKE_TRUE) != std::string_view::npos;
            found_awake = true;
            continue;
        }

        if (!found_task_section) {
            if (line.find(KEY_TASK_START) != std::string_view::npos) {
                found_task_section = true;
                continue;
            }
        }

        if (!found_task_section || exited_task_section) continue;

        if (line.empty()) {
            exited_task_section = true;
            continue;
        }

        if (line.find(KEY_TASK_HEADER) != std::string_view::npos) {
            if (line.find(KEY_TYPE_STANDARD) != std::string_view::npos) {
                // std::min butuh <algorithm>
                size_t copy_len = std::min(line.size(), sizeof(saved_task_line) - 1);
                memcpy(saved_task_line, line.data(), copy_len);
                saved_task_line[copy_len] = '\0';
                current_task_line_view = std::string_view(saved_task_line, copy_len);
            } else {
                current_task_line_view = {};
            }
        }
        else if (!current_task_line_view.empty() && line.find(KEY_ACT_RECORD) != std::string_view::npos) {
            RecentAppList app;
            app.visible = current_task_line_view.find(KEY_VISIBLE_TRUE) != std::string_view::npos;

            size_t u0_pos = line.find(" u0 ");
            if (u0_pos != std::string_view::npos) {
                size_t pkg_start = u0_pos + 4;
                size_t slash_pos = line.find('/', pkg_start);
                
                if (slash_pos != std::string_view::npos) {
                    app.package_name = std::string(line.substr(pkg_start, slash_pos - pkg_start));
                    result.recent_app.push_back(std::move(app));
                }
            }
            current_task_line_view = {}; 
        }
    }
}

void Power(DumpsysPower &result) {
    // Clear previous results
    result.screen_awake = false;
    result.is_plugged = false;
    result.battery_saver = false;
    result.battery_saver_sticky = false;

    auto pipe = popen_direct({"/system/bin/dumpsys", "power"});

    if (!pipe.stream) {
        std::string error_msg = "popen failed: ";
        error_msg += strerror(errno);
        throw std::runtime_error(error_msg);
    }

    char buffer[1024];
    bool found_wakefulness = false;
    bool found_is_plugged = false;
    bool found_battery_saver = false;
    bool found_battery_saver_sticky = false;

    while (fgets(buffer, sizeof(buffer), pipe.stream) != nullptr) {
        std::string line(buffer);

        // We've got all information needed, do not process any further
        if (found_wakefulness && found_is_plugged && found_battery_saver && found_battery_saver_sticky) {
            break;
        }

        // Remove trailing newline
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }

        if (!found_wakefulness && line.find("mWakefulness=") != std::string::npos) {
            result.screen_awake = line.find("mWakefulness=Awake") != std::string::npos;
            found_wakefulness = true;
            continue;
        }

        if (!found_is_plugged && line.find("mIsPowered=") != std::string::npos) {
            result.is_plugged = line.find("mIsPowered=true") != std::string::npos;
            found_is_plugged = true;
            continue;
        }

        if (!found_battery_saver && line.find("mSettingBatterySaverEnabled=") != std::string::npos) {
            result.battery_saver = line.find("mSettingBatterySaverEnabled=true") != std::string::npos;
            found_battery_saver = true;
            continue;
        }

        if (!found_battery_saver_sticky && line.find("mSettingBatterySaverEnabledSticky=") != std::string::npos) {
            result.battery_saver_sticky = line.find("mSettingBatterySaverEnabledSticky=true") != std::string::npos;
            found_battery_saver_sticky = true;
            continue;
        }
    }

    // Handle missing information
    if (!found_wakefulness) {
        throw std::runtime_error("unable to find wakefulness state");
    }

    if (!found_is_plugged) {
        throw std::runtime_error("unable to find charging state");
    }

    if (!found_battery_saver) {
        throw std::runtime_error("unable to find battery saver state");
    }

    if (!found_battery_saver_sticky) {
        throw std::runtime_error("unable to find battery saver sticky state");
    }
}

pid_t GetAppPID(const std::string &package_name) {
    DIR* dir = opendir("/proc");
    if (!dir) return 0;

    struct dirent* ent;
    pid_t found_pid = 0;
    char cmdline_path[64];
    char cmdline_buf[256];

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;

        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", ent->d_name);
        int fd = open(cmdline_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t len = read(fd, cmdline_buf, sizeof(cmdline_buf) - 1);
            close(fd);
            if (len > 0) {
                cmdline_buf[len] = 0;
                if (strcmp(cmdline_buf, package_name.c_str()) == 0) {
                    found_pid = atoi(ent->d_name);
                    break;
                }
            }
        }
    }
    closedir(dir);
    return found_pid;
}

} // namespace Dumpsys