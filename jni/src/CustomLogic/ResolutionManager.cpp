#include "ResolutionManager.hpp"
#include "EncoreLog.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <vector>

void ResolutionManager::LoadGameMap(const std::string& configPath) {
    gameRatios.clear();
    
    int fd = open(configPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        // ADDED: Log error
        LOGE("ResolutionManager: Failed to open config at %s", configPath.c_str());
        return;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) { close(fd); return; }
    
    std::string content(sb.st_size, '\0');
    read(fd, &content[0], sb.st_size);
    close(fd);

    const char* ptr = content.c_str();
    const char* end = ptr + content.length();

    while (ptr < end) {
        while (ptr < end && (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n')) ptr++;
        if (ptr >= end) break;
        if (*ptr == '#') { 
             while (ptr < end && *ptr != '\n') ptr++;
             continue;
        }

        const char* lineStart = ptr;
        while (ptr < end && *ptr != '\n') ptr++;
        std::string line(lineStart, ptr - lineStart);

        size_t delPos = line.find(':');
        if (delPos != std::string::npos) {
            std::string pkg = line.substr(0, delPos);
            std::string ratio = line.substr(delPos + 1);
            
            // Trim whitespace logic (Manual implementation for speed)
            // Trim right
            pkg.erase(pkg.find_last_not_of(" \t\r\n") + 1);
            ratio.erase(ratio.find_last_not_of(" \t\r\n") + 1);
            // Trim left
            pkg.erase(0, pkg.find_first_not_of(" \t\r\n"));
            ratio.erase(0, ratio.find_first_not_of(" \t\r\n"));

            gameRatios.push_back({pkg, ratio});
        }
    }
    LOGI("ResolutionManager: Loaded %zu configs from %s", gameRatios.size(), configPath.c_str());
}

std::string ResolutionManager::GetRatio(const std::string& pkg) {
    for (const auto& item : gameRatios) {
        if (item.first == pkg) return item.second;
    }
    return "";
}

void ResolutionManager::ApplyGameMode(const std::string& packageName) {
    std::string ratio = GetRatio(packageName);
    if (!ratio.empty()) {
        std::vector<const char*> args = {
            "/system/bin/cmd", 
            "game", 
            "mode", 
            "set", 
            "--downscale", 
            ratio.c_str(), 
            packageName.c_str(), 
            NULL 
        };
        ExecuteCmdDirect(args);
        LOGI("ResolutionManager: Applied %s to %s", ratio.c_str(), packageName.c_str());
    } else {
        // Optional: Log if game not in config
        // LOGD("ResolutionManager: No config for %s", packageName.c_str());
    }
}

void ResolutionManager::ResetGameMode(const std::string& packageName) {
    std::vector<const char*> args = {
        "/system/bin/cmd", 
        "game", 
        "mode", 
        "set", 
        "standard", 
        packageName.c_str(), 
        NULL 
    };
    ExecuteCmdDirect(args);
    LOGD("ResolutionManager: Reset %s", packageName.c_str());
}

void ResolutionManager::ExecuteCmdDirect(const std::vector<const char*>& args) {
    pid_t pid = fork();
    
    if (pid == -1) {
        LOGE("ResolutionManager: Fork failed");
        return;
    }
    
    if (pid == 0) {
        int devNull = open("/dev/null", O_RDWR);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        close(devNull);

        execv(args[0], const_cast<char* const*>(args.data()));
        _exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
}