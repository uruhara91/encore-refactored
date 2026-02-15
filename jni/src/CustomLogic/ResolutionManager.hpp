#pragma once
#include <string>
#include <vector>
#include <utility>

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
    
    // Gunakan vector pair untuk iterasi cepat jika jumlah game sedikit (<100)
    // atau tetap std::map jika butuh lookup cepat. 
    // Untuk optimasi memori dan alokasi, vector<pair> lebih ramah cache processor daripada map node.
    std::vector<std::pair<std::string, std::string>> gameRatios;
    
    // Helper untuk mencari ratio di vector
    std::string GetRatio(const std::string& pkg);

    // Eksekusi langsung tanpa shell
    void ExecuteCmdDirect(const std::vector<const char*>& args);
};