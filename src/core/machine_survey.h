#pragma once

#include <QByteArray>

#include <vector>

#include "core/presets.h"

// What the presets need to know about this machine that the encoder probe
// does not tell: its displays, memory, processor and power. Each reader has
// a parser beside it, so the parsing is checked without the machine.
namespace machinesurvey {

// Hyprland's `hyprctl -j monitors`: pixel sizes as captured (a rotated
// display is taller than wide) and refresh rates, enabled ones only.
std::vector<presets::Display> parseHyprlandMonitors(const QByteArray &json);
// The displays: Hyprland's list when it answers, else Qt's screens.
std::vector<presets::Display> displays();

// /proc/meminfo: MemTotal and MemAvailable, in bytes.
void parseMemInfo(const QByteArray &text, qint64 *total, qint64 *available);

struct Power {
    bool battery = false;     // there is one
    bool onBattery = false;   // and nothing is charging it
};
// /sys/class/power_supply, or another folder laid out like it: the one
// given, else OMABROADCAST_POWER_DIR when set.
Power readPower(const QString &folder = {});

// Displays, memory, processor and power filled in; the encoders, speeds and
// upload are the caller's.
void fill(presets::Survey *survey);

}  // namespace machinesurvey
