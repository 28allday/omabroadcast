#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "core/profile.h"
#include "output/encoders.h"

// Ready-made set-ups: what someone wants to do, fitted to what this machine
// measured. plan() is pure (a survey and the choices in; a profile, starter
// studios and the reasons for each setting out), so it can be checked on
// any machine. Nothing here knows any GPU maker: encoders
// are ranked by what they are and how fast they ran here.
namespace presets {

enum class Scenario { Stream, Record, Tutorial, Edit, Replay, Camera, Light };

QString scenarioId(Scenario scenario);              // "stream", …
bool scenarioFromId(const QString &id, Scenario *scenario);
QString scenarioName(Scenario scenario);            // for people
QString scenarioBlurb(Scenario scenario);           // one line, for people
std::vector<Scenario> scenarios();                  // in the order offered

struct Display {
    QString name;            // the connector, "DP-1"
    QString description;     // the monitor's own name
    int width = 0, height = 0;   // pixels, as captured (rotation applied)
    double refresh = 60;     // Hz
};

// How fast an encoder ran here at one size and rate (frames per second,
// open time left out); opened false when it would not open at all.
struct EncoderSpeed {
    std::string key;         // encoderKey()
    int width = 0, height = 0;
    int fps = 60;
    bool opened = false;
    double framesPerSecond = 0;
};

struct Survey {
    std::vector<Display> displays;
    std::vector<EncoderInfo> encoders;      // as probed
    EngineDevice engine;                    // what composites
    std::vector<EncoderSpeed> speeds;       // as measured
    qint64 memoryBytes = 0;                 // installed
    qint64 availableMemoryBytes = 0;        // free for us now
    int cpuThreads = 0;
    bool battery = false;                   // a laptop
    bool onBattery = false;                 // running on it now
    int uploadKbps = 0;                     // what a test (or the person) said; 0 = unknown
    bool uploadMeasured = false;            // measured, rather than typed
};

struct Choices {
    Scenario scenario = Scenario::Record;
    int display = 0;                        // index into Survey::displays; the canvas takes its size
    bool screen = false;                    // that display shown in the starter studios, only when chosen
    QString camera;                         // a video device's node name; empty = none
    QString cameraName;                     // its description, for studio names
    QString microphone = QStringLiteral("none"); // "" automatic, "none" off, else node.name
    QString microphoneName;                 // its description, for the source's name
    bool desktopSound = false;              // the desktop's sound, only when chosen
    // Stream
    QString service = QStringLiteral("twitch");  // a streamServices() id or "custom"
    QString server;                         // empty = the service's first
    QString key;
    bool fastMotion = true;                 // games rather than talking and desktop
};

// The sizes and rates plan() would like timed before it decides: the
// candidate encoders for the scenario at each size it might pick.
struct SpeedJob {
    std::string key;
    int width = 0, height = 0;
    int fps = 60;
    bool operator==(const SpeedJob &) const = default;
};
std::vector<SpeedJob> speedJobs(const Survey &survey, const Choices &choices);

// What a starter studio holds, in order from the back.
struct StudioItem {
    enum class Kind { Display, Camera, Backdrop, Title, Microphone };
    enum class Place { Full, Corner, Centre };
    Kind kind = Kind::Display;
    Place place = Place::Full;
    QString text;                           // a title's words
};
struct Studio {
    QString name;
    std::vector<StudioItem> items;
};

// One line of the summary: "Recording" · "AV1, 2560×1440, quality 20" · why.
struct Reason {
    QString what, value, why;
};

struct Plan {
    Profile profile;                        // name already set; streams carry the key
    std::vector<Studio> studios;
    std::vector<Reason> reasons;
    QStringList warnings;                   // things to know, for people
    bool ok = false;                        // false: nothing here can do it (warnings say why)
};
Plan plan(const Survey &survey, const Choices &choices);

// Standard heights at or below `height`, biggest first; widths follow the
// canvas's shape (even). The canvas's own height comes first when it is not
// a standard one.
std::vector<std::pair<int, int>> sizeLadder(int width, int height);
// About what a stream of this codec needs at this height and rate to look
// right, in kb/s.
int neededKbps(const std::string &codec, int height, int fps);

}  // namespace presets
