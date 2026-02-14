#include "ResolutionManager.hpp"
#include "EncoreLog.hpp"
#include <fstream>
#include <sstream>
#include <stdlib.h>

void ResolutionManager::LoadGameMap(const std::string& configPath) {
    gameRatios.clear();
    std::ifstream file(configPath);
    std::string line;
    
    // Format: com.mobile.legends:0.7
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string segment;
        std::vector<std::string> seglist;
        
        while(std::getline(ss, segment, ':')) {
            seglist.push_back(segment);
        }

        if (seglist.size() == 2) {
            // Trim logic here recommended
            gameRatios[seglist[0]] = seglist[1];
        }
    }
    LOGI("ResolutionManager: Loaded %zu game configs", gameRatios.size());
}

void ResolutionManager::ApplyGameMode(const std::string& packageName) {
    if (gameRatios.find(packageName) != gameRatios.end()) {
        std::string ratio = gameRatios[packageName];
        // Logic Anda: cmd game mode set --downscale [ratio] [pkg]
        std::string cmd = "cmd game mode set --downscale " + ratio + " " + packageName;
        ExecuteCmd(cmd);
        LOGI("ResolutionManager: Applied Downscale %s for %s", ratio.c_str(), packageName.c_str());
    } else {
        // Default behavior if game not in list but detected as game?
        // Optional: Apply standard mode
        // ExecuteCmd("cmd game mode set standard " + packageName);
    }
}

void ResolutionManager::ResetGameMode(const std::string& packageName) {
    // Reset ke standard saat keluar game
    std::string cmd = "cmd game mode set standard " + packageName;
    ExecuteCmd(cmd);
    LOGD("ResolutionManager: Reset to Standard for %s", packageName.c_str());
}

void ResolutionManager::ExecuteCmd(const std::string& cmd) {
    system(cmd.c_str());
}