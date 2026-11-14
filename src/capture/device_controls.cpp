#include "device_controls.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace {

int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

// The device file, opened only for the call. V4L2 allows controls to be read
// and set while another process streams from the same device.
struct Device {
    int fd = -1;
    explicit Device(const std::string &path) : fd(path.empty() ? -1 : open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC)) {}
    ~Device() {
        if (fd >= 0) close(fd);
    }
};

}  // namespace

std::vector<DeviceControl> listDeviceControls(const std::string &devicePath) {
    std::vector<DeviceControl> out;
    Device dev(devicePath);
    if (dev.fd < 0) return out;
    v4l2_queryctrl q{};
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (xioctl(dev.fd, VIDIOC_QUERYCTRL, &q) == 0) {
        const uint32_t id = q.id;
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
        if (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) continue;
        DeviceControl c;
        c.id = id;
        c.name = reinterpret_cast<const char *>(q.name);
        c.minimum = q.minimum;
        c.maximum = q.maximum;
        c.step = q.step > 0 ? q.step : 1;
        c.defaultValue = q.default_value;
        c.inactive = q.flags & V4L2_CTRL_FLAG_INACTIVE;
        switch (q.type) {
        case V4L2_CTRL_TYPE_INTEGER: c.type = DeviceControl::Type::Integer; break;
        case V4L2_CTRL_TYPE_BOOLEAN: c.type = DeviceControl::Type::Boolean; break;
        case V4L2_CTRL_TYPE_MENU:
        case V4L2_CTRL_TYPE_INTEGER_MENU: {
            c.type = DeviceControl::Type::Menu;
            // A driver's range can run to INT_MAX: i never wraps, and no more
            // than a menu could hold are asked for.
            for (int64_t i = std::max<int64_t>(0, q.minimum); i <= q.maximum && i - q.minimum < 256; ++i) {
                v4l2_querymenu m{};
                m.id = id;
                m.index = uint32_t(i);
                if (xioctl(dev.fd, VIDIOC_QUERYMENU, &m) != 0) continue;
                c.menu.emplace_back(int(i), q.type == V4L2_CTRL_TYPE_MENU ? std::string(reinterpret_cast<const char *>(m.name))
                                                                     : std::to_string(m.value));
            }
            break;
        }
        default: continue;  // buttons, strings, compound controls: not picture settings
        }
        v4l2_control v{};
        v.id = id;
        if (xioctl(dev.fd, VIDIOC_G_CTRL, &v) == 0) c.value = v.value;
        else c.value = c.defaultValue;
        out.push_back(std::move(c));
    }
    return out;
}

bool setDeviceControl(const std::string &devicePath, uint32_t id, int value) {
    Device dev(devicePath);
    if (dev.fd < 0) return false;
    v4l2_control v{};
    v.id = id;
    v.value = value;
    return xioctl(dev.fd, VIDIOC_S_CTRL, &v) == 0;
}

int applyDeviceControls(const std::string &devicePath, const std::map<uint32_t, int> &values) {
    Device dev(devicePath);
    if (dev.fd < 0) return 0;
    // Twice: a manual value (exposure, focus, white balance) is refused while
    // its automatic switch is on, and the switch's id may sort either side of
    // it; the second pass sets what the first pass made settable.
    int done = 0;
    for (int pass = 0; pass < 2; ++pass) {
        done = 0;
        for (const auto &[id, value] : values) {
            v4l2_control v{};
            v.id = id;
            v.value = value;
            if (xioctl(dev.fd, VIDIOC_S_CTRL, &v) == 0) ++done;
        }
    }
    return done;
}
