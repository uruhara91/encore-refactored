#include "FreezeManager.hpp"
#include "EncoreLog.hpp"
#include <fstream>
#include <dirent.h>
#include <signal.h>
#include <cstring>

void FreezeManager::LoadConfig(const std::string& configPath) {
    freezeList.clear();
    std::ifstream file(configPath);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line[0] != '#') {
            // Trim newline chars logic if needed
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            freezeList.push_back(line);
        }
    }
    LOGI("FreezeManager: Loaded %zu apps to freeze", freezeList.size());
}

void FreezeManager::ApplyFreeze(bool freeze) {
    int signal = freeze ? SIGSTOP : SIGCONT;
    for (const auto& pkg : freezeList) {
        SendSignalToPkg(pkg, signal);
    }
    LOGD("FreezeManager: %s operation completed", freeze ? "Freeze" : "Unfreeze");
}

// Logic efisien scanning /proc tanpa panggil 'pidof' shell command
std::vector<int> FreezeManager::GetPidsByPackageName(const std::string& packageName) {
    std::vector<int> pids;
    DIR* dir = opendir("/proc");
    if (!dir) return pids;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit(*ent->d_name)) continue;

        int pid = atoi(ent->d_name);
        std::string cmdPath = "/proc/" + std::string(ent->d_name) + "/cmdline";
        std::ifstream cmdFile(cmdPath);
        std::string cmdline;
        
        if (std::getline(cmdFile, cmdline, '\0')) { // cmdline is null terminated
            if (cmdline == packageName) {
                pids.push_back(pid);
            }
        }
    }
    closedir(dir);
    return pids;
}

void FreezeManager::SendSignalToPkg(const std::string& pkg, int signal) {
    std::vector<int> pids = GetPidsByPackageName(pkg);
    for (int pid : pids) {
        kill(pid, signal);
    }
}