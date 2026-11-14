#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "output/encoders.h"

// Opening an encoder takes driver locks the frame thread's GPU waits need too:
// a probe in-process stalls the preview and any recording for hundreds of
// milliseconds. So the probe and the fit checks run in a short-lived child
// process (this same binary, a hidden verb), whose locks are its own.
namespace encoderhelper {

// The child side. `omabroadcast --encoder-probe` prints what the machine can
// encode with; `--encoder-fit W H FPS` reads that list on stdin and prints the
// keys of the encoders that will not open at that size, and of the hardware
// ones that open but, timed for a moment, do not keep up there; `--encoder-bench
// SECONDS` reads that list and some jobs on stdin and prints how fast each
// job's encoder keeps up.
bool isHelperCall(int argc, char **argv);
int run(int argc, char **argv);

// The program the parent runs as the helper: this binary, unless
// OMABROADCAST_HELPER names another.
QString program();

// One speed test: an encoder (by encoderKey()) at a size and rate.
// bitrateKbps 0 drives it as a recording does (constant quality); above 0 as
// a stream does (constant bitrate).
struct BenchJob {
    std::string key;
    int width = 0, height = 0;
    int fpsNum = 60, fpsDen = 1;
    int bitrateKbps = 0;
};
// What it managed: frames per second sustained once open (the time it takes
// to open is left out). opened is false for an encoder that would not open
// (or encode) at that size, or a key the list does not hold; note says why.
struct BenchResult {
    std::string key;
    int width = 0, height = 0;
    int fpsNum = 60, fpsDen = 1;
    int bitrateKbps = 0;
    bool opened = false;
    double framesPerSecond = 0;
    // What it made of the test pictures, per second of picture: at constant
    // quality, a guide to how big a recording of busy content gets.
    int producedKbps = 0;
    std::string note;
};

// A picture that is hard to make small, number `index` of a moving
// sequence: blocks and edges as on a desktop, fine texture that pans with
// them (so motion search has work to do and something to find), and grain of
// its own on every frame. What the bench and the upload test send.
engine::YuvFrame busyPicture(int width, int height, engine::ColourFormat format, int index);

// The parent side: the arguments for each call, the stdin for a fit, and the
// readers for what comes back. A reader returns false when the child said
// nothing usable, and the caller does the work in-process instead.
// Those named by encoderKey are listed as unavailable without being opened.
QStringList probeArguments(const QStringList &skip = {});
QStringList fitArguments(int width, int height, int fps);
QByteArray fitInput(const std::vector<EncoderInfo> &encoders);
bool readProbe(const QByteArray &output, std::vector<EncoderInfo> *encoders);
// One encoder the probe has started (done false) or finished trying, from a line it wrote.
bool readProgress(const QByteArray &line, EncoderInfo *encoder, bool *done);
// What a fit found: unfit would not open at the size (or encoded nothing);
// slow opened but did not reach the frame rate with the headroom a recording
// needs; fps is what every timed encoder managed (hardware ones only: timing
// a processor encoder would take every core for seconds, and an editing codec
// is never picked unasked).
struct Fit {
    std::set<std::string> unfit, slow;
    std::map<std::string, double> fps;
};
bool readFit(const QByteArray &output, Fit *fit);

// The bench: each job is opened with the settings the recorder would use
// (hardware frames uploaded where it uploads them) and fed moving, detailed
// pictures as fast as it takes them, for up to secondsPerJob each (or a frame
// cap). The child prints each job's result as it finishes (readBenchLine()
// reads one) and then all of them (readBench()). Unlike the probe and the
// fit, the bench has no in-process fallback: when the helper says nothing
// usable, there is no answer.
QStringList benchArguments(double secondsPerJob);
QByteArray benchInput(const std::vector<EncoderInfo> &encoders, const std::vector<BenchJob> &jobs);
bool readBench(const QByteArray &output, std::vector<BenchResult> *results);
bool readBenchLine(const QByteArray &line, BenchResult *result);

}  // namespace encoderhelper
