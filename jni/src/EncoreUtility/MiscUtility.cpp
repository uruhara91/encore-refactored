/*
 * Copyright (C) 2024-2025 Rem01Gaming
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

#include "EncoreUtility.hpp"
#include <ModuleProperty.hpp>
#include <ShellUtility.hpp>

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
    if (cpu_governor_paths.empty()) return;

    for (const auto& path : cpu_governor_paths) {
        int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            write(fd, governor.c_str(), governor.length());
            close(fd);
        }
    }
}

bool IsCharging() {
    int fd = open("/sys/class/power_supply/battery/status", O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;

    char buf[16];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);

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

static void run_async_cmd(const char* args[]) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execvp(args[0], (char *const *)args);
            _exit(127);
        }
        _exit(0); 
    } else if (pid > 0) {
        waitpid(pid, NULL, 0); 
    }
}

void set_do_not_disturb(bool do_not_disturb) {
    const char *args[] = {"/system/bin/cmd", "notification", "set_dnd", do_not_disturb ? "priority" : "off", NULL};
    run_async_cmd(args);
}

void notify(const char *message) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull);
            }
            if (setgid(2000) != 0 || setuid(2000) != 0) { _exit(126); }
            
            const char *args[] = {"/system/bin/cmd", "notification", "post", "-t", NOTIFY_TITLE, LOG_TAG, message, NULL};
            execvp(args[0], (char *const *)args);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

void is_kanged(void) {
    std::vector<ModuleProperties> module_properties;

    try {
        ModuleProperty::Get(MODULE_PROP, module_properties);

        for (const auto &property : module_properties) {
            if (property.key == "name" && property.value != "Encore Tweaks") {
                goto doorprize;
            }

            if (property.key == "author" && property.value != "Rem01Gaming") {
                goto doorprize;
            }
        }
    } catch (const std::exception &e) {
        LOGE_TAG("ModuleProperty", "{}", e.what());
    }

    return;

doorprize:
    LOGC("Module modified by 3rd party, exiting");
    notify("Trying to rename me?");
    exit(EXIT_FAILURE);
}
