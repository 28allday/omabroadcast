#pragma once

#include <functional>
#include <string>
#include <vector>

#include "queued_sink.h"

extern "C" {
#include <libavformat/avformat.h>
}

// The file formats a recording can be written in.
struct FileFormat {
    const char *id;         // what a profile saves: "mkv", "fmp4", …
    const char *label;      // for people
    const char *extension;
    const char *muxer;      // FFmpeg's name
    const char *movflags;   // or nullptr
    bool chapters;          // whether markers survive in the file
    bool survivesCrash;     // whether a file cut short still plays
};
const std::vector<FileFormat> &fileFormats();
// Whether the format can hold this codec, asking the muxer.
bool formatCarries(const std::string &format, AVCodecID codec);
// The stream's own four-character tag where the container knows it for the
// codec, else 0 so the muxer picks one. ProRes has a tag per profile (apcs
// for LT, apch for HQ…); the muxer's own pick would label every file HQ.
unsigned keptCodecTag(const AVOutputFormat *format, const AVCodecParameters *params);
// The format by id; Matroska when it is unknown. "custom:<muxer>" is any
// FFmpeg muxer by its own name, written to a file with its first extension.
const FileFormat &fileFormat(const std::string &id);
// Why FFmpeg's muxer of this name cannot write a recording, or "" when it can.
std::string muxerProblem(const std::string &muxer);
// The muxer's own description, such as "NUT"; "" when there is none.
std::string muxerLabel(const std::string &muxer);

// Writes encoded packets to a file, or to a run of files when it splits.
// Every file starts on a keyframe; the sound is cut at exactly the same
// instant as the picture, so a split or a pause neither loses nor repeats any
// of it. Times are on the encoder's clock (EncodedPacket::ptsNs).
class FileSink : public QueuedSink {
public:
    struct Settings {
        std::string path;           // the first file
        std::string format = "mkv";
        // "key=value …", the container's own options, set after ours; one it
        // does not know fails the start by name.
        std::string options;
        int64_t splitSeconds = 0;   // 0 = never by time
        int64_t splitBytes = 0;     // 0 = never by size
        // The next file's path when it splits, given its number from 2.
        std::function<std::string(int)> nextPath;
        // Packets from before this are not wanted (a sink joining a running
        // encoder starts at its first keyframe from here).
        int64_t startNs = 0;
    };
    struct Stats {
        uint64_t bytes = 0;         // every file so far
        int64_t durationNs = 0;     // what is in the files, pauses left out
        int files = 0;
        bool paused = false;
        std::string path;           // the file being written
    };

    FileSink();
    ~FileSink() override;

    // Opens the first file now, so a path that cannot be written fails here.
    bool start(const Settings &settings, std::shared_ptr<const EncodedLayout> layout, std::string *error);
    // Stops at this instant: the picture before it, the sound up to it.
    void finishAt(int64_t ns);
    void pause(int64_t ns);
    void resume(int64_t ns);
    bool paused() const;
    // A new file from the next keyframe.
    void split();
    // A named point to jump to, where the format keeps them.
    void chapter(const std::string &name, int64_t ns);

    Stats stats() const;
    // Every file written, in order, once each is closed.
    std::vector<std::string> closedFiles() const;

    std::function<void(const std::string &)> onError;
    // Something went wrong that it carries on through (a split that could not
    // open its new file): worth saying, not a reason to stop.
    std::function<void(const std::string &)> onWarning;
    std::function<void(const std::string &)> onFileClosed;
    // Asked when it waits on a keyframe (a split, a resume) to come sooner:
    // one on the first picture at or after this instant.
    std::function<void(int64_t)> wantKeyframe;

    struct Part;

protected:
    void handle(const EncodedPacket &p) override;
    void finish() override;
    void overflow() override;

private:
    bool openPart(Part &part, const std::string &path, std::string *error);
    // Into the span its time falls in, or with `pastEnd` the file's last one.
    void write(Part &part, const EncodedPacket &p, bool pastEnd = false);
    void closePart(Part &part);
    void startWriting(int64_t keyNs);
    void checkClosing();

    Settings m_settings;
    std::shared_ptr<const EncodedLayout> m_layout;
    mutable std::mutex m_stateMutex;   // the requests and stats below
    std::unique_ptr<Part> m_part, m_next;
    // Waiting for a keyframe at or after this (start, resume); audio
    // meanwhile is held, then kept from the keyframe on.
    bool m_waiting = true;
    int64_t m_waitAfter = 0;
    std::vector<EncodedPacket> m_held;
    int64_t m_finishAt = INT64_MAX;
    int64_t m_pauseAt = INT64_MAX;
    bool m_resumeAsked = false;
    int64_t m_resumeAfter = 0;
    bool m_splitAsked = false;
    int64_t m_splitAt = INT64_MAX;     // the cut of a split under way
    // A new file that would not open: no other try before this, and said once
    // until one works, so each picture does not become a keyframe and a warning.
    int64_t m_splitRetryNs = INT64_MIN;
    bool m_splitWarned = false;
    std::vector<EncodedPacket> m_nextHeld;  // the next file's sound until it opens
    int m_partNumber = 1;
    struct Marker { std::string name; int64_t ns; };
    std::vector<Marker> m_markers;
    Stats m_stats;
    uint64_t m_closedBytes = 0;
    std::vector<std::string> m_closed;
    std::vector<int64_t> m_seen;       // per stream, the latest instant seen
    int64_t m_decodedNs = INT64_MIN;   // the latest picture's decoding time
    int64_t m_doneNs = 0;              // file time in closed files
    bool m_failed = false;
};
