#pragma once
#include <vector>
#include <string>
#include <unordered_set>

class FreezeManager {
public:
    static FreezeManager& GetInstance() {
        static FreezeManager instance;
        return instance;
    }

    void LoadConfig(const std::string& configPath);
    void ApplyFreeze(bool freeze); // true = Freeze, false = Unfreeze

private:
    FreezeManager() = default;
    std::vector<std::string> freezeList;
    
    // Helper untuk mencari PID dari nama package
    std::vector<int> GetPidsByPackageName(const std::string& packageName);
    void SendSignalToPkg(const std::string& pkg, int signal);
};