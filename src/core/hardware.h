#pragma once

#include <dirent.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// What the capture and output code both ask of this machine's hardware.
namespace hardware {

// The graphics cards' render nodes, /dev/dri/renderD128 first; renderD1000
// after renderD999.
inline std::vector<std::string> renderNodes() {
    std::vector<std::string> nodes;
    if (DIR *dir = opendir("/dev/dri")) {
        while (dirent *e = readdir(dir)) {
            const std::string n = e->d_name;
            if (n.rfind("renderD", 0) == 0) nodes.push_back("/dev/dri/" + n);
        }
        closedir(dir);
    }
    std::sort(nodes.begin(), nodes.end(), [](const std::string &a, const std::string &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return nodes;
}

// JPEG decoders to share pictures of this size between (one decoder uses one
// core): above 1080p a quarter of the cores, 2 to 4 of them.
inline int jpegDecoders(int64_t width, int64_t height) {
    if (width * height <= 1920 * 1080) return 1;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return int(std::clamp(cores / 4, 2u, 4u));
}

}  // namespace hardware
