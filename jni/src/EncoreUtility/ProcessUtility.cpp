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

#include <dirent.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "EncoreUtility.hpp"
#include <EncoreLog.hpp>

uid_t get_uid_by_package_name(const std::string &package_name) {
    struct stat st{};

    if (stat(("/data/data/" + package_name).c_str(), &st) != 0) {
        return 0;
    }

    return st.st_uid;
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
                cmdlineBuf[len] = '\0'; 
                
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

std::string GetFocusedPackage() {
    FILE* pipe = popen("/system/bin/dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'", "r");
    if (!pipe) return "";

    char buffer[512];
    std::string pkg = "";
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        size_t slash_pos = line.find("/");
        if (slash_pos != std::string::npos) {
            size_t space_pos = line.rfind(" ", slash_pos);
            if (space_pos != std::string::npos) {
                pkg = line.substr(space_pos + 1, slash_pos - space_pos - 1);
                
                pkg.erase(std::remove_if(pkg.begin(), pkg.end(), [](unsigned char c) {
                    return !(std::isalnum(c) || c == '.' || c == '_');
                }), pkg.end());
            }
        }
    }
    pclose(pipe);
    return pkg;
}