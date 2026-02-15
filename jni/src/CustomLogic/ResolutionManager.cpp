#include "ResolutionManager.hpp"
#include "EncoreLog.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>

void ResolutionManager::LoadGameMap(const std::string& configPath) {
    gameRatios.clear();
    
    // Gunakan metode baca raw yang sama (bisa dipindah ke util class sebenarnya)
    int fd = open(configPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return;

    struct stat sb;
    if (fstat(fd, &sb) == -1) { close(fd); return; }
    
    std::string content(sb.st_size, '\0');
    read(fd, &content[0], sb.st_size);
    close(fd);

    const char* ptr = content.c_str();
    const char* end = ptr + content.length();

    // Parser manual ultra cepat
    while (ptr < end) {
        while (ptr < end && isspace(*ptr)) ptr++;
        if (ptr >= end) break;
        if (*ptr == '#') { // Skip comment line
             while (ptr < end && *ptr != '\n') ptr++;
             continue;
        }

        const char* lineStart = ptr;
        while (ptr < end && *ptr != '\n') ptr++;
        std::string line(lineStart, ptr - lineStart);

        // Cari delimiter ':'
        size_t delPos = line.find(':');
        if (delPos != std::string::npos) {
            std::string pkg = line.substr(0, delPos);
            std::string ratio = line.substr(delPos + 1);
            
            // Trim whitespace manual
            // (Implementasi trim sederhana)
            // ... (bisa ditambahkan jika perlu, asumsi config rapi)
            
            gameRatios.push_back({pkg, ratio});
        }
    }
    LOGI("ResolutionManager: Loaded %zu configs", gameRatios.size());
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
        // Argumen untuk execv harus null-terminated array of char*
        // cmd game mode set --downscale <ratio> <pkg>
        std::vector<const char*> args = {
            "/system/bin/cmd", 
            "game", 
            "mode", 
            "set", 
            "--downscale", 
            ratio.c_str(), 
            packageName.c_str(), 
            NULL // Sentinel wajib
        };
        ExecuteCmdDirect(args);
        LOGI("ResolutionManager: Applied %s to %s", ratio.c_str(), packageName.c_str());
    }
}

void ResolutionManager::ResetGameMode(const std::string& packageName) {
    // cmd game mode set standard <pkg>
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

// INI JANTUNG OPTIMASINYA
void ResolutionManager::ExecuteCmdDirect(const std::vector<const char*>& args) {
    pid_t pid = fork();
    
    if (pid == -1) {
        LOGE("ResolutionManager: Fork failed");
        return;
    }
    
    if (pid == 0) {
        // --- CHILD PROCESS ---
        // Redirect stdout/stderr ke /dev/null agar tidak mengotori logcat atau buffer
        int devNull = open("/dev/null", O_RDWR);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        close(devNull);

        // Eksekusi langsung binary tanpa shell
        // const_cast aman di sini karena execv tidak memodifikasi arg
        execv(args[0], const_cast<char* const*>(args.data()));
        
        // Jika sampai sini, berarti execv gagal
        _exit(127);
    } else {
        // --- PARENT PROCESS ---
        // Tunggu child selesai agar tidak jadi zombie process
        // WNOHANG agar tidak nge-block thread utama jika child hang (opsional)
        // Tapi biasanya cmd cepat, jadi waitpid biasa aman.
        int status;
        waitpid(pid, &status, 0);
    }
}