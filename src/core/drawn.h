#pragma once

#include <QImage>
#include <QElapsedTimer>
#include "engine/engine.h"
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

class QBuffer;
class QMovie;
class QTimer;

// Work that may wait on a disk for as long as the disk likes: a FIFO no one
// writes to, a path on a mount that has gone away. Each job has a thread of its
// own, which can be left behind where the main and frame threads could not;
// what it found comes back as a function run on the owner's thread.
class FileWork {
public:
    explicit FileWork(QObject *owner);
    // Answers from here on reach no one.
    ~FileWork();
    // Says whether an answer is still wanted, for work that waited long enough
    // to find the program closing.
    class Token {
    public:
        bool wanted() const;
    private:
        friend class FileWork;
        struct State;
        explicit Token(std::shared_ptr<State> state) : m_state(std::move(state)) {}
        std::shared_ptr<State> m_state;
    };
    using Answer = std::function<void()>;
    void run(std::function<Answer(const Token &)> work);
    // Waits a moment for running jobs to end, as they use Qt's fonts and image
    // plugins; those the system is holding are not waited for. How many are left.
    static int settle(int milliseconds);

private:
    std::shared_ptr<Token::State> m_state;
};

// Whether each path is there (`folders` true: as a folder, else as an ordinary
// file), for the main thread: a path on a mount that has gone away never
// answers, so this waits only a moment and is false without an answer, and at
// once while an earlier check has still not returned.
bool pathsThere(const QStringList &paths, const QList<bool> &folders, QList<bool> *there);

// Sources OmaBroadcast draws itself: text, animated images, slideshows. Text
// is a plain function of its settings; the moving kinds are animators that
// hand over a new picture whenever theirs changes.

// Text from its settings: text, font, size, bold, italic, align (left, centre,
// right), wrapWidth (0 = none), colour, background; outlineWidth and
// outlineColour; shadow, shadowColour, shadowX, shadowY, shadowBlur;
// gradient, gradientColour, gradientAngle (degrees), antialias (true); richText (Qt HTML, false
// for literal plain text). `text` is what to draw (the setting, or a file's
// contents). Relative HTML resources resolve beside the selected text file.
QImage renderText(const QVariantMap &settings, const QString &text);

// A text file's contents, or its last `lines` lines when lines > 0 (a chat or
// "now playing" log); empty when it cannot be read. Ordinary files only, and
// only so much of one. It can still wait on a disk: not on the main thread.
QString readTextFile(const QString &path, int lines);

// What tells a file as it is from the file as it was without reading it: which
// file it is (another renamed over it is a different one), how big, and when
// it was last written.
struct FileStamp {
    bool taken = false;    // one never taken is the same as no other
    bool there = false;
    quint64 device = 0, inode = 0;
    qint64 size = 0, modifiedNs = 0;
    // Last written long enough before the stamp was taken that a write since
    // must show in its time, however coarsely the disk keeps it. Until then a
    // second write of the same size could look the same: the file is read.
    bool settled = false;
    // The file is as it was at `before`: nothing to read again.
    bool unchangedSince(const FileStamp &before) const;
};
// Asks the disk, which can wait as readTextFile can: not on the main thread.
FileStamp fileStamp(const QString &path);

// A drawn picture as the engine takes it, with no copy made: BGRA (ARGB32, or
// its premultiplied twin with `premultiplied`), its rows as far apart as the
// picture keeps them, which can be more than its width takes. The pixels stay
// the picture's: it must be held, and not written to, until the upload is done.
engine::CpuFrame drawnFrame(const QImage &picture, bool premultiplied, bool linearAlpha);

// Three passes of a box blur on a premultiplied image: close to a Gaussian.
void blurImage(QImage &image, int radius);

class Animator : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // What it was made from; a change makes a new animator.
    QVariantMap settings;

signals:
    void frame(const QImage &image);
    void failed(const QString &why);
};

// An animated GIF, WebP or APNG, at its own frame timings, looping.
class MovieAnimator : public Animator {
    Q_OBJECT
public:
    // The whole file: frames then come from memory, so playing never waits on a disk.
    MovieAnimator(const QByteArray &file, QObject *parent = nullptr);
    static bool isAnimated(const QString &path);

private:
    QBuffer *m_buffer;
    QMovie *m_movie;
};

// Typed ordered inputs, with compatibility for older folder/files settings.
// A folder expands its readable images in name order, without recursion.
QVariantList slideshowEntries(const QVariantMap &settings);
QStringList slideshowFiles(const QVariantMap &settings);

// Playback owns time and decoded endpoints; the render engine owns transitions.
// Files are found and pictures decoded off the main thread: the clock waits
// for a picture that has not come, so a slide on a dead mount just stays up.
class SlideshowAnimator : public Animator {
    Q_OBJECT
public:
    struct Presentation {
        QImage from, to;
        engine::Transition type = engine::Transition::Fade;
        float progress = 1.f;
        quint64 serial = 0;
        bool usePrevious = false; // snapshot current engine output on interruption
    };
    SlideshowAnimator(const QVariantMap &settings, int canvasWidth, int canvasHeight, QObject *parent = nullptr);
    QStringList files() const { return m_files; }
    Presentation presentation() const { return m_view; }
    QVariantMap playback() const;
    bool control(const QString &action, double value = 0);
    void setActive(bool active);

signals:
    void presentationChanged();

private:
    void tick();
    void reset(bool paused);
    void advance(int direction);
    void go(int index, bool ending = false);
    void publish();
    void findFiles(bool reloading);
    void filesFound(const QStringList &files, bool reloading);
    // Decodes `path`, then pictureReady. `whole`: the picture shows at once, with nothing to go from.
    void show(const QString &path, bool whole);
    void pictureReady(const QImage &picture, bool whole);
    QStringList m_files;
    int m_index = -1, m_width, m_height;
    double m_intervalMs, m_durationMs, m_elapsed = 0;
    bool m_loop, m_shuffle, m_manual, m_hide;
    bool m_active = true, m_paused = false, m_ended = false, m_transitioning = false;
    bool m_ending = false, m_stepWhilePaused = false;
    QString m_visibility;
    Presentation m_view;
    QElapsedTimer m_clock;
    qint64 m_lastTick = 0;
    QTimer *m_timer;
    FileWork m_work;
    quint64 m_findSerial = 0, m_pictureSerial = 0;  // only the newest answer counts
    bool m_loading = false;      // the picture asked for has not come
    bool m_interrupted = false;  // that picture cuts into a transition under way
};
