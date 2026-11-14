#include "tempo.h"
#include "format.h"
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}
#include <cmath>
#include <cstdio>
#include <cstring>

AudioTempo::~AudioTempo() { close(); }
void AudioTempo::close() { avfilter_graph_free(&m_graph); m_input = m_output = nullptr; m_frames = 0; }
bool AudioTempo::open(int rate, int channels, double speed) {
    close(); m_rate = rate; m_channels = channels;
    m_graph = avfilter_graph_alloc();
    if (!m_graph) return false;
    m_graph->nb_threads = 1;
    char args[256];
    std::snprintf(args,sizeof args,"time_base=1/%d:sample_rate=%d:sample_fmt=flt:channel_layout=%s",rate,rate,AudioFormat::layoutName(channels));
    if (avfilter_graph_create_filter(&m_input,avfilter_get_by_name("abuffer"),"input",args,nullptr,m_graph)<0 ||
        avfilter_graph_create_filter(&m_output,avfilter_get_by_name("abuffersink"),"output",nullptr,nullptr,m_graph)<0) { close(); return false; }
    // Keep each stage in [0.5,2] for the overlap/add path at every offered speed.
    const int stages = speed < .5 || speed > 2 ? 2 : 1;
    const double factor = stages == 2 ? std::sqrt(speed) : speed;
    auto *previous = m_input;
    for (int i=0;i<stages;++i) {
        AVFilterContext *filter = nullptr;
        char name[32]; std::snprintf(name,sizeof name,"tempo%d",i);
        std::snprintf(args,sizeof args,"tempo=%.12f",factor);
        if (avfilter_graph_create_filter(&filter,avfilter_get_by_name("atempo"),name,args,nullptr,m_graph)<0 || avfilter_link(previous,0,filter,0)<0) { close(); return false; }
        previous = filter;
    }
    if (avfilter_link(previous,0,m_output,0)<0 || avfilter_graph_config(m_graph,nullptr)<0) { close(); return false; }
    return true;
}
bool AudioTempo::push(const float *samples,int frames) {
    if (!m_input) return false;
    if (!samples) return av_buffersrc_add_frame_flags(m_input,nullptr,0)>=0;
    auto *frame = av_frame_alloc();
    if (!frame) return false;
    frame->format = AV_SAMPLE_FMT_FLT; frame->sample_rate = m_rate;
    frame->nb_samples = frames; frame->pts = m_frames;
    av_channel_layout_from_string(&frame->ch_layout,AudioFormat::layoutName(m_channels));
    bool ok = av_frame_get_buffer(frame,0)>=0;
    if (ok) {
        std::memcpy(frame->data[0],samples,size_t(frames)*m_channels*sizeof(float));
        ok = av_buffersrc_add_frame_flags(m_input,frame,0)>=0;
    }
    av_frame_free(&frame);
    if (ok) m_frames += frames;
    return ok;
}
int AudioTempo::take(std::vector<float> &samples) {
    auto *frame = av_frame_alloc(); if (!frame) return 0;
    int frames = 0;
    if (m_output && av_buffersink_get_frame(m_output,frame)>=0) {
        frames = frame->nb_samples;
        samples.resize(size_t(frames)*m_channels);
        std::memcpy(samples.data(),frame->data[0],samples.size()*sizeof(float));
    }
    av_frame_free(&frame); return frames;
}
