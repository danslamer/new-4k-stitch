#ifndef ROI_CONFIG_H
#define ROI_CONFIG_H

#include <string>

struct RoiOffset {
    int offset_x = 0;
    int offset_y = 0;
    RoiOffset() = default;
    RoiOffset(int x, int y) : offset_x(x), offset_y(y) {}
};

struct StitchGlobalConfig {
    RoiOffset roi_offsets[4];
    
    int feather_width = 120;
    double feather_strength = 2.0;
    bool feather_enabled = true;
    
    bool save_enabled = false;
    int save_interval = 30;
    
    int step_size = 1;
    std::string mode = "dataset";
    int selected_cam = 1;
};

class RoiConfig {
public:
    static bool LoadFromFile(const std::string& path, StitchGlobalConfig& config);
    static bool SaveToFile(const std::string& path, const StitchGlobalConfig& config);
};

#endif