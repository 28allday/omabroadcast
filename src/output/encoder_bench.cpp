#include "encoder_bench.h"

EncoderBench::EncoderBench(QObject *parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { end(false); });
}

EncoderBench::~EncoderBench() {
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
        delete m_process;
    }
}

void EncoderBench::start(const std::vector<EncoderInfo> &encoders, const std::vector<encoderhelper::BenchJob> &jobs,
                         double secondsPerJob) {
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
        delete m_process;
    }
    m_results.clear();
    m_buffer.clear();
    m_jobs = jobs.size();
    m_seconds = secondsPerJob;
    if (jobs.empty()) {
        QMetaObject::invokeMethod(this, [this] { emit finished(true); }, Qt::QueuedConnection);
        return;
    }
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (process != m_process) return;
        m_buffer += process->readAllStandardOutput();
        readLines();
    });
    connect(process, &QProcess::finished, this, [this, process] {
        if (process != m_process) return;
        m_buffer += process->readAllStandardOutput();
        if (!m_buffer.endsWith('\n')) m_buffer += '\n';
        readLines();
        end(m_results.size() == m_jobs);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_process || error != QProcess::FailedToStart) return;  // the others end in finished()
        end(false);
    });
    process->start(encoderhelper::program(), encoderhelper::benchArguments(secondsPerJob));
    process->write(encoderhelper::benchInput(encoders, jobs));
    process->closeWriteChannel();
    armTimer();
}

void EncoderBench::cancel() {
    if (m_process) end(false);
}

void EncoderBench::armTimer() { m_timer.start(int(m_seconds * 1000) + m_graceMs); }

void EncoderBench::readLines() {
    for (int at; (at = m_buffer.indexOf('\n')) >= 0;) {
        const QByteArray line = m_buffer.left(at);
        m_buffer.remove(0, at + 1);
        encoderhelper::BenchResult r;
        if (!encoderhelper::readBenchLine(line, &r)) continue;
        m_results.push_back(r);
        armTimer();
        emit result(r);
    }
}

void EncoderBench::end(bool complete) {
    m_timer.stop();
    if (QProcess *process = m_process) {
        m_process = nullptr;
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
        process->deleteLater();
    }
    emit finished(complete);
}
