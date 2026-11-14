#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QQuickItem>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/peak_meter.h"
#include "capture/picture.h"

class AudioPlayback;

class MediaStream;
class PreviewView;
struct SwsContext;

// Plays one file back for checking: the playback window's picture, sound,
// meters and transport. The file is read with the decoder media sources use,
// so it plays as the program's own FFmpeg reads it. Nothing reaches the
// engine, the mixer or any output; the sound goes to one output of its own.
//
// Shuttle: forward runs at 1, 2 and 4 times; backward (which the decoder
// cannot play) steps back through still pictures at those rates, silent.
//
// The sound and the pictures are each taken from the decoder on a thread of
// their own, so nothing the window is busy with holds either back; they rest
// while nothing plays and no picture is on its way.
class RecordingPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY fileChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileChanged)
    Q_PROPERTY(QVariantList info READ info NOTIFY fileChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY fileChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY fileChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY fileChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(int channels READ channels NOTIFY fileChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    // 0 stopped; 1, 2, 4 forward; -1, -2, -4 backward.
    Q_PROPERTY(int shuttle READ shuttle NOTIFY shuttleChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(QString soundNote READ soundNote NOTIFY soundChanged)
    // {db: [..], levels: [0..1 ..], maximumDb, clipped}, as the mixer's meters.
    Q_PROPERTY(QVariantMap meter READ meter NOTIFY meterChanged)
    Q_PROPERTY(QQuickItem *view READ view WRITE setView NOTIFY viewChanged)

public:
    explicit RecordingPlayer(QObject *parent = nullptr);
    ~RecordingPlayer() override;

    // Opens `path` paused on its first picture; empty closes.
    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE void close();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void forward();   // L: plays, then faster
    Q_INVOKABLE void backward();  // J: steps back, then faster
    Q_INVOKABLE void stepFrames(int frames);
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void toStart() { seek(0); }
    Q_INVOKABLE void toEnd();
    Q_INVOKABLE void resetPeak();

    QString path() const { return m_path; }
    QString fileName() const;
    QVariantList info() const { return m_info; }
    int videoWidth() const { return m_width; }
    int videoHeight() const { return m_height; }
    double frameRate() const { return m_fps; }
    double duration() const { return m_duration; }
    int channels() const { return m_channels; }
    double position() const { return m_position; }
    int shuttle() const { return m_shuttle; }
    QString problem() const { return m_problem; }
    QString soundNote() const { return m_soundNote; }
    QVariantMap meter() const { return m_meter; }
    QQuickItem *view() const;
    void setView(QQuickItem *view);

    // Where the sound goes ("" for none) and why it is silent, when it is:
    // asked again at every open and whenever the app's outputs change.
    std::function<std::string(QString *why)> soundTarget;
    void refreshSound();
    // True peak or sample peak, as the mixer's meters are set.
    void setTruePeak(bool on);

    // What a file is, for the window's info: container, length, size, and each
    // stream's codec, picture or sound. Empty when it cannot be read.
    struct Facts { int width = 0, height = 0, channels = 2; double fps = 0, duration = 0; };
    static QVariantList probe(const QString &path, Facts *facts = nullptr);
    // The picture `p` as straight RGB at most `maxWidth` × `maxHeight`, keeping its shape.
    // An HDR picture (PQ, HLG) is brought down to the screen's SDR as the Tone
    // map filter does it, and BT.2020 colour to the screen's. From any thread.
    QImage toImage(const Picture &p, int maxWidth, int maxHeight);
    // How many of `frames` of sound about to go to the output to leave out, with
    // `queued` already waiting there and the output taking `quantum` at a time.
    // None while the output keeps up. Once what waits is well over what was
    // asked for (the sound has slipped late), frames are left out until it is
    // back down; `catching` carries that from one call to the next.
    static int soundToDrop(uint64_t queued, int frames, int quantum, bool *catching);
    // Since the file opened: frames of sound taken from the decoder, and
    // pictures handed to the view.
    uint64_t soundFrames() const { return m_soundFrames.load(); }
    uint64_t picturesShown() const { return m_picturesShown; }

signals:
    void fileChanged();
    void durationChanged();
    void positionChanged();
    void shuttleChanged();
    void problemChanged();
    void soundChanged();
    void meterChanged();
    void viewChanged();

private:
    void tick();
    void wake();
    void setShuttle(int shuttle);
    void setProblem(const QString &problem);
    void startSound();
    double pollPosition();
    void updateTarget();
    void sendSeek(double to);
    void seekDone();
    void backStep();
    void showPicture();
    void measured(double seconds, uint64_t opening);
    void startThreads();
    void stopThreads();
    // Each on a thread of its own.
    void pumpSound();
    void pumpPictures();
    void measure(QByteArray file, uint64_t opening);

    QString m_path;
    QVariantList m_info;
    int m_width = 0, m_height = 0, m_channels = 2;
    double m_fps = 30, m_duration = 0, m_position = 0;
    // The length the file states; none (0) and it is found by reading it through.
    double m_stated = 0;
    bool m_measured = false;
    int m_shuttle = 0;
    QString m_problem, m_soundNote;
    QVariantMap m_meter;
    bool m_meterQuiet = true;
    std::unique_ptr<MediaStream> m_stream;
    std::unique_ptr<AudioPlayback> m_sound;
    std::string m_soundTarget;
    PeakMeter m_peak{2};     // m_peakMutex
    std::mutex m_peakMutex;
    bool m_truePeak = false;
    QPointer<PreviewView> m_view;
    QTimer m_timer;          // the clock, the end and the meters, while anything moves
    QTimer m_back;           // the next backward step
    QElapsedTimer m_meterClock, m_backClock, m_awaitClock;
    // A seek whose picture has not shown yet; one asked for meanwhile waits
    // for it (m_position says where to), so each gets to show its picture.
    bool m_awaiting = false, m_deferred = false;
    std::atomic<uint64_t> m_seekSerial{0};
    uint64_t m_opening = 0, m_picturesShown = 0;

    // The threads. m_workMutex: whether they run, and the picture on its way to the view.
    std::thread m_soundThread, m_pictureThread, m_measureThread;
    std::mutex m_workMutex;
    std::condition_variable m_workWake;
    bool m_workQuit = false, m_workRun = false, m_redraw = false;
    QImage m_ready;
    bool m_readyFresh = false, m_posted = false;
    uint64_t m_readySeek = 0;
    std::atomic<bool> m_measureQuit{false};
    std::atomic<int> m_targetWidth{0}, m_targetHeight{0};  // the view, in pixels
    std::atomic<uint64_t> m_soundFrames{0};
    Picture m_picture;       // the picture thread's while it runs
    std::mutex m_convertMutex;  // m_sws, m_deep, m_tables
    SwsContext *m_sws = nullptr;
    std::vector<uint16_t> m_deep;
    struct ScreenTables;
    std::unique_ptr<ScreenTables> m_tables;
};
