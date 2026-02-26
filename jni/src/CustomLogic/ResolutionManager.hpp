#pragma once
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>

class ResolutionManager {
public:
    static ResolutionManager& GetInstance() {
        static ResolutionManager instance;
        return instance;
    }

    void ApplyGameMode(const std::string& packageName);
    void ResetGameMode(const std::string& packageName);

private:
    ResolutionManager() = default;
    std::unordered_map<std::string, std::string> appliedCache; 
    void ExecuteCmdDirect(const std::vector<const char*>& args);
};