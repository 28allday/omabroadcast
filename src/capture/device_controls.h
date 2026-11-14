#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// A capture device's own picture controls (brightness, contrast, exposure,
// white balance, …) read and set through V4L2 on its device file. Which
// controls exist is the device's business: they are listed, never assumed.
struct DeviceControl {
    enum class Type { Integer, Boolean, Menu };
    uint32_t id = 0;
    std::string name;
    Type type = Type::Integer;
    int minimum = 0, maximum = 0, step = 1, defaultValue = 0, value = 0;
    std::vector<std::pair<int, std::string>> menu;   // value, label
    bool inactive = false;   // e.g. exposure while exposure is automatic
};

std::vector<DeviceControl> listDeviceControls(const std::string &devicePath);
bool setDeviceControl(const std::string &devicePath, uint32_t id, int value);
// Applies saved values (control id → value); returns how many took.
int applyDeviceControls(const std::string &devicePath, const std::map<uint32_t, int> &values);
