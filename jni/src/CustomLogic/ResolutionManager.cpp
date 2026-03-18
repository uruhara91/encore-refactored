#include "ResolutionManager.hpp"
#include "EncoreLog.hpp"
#include "EncoreUtility.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

void ResolutionManager::ApplyGameMode(const std::string& packageName, const std::string& ratio) {
    if (ratio == "1.0" || ratio.empty()) {
        return; 
    }

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

void ResolutionManager::SyncGameModes(const std::vector<EncoreGameList>& current_games) {
    std::thread([this, current_games]() {
        pthread_setname_np(pthread_self(), "ResManagerSync");
        
        std::unordered_map<std::string, std::string> new_target_ratios;
        for (const auto& game : current_games) {
            if (game.downscale_ratio != "1.0" && !game.downscale_ratio.empty()) {
                new_target_ratios[game.package_name] = game.downscale_ratio;
            }
        }

        for (auto it = appliedCache.begin(); it != appliedCache.end(); ) {
            const std::string& pkg = it->first;
            if (!new_target_ratios.contains(pkg)) {
                std::string pkg_to_reset = pkg; 
                ResetGameMode(pkg_to_reset);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                it = appliedCache.begin(); 
            } else {
                ++it;
            }
        }

        for (const auto& [pkg, ratio] : new_target_ratios) {
            if (!appliedCache.contains(pkg) || appliedCache[pkg] != ratio) {
                ApplyGameMode(pkg, ratio);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }
        
        LOGI("ResolutionManager: Sync complete.");
    }).detach();
}