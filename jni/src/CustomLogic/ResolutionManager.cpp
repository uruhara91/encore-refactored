#include "ResolutionManager.hpp"
#include "EncoreLog.hpp"
#include <GameRegistry.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

void ResolutionManager::ApplyGameMode(const std::string& packageName) {
    auto game = game_registry.find_game(packageName);
    if (!game || game->downscale_ratio == "1.0" || game->downscale_ratio.empty()) {
        return;
    }

    std::string ratio = game->downscale_ratio;

    if (appliedCache.contains(packageName) && appliedCache[packageName] == ratio) return;

    std::vector<const char*> args = {
        "/system/bin/cmd", "game", "mode", "set", "--downscale", 
        ratio.c_str(), packageName.c_str(), NULL 
    };
    ExecuteCmdDirect(args);
    
    appliedCache[packageName] = ratio;
    LOGI("ResolutionManager: Applied %s to %s", ratio.c_str(), packageName.c_str());
}

void ResolutionManager::ResetGameMode(const std::string& packageName) {
    if (!appliedCache.contains(packageName)) return;

    std::vector<const char*> args = {
        "/system/bin/cmd", "game", "mode", "set", "standard", 
        packageName.c_str(), NULL 
    };
    ExecuteCmdDirect(args);
    
    appliedCache.erase(packageName);
    LOGD("ResolutionManager: Reset %s", packageName.c_str());
}

void ResolutionManager::ExecuteCmdDirect(const std::vector<const char*>& args) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            int devNull = open("/dev/null", O_RDWR);
            if (devNull >= 0) {
                dup2(devNull, STDOUT_FILENO);
                dup2(devNull, STDERR_FILENO);
                close(devNull);
            }
            execv(args[0], const_cast<char* const*>(args.data()));
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}