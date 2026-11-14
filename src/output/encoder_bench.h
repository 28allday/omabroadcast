#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <vector>

#include "output/encoder_helper.h"

// Runs encoder speed tests in the helper process (see encoder_helper.h):
// opening a hardware encoder in this process would stall the frame thread.
// Each job's result is told as it comes; finished() comes once, after the
// last job, a cancel, a timeout or a helper that died.
class EncoderBench : public QObject {
    Q_OBJECT
public:
    explicit EncoderBench(QObject *parent = nullptr);
    ~EncoderBench() override;

    // Starts the jobs (stopping any already running). A job that takes longer
    // than secondsPerJob plus jobTimeoutMs() to answer ends the run.
    void start(const std::vector<EncoderInfo> &encoders, const std::vector<encoderhelper::BenchJob> &jobs,
               double secondsPerJob = 2.0);
    // Stops at once; finished(false) follows.
    void cancel();
    bool running() const { return m_process != nullptr; }
    const std::vector<encoderhelper::BenchResult> &results() const { return m_results; }

    // Grace for opening and closing on top of each job's seconds.
    int jobTimeoutMs() const { return m_graceMs; }
    void setJobTimeoutMs(int ms) { m_graceMs = ms; }

signals:
    void result(const encoderhelper::BenchResult &result);
    // complete: every job answered.
    void finished(bool complete);

private:
    void readLines();
    void end(bool complete);
    void armTimer();

    QPointer<QProcess> m_process;
    QTimer m_timer;
    QByteArray m_buffer;
    std::vector<encoderhelper::BenchResult> m_results;
    size_t m_jobs = 0;
    double m_seconds = 2.0;
    int m_graceMs = 30000;
};
