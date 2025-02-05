/*
 * Copyright (C) 2019 Trinity. All rights reserved.
 * Copyright (C) 2019 Wang LianJie <wlanjie888@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
//
// Created by wlanjie on 2019-06-29.
//

#include "ffmpeg_decode.h"
#include "android_xlog.h"

#define MIN_FRAMES 25
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)

static char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static AVPacket flush_pkt;

int frame_queue_init( FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last) {
    memset(f, 0, sizeof(FrameQueue));
    int result = pthread_mutex_init(&f->mutex, NULL);
    if (result != 0) {
        av_log(NULL, AV_LOG_FATAL, "Frame Init create mutex: %d\n", result);
        return AVERROR(ENOMEM);
    }
    result = pthread_cond_init(&f->cond, NULL);
    if (result != 0) {
        av_log(NULL, AV_LOG_FATAL, "Frame Init create cond: %d\n", result);
        return AVERROR(ENOMEM);
    }
    f->packet_queue = pktq;
    f->max_size = max_size;
    f->keep_last = keep_last;
    for (int i = 0; i < f->max_size; i++) {
        if (!(f->queue[i].frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
    }
    return 0;
}

Frame *frame_queue_peek_writable(FrameQueue *f) {
    pthread_mutex_lock(&f->mutex);
    while (f->size >= f->max_size && !f->packet_queue->abort_request) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);
    if (f->packet_queue->abort_request) {
        return NULL;
    }
    return &f->queue[f->windex];
}

Frame *frame_queue_peek_readable(FrameQueue *f) {
    pthread_mutex_lock(&f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->packet_queue->abort_request) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);
    if (f->packet_queue->abort_request) {
        return NULL;
    }
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}


void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size) {
        f->windex = 0;
    }
    pthread_mutex_lock(&f->mutex);
    f->size++;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

Frame *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/* return the number of undisplayed frames in the queue */
int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

/* return last shown position */
int64_t frame_queue_last_pos(FrameQueue *f) {
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->packet_queue->serial)
        return fp->pos;
    else
        return -1;
}

Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

Frame *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size) {
        f->rindex = 0;
    }
    pthread_mutex_lock(&f->mutex);
    f->size--;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

#ifdef __APPLE__
void free_picture(Frame *vp) {
    if (vp->bmp) {
        SDL_DestroyTexture(vp->bmp);
        vp->bmp = NULL;
    }
}
#endif

void frame_queue_signal(FrameQueue *f) {
    pthread_mutex_lock(&f->mutex);
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

void frame_queue_destory(FrameQueue *f) {
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
#ifdef __APPLE__
        free_picture(vp);
#endif
    }
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
}

int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    int result = pthread_mutex_init(&q->mutex, NULL);
    if (result != 0) {
        av_log(NULL, AV_LOG_FATAL, "Packet Init mutex: %d\n", result);
        return AVERROR(ENOMEM);
    }
    result = pthread_cond_init(&q->cond, NULL);
    if (result != 0) {
        av_log(NULL, AV_LOG_FATAL, "Packet Init cond: %d\n", result);
        return AVERROR(ENOMEM);
    }
    q->abort_request = 0;
    return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList *pkt1;
    int ret;
    pthread_mutex_lock(&q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            if (serial) {
                *serial = pkt1->serial;
            }
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

int packet_queue_put_private(PacketQueue *q, AVPacket *packet) {
    MyAVPacketList *pkt1;
    if (q->abort_request) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1) {
        return AVERROR(ENOMEM);
    }
    pkt1->pkt = *packet;
    pkt1->next = NULL;
    if (packet == &flush_pkt) {
        q->serial++;
    }
    pkt1->serial = q->serial;
    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    pthread_cond_signal(&q->cond);
    return 0;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    int ret;
    pthread_mutex_lock(&q->mutex);
    ret = packet_queue_put_private(q, pkt);
    pthread_mutex_unlock(&q->mutex);
    if (pkt != &flush_pkt && ret < 0) {
        av_packet_unref(pkt);
    }
    return ret;
}

int packet_queue_put_nullpacket(PacketQueue *q, int stream_index) {
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

void packet_queue_start(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    pthread_mutex_unlock(&q->mutex);
}

void packet_queue_abort(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList *pkt, *pkt1;
    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

int configure_filter_graph(AVFilterGraph *graph, const char *filtergraph, AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {
    int ret;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            LOGE("avfilter_inout_alloc failed");
            return AVERROR(ENOMEM);
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = NULL;

        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = NULL;
        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            av_err2str(ret);
            LOGE("avfilter_graph_parse_ptr failed: ret", ret);
            return ret;
        }
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            av_err2str(ret);
            LOGE("avfilter_link failed: %d", ret);
            return ret;
        }
    }
    for (int i = 0; i < graph->nb_filters - nb_filters; i++) {
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);
    }
    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        av_err2str(ret);
        LOGE("avfilter_graph_config failed: %d message: %s", ret, av_err2str(ret));
        return ret;
    }
    return ret;
}

int configure_audio_filters(MediaDecode *media_decode, const char *afilters, int force_outout_format) {
    int ret;
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char asrc_args[256];

    avfilter_graph_free(&media_decode->agraph);
    if (!(media_decode->agraph = avfilter_graph_alloc())) {
        LOGE("avfilter_graph_alloc");
        return AVERROR(ENOMEM);
    }
    ret = snprintf(asrc_args, sizeof(asrc_args), "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   media_decode->audio_filter_src.freq, av_get_sample_fmt_name(media_decode->audio_filter_src.fmt),
                   media_decode->audio_filter_src.channels, 1, media_decode->audio_filter_src.freq);
    if (media_decode->audio_filter_src.channel_layout) {
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret, ":channel_layout=0x%"PRIx64, media_decode->audio_filter_src.channel_layout);
    }
    ret = avfilter_graph_create_filter(&filt_asrc, avfilter_get_by_name("abuffer"), "in", asrc_args, NULL, media_decode->agraph);
    if (ret < 0) {
        LOGE("avfilter_graph_create_filter abuffer failed: %d", ret);
        avfilter_graph_free(&media_decode->agraph);
        av_err2str(ret);
        return ret;
    }
    ret = avfilter_graph_create_filter(&filt_asink, avfilter_get_by_name("abuffersink"), "out", NULL, NULL, media_decode->agraph);
    if (ret < 0) {
        LOGE("avfilter_graph_create_filter abuffersink failed: %d", ret);
        avfilter_graph_free(&media_decode->agraph);
        av_err2str(ret);
        return ret;
    }
    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0) {
        LOGE("av_opt_set_int_list sample_fmts failed: %d", ret);
        avfilter_graph_free(&media_decode->agraph);
        av_err2str(ret);
        return ret;
    }
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0) {
        LOGE("av_opt_set_int all_channel_counts failed: %d", ret);
        avfilter_graph_free(&media_decode->agraph);
        av_err2str(ret);
        return ret;
    }
    if (force_outout_format) {
        channel_layouts[0] = media_decode->audio_filter_src.channel_layout;
        channels[0] = media_decode->audio_filter_src.channels;
        sample_rates[0] = media_decode->audio_filter_src.freq;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0) {
            LOGE("av_opt_set_int_list channel_layout failed: %d", ret);
            avfilter_graph_free(&media_decode->agraph);
            av_err2str(ret);
            return ret;
        }
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0) {
            LOGE("av_opt_set_int_list all_channel_counts failed: %d", ret);
            avfilter_graph_free(&media_decode->agraph);
            av_err2str(ret);
            return ret;
        }
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0) {
            LOGE("av_opt_set_int_list channel_counts failed: %d", ret);
            avfilter_graph_free(&media_decode->agraph);
            av_err2str(ret);
            return ret;
        }
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0) {
            LOGE("av_opt_set_int_list sample_rates failed: %d", ret);
            avfilter_graph_free(&media_decode->agraph);
            av_err2str(ret);
            return ret;
        }
    }
    if ((ret = configure_filter_graph(media_decode->agraph, afilters, filt_asrc, filt_asink)) < 0) {
        LOGE("configure_filtergraph error: %d args: %s", ret, asrc_args);
        avfilter_graph_free(&media_decode->agraph);
        return ret;
    }
    media_decode->in_audio_filter = filt_asrc;
    media_decode->out_audio_filter = filt_asink;
    return ret;
}

int decoder_decode_frame(MediaDecode* media_decode, Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int got_frame = 0;
    int64_t time = 0;
    do {
        int ret = -1;
        if (d->queue->abort_request) {
            return -1;
        }
        if (!d->packet_pending || d->queue->serial != d->pkt_serial) {
            AVPacket pkt;
            do {
                if (d->queue->nb_packets == 0) {
                    pthread_cond_signal(&d->empty_queue_cond);
                }
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0) {
                    return -1;
                }
                if (pkt.data == flush_pkt.data) {
                    avcodec_flush_buffers(d->codec_context);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            } while (pkt.data == flush_pkt.data || d->queue->serial != d->pkt_serial);
            av_packet_unref(&d->pkt);
            d->pkt_temp = d->pkt = pkt;
            d->packet_pending = 1;
        }

        switch (d->codec_context->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                ret = avcodec_decode_video2(d->codec_context, frame, &got_frame, &d->pkt_temp);
                if (got_frame) {
                    frame->pts = frame->pkt_dts;
                    time = (int64_t) (d->pkt_temp.pts * av_q2d(media_decode->video_stream->time_base) * 1000);
                    // 播放到指定时间
                    media_decode->finish = time > media_decode->end_time;
                }
                break;

            case AVMEDIA_TYPE_AUDIO:
                ret = avcodec_decode_audio4(d->codec_context, frame, &got_frame, &d->pkt_temp);
                if (got_frame) {
                    AVRational tb = (AVRational) { 1, frame->sample_rate };
                    if (frame->pts != AV_NOPTS_VALUE) {
                        frame->pts = av_rescale_q(frame->pts, d->codec_context->time_base, tb);
                    } else if (frame->pkt_pts != AV_NOPTS_VALUE) {
                        frame->pts = av_rescale_q(frame->pkt_pts, av_codec_get_pkt_timebase(d->codec_context), tb);
                    } else if (d->next_pts != AV_NOPTS_VALUE) {
                        frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                    }
                    if (frame->pts != AV_NOPTS_VALUE) {
                        d->next_pts = frame->pts + frame->nb_samples;
                        d->next_pts_tb = tb;
                    }
                    time = (int64_t) (d->pkt_temp.pts * av_q2d(media_decode->audio_stream->time_base) * 1000);
                }
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                ret = avcodec_decode_subtitle2(d->codec_context, sub, &got_frame, &d->pkt_temp);
                break;
            default:
                break;
        }
        if (ret < 0) {
            d->packet_pending = 0;
        } else {
            d->pkt_temp.dts = d->pkt_temp.pts = AV_NOPTS_VALUE;
            if (d->pkt_temp.data) {
                if (d->codec_context->codec_type != AVMEDIA_TYPE_AUDIO) {
                    ret = d->pkt_temp.size;
                }
                d->pkt_temp.data += ret;
                d->pkt_temp.size -= ret;
                if (d->pkt_temp.size <= 0) {
                    d->packet_pending = 0;
                }
            } else {
                if (!got_frame) {
                    d->packet_pending = 0;
                    d->finished = d->pkt_serial;
                }
            }
        }

    } while ((!got_frame && !d->finished) || (time < media_decode->start_time && media_decode->precision_seek));
    return got_frame;
}

void decoder_release(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->codec_context);
}

void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    pthread_join(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

int64_t get_valid_channel_layout(int64_t channel_layout, int channels) {
    return (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels) ? channel_layout : 0;
}

int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2) {
    if (channel_count1 == 1 && channel_count2 == 1) {
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    } else {
        return channel_count1 != channel_count2 || fmt1 != fmt2;
    }
}

void* audio_thread(void *arg) {
    MediaDecode *media_decode = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
    int got_frame;
    AVRational tb;
    int ret = 0;
    if (!frame) {
        return NULL;
    }
    do {
        if ((got_frame = decoder_decode_frame(media_decode, &media_decode->audio_decode, frame, NULL)) < 0) {
            LOGE("decoder_decode_frame: %d", got_frame);
            avfilter_graph_free(&media_decode->agraph);
            av_frame_free(&frame);
            return NULL;
        }
        if (got_frame) {
            tb = (AVRational) { 1, frame->sample_rate };
            dec_channel_layout = get_valid_channel_layout(frame->channel_layout, av_frame_get_channels(frame));
            reconfigure = cmp_audio_fmts(media_decode->audio_filter_src.fmt, media_decode->audio_filter_src.channels, frame->format, av_frame_get_channels(frame));
            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(buf1, sizeof(buf1), -1, media_decode->audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       media_decode->audio_filter_src.freq, media_decode->audio_filter_src.channels, av_get_sample_fmt_name(media_decode->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, av_frame_get_channels(frame), av_get_sample_fmt_name(frame->format), buf2, media_decode->audio_decode.pkt_serial);
                media_decode->audio_filter_src.fmt = frame->format;
                media_decode->audio_filter_src.channel_layout = dec_channel_layout;
                media_decode->audio_filter_src.channels = av_frame_get_channels(frame);
                media_decode->audio_filter_src.freq = frame->sample_rate;
                last_serial = media_decode->audio_decode.pkt_serial;
                if ((ret = configure_audio_filters(media_decode, NULL, 1)) < 0) {
                    avfilter_graph_free(&media_decode->agraph);
                    av_frame_free(&frame);
                    return NULL;
                }
            }
            if ((ret = av_buffersrc_add_frame(media_decode->in_audio_filter, frame)) < 0) {
                LOGE("av_buffersrc_add_frame error: %s", av_err2str(ret));
                avfilter_graph_free(&media_decode->agraph);
                av_frame_free(&frame);
                av_err2str(ret);
                return NULL;
            }
            while ((ret = av_buffersink_get_frame_flags(media_decode->out_audio_filter, frame, 0)) >= 0) {
                tb = media_decode->out_audio_filter->inputs[0]->time_base;
                if (!(af = frame_queue_peek_writable(&media_decode->sample_frame_queue))) {
                    avfilter_graph_free(&media_decode->agraph);
                    av_frame_free(&frame);
                    return NULL;
                }
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = av_frame_get_pkt_pos(frame);
                af->serial = media_decode->audio_decode.pkt_serial;
                af->duration = av_q2d((AVRational) { frame->nb_samples, frame->sample_rate });
                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&media_decode->sample_frame_queue);
                if (media_decode->audio_packet_queue.serial != media_decode->audio_decode.pkt_serial) {
                    break;
                }
            }
            if (ret == AVERROR_EOF) {
                media_decode->audio_decode.finished = media_decode->audio_decode.pkt_serial;
            }
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    return NULL;
}

int get_video_frame(MediaDecode *media_decode, AVFrame *frame) {
    int got_picture;
    if ((got_picture = decoder_decode_frame(media_decode, &media_decode->video_decode, frame, NULL)) < 0) {
        return got_picture;
    }
    if (got_picture) {
        double dpts = NAN;
        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(media_decode->video_stream->time_base) * frame->pts;
        }
        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(media_decode->ic, media_decode->video_stream, frame);
    }
    return got_picture;
}

int configure_video_filters(AVFilterGraph *graph, MediaDecode *media_decode, const char *vfilters, AVFrame *frame) {
    int ret;
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    char sws_flags_str[512] = "";
    char buffersrc_args[256] = "";
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = media_decode->video_stream->codecpar;
    AVRational fr = av_guess_frame_rate(media_decode->ic, media_decode->video_stream, NULL);
    av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", "bicubic");
    if (strlen(sws_flags_str)) {
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';
    }
    graph->scale_sws_opts = av_strdup(sws_flags_str);
    snprintf(buffersrc_args, sizeof(buffersrc_args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format, media_decode->video_stream->time_base.num, media_decode->video_stream->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den) {
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);
    }
    if ((ret = avfilter_graph_create_filter(&filt_src, avfilter_get_by_name("buffer"), "ffplay_buffer", buffersrc_args, NULL, graph)) < 0) {
        char* error = av_err2str(ret);
        return ret;
    }
    if ((ret = avfilter_graph_create_filter(&filt_out, avfilter_get_by_name("buffersink"), "ffplay_buffersink", NULL, NULL, graph)) < 0) {
        char* error = av_err2str(ret);
        return ret;
    }
    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0) {
        char* error = av_err2str(ret);
        return ret;
    }
    last_filter = filt_out;
    if ((ret = configure_filter_graph(graph, vfilters, filt_src, last_filter)) < 0) {
        return ret;
    }
    media_decode->in_video_filter = filt_src;
    media_decode->out_video_filter = filt_out;
    return ret;
}

int queue_picture(MediaDecode *media_decode, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    Frame *vp;
    if (!(vp = frame_queue_peek_writable(&media_decode->video_frame_queue))) {
        return -1;
    }
    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

#ifdef __APPLE__
    if (!vp->bmp || !vp->allocated ||
        vp->width != src_frame->width ||
        vp->height != src_frame->height ||
        vp->format != src_frame->format) {
        SDL_Event event;
        vp->allocated = 0;
        vp->reallocated = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;

        /* the allocation must be done in the main thread to avoid
         locking problems. */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);

        /* wait until the picture is allocated */
        pthread_mutex_lock(&is->video_queue.mutex);
        while (!vp->allocated && !is->videoq.abort_request) {
            pthread_cond_wait(&is->video_queue.cond, &is->video_queue.mutex);
        }
        /* if the queue is aborted, we have to pop the pending ALLOC event or wait for the allocation to complete */
        //TODO 2.0
        if (is->videoq.abort_request && SDL_PeepEvents(&event, 1, SDL_GETEVENT, FF_ALLOC_EVENT, FF_ALLOC_EVENT) != 1) {
            while (!vp->allocated && !is->abort_request) {
                pthread_cond_wait(&is->video_queue.cond, &is->video_queue.mutex);
            }
        }
        pthread_mutex_unlock(&is->video_queue.mutex);
        if (is->videoq.abort_request) {
            return -1;
        }
    }
    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;

        av_frame_move_ref(vp->frame, src_frame);
        frame_queue_push(&is->video_queue);
    }
#else
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;
    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&media_decode->video_frame_queue);
#endif
    return 0;
}

void* video_thread(void *arg) {
    int ret;
    MediaDecode *media_decode = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    AVRational tb = media_decode->video_stream->time_base;
    AVRational frame_rate = av_guess_frame_rate(media_decode->ic, media_decode->video_stream, NULL);
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    if (!graph) {
        av_frame_free(&frame);
        return NULL;
    }
    if (!frame) {
        avfilter_graph_free(&graph);
        return NULL;
    }
    while(1) {
        ret = get_video_frame(media_decode, frame);
        if (ret < 0) {
            avfilter_graph_free(&graph);
            av_frame_free(&frame);
            return 0;
        }
        if (!ret) {
            continue;
        }
        if (last_w != frame->width || last_h != frame->height
            || last_format != frame->format
            || last_serial != media_decode->video_decode.pkt_serial
            || last_vfilter_idx != media_decode->video_filter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name((enum AVPixelFormat) frame->format), "none"), media_decode->video_decode.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(graph, media_decode, NULL, frame)) < 0) {
#ifdef __APPLE__
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
#endif
                avfilter_graph_free(&graph);
                av_frame_free(&frame);
                return NULL;
            }
            filt_in = media_decode->in_video_filter;
            filt_out = media_decode->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = media_decode->video_decode.pkt_serial;
            last_vfilter_idx = media_decode->video_filter_idx;
            frame_rate = filt_out->inputs[0]->frame_rate;
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0) {
            av_err2str(ret);
            avfilter_graph_free(&graph);
            av_frame_free(&frame);
            return NULL;
        }
        while (ret >= 0) {
            media_decode->frame_last_returned_time = av_gettime_relative() / 1000000.0;
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    media_decode->video_decode.finished = media_decode->video_decode.pkt_serial;
                }
                ret = 0;
                break;
            }
            media_decode->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - media_decode->frame_last_returned_time;
            if (fabs(media_decode->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0) {
                media_decode->frame_last_filter_delay = 0;
            }
            tb = filt_out->inputs[0]->time_base;
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational) { frame_rate.den, frame_rate.num }) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(media_decode, frame, pts, duration, av_frame_get_pkt_pos(frame), media_decode->video_decode.pkt_serial);
            av_frame_unref(frame);
        }
    }
    return NULL;
}

void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, pthread_cond_t empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->codec_context = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
}

int decoder_start(Decoder *d, void* (*fn) (void*), void *arg) {
    packet_queue_start(d->queue);
    int result = pthread_create(&d->decoder_tid, NULL, fn, arg);
    if (result != 0) {
        av_log(NULL, AV_LOG_ERROR, "create decode thread failed: %d\n", result);
        return AVERROR(ENOMEM);
    }
    return 0;
}

int stream_component_open(MediaDecode *media_decode, int stream_index) {
    int ret;
    AVFormatContext *ic = media_decode->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    AVDictionary *opts = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int stream_lowres = 0;
    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) {
        av_err2str(ret);
        avcodec_free_context(&avctx);
        return ret;
    }

    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);
    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING, "No codec could be found with id %d\n", avctx->codec_id);
        avcodec_free_context(&avctx);
        return AVERROR(ENOMEM);
    }
    avctx->codec_id = codec->id;
    if (stream_lowres > av_codec_get_max_lowres(codec)) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
               av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
#if FF_API_EMU_EDGE
    if (codec->capabilities & AV_CODEC_CAP_DR1) {
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
    }
#endif
    av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres) {
        av_dict_set_int(&opts, "lowers", stream_lowres, 0);
    }
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    }
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        avcodec_free_context(&avctx);
        av_err2str(ret);
        return ret;
    }
    media_decode->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO: {
            AVFilterLink *link;
            media_decode->audio_filter_src.freq = avctx->sample_rate;
            media_decode->audio_filter_src.channels = avctx->channels;
            media_decode->audio_filter_src.channel_layout = avctx->channel_layout;
            media_decode->audio_filter_src.fmt = avctx->sample_fmt;
            if ((ret = configure_audio_filters(media_decode, NULL, 0)) < 0) {
                avcodec_free_context(&avctx);
                LOGE("configure_audio_filters failed: %d", ret);
                return ret;
            }
            link = media_decode->out_audio_filter->inputs[0];
            sample_rate = link->sample_rate;
            channel_layout = link->channel_layout;
            nb_channels = link->channels;
            media_decode->audio_tgt.fmt = AV_SAMPLE_FMT_S16;
            media_decode->audio_tgt.freq = 44100;
            media_decode->audio_tgt.channel_layout = AV_CH_LAYOUT_MONO;
            media_decode->audio_tgt.channels = 1;
            media_decode->audio_tgt.frame_size = av_samples_get_buffer_size(NULL, media_decode->audio_tgt.channels, 1, media_decode->audio_tgt.fmt, 1);
            media_decode->audio_tgt.bytes_per_sec = av_samples_get_buffer_size(NULL, media_decode->audio_tgt.channels, media_decode->audio_tgt.freq, media_decode->audio_tgt.fmt, 1);
            if (media_decode->audio_tgt.bytes_per_sec <= 0 || media_decode->audio_tgt.frame_size <= 0) {
                return -1;
            }
            if (media_decode->audio_event) {
                media_decode->audio_event->on_audio_prepare_event(media_decode->audio_event, ret);
            }
            media_decode->audio_src = media_decode->audio_tgt;
            media_decode->audio_stream_index = stream_index;
            media_decode->audio_stream = ic->streams[stream_index];

            decoder_init(&media_decode->audio_decode, avctx, &media_decode->audio_packet_queue, media_decode->continue_read_thread);
            if ((media_decode->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !media_decode->ic->iformat->read_seek) {
                media_decode->audio_decode.start_pts = media_decode->audio_stream->start_time;
                media_decode->audio_decode.start_pts_tb = media_decode->audio_stream->time_base;
            }
            if ((ret = decoder_start(&media_decode->audio_decode, audio_thread, media_decode)) < 0) {
                avcodec_free_context(&avctx);
                return ret;
            }
#ifdef __APPLE__
            SDL_PauseAudio(0);
#endif
        }
            break;
        case AVMEDIA_TYPE_VIDEO:
            media_decode->video_stream_index = stream_index;
            media_decode->video_stream = ic->streams[stream_index];
            decoder_init(&media_decode->video_decode, avctx, &media_decode->video_packet_queue, media_decode->continue_read_thread);
            if ((ret = decoder_start(&media_decode->video_decode, video_thread, media_decode)) < 0) {
                avcodec_free_context(&avctx);
                return ret;
            }
            media_decode->queue_attachments_req = 1;
            break;

        case AVMEDIA_TYPE_SUBTITLE:
            break;
        default:
            break;
    }
    return ret;
}

static void stream_component_close(MediaDecode *media_decode, int stream_index) {
    AVFormatContext *ic = media_decode->ic;
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    avctx = ic->streams[stream_index]->codec;

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            decoder_abort(&media_decode->audio_decode, &media_decode->sample_frame_queue);
#ifdef __APPLE__
            SDL_CloseAudio();
#endif
            decoder_release(&media_decode->audio_decode);

#ifdef __APPLE__
        if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = NULL;
                is->rdft_bits = 0;
            }
#endif
            break;
        case AVMEDIA_TYPE_VIDEO:
            decoder_abort(&media_decode->video_decode, &media_decode->video_frame_queue);
            decoder_release(&media_decode->video_decode);
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            media_decode->audio_stream = NULL;
            media_decode->audio_stream_index = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            media_decode->video_stream = NULL;
            media_decode->video_stream_index = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
#ifdef __APPLE__
            is->subtitle_st = NULL;
#endif
            media_decode->subtitle_stream_index = -1;
            break;
        default:
            break;
    }
}

void stream_close(MediaDecode *media_decode) {
    LOGI("enter stream close: %s", media_decode->file_name);
    if (!media_decode->file_name) {
        return;
    }
    if (!media_decode->ic) {
        return;
    }
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    media_decode->abort_request = 1;
    pthread_join(media_decode->read_thread, NULL);

    /* close each stream */
    // TODO 考虑没有音频的情况,是否会崩溃
    if (media_decode->audio_stream_index >= 0)
        stream_component_close(media_decode, media_decode->audio_stream_index);
    if (media_decode->video_stream_index >= 0)
        stream_component_close(media_decode, media_decode->video_stream_index);
    if (media_decode->subtitle_stream_index >= 0)
        stream_component_close(media_decode, media_decode->subtitle_stream_index);

    avformat_close_input(&media_decode->ic);

    packet_queue_destroy(&media_decode->video_packet_queue);
    packet_queue_destroy(&media_decode->audio_packet_queue);
    packet_queue_destroy(&media_decode->subtitle_packet_queue);

    /* free all pictures */
    frame_queue_destory(&media_decode->video_frame_queue);
    frame_queue_destory(&media_decode->sample_frame_queue);
    frame_queue_destory(&media_decode->subtitle_frame_queue);
    pthread_cond_destroy(&media_decode->continue_read_thread);
#ifdef __APPLE__
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
#endif
    av_free(media_decode->file_name);
    media_decode->file_name = NULL;
    LOGI("leave stream close");
}

void read_thread_failed(MediaDecode *media_decode, AVFormatContext *ic, pthread_mutex_t wait_mutex) {
    if (ic && !media_decode->ic) {
        avformat_close_input(&ic);
    }

#ifdef __APPLE__
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
#endif
    pthread_mutex_destroy(&wait_mutex);
}

int complete_state(MediaDecode* media_decode) {
    // TODO 会调用多次
    // 后面需要想办法优化
    if (media_decode->state_event && media_decode->state_event->on_complete_event) {
        return media_decode->state_event->on_complete_event(media_decode->state_event);
    }
    return 0;
}

void* read_thread(void *arg) {
    LOGI("enter read_thread ");
    MediaDecode *media_decode = arg;
    AVFormatContext *ic = NULL;
    int ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    pthread_mutex_t wait_mutex;
    ret = pthread_mutex_init(&wait_mutex, NULL);
    int64_t pkt_ts;
    if (ret != 0) {
        av_log(NULL, AV_LOG_FATAL, "Read Thread mutex: %d\n", ret);
        read_thread_failed(media_decode, NULL, wait_mutex);
        return NULL;
    }
    memset(st_index, -1, sizeof(st_index));
    media_decode->eof = 0;
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Can't allocate context.\n");
        read_thread_failed(media_decode, ic, wait_mutex);
        return NULL;
    }
    ret = avformat_open_input(&ic, media_decode->file_name, NULL, NULL);
    if (ret < 0) {
        read_thread_failed(media_decode, ic, wait_mutex);
        LOGE("open: %s error: %s", media_decode->file_name, av_err2str(ret));
        return NULL;
    }
    media_decode->ic = ic;
    av_format_inject_global_side_data(ic);
    media_decode->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    /* 如果start_time != AV_NOPTS_VALUE,则从start_time位置开始播放 */
    if (media_decode->start_time != 0) {
        int64_t timestamp = media_decode->start_time;

        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE) {
            timestamp += ic->start_time;
        }
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp * (AV_TIME_BASE / 1000), INT64_MAX, 0);
        if (ret < 0) {
            av_err2str(ret);
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   media_decode->file_name, (double)timestamp / AV_TIME_BASE);
        }
    }

    av_dump_format(ic, 0, media_decode->file_name, 0);

    for (int i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (wanted_stream_spec[type] && st_index[i] == -1) {
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0) {
                st_index[type] = i;
            }
        }

    }
    for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO],
                                                       st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
    st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
                                                          (st_index[AVMEDIA_TYPE_AUDIO] > 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
                                                          NULL, 0);

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        ret = stream_component_open(media_decode, st_index[AVMEDIA_TYPE_AUDIO]);
    }
    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(media_decode, st_index[AVMEDIA_TYPE_VIDEO]);
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(media_decode, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }
    if (media_decode->video_stream < 0 && media_decode->audio_stream < 0) {
        read_thread_failed(media_decode, ic, wait_mutex);
        return NULL;
    }
    while (!media_decode->abort_request) {
        if (media_decode->paused != media_decode->last_paused) {
            media_decode->last_paused = media_decode->paused;
            if (media_decode->paused) {
                media_decode->read_pause_return = av_read_pause(ic);
            } else {
                av_read_play(ic);
            }
        }
        if (media_decode->seek_req) {
            int64_t seek_target = media_decode->seek_pos * (AV_TIME_BASE / 1000);
            int64_t seek_min = media_decode->seek_rel > 0 ? seek_target - media_decode->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = media_decode->seek_rel < 0 ? seek_target - media_decode->seek_rel - 2 : INT64_MAX;

            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables
//            ret = av_seek_frame(ic, -1,  seek_target * (AV_TIME_BASE / 1000), AVSEEK_FLAG_BACKWARD);
            ret = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max,  0 & (~AVSEEK_FLAG_BYTE));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", media_decode->file_name);
            } else {
                if (media_decode->audio_stream_index >= 0) {
                    packet_queue_flush(&media_decode->audio_packet_queue);
                    packet_queue_put(&media_decode->audio_packet_queue, &flush_pkt);
                }
                if (media_decode->video_stream_index >= 0) {
                    packet_queue_flush(&media_decode->video_packet_queue);
                    packet_queue_put(&media_decode->video_packet_queue, &flush_pkt);
                }
                if (media_decode->subtitle_stream_index >= 0) {
                    packet_queue_flush(&media_decode->subtitle_packet_queue);
                    packet_queue_put(&media_decode->subtitle_packet_queue, &flush_pkt);
                }
            }
            media_decode->seek_req = 0;
            media_decode->queue_attachments_req = 1;
            media_decode->eof = 0;
            if (media_decode->seek_event) {
                media_decode->seek_event->on_seek_event(media_decode->seek_event, media_decode->seek_flags);
            }
        }
        if (media_decode->queue_attachments_req) {
            if (media_decode->video_stream && media_decode->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_copy_packet(&copy, &media_decode->video_stream->attached_pic)) < 0) {
                    read_thread_failed(media_decode, ic, wait_mutex);
                }
                packet_queue_put(&media_decode->video_packet_queue, &copy);
                packet_queue_put_nullpacket(&media_decode->video_packet_queue, media_decode->video_stream);
            }
            media_decode->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if ((media_decode->audio_packet_queue.size + media_decode->video_packet_queue.size + media_decode->subtitle_packet_queue.size > MAX_QUEUE_SIZE ||
             ((media_decode->audio_packet_queue.nb_packets > MIN_FRAMES || media_decode->audio_stream < 0 || media_decode->audio_packet_queue.abort_request) &&
              (media_decode->video_packet_queue.nb_packets > MIN_FRAMES || media_decode->video_stream < 0 || media_decode->video_packet_queue.abort_request ||
               (media_decode->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) &&
              (media_decode->subtitle_packet_queue.nb_packets > MIN_FRAMES || media_decode->subtitle_stream_index < 0 || media_decode->subtitle_packet_queue.abort_request)))) {
            /* wait 10 ms */
            pthread_mutex_lock(&wait_mutex);
//            SDL_CondWaitTimeout(media_decode->continue_read_thread, wait_mutex, 10);
            pthread_mutex_unlock(&wait_mutex);
            continue;
        }
//        LOGE("video_finished: %d serial: %d audio_finish: %d serial: %d", media_decode->video_decode.finished, media_decode->video_packet_queue.serial, media_decode->audio_decode.finished, media_decode->audio_packet_queue.serial);
        if (!media_decode->paused &&
            (!media_decode->video_stream || (media_decode->video_decode.finished == media_decode->video_packet_queue.serial && frame_queue_nb_remaining(&media_decode->video_frame_queue) == 0))) {
            media_decode->paused = 0;
            media_decode->audio_decode.finished = 0;
            media_decode->video_decode.finished = 0;
            LOGI("player finish");
            int exit = complete_state(media_decode);
            // 是否退出
            if (exit) {
                LOGE("player finish exit");
                return NULL;
            }
        }
        // 播放到指定时间
        // 如果是循环播放 seek到开始时间
        if (media_decode->finish) {
            media_decode->finish = 0;
            int exit = complete_state(media_decode);
            LOGI("media_decode->finish");
            if (exit) {
                LOGE("complete state exit");
                return NULL;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !media_decode->eof) {
                if (media_decode->video_stream_index >= 0) {
                    packet_queue_put_nullpacket(&media_decode->video_packet_queue, media_decode->video_stream_index);
                }
                if (media_decode->audio_stream_index >= 0) {
                    packet_queue_put_nullpacket(&media_decode->audio_packet_queue, media_decode->audio_stream_index);
                }
                if (media_decode->subtitle_stream_index >= 0) {
                    packet_queue_put_nullpacket(&media_decode->subtitle_packet_queue, media_decode->subtitle_stream_index);
                }
                media_decode->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                break;
            }
            continue;
        } else {
            media_decode->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        // TODO
        int64_t duration = AV_NOPTS_VALUE;
        pkt_in_play_range = duration == AV_NOPTS_VALUE || (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                                                          av_q2d(ic->streams[pkt->stream_index]->time_base) -
                                                          (double)(media_decode->start_time != 0 ? media_decode->start_time : 0) / 1000000 <= ((double) duration / 1000000);
        if (pkt->stream_index == media_decode->audio_stream_index && pkt_in_play_range) {
            packet_queue_put(&media_decode->audio_packet_queue, pkt);
        } else if (pkt->stream_index == media_decode->video_stream_index && pkt_in_play_range && !(media_decode->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&media_decode->video_packet_queue, pkt);
        } else if (pkt->stream_index == media_decode->subtitle_stream_index && pkt_in_play_range) {
            packet_queue_put(&media_decode->subtitle_packet_queue, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    LOGE("read thread exit");
    pthread_exit(0);
}

int stream_open(MediaDecode* media_decode, const char *filename) {
    LOGE("stream open: %s", filename);
    media_decode->file_name = av_strdup(filename);
    if (!media_decode->file_name) {
        stream_close(media_decode);
        return -1;
    }
    if (frame_queue_init(&media_decode->video_frame_queue, &media_decode->video_packet_queue, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) {
        stream_close(media_decode);
        return -1;
    }
    if (frame_queue_init(&media_decode->subtitle_frame_queue, &media_decode->subtitle_packet_queue, SUBPICTURE_QUEUE_SIZE, 0) < 0) {
        stream_close(media_decode);
        return -1;
    }
    if (frame_queue_init(&media_decode->sample_frame_queue, &media_decode->audio_packet_queue, SAMPLE_QUEUE_SIZE, 1) < 0) {
        stream_close(media_decode);
        return -1;
    }
    if (packet_queue_init(&media_decode->video_packet_queue) < 0 ||
        packet_queue_init(&media_decode->audio_packet_queue) < 0 ||
        packet_queue_init(&media_decode->subtitle_packet_queue) < 0) {
        stream_close(media_decode);
        return -1;
    }
    pthread_cond_init(&media_decode->continue_read_thread, NULL);
    int result = pthread_create(&media_decode->read_thread, NULL, read_thread, media_decode);
    if (result != 0) {
        stream_close(media_decode);
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %d\n", result);
        return -1;
    }
    return 0;
}

// 开始解码
int av_decode_start(MediaDecode* decode, const char* file_name) {
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t*) &flush_pkt;
    int result = stream_open(decode, file_name);
    return result;
}

/* seek in the stream */
void stream_seek(MediaDecode *media_decode, int64_t pos, int64_t rel, int seek_by_bytes) {
    if (!media_decode->seek_req) {
        media_decode->seek_pos = pos;
        media_decode->seek_rel = rel;
        media_decode->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes) {
            media_decode->seek_flags |= AVSEEK_FLAG_BYTE;
        }
        media_decode->seek_req = 1;
        pthread_cond_signal(&media_decode->continue_read_thread);
    }
}

// seek到某个时间点, 以毫秒为准
void av_seek(MediaDecode* decode, int64_t time) {
    stream_seek(decode, time, 0, 0);
}

// 释放解码资源
void av_decode_destroy(MediaDecode* decode) {
    if (decode) {
        stream_close(decode);
    }
}
