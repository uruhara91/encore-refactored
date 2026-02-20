#include "BypassManager.hpp"
#include "EncoreLog.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string_view>

void BypassManager::Init() {
    // FIX: Gunakan .c_str() untuk kompatibilitas dengan access()
    if (access(PATH_CMD.c_str(), F_OK) == 0) {
        targetPath = PATH_CMD;
        mode = 0;
        LOGI("BypassManager: Detected MTK current_cmd interface");
    } else if (access(PATH_EN.c_str(), F_OK) == 0) {
        targetPath = PATH_EN;
        mode = 1;
        LOGI("BypassManager: Detected MTK en_power_path interface");
    } else {
        LOGE("BypassManager: No supported bypass interface found");
    }
}

void BypassManager::SetBypass(bool enable) {
    if (targetPath.empty()) return;

    int fd = open(targetPath.c_str(), O_WRONLY | O_CLOEXEC);
    
    if (fd == -1) {
        LOGE("BypassManager: Failed to open %s", targetPath.c_str());
        return;
    }

    std::string_view val;
    if (mode == 0) {
        // Mode current_cmd
        val = enable ? "0 1" : "0 0";
    } else {
        // Mode en_power_path
        val = enable ? "1" : "0";
    }

    // val.data() mengambil pointer char, val.size() mengambil ukuran O(1)
    ssize_t bytesWritten = write(fd, val.data(), val.size());
    
    if (bytesWritten == -1) {
        LOGE("BypassManager: Failed to write to %s", targetPath.c_str());
    } else {
        LOGD("BypassManager: Set to %s", enable ? "ON" : "OFF");
    }

    // Selalu tutup file descriptor
    close(fd);
}