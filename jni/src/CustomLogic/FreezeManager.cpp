#include "FreezeManager.hpp"
#include "EncoreLog.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <csignal>

// Helper: Read file to string (Optimized)
bool FreezeManager::ReadFileToString(const std::string& path, std::string& outContent) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        // ADDED: Log error if file cannot be opened
        LOGE("FreezeManager: Failed to open config at %s", path.c_str());
        return false;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) { 
        close(fd); 
        return false; 
    }
    
    size_t fileSize = static_cast<size_t>(sb.st_size);
    outContent.resize(fileSize);
    
    size_t totalRead = 0;
    while (totalRead < fileSize) {
        ssize_t bytes = read(fd, &outContent[totalRead], fileSize - totalRead);
        if (bytes <= 0) break;
        totalRead += bytes;
    }
    close(fd);
    return true;
}

void FreezeManager::LoadConfig(const std::string& configPath) {
    freezeList.clear();
    
    std::string content;
    if (!ReadFileToString(configPath, content)) return;

    const char* ptr = content.c_str();
    const char* end = ptr + content.length();
    
    while (ptr < end) {
        while (ptr < end && (*ptr == '\n' || *ptr == '\r' || *ptr == ' ' || *ptr == '\t')) ptr++;
        if (ptr >= end) break;

        const char* lineEnd = ptr;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') lineEnd++;

        if (*ptr != '#') {
            std::string line(ptr, lineEnd - ptr);
            if (!line.empty()) {
                freezeList.push_back(line);
            }
        }
        ptr = lineEnd;
    }
    LOGI("FreezeManager: Loaded %zu apps to freeze from %s", freezeList.size(), configPath.c_str());
}

void FreezeManager::ApplyFreeze(bool freeze) {
    if (freezeList.empty()) {
        LOGI("FreezeManager: List empty, skipping freeze/unfreeze");
        return;
    }
    int signal = freeze ? SIGSTOP : SIGCONT;
    
    LOGI("FreezeManager: Applying %s to %zu apps...", freeze ? "FREEZE" : "UNFREEZE", freezeList.size());

    for (const auto& pkg : freezeList) {
        SendSignalToPkg(pkg, signal);
    }
}

void FreezeManager::SendSignalToPkg(const std::string& targetPkg, int signal) {
    DIR* dir = opendir("/proc");
    if (!dir) return;

    struct dirent* ent;
    char cmdlinePath[64]; 
    char cmdlineBuf[256]; 
    
    bool found = false; // Flag to check if we found the app

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;

        snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%s/cmdline", ent->d_name);

        int fd = open(cmdlinePath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t len = read(fd, cmdlineBuf, sizeof(cmdlineBuf) - 1);
            close(fd);

            if (len > 0) {
                cmdlineBuf[len] = 0; 
                if (strcmp(cmdlineBuf, targetPkg.c_str()) == 0) {
                    int pid = atoi(ent->d_name);
                    if (kill(pid, signal) == 0) {
                         // LOGD("FreezeManager: Signal %d -> %d (%s)", signal, pid, targetPkg.c_str());
                         found = true;
                    }
                }
            }
        }
    }
    closedir(dir);
    
    if (!found && signal == SIGSTOP) {
        // Optional: Log if app not found during freeze (might be noisy)
        // LOGD("FreezeManager: App %s not running", targetPkg.c_str());
    }
}