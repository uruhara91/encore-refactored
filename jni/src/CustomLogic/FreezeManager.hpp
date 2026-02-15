#pragma once
#include <vector>
#include <string>

class FreezeManager {
public:
    static FreezeManager& GetInstance() {
        static FreezeManager instance;
        return instance;
    }

    void LoadConfig(const std::string& configPath);
    void ApplyFreeze(bool freeze); 

private:
    FreezeManager() = default;
    std::vector<std::string> freezeList;
    
    // Helper internal
    void SendSignalToPkg(const std::string& pkg, int signal);
    
    // Fungsi utilitas cepat untuk membaca seluruh file ke string (mengurangi syscall read berulang)
    bool ReadFileToString(const std::string& path, std::string& outContent);
};