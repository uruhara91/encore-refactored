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

void FreezeManager::LoadConfig(const std::string& configPath) {
    freezeList.clear();
    
    std::string content;
    if (!ReadFileToString(configPath, content)) return;

    // Parsing manual tanpa stringstream untuk performa
    const char* ptr = content.c_str();
    const char* end = ptr + content.length();
    
    while (ptr < end) {
        // Lewati whitespace/newline di awal
        while (ptr < end && (*ptr == '\n' || *ptr == '\r' || *ptr == ' ' || *ptr == '\t')) ptr++;
        if (ptr >= end) break;

        // Cari akhir baris
        const char* lineEnd = ptr;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') lineEnd++;

        // Abaikan komentar '#'
        if (*ptr != '#') {
            std::string line(ptr, lineEnd - ptr);
            if (!line.empty()) {
                freezeList.push_back(line);
            }
        }
        ptr = lineEnd;
    }
    LOGI("FreezeManager: Loaded %zu apps to freeze", freezeList.size());
}

void FreezeManager::ApplyFreeze(bool freeze) {
    if (freezeList.empty()) return;
    int signal = freeze ? SIGSTOP : SIGCONT; // SIGSTOP (19), SIGCONT (18)
    
    // Optimasi: Tidak perlu log "Starting..." jika list kosong, 
    // tapi karena kita sudah cek empty, aman.
    
    for (const auto& pkg : freezeList) {
        SendSignalToPkg(pkg, signal);
    }
}

// Implementasi SendSignalToPkg yang menyatu dengan pencarian PID
// Ini menghindari iterasi /proc berulang kali (sekali iterasi /proc untuk semua target kalau mau ultra optimal, 
// tapi untuk menjaga struktur kode tetap rapi, kita optimalkan loop-nya saja).
void FreezeManager::SendSignalToPkg(const std::string& targetPkg, int signal) {
    DIR* dir = opendir("/proc");
    if (!dir) return;

    struct dirent* ent;
    char cmdlinePath[64]; // Buffer stack (cepat)
    char cmdlineBuf[256]; // Buffer untuk isi cmdline
    
    while ((ent = readdir(dir)) != NULL) {
        // Filter cepat: pastikan digit (PID)
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;

        // Bangun path string secara manual (hindari std::string alloc di loop)
        snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%s/cmdline", ent->d_name);

        int fd = open(cmdlinePath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t len = read(fd, cmdlineBuf, sizeof(cmdlineBuf) - 1);
            close(fd);

            if (len > 0) {
                cmdlineBuf[len] = 0; // Null terminate
                // Cek apakah cmdline == targetPkg
                // cmdline di Android dipisahkan null, arg pertama adalah package name
                if (strcmp(cmdlineBuf, targetPkg.c_str()) == 0) {
                    int pid = atoi(ent->d_name);
                    if (kill(pid, signal) == 0) {
                         // Optional debug log, matikan untuk production extreme performance
                         // LOGD("FreezeManager: Signal %d -> %d (%s)", signal, pid, targetPkg.c_str());
                    }
                }
            }
        }
    }
    closedir(dir);
}

bool FreezeManager::ReadFileToString(const std::string& path, std::string& outContent) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;

    // Ambil ukuran file untuk reservasi string
    struct stat sb;
    if (fstat(fd, &sb) == -1) { close(fd); return false; }
    
    outContent.resize(sb.st_size);
    
    // ReadAll loop
    size_t totalRead = 0;
    while (totalRead < sb.st_size) {
        ssize_t bytes = read(fd, &outContent[totalRead], sb.st_size - totalRead);
        if (bytes <= 0) break;
        totalRead += bytes;
    }
    close(fd);
    return true;
}