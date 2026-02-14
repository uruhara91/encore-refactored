#include "BypassManager.hpp"
#include <filesystem> // C++17 filesystem

void BypassManager::Init() {
    if (std::filesystem::exists(PATH_CMD)) {
        targetPath = PATH_CMD;
        mode = 0;
        LOGI("BypassManager: Detected MTK current_cmd interface");
    } else if (std::filesystem::exists(PATH_EN)) {
        targetPath = PATH_EN;
        mode = 1;
        LOGI("BypassManager: Detected MTK en_power_path interface");
    } else {
        LOGE("BypassManager: No supported bypass interface found");
    }
}

void BypassManager::SetBypass(bool enable) {
    if (targetPath.empty()) return;

    std::ofstream file(targetPath);
    if (!file.is_open()) {
        LOGE("BypassManager: Failed to open %s", targetPath.c_str());
        return;
    }

    if (mode == 0) {
        // Format logic Anda: "0 1" on, "0 0" off
        file << (enable ? "0 1" : "0 0");
    } else {
        file << (enable ? "1" : "0"); 
    }
    
    LOGD("BypassManager: Set to %s", enable ? "ON" : "OFF");
}