#include "drawn.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImageReader>
#include <QLinearGradient>
#include <QMovie>
#include <QPainter>
#include <QPixmap>
#include <QRandomGenerator>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextTable>
#include <QUrl>
#include <QTimer>

#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <semaphore>
#include <thread>

namespace {

// Jobs running, for FileWork::settle.
std::mutex s_jobsMutex;
std::condition_variable s_jobsDone;
int s_jobs = 0;

// As much of a text file as is read: a long one is cut off, not read for ever.
constexpr qint64 kWholeTextBytes = 1024 * 1024, kTailTextBytes = 256 * 1024;

// How long after its last write a file's stamp can be trusted (see FileStamp::settled).
constexpr qint64 kStampSettleNs = 2'500'000'000;

QRect inkBounds(const QImage &image) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) if (qAlpha(row[x])) {
            left = std::min(left,x); right = std::max(right,x);
            top = std::min(top,y); bottom = y;
        }
    }
    return right >= left ? QRect(QPoint(left,top),QPoint(right,bottom)) : QRect();
}

// The outline pass paints glyph strokes only. Painting an inline image or an
// HTML background again beneath the fill would increase its opacity.
void clearNonTextContent(QTextDocument &document) {
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto format = block.blockFormat(); format.setBackground(Qt::transparent);
        cursor.setBlockFormat(format);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
            const QUrl url(fragment.charFormat().toImageFormat().name());
            const QVariant resource = document.resource(QTextDocument::ImageResource,url);
            QImage image = resource.value<QImage>();
            if (image.isNull()) image = resource.value<QPixmap>().toImage();
            if (image.isNull() && resource.metaType().id() == QMetaType::QByteArray)
                image.loadFromData(resource.toByteArray());
            if (image.isNull()) continue;
            QImage empty(image.size(),QImage::Format_ARGB32_Premultiplied);
            empty.setDevicePixelRatio(image.devicePixelRatio()); empty.fill(Qt::transparent);
            document.addResource(QTextDocument::ImageResource,url,empty);
            document.addResource(QTextDocument::ImageResource,document.baseUrl().resolved(url),empty);
        }
    }
    std::function<void(QTextFrame *)> clearFrame = [&](QTextFrame *frame) {
        auto format = frame->frameFormat();
        format.setBackground(Qt::transparent); format.setBorderBrush(Qt::transparent);
        frame->setFrameFormat(format);
        if (auto *table = qobject_cast<QTextTable *>(frame))
            for (int row = 0; row < table->rows(); ++row)
                for (int column = 0; column < table->columns(); ++column) {
                    auto cell = table->cellAt(row,column);
                    auto cellFormat = cell.format().toTableCellFormat();
                    cellFormat.setBackground(Qt::transparent); cellFormat.setBorderBrush(Qt::transparent);
                    cell.setFormat(cellFormat);
                }
        for (auto *child : frame->childFrames()) clearFrame(child);
    };
    clearFrame(document.rootFrame());
}

}  // namespace

struct FileWork::Token::State {
    std::mutex mutex;
    QObject *owner;
};

bool FileWork::Token::wanted() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->owner != nullptr;
}

FileWork::FileWork(QObject *owner) : m_state(std::make_shared<Token::State>()) { m_state->owner = owner; }

FileWork::~FileWork() {
    std::lock_guard lock(m_state->mutex);
    m_state->owner = nullptr;
}

void FileWork::run(std::function<Answer(const Token &)> work) {
    {
        std::lock_guard lock(s_jobsMutex);
        ++s_jobs;
    }
    std::thread([state = m_state, work = std::move(work)] {
        // A few at a time, so a collection full of pictures does not decode them
        // all at once. One the system is holding keeps its turn for good, so
        // after a moment the next goes anyway.
        static std::counting_semaphore<4> turns(4);
        const bool turn = turns.try_acquire_for(std::chrono::seconds(1));
        const Token token(state);
        Answer answer = token.wanted() ? work(token) : Answer();
        if (turn) turns.release();
        {
            std::lock_guard lock(state->mutex);
            if (state->owner && answer) QMetaObject::invokeMethod(state->owner, std::move(answer), Qt::QueuedConnection);
        }
        std::lock_guard lock(s_jobsMutex);
        --s_jobs;
        s_jobsDone.notify_all();
    }).detach();
}

int FileWork::settle(int milliseconds) {
    std::unique_lock lock(s_jobsMutex);
    s_jobsDone.wait_for(lock, std::chrono::milliseconds(milliseconds), [] { return s_jobs == 0; });
    return s_jobs;
}

bool pathsThere(const QStringList &paths, const QList<bool> &folders, QList<bool> *there) {
    there->clear();
    if (paths.isEmpty()) return true;
    struct Check {
        std::mutex mutex;
        std::condition_variable answered;
        bool done = false;
        QList<bool> there;
    };
    static std::shared_ptr<Check> stuck;
    if (stuck) {
        std::lock_guard lock(stuck->mutex);
        if (!stuck->done) return false;
    }
    stuck.reset();
    auto check = std::make_shared<Check>();
    std::thread([check, paths, folders] {
        QList<bool> found;
        for (qsizetype i = 0; i < paths.size(); ++i) {
            const QFileInfo file(paths[i]);
            found << (folders[i] ? file.isDir() : file.isFile());
        }
        std::lock_guard lock(check->mutex);
        check->there = std::move(found);
        check->done = true;
        check->answered.notify_all();
    }).detach();
    std::unique_lock lock(check->mutex);
    if (!check->answered.wait_for(lock, std::chrono::milliseconds(100), [&] { return check->done; })) {
        stuck = check;
        return false;
    }
    *there = check->there;
    return true;
}

void blurImage(QImage &image, int radius) {
    if (radius <= 0 || image.isNull()) return;
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = image.width(), h = image.height();
    std::vector<int> line(size_t(std::max(w, h)) * 4);
    auto pass = [&](bool horizontal) {
        const int n = horizontal ? w : h, count = horizontal ? h : w;
        for (int k = 0; k < count; ++k) {
            auto px = [&](int i) -> uchar * {
                return horizontal ? image.scanLine(k) + size_t(i) * 4 : image.scanLine(i) + size_t(k) * 4;
            };
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 4; ++c) line[size_t(i) * 4 + c] = px(i)[c];
            int sum[4] = {0, 0, 0, 0};
            for (int i = -radius; i <= radius; ++i)
                for (int c = 0; c < 4; ++c) sum[c] += line[size_t(std::clamp(i, 0, n - 1)) * 4 + c];
            const int span = radius * 2 + 1;
            for (int i = 0; i < n; ++i) {
                for (int c = 0; c < 4; ++c) px(i)[c] = uchar(sum[c] / span);
                const int out = std::clamp(i - radius, 0, n - 1), in = std::clamp(i + radius + 1, 0, n - 1);
                for (int c = 0; c < 4; ++c) sum[c] += line[size_t(in) * 4 + c] - line[size_t(out) * 4 + c];
            }
        }
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
}

QImage renderText(const QVariantMap &s, const QString &text) {
    QFont font(s.value(QStringLiteral("font"), QFont().family()).toString());
    // Bounds that keep the picture within what a canvas can show.
    const int size = std::clamp(s.value(QStringLiteral("size"), 96).toInt(), 4, 2048);
    font.setPixelSize(size);
    font.setBold(s.value(QStringLiteral("bold")).toBool());
    font.setItalic(s.value(QStringLiteral("italic")).toBool());
    const bool antialias = s.value(QStringLiteral("antialias"),true).toBool();
    if (!antialias) font.setStyleStrategy(QFont::NoAntialias);
    const QString align = s.value(QStringLiteral("align"), QStringLiteral("left")).toString();
    const int wrap = std::clamp(s.value(QStringLiteral("wrapWidth"), 0).toInt(), 0, 8192);
    QTextDocument document;
    document.setUndoRedoEnabled(false);
    document.setDocumentMargin(0);
    document.setDefaultFont(font);
    QTextOption option;
    option.setAlignment(align == QLatin1String("centre") ? Qt::AlignHCenter : align == QLatin1String("right") ? Qt::AlignRight : Qt::AlignLeft);
    option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    document.setDefaultTextOption(option);
    const QString file = s.value(QStringLiteral("file")).toString();
    if (!file.isEmpty()) document.setBaseUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()+QLatin1Char('/')));
    if (s.value(QStringLiteral("richText")).toBool()) document.setHtml(text);
    else document.setPlainText(text);
    // Use the natural width when wrapping is off; adjustSize() would introduce
    // its own preferred line length and wrap otherwise unbounded titles.
    document.setTextWidth(wrap ? wrap : -1);
    const QSizeF textSize = document.size();

    const qreal outline = std::clamp(s.value(QStringLiteral("outlineWidth"), 0).toDouble(), 0.0, 256.0);
    const bool shadow = s.value(QStringLiteral("shadow")).toBool();
    const qreal sx = shadow ? std::clamp(s.value(QStringLiteral("shadowX"), size / 16).toDouble(), -1024.0, 1024.0) : 0;
    const qreal sy = shadow ? std::clamp(s.value(QStringLiteral("shadowY"), size / 16).toDouble(), -1024.0, 1024.0) : 0;
    const int blur = shadow ? std::clamp(s.value(QStringLiteral("shadowBlur"), size / 24).toInt(), 0, 256) : 0;
    // Mixed-size italic runs can overhang the document's advance width.
    qreal overhang = size / 8.0;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QFontMetricsF metrics(fragment.charFormat().font().resolve(font));
            overhang = std::max(overhang,metrics.height()/4);
        }
    const qreal pad = overhang + outline;
    const qreal left = pad + std::max<qreal>(0, -sx) + blur * 3, top = pad + std::max<qreal>(0, -sy) + blur * 3;
    const qreal right = pad + std::max<qreal>(0, sx) + blur * 3, bottom = pad + std::max<qreal>(0, sy) + blur * 3;
    // A very long text is cut off rather than made into a picture of gigabytes.
    const int w = std::clamp(int(std::ceil(std::min<qreal>(textSize.width() + left + right, 16384))), 1, 16384);
    const int h = std::clamp(int(std::ceil(std::min<qreal>(textSize.height() + top + bottom, 16384))), 1, 16384);
    const QColor colour(s.value(QStringLiteral("colour"), QStringLiteral("#ffffff")).toString());
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text,colour);
    auto paintDocument = [&] {
        QImage image(w,h,QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing,antialias);
        painter.translate(left,top);
        document.documentLayout()->draw(&painter,context);
        painter.end();
        return image;
    };
    const bool graded = s.value(QStringLiteral("gradient")).toBool();
    if (graded) {
        // The gradient supplies both colour and opacity. Start from opaque
        // glyph coverage so a translucent source colour is not applied twice.
        QTextCursor cursor(&document); cursor.select(QTextCursor::Document);
        QTextCharFormat mask; mask.setForeground(Qt::white); cursor.mergeCharFormat(mask);
    }
    QImage fill = paintDocument();
    if (graded) {
        const qreal angle = s.value(QStringLiteral("gradientAngle"), 90).toDouble() * M_PI / 180.0;
        const QRectF box = inkBounds(fill);
        const QPointF centre = box.center(), direction(std::cos(angle),std::sin(angle));
        const qreal reach = std::abs(direction.x())*box.width()/2 + std::abs(direction.y())*box.height()/2;
        QLinearGradient gradient(centre-direction*reach,centre+direction*reach);
        gradient.setColorAt(0,colour);
        gradient.setColorAt(1,QColor(s.value(QStringLiteral("gradientColour"), QStringLiteral("#ff8c00")).toString()));
        QPainter painter(&fill);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(fill.rect(),gradient);
    }
    QImage lettering(w,h,QImage::Format_ARGB32_Premultiplied);
    lettering.fill(Qt::transparent);
    if (outline > 0) {
        QTextCursor cursor(&document); cursor.select(QTextCursor::Document);
        QTextCharFormat format;
        format.setForeground(Qt::transparent);
        format.setBackground(Qt::transparent);
        format.setTextOutline(QPen(QColor(s.value(QStringLiteral("outlineColour"), QStringLiteral("#000000")).toString()),
                                  outline*2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        cursor.mergeCharFormat(format);
        clearNonTextContent(document);
        lettering = paintDocument();
    }
    { QPainter painter(&lettering); painter.drawImage(0,0,fill); }
    QImage canvas(w,h,QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(s.value(QStringLiteral("background"), QStringLiteral("#00000000")).toString()));
    QPainter painter(&canvas);
    if (shadow) {
        QImage shade = lettering;
        {
            QPainter tint(&shade);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(shade.rect(),QColor(s.value(QStringLiteral("shadowColour"), QStringLiteral("#b0000000")).toString()));
        }
        blurImage(shade,blur);
        painter.drawImage(QPointF(sx,sy),shade);
    }
    painter.drawImage(0,0,lettering);
    painter.end();
    return canvas;
}

QString readTextFile(const QString &path, int lines) {
    // A FIFO would never open and /dev/zero never end.
    if (!QFileInfo(path).isFile()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    // A log can grow without bound: only its tail is read when lines are asked for.
    if (lines > 0 && f.size() > kTailTextBytes) f.seek(f.size() - kTailTextBytes);
    QString text = QString::fromUtf8(f.read(lines > 0 ? kTailTextBytes : kWholeTextBytes));
    while (text.endsWith(QLatin1Char('\n'))) text.chop(1);
    if (lines <= 0) return text;
    const QStringList all = text.split(QLatin1Char('\n'));
    return all.mid(std::max<qsizetype>(0, all.size() - lines)).join(QLatin1Char('\n'));
}

bool FileStamp::unchangedSince(const FileStamp &before) const {
    return taken && before.taken && before.settled && there == before.there && device == before.device &&
           inode == before.inode && size == before.size && modifiedNs == before.modifiedNs;
}

FileStamp fileStamp(const QString &path) {
    FileStamp stamp;
    stamp.taken = true;
    struct stat info;
    if (::stat(QFile::encodeName(path).constData(), &info) != 0) {
        stamp.settled = true;  // nothing there to be written
        return stamp;
    }
    stamp.there = true;
    stamp.device = quint64(info.st_dev);
    stamp.inode = quint64(info.st_ino);
    stamp.size = qint64(info.st_size);
    stamp.modifiedNs = qint64(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
    // The coarsest clock a disk here keeps is FAT's two seconds; a time ahead
    // of this machine's (another machine's disk) never settles, and is read.
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    const qint64 nowNs = qint64(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
    stamp.settled = nowNs - stamp.modifiedNs > kStampSettleNs;
    return stamp;
}

engine::CpuFrame drawnFrame(const QImage &picture, bool premultiplied, bool linearAlpha) {
    Q_ASSERT(picture.isNull() || picture.format() == QImage::Format_ARGB32 || picture.format() == QImage::Format_ARGB32_Premultiplied);
    return {picture.width(), picture.height(), int(picture.bytesPerLine()), engine::PixelFormat::BGRA,
            picture.constBits(), premultiplied, false, linearAlpha};
}

MovieAnimator::MovieAnimator(const QByteArray &file, QObject *parent) : Animator(parent), m_buffer(new QBuffer(this)) {
    m_buffer->setData(file);
    m_buffer->open(QIODevice::ReadOnly);
    m_movie = new QMovie(m_buffer, {}, this);
    connect(m_movie, &QMovie::frameChanged, this, [this] { emit frame(m_movie->currentImage()); });
    connect(m_movie, &QMovie::error, this, [this] { emit failed(m_movie->lastErrorString()); });
    m_movie->start();
}

bool MovieAnimator::isAnimated(const QString &path) {
    if (!QFileInfo(path).isFile()) return false;
    QImageReader reader(path);
    return reader.supportsAnimation() && reader.imageCount() != 1;
}

QVariantList slideshowEntries(const QVariantMap &settings) {
    QVariantList out;
    if (settings.contains("entries")) {
        for (const auto &value : settings.value("entries").toList()) {
            const auto entry = value.toMap(); const auto path = entry.value("path").toString();
            if (!path.isEmpty()) out << QVariantMap{{"path", path}, {"folder", entry.value("folder", false).toBool()}};
        }
    } else if (const auto folder = settings.value("folder").toString(); !folder.isEmpty()) {
        out << QVariantMap{{"path", folder}, {"folder", true}};
    } else {
        for (const auto &path : settings.value("files").toStringList())
            if (!path.isEmpty()) out << QVariantMap{{"path", path}, {"folder", QFileInfo(path).isDir()}};
    }
    return out;
}

QStringList slideshowFiles(const QVariantMap &settings) {
    QStringList out;
    const auto picture = [](const QString &path) { return QFileInfo(path).isFile() && QImageReader(path).canRead(); };
    for (const auto &value : slideshowEntries(settings)) {
        const auto entry = value.toMap(); const auto path = entry.value("path").toString();
        if (entry.value("folder").toBool()) {
            for (const auto &file : QDir(path).entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase))
                if (picture(file.absoluteFilePath())) out << file.absoluteFilePath();
        } else if (picture(path)) out << path;
    }
    return out;
}

SlideshowAnimator::SlideshowAnimator(const QVariantMap &s, int canvasWidth, int canvasHeight, QObject *parent)
    : Animator(parent),
      m_width(std::clamp(s.value("width", canvasWidth).toInt(), 1, 16384)),
      m_height(std::clamp(s.value("height", canvasHeight).toInt(), 1, 16384)),
      m_intervalMs(std::clamp(s.value("interval", 5.).toDouble(), .2, 86400.) * 1000.),
      m_durationMs(std::clamp(s.value("fade", 700).toDouble(), 0., m_intervalMs)),
      m_loop(s.value("loop", true).toBool()), m_shuffle(s.value("shuffle", false).toBool()),
      m_manual(s.value("manual", false).toBool()), m_hide(s.value("hideWhenDone", false).toBool()),
      m_visibility(s.value("visibility", "continue").toString()), m_timer(new QTimer(this)), m_work(this) {
    settings = s;
    const auto type = s.value("transition", "fade").toString();
    m_view.type = type == "cut" ? engine::Transition::Cut : type == "swipe" ? engine::Transition::Swipe :
                  type == "slide" ? engine::Transition::Slide : engine::Transition::Fade;
    if (m_view.type == engine::Transition::Cut) m_durationMs = 0;
    m_clock.start(); m_timer->setInterval(16); m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &SlideshowAnimator::tick);
    findFiles(false);
}

void SlideshowAnimator::findFiles(bool reloading) {
    const quint64 serial = ++m_findSerial;
    m_work.run([this, serial, reloading, s = settings](const FileWork::Token &) -> FileWork::Answer {
        const QStringList files = slideshowFiles(s);
        return [this, serial, reloading, files] { if (serial == m_findSerial) filesFound(files, reloading); };
    });
}

void SlideshowAnimator::filesFound(const QStringList &files, bool reloading) {
    m_files = files;
    if (m_shuffle) std::shuffle(m_files.begin(), m_files.end(), *QRandomGenerator::global());
    if (!m_files.isEmpty()) {
        reset(reloading ? m_paused : false);
        m_timer->start();
        return;
    }
    if (reloading) {
        m_timer->stop(); m_index = -1; m_elapsed = 0; m_ended = m_ending = m_transitioning = m_stepWhilePaused = m_loading = false;
        ++m_pictureSerial;
        m_view.to = QImage(m_width, m_height, QImage::Format_ARGB32); m_view.to.fill(Qt::transparent); m_view.from = m_view.to;
        m_view.progress = 1.f; m_view.usePrevious = false; ++m_view.serial; publish();
    }
    emit failed(tr("no images"));
}

void SlideshowAnimator::show(const QString &path, bool whole) {
    const quint64 serial = ++m_pictureSerial;
    m_loading = true;
    m_work.run([this, serial, path, whole, width = m_width, height = m_height](const FileWork::Token &) -> FileWork::Answer {
        // Letterboxed into the slideshow's box, on transparent.
        QImage out(width, height, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        const QImage img = QFileInfo(path).isFile() ? QImage(path) : QImage();
        if (!img.isNull()) {
            const QSizeF s = QSizeF(img.size()).scaled(width, height, Qt::KeepAspectRatio);
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QRectF((width - s.width()) / 2, (height - s.height()) / 2, s.width(), s.height()), img);
        }
        return [this, serial, whole, out] { if (serial == m_pictureSerial) pictureReady(out, whole); };
    });
}

void SlideshowAnimator::pictureReady(const QImage &picture, bool whole) {
    m_loading = false;
    whole = whole || m_view.to.isNull();
    m_view.usePrevious = !whole && m_interrupted;
    m_view.from = whole ? picture : m_view.to;
    m_view.to = picture;
    m_view.progress = whole || !m_transitioning ? 1.f : 0.f;
    ++m_view.serial;
    m_lastTick = m_clock.elapsed();
    publish();
}

QVariantMap SlideshowAnimator::playback() const {
    return {{"index", m_index}, {"count", m_files.size()}, {"paused", m_paused}, {"ended", m_ended},
            {"manual", m_manual}, {"transitioning", m_transitioning},
            {"file", m_index >= 0 ? m_files.value(m_index) : QString()}};
}

void SlideshowAnimator::publish() {
    emit presentationChanged();
    if (!m_transitioning) emit frame(m_view.to);
}

void SlideshowAnimator::reset(bool paused) {
    if (m_files.isEmpty()) return;
    m_index = 0; m_paused = paused; m_ended = m_ending = m_transitioning = m_stepWhilePaused = false;
    m_elapsed = 0; m_lastTick = m_clock.elapsed();
    show(m_files.first(), true);
}

void SlideshowAnimator::go(int index, bool ending) {
    // While a picture is on its way what it would have cut into still counts.
    if (!m_loading) m_interrupted = m_transitioning;
    m_elapsed = 0; m_ending = ending; m_ended = false;
    m_transitioning = m_durationMs > 0;
    if (!m_transitioning) { m_ended = ending; m_ending = false; }
    if (!ending) { m_index = index; show(m_files[index], false); return; }
    ++m_pictureSerial;
    QImage clear(m_width, m_height, QImage::Format_ARGB32);
    clear.fill(Qt::transparent);
    pictureReady(clear, false);
}

void SlideshowAnimator::advance(int direction) {
    if (m_index < 0) return;
    if ((m_ended || m_ending) && direction < 0) { go(m_index); return; }
    int next = m_index + direction;
    if (next >= m_files.size()) {
        if (!m_loop) {
            if (m_ended || m_ending) return;
            if (m_hide) go(m_index, true);
            else {
                if (m_transitioning) m_ending = true;
                else { m_ended = true; m_elapsed = 0; }
                publish();
            }
            return;
        }
        if (m_shuffle && m_files.size() > 1) {
            const auto last = m_files[m_index];
            std::shuffle(m_files.begin(), m_files.end(), *QRandomGenerator::global());
            if (m_files.first() == last) m_files.swapItemsAt(0, 1);
        }
        next = 0;
    } else if (next < 0) {
        if (!m_loop) return;
        next = m_files.size() - 1;
    }
    go(next);
}

bool SlideshowAnimator::control(const QString &action, double value) {
    if (action == "reload") { findFiles(true); return true; }
    if (m_index < 0) return false;
    tick();
    if (action == "pause") { m_paused = value != 0; m_stepWhilePaused = false; }
    else if (action == "restart") reset(false);
    else if (action == "stop") reset(true);
    else if (action == "next" || action == "previous") { advance(action == "next" ? 1 : -1); m_stepWhilePaused = m_paused && m_transitioning; }
    else return false;
    m_lastTick = m_clock.elapsed();
    emit presentationChanged();
    return true;
}

void SlideshowAnimator::setActive(bool active) {
    if (active == m_active) return;
    tick(); m_active = active; m_lastTick = m_clock.elapsed();
    if (m_visibility == "restart") reset(active ? false : m_paused);
}

void SlideshowAnimator::tick() {
    const qint64 now = m_clock.elapsed();
    double delta = now - m_lastTick; m_lastTick = now;
    if (m_loading || m_index < 0 || (!m_active && m_visibility != "continue") || (m_paused && !m_stepWhilePaused)) return;
    // Consume elapsed time, including a late timer tick, without extending the
    // requested slide interval by the transition duration.
    for (int steps = 0; steps < 32; ++steps) {
        if (m_transitioning) {
            m_elapsed += delta;
            m_view.progress = float(std::min(1., m_elapsed / m_durationMs));
            if (m_elapsed < m_durationMs) { publish(); return; }
            delta = m_elapsed - m_durationMs; m_elapsed = 0;
            m_transitioning = m_stepWhilePaused = false; m_ended = m_ending; m_ending = false; publish();
        }
        if (m_manual || m_paused || m_ended || (m_files.size() == 1 && m_loop)) return;
        const double wait = !m_loop && m_index + 1 == m_files.size() && !m_hide ? m_intervalMs : m_intervalMs - m_durationMs;
        m_elapsed += delta;
        if (m_elapsed < wait) return;
        delta = m_elapsed - wait; m_elapsed = 0; advance(1);
    }
}
