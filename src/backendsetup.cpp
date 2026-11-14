#include "backend.h"

#include <QTimer>
#include <QUuid>

#include "core/machine_survey.h"
#include "core/starter_studios.h"
#include "output/bandwidth_test.h"
#include "output/encoder_bench.h"
#include "output/services.h"

// Tools → Set up for…: survey the machine, measure the upload (for a stream)
// and the encoders at the sizes the plan could pick, then plan, show why,
// and on Create make a new profile and studio collection.

QVariantList Backend::setupScenarios() const {
    QVariantList out;
    for (presets::Scenario s : presets::scenarios())
        out << QVariantMap{{QStringLiteral("id"), presets::scenarioId(s)}, {QStringLiteral("name"), presets::scenarioName(s)},
                           {QStringLiteral("blurb"), presets::scenarioBlurb(s)}};
    return out;
}

QVariantList Backend::setupDisplays() const {
    QVariantList out;
    int index = 0;
    for (const presets::Display &d : machinesurvey::displays()) {
        const QString name = d.description.isEmpty() ? d.name : QStringLiteral("%1 (%2)").arg(d.description, d.name);
        out << QVariantMap{{QStringLiteral("index"), index++},
                           {QStringLiteral("label"), tr("%1 — %2×%3, %4 Hz").arg(name).arg(d.width).arg(d.height).arg(qRound(d.refresh))}};
    }
    if (out.isEmpty()) out << QVariantMap{{QStringLiteral("index"), 0}, {QStringLiteral("label"), tr("Display")}};
    return out;
}

void Backend::setSetupStep(const QString &step, double progress) {
    m_setupStep = step;
    m_setupProgress = progress;
    emit setupChanged();
}

void Backend::startSetupCheck(const QVariantMap &values) {
    if (m_setupBusy) return;
    presets::Choices c;
    if (!presets::scenarioFromId(values.value(QStringLiteral("scenario")).toString(), &c.scenario)) return;
    c.display = values.value(QStringLiteral("display")).toInt();
    c.screen = values.value(QStringLiteral("screen")).toBool();
    c.camera = values.value(QStringLiteral("camera")).toString();
    c.cameraName = values.value(QStringLiteral("cameraName")).toString();
    c.microphone = values.value(QStringLiteral("microphone"), QStringLiteral("none")).toString();
    c.microphoneName = values.value(QStringLiteral("microphoneName")).toString();
    c.desktopSound = values.value(QStringLiteral("desktopSound")).toBool();
    c.service = values.value(QStringLiteral("service"), QStringLiteral("custom")).toString();
    c.server = values.value(QStringLiteral("server")).toString();
    c.key = values.value(QStringLiteral("key")).toString().trimmed();
    c.fastMotion = values.value(QStringLiteral("fastMotion"), true).toBool();
    m_setupChoices = c;
    m_setupSurvey = {};
    machinesurvey::fill(&m_setupSurvey);
    m_setupSurvey.encoders = m_session.encoders();
    m_setupSurvey.engine = m_session.engineDevice();
    m_setupSurvey.uploadKbps = values.value(QStringLiteral("uploadKbps")).toInt();
    m_setupResult = {};
    m_setupResultNote.clear();
    m_setupBusy = true;
    ++m_setupRun;
    setSetupStep(tr("Looking at this machine…"), 0);
    if (c.scenario == presets::Scenario::Stream) setupMeasureUpload();
    else setupMeasureSpeeds();
}

QVariantMap Backend::setupUploadTest(const QString &service) const {
    std::string why;
    const bool can = BandwidthTest::canTest(service.toStdString(), &why);
    return {{QStringLiteral("canTest"), can}, {QStringLiteral("why"), QString::fromStdString(why)}};
}

// A stream's upload first (its sizes depend on it), unless it was typed or
// cannot be tested without going live.
void Backend::setupMeasureUpload() {
    const presets::Choices &c = m_setupChoices;
    std::string why;
    if (m_setupSurvey.uploadKbps > 0 || c.key.isEmpty() || !BandwidthTest::canTest(c.service.toStdString(), &why)) {
        setupMeasureSpeeds();
        return;
    }
    if (!m_setupUpload) {
        m_setupUpload = new BandwidthTest(this);
        connect(m_setupUpload, &BandwidthTest::progress, this, [this](double fraction, int kbps) {
            if (m_setupBusy) setSetupStep(tr("Testing your upload: %1 kb/s…").arg(kbps), 0.4 * fraction);
        });
    }
    disconnect(m_setupUpload, &BandwidthTest::finished, this, nullptr);
    const quint64 run = m_setupRun;
    connect(m_setupUpload, &BandwidthTest::finished, this, [this, run](const BandwidthTest::Result &r) {
        if (run != m_setupRun) return;
        if (r.ok) {
            m_setupSurvey.uploadKbps = r.measuredKbps;
            m_setupSurvey.uploadMeasured = true;
        } else {
            m_setupResultNote = tr("The upload test did not finish: %1").arg(QString::fromStdString(r.error));
        }
        setupMeasureSpeeds();
    });
    BandwidthTest::Settings s;
    s.serviceId = c.service.toStdString();
    s.server = c.server.toStdString();
    s.key = c.key.toStdString();
    // Enough above the service's ceiling to show the ceiling is safe; the
    // test's own top otherwise.
    if (const StreamService *service = streamService(s.serviceId); service && service->maxBitrateKbps)
        s.maxKbps = service->maxBitrateKbps * 4 / 3 + 500;
    setSetupStep(tr("Connecting for the upload test…"), 0);
    m_setupUpload->start(s);
}

// Each encoder the plan could pick, timed at the sizes it could pick, in the
// helper process.
void Backend::setupMeasureSpeeds() {
    if (m_session.probing() || m_session.encoders().empty()) {
        // The encoder list is still being made at start-up.
        setSetupStep(tr("Finding the encoders…"), m_setupProgress);
        const quint64 run = m_setupRun;
        QTimer::singleShot(500, this, [this, run] {
            if (run != m_setupRun) return;
            if (!m_session.probing() && m_session.encoders().empty()) {
                m_setupResultNote = tr("No encoder was found on this machine.");
                setupFinish();
                return;
            }
            m_setupSurvey.encoders = m_session.encoders();
            m_setupSurvey.engine = m_session.engineDevice();
            setupMeasureSpeeds();
        });
        return;
    }
    m_setupSurvey.encoders = m_session.encoders();
    m_setupSurvey.engine = m_session.engineDevice();
    const std::vector<presets::SpeedJob> wanted = presets::speedJobs(m_setupSurvey, m_setupChoices);
    if (wanted.empty()) {
        setupFinish();
        return;
    }
    const bool stream = m_setupChoices.scenario == presets::Scenario::Stream;
    std::vector<encoderhelper::BenchJob> jobs;
    for (const presets::SpeedJob &w : wanted) {
        encoderhelper::BenchJob j;
        j.key = w.key;
        j.width = w.width;
        j.height = w.height;
        j.fpsNum = w.fps;
        // A stream's encoder runs at a constant bitrate; a recording's at a constant quality.
        if (stream) j.bitrateKbps = presets::neededKbps(findEncoder(m_setupSurvey.encoders, w.key).codec, w.height, w.fps);
        jobs.push_back(j);
    }
    if (!m_setupBench) m_setupBench = new EncoderBench(this);
    disconnect(m_setupBench, nullptr, this, nullptr);
    const quint64 run = m_setupRun;
    const double base = m_setupProgress, total = double(jobs.size());
    connect(m_setupBench, &EncoderBench::result, this, [this, run, base, total](const encoderhelper::BenchResult &r) {
        if (run != m_setupRun) return;
        m_setupSurvey.speeds.push_back({r.key, r.width, r.height, r.fpsNum / std::max(1, r.fpsDen), r.opened, r.framesPerSecond});
        const double done = double(m_setupSurvey.speeds.size());
        const QString label = QString::fromStdString(findEncoder(m_setupSurvey.encoders, r.key).label);
        setSetupStep(tr("Timed %1 at %2×%3 (%4 of %5)…").arg(label).arg(r.width).arg(r.height).arg(int(done)).arg(int(total)),
                     base + (1 - base) * done / total);
    });
    connect(m_setupBench, &EncoderBench::finished, this, [this, run](bool) {
        if (run == m_setupRun) setupFinish();
    });
    setSetupStep(tr("Timing the encoders…"), base);
    m_setupBench->start(m_setupSurvey.encoders, jobs, 1.0);
}

void Backend::cancelSetupCheck() {
    if (!m_setupBusy) return;
    ++m_setupRun;
    if (m_setupUpload && m_setupUpload->running()) m_setupUpload->cancel();
    if (m_setupBench && m_setupBench->running()) m_setupBench->cancel();
    setupFinish(true);
}

void Backend::setupFinish(bool cancelled) {
    m_setupBusy = false;
    if (cancelled) {
        m_setupResult = {{QStringLiteral("cancelled"), true}};
        setSetupStep({}, 0);
        return;
    }
    m_setupPlan = presets::plan(m_setupSurvey, m_setupChoices);
    QVariantList reasons;
    for (const presets::Reason &r : m_setupPlan.reasons)
        reasons << QVariantMap{{QStringLiteral("what"), r.what}, {QStringLiteral("value"), r.value}, {QStringLiteral("why"), r.why}};
    QStringList warnings = m_setupPlan.warnings;
    if (!m_setupResultNote.isEmpty()) warnings.prepend(m_setupResultNote);
    m_setupResult = {{QStringLiteral("ok"), m_setupPlan.ok}, {QStringLiteral("name"), m_setupPlan.profile.name},
                     {QStringLiteral("reasons"), reasons}, {QStringLiteral("warnings"), warnings}};
    setSetupStep(tr("Done."), 1);
}

bool Backend::applySetup(const QString &name) {
    if (!m_setupPlan.ok || m_setupBusy) return false;
    if (streaming()) return profileError(tr("Stop streaming before setting up a new profile."));
    Profile profile = m_setupPlan.profile;
    const QString wanted = name.trimmed().isEmpty() ? profile.name : name.trimmed().left(120);
    // The stream key goes to the keyring, as Settings → Stream keeps it.
    for (StreamTarget &t : profile.streams) {
        t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (t.key.isEmpty() || !m_secrets.available()) continue;
        QString why;
        if (!m_secrets.store(t.id, streamKeyLabel(t), t.key.toUtf8(), &why))
            return profileError(tr("The keyring did not take the stream key: %1").arg(why));
        t.key.clear();
    }
    presets::Plan named = m_setupPlan;
    named.profile.name = wanted;
    if (!createProfile(profile, wanted)) return false;
    if (!createCollection(starterCollection(named, m_setupChoices))) return false;
    dismissSetupOffer();
    setStatus(tr("Set up for %1.").arg(presets::scenarioName(m_setupChoices.scenario).toLower()));
    return true;
}

void Backend::dismissSetupOffer() {
    if (!m_setupOffered) return;
    m_setupOffered = false;
    // Saving any preference writes global.json, so the next start is not "fresh".
    setPreference(QStringLiteral("setupOffered"), true);
    emit setupChanged();
}
