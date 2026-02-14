#pragma once
#include <string>
#include <map>

class ResolutionManager {
public:
    static ResolutionManager& GetInstance() {
        static ResolutionManager instance;
        return instance;
    }

    void LoadGameMap(const std::string& configPath);
    void ApplyGameMode(const std::string& packageName);
    void ResetGameMode(const std::string& packageName);

private:
    ResolutionManager() = default;
    // Map pkg -> ratio string (e.g., "0.7")
    std::map<std::string, std::string> gameRatios; 
    
    void ExecuteCmd(const std::string& cmd);
};