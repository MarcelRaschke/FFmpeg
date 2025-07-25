/*
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2011 Mina Nagy Zaki
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * resampling audio filter
 */

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/downmix_info.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libswresample/swresample.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"

typedef struct AResampleContext {
    const AVClass *class;
    int sample_rate_arg;
    double ratio;
    struct SwrContext *swr;
    int64_t next_pts;
    int more_data;
} AResampleContext;

static av_cold int preinit(AVFilterContext *ctx)
{
    AResampleContext *aresample = ctx->priv;

    aresample->next_pts = AV_NOPTS_VALUE;
    aresample->swr = swr_alloc();
    if (!aresample->swr)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AResampleContext *aresample = ctx->priv;
    swr_free(&aresample->swr);
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const AResampleContext *aresample = ctx->priv;
    enum AVSampleFormat out_format;
    AVChannelLayout out_layout = { 0 };
    int64_t out_rate;

    AVFilterFormats        *in_formats, *out_formats;
    AVFilterFormats        *in_samplerates, *out_samplerates;
    AVFilterChannelLayouts *in_layouts, *out_layouts;
    int ret;

    if (aresample->sample_rate_arg > 0)
        av_opt_set_int(aresample->swr, "osr", aresample->sample_rate_arg, 0);
    av_opt_get_sample_fmt(aresample->swr, "osf", 0, &out_format);
    av_opt_get_int(aresample->swr, "osr", 0, &out_rate);

    in_formats      = ff_all_formats(AVMEDIA_TYPE_AUDIO);
    if ((ret = ff_formats_ref(in_formats, &cfg_in[0]->formats)) < 0)
        return ret;

    in_samplerates  = ff_all_samplerates();
    if ((ret = ff_formats_ref(in_samplerates, &cfg_in[0]->samplerates)) < 0)
        return ret;

    in_layouts      = ff_all_channel_counts();
    if ((ret = ff_channel_layouts_ref(in_layouts, &cfg_in[0]->channel_layouts)) < 0)
        return ret;

    if(out_rate > 0) {
        int ratelist[] = { out_rate, -1 };
        out_samplerates = ff_make_format_list(ratelist);
    } else {
        out_samplerates = ff_all_samplerates();
    }

    if ((ret = ff_formats_ref(out_samplerates, &cfg_out[0]->samplerates)) < 0)
        return ret;

    if(out_format != AV_SAMPLE_FMT_NONE) {
        int formatlist[] = { out_format, -1 };
        out_formats = ff_make_format_list(formatlist);
    } else
        out_formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
    if ((ret = ff_formats_ref(out_formats, &cfg_out[0]->formats)) < 0)
        return ret;

    av_opt_get_chlayout(aresample->swr, "ochl", 0, &out_layout);
    if (av_channel_layout_check(&out_layout)) {
        const AVChannelLayout layout_list[] = { out_layout, { 0 } };
        out_layouts = ff_make_channel_layout_list(layout_list);
    } else
        out_layouts = ff_all_channel_counts();
    av_channel_layout_uninit(&out_layout);

    return ff_channel_layouts_ref(out_layouts, &cfg_out[0]->channel_layouts);
}

#define SWR_CH_MAX 64

static int config_output(AVFilterLink *outlink)
{
    int ret;
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    AResampleContext *aresample = ctx->priv;
    AVChannelLayout out_layout = { 0 };
    int64_t out_rate;
    const AVFrameSideData *sd;
    enum AVSampleFormat out_format;
    char inchl_buf[128], outchl_buf[128];

    ret = swr_alloc_set_opts2(&aresample->swr,
                              &outlink->ch_layout, outlink->format, outlink->sample_rate,
                              &inlink->ch_layout, inlink->format, inlink->sample_rate,
                                         0, ctx);
    if (ret < 0)
        return ret;

    sd = av_frame_side_data_get(inlink->side_data, inlink->nb_side_data,
                                AV_FRAME_DATA_DOWNMIX_INFO);
    if (sd) {
        const AVDownmixInfo *di = (AVDownmixInfo *)sd->data;
        enum AVMatrixEncoding matrix_encoding = AV_MATRIX_ENCODING_NONE;
        double center_mix_level, surround_mix_level;

        switch (di->preferred_downmix_type) {
        case AV_DOWNMIX_TYPE_LTRT:
            matrix_encoding    = AV_MATRIX_ENCODING_DOLBY;
            center_mix_level   = di->center_mix_level_ltrt;
            surround_mix_level = di->surround_mix_level_ltrt;
            break;
        case AV_DOWNMIX_TYPE_DPLII:
            matrix_encoding    = AV_MATRIX_ENCODING_DPLII;
            center_mix_level   = di->center_mix_level_ltrt;
            surround_mix_level = di->surround_mix_level_ltrt;
            break;
        default:
            center_mix_level   = di->center_mix_level;
            surround_mix_level = di->surround_mix_level;
            break;
        }

        av_log(ctx, AV_LOG_VERBOSE, "Mix levels: center %f - "
               "surround %f - lfe %f.\n",
               center_mix_level, surround_mix_level, di->lfe_mix_level);

        av_opt_set_double(aresample->swr, "clev", center_mix_level, 0);
        av_opt_set_double(aresample->swr, "slev", surround_mix_level, 0);
        av_opt_set_double(aresample->swr, "lfe_mix_level", di->lfe_mix_level, 0);
        av_opt_set_int(aresample->swr, "matrix_encoding", matrix_encoding, 0);

        if (av_channel_layout_compare(&outlink->ch_layout, &out_layout))
            av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                                      AV_FRAME_DATA_DOWNMIX_INFO);
    }

    ret = swr_init(aresample->swr);
    if (ret < 0)
        return ret;

    av_opt_get_int(aresample->swr, "osr", 0, &out_rate);
    av_opt_get_chlayout(aresample->swr, "ochl", 0, &out_layout);
    av_opt_get_sample_fmt(aresample->swr, "osf", 0, &out_format);
    outlink->time_base = (AVRational) {1, out_rate};

    av_assert0(outlink->sample_rate == out_rate);
    av_assert0(!av_channel_layout_compare(&outlink->ch_layout, &out_layout));
    av_assert0(outlink->format == out_format);

    av_channel_layout_uninit(&out_layout);

    aresample->ratio = (double)outlink->sample_rate / inlink->sample_rate;

    av_channel_layout_describe(&inlink ->ch_layout, inchl_buf,  sizeof(inchl_buf));
    av_channel_layout_describe(&outlink->ch_layout, outchl_buf, sizeof(outchl_buf));

    av_log(ctx, AV_LOG_VERBOSE, "ch:%d chl:%s fmt:%s r:%dHz -> ch:%d chl:%s fmt:%s r:%dHz\n",
           inlink ->ch_layout.nb_channels, inchl_buf,  av_get_sample_fmt_name(inlink->format),  inlink->sample_rate,
           outlink->ch_layout.nb_channels, outchl_buf, av_get_sample_fmt_name(outlink->format), outlink->sample_rate);
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *insamplesref, AVFrame **outsamplesref_ret)
{
    AVFilterContext *ctx = inlink->dst;
    AResampleContext *aresample = ctx->priv;
    const int n_in  = insamplesref->nb_samples;
    int64_t delay;
    int n_out       = n_in * aresample->ratio + 32;
    AVFilterLink *const outlink = inlink->dst->outputs[0];
    AVFrame *outsamplesref;
    int ret;

    *outsamplesref_ret = NULL;
    delay = swr_get_delay(aresample->swr, outlink->sample_rate);
    if (delay > 0)
        n_out += FFMIN(delay, FFMAX(4096, n_out));

    outsamplesref = ff_get_audio_buffer(outlink, n_out);
    if (!outsamplesref)
        return AVERROR(ENOMEM);

    av_frame_copy_props(outsamplesref, insamplesref);
    outsamplesref->format                = outlink->format;
    ret = av_channel_layout_copy(&outsamplesref->ch_layout, &outlink->ch_layout);
    if (ret < 0) {
        av_frame_free(&outsamplesref);
        return ret;
    }
    outsamplesref->sample_rate           = outlink->sample_rate;

    if (av_channel_layout_compare(&outsamplesref->ch_layout, &insamplesref->ch_layout))
        av_frame_side_data_remove_by_props(&outsamplesref->side_data, &outsamplesref->nb_side_data,
                                           AV_SIDE_DATA_PROP_CHANNEL_DEPENDENT);

    if(insamplesref->pts != AV_NOPTS_VALUE) {
        int64_t inpts = av_rescale(insamplesref->pts, inlink->time_base.num * (int64_t)outlink->sample_rate * inlink->sample_rate, inlink->time_base.den);
        int64_t outpts= swr_next_pts(aresample->swr, inpts);
        aresample->next_pts =
        outsamplesref->pts  = ROUNDED_DIV(outpts, inlink->sample_rate);
    } else {
        outsamplesref->pts  = AV_NOPTS_VALUE;
    }
    n_out = swr_convert(aresample->swr, outsamplesref->extended_data, n_out,
                                 (void *)insamplesref->extended_data, n_in);
    if (n_out <= 0) {
        av_frame_free(&outsamplesref);
        return 0;
    }

    aresample->more_data = outsamplesref->nb_samples == n_out; // Indicate that there is probably more data in our buffers

    outsamplesref->nb_samples  = n_out;

    *outsamplesref_ret = outsamplesref;
    return 1;
}

static int flush_frame(AVFilterLink *outlink, int final, AVFrame **outsamplesref_ret)
{
    AVFilterContext *ctx = outlink->src;
    AResampleContext *aresample = ctx->priv;
    AVFilterLink *const inlink = outlink->src->inputs[0];
    AVFrame *outsamplesref;
    int n_out = 4096;
    int64_t pts;

    outsamplesref = ff_get_audio_buffer(outlink, n_out);
    *outsamplesref_ret = outsamplesref;
    if (!outsamplesref)
        return AVERROR(ENOMEM);

    pts = swr_next_pts(aresample->swr, INT64_MIN);
    pts = ROUNDED_DIV(pts, inlink->sample_rate);

    n_out = swr_convert(aresample->swr, outsamplesref->extended_data, n_out, final ? NULL : (void*)outsamplesref->extended_data, 0);
    if (n_out <= 0) {
        av_frame_free(&outsamplesref);
        return n_out;
    }

    outsamplesref->sample_rate = outlink->sample_rate;
    outsamplesref->nb_samples  = n_out;

    outsamplesref->pts = pts;

    return 1;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AResampleContext *aresample = ctx->priv;
    AVFrame *frame;
    int ret = 0, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    // First try to get data from the internal buffers
    if (aresample->more_data) {
        AVFrame *outsamplesref;

        ret = flush_frame(outlink, 0, &outsamplesref);
        if (ret < 0)
            return ret;
        if (ret > 0)
            return ff_filter_frame(outlink, outsamplesref);
    }
    aresample->more_data = 0;

    // Then consume frames from inlink
    while ((ret = ff_inlink_consume_frame(inlink, &frame))) {
        AVFrame *outsamplesref;
        if (ret < 0)
            return ret;

        ret = filter_frame(inlink, frame, &outsamplesref);
        av_frame_free(&frame);
        if (ret < 0)
            return ret;
        if (ret > 0)
            return ff_filter_frame(outlink, outsamplesref);
    }

    // If we hit the end flush
    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        AVFrame *outsamplesref;

        ret = flush_frame(outlink, 1, &outsamplesref);
        if (ret < 0)
            return ret;
        if (ret > 0)
            return ff_filter_frame(outlink, outsamplesref);
        ff_outlink_set_status(outlink, status, aresample->next_pts);
        return 0;
    }

    // If not, request more data from the input
    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static const AVClass *resample_child_class_iterate(void **iter)
{
    const AVClass *c = *iter ? NULL : swr_get_class();
    *iter = (void*)(uintptr_t)c;
    return c;
}

static void *resample_child_next(void *obj, void *prev)
{
    AResampleContext *s = obj;
    return prev ? NULL : s->swr;
}

#define OFFSET(x) offsetof(AResampleContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption options[] = {
    {"sample_rate", NULL, OFFSET(sample_rate_arg), AV_OPT_TYPE_INT, {.i64=0},  0,        INT_MAX, FLAGS },
    {NULL}
};

static const AVClass aresample_class = {
    .class_name       = "aresample",
    .item_name        = av_default_item_name,
    .option           = options,
    .version          = LIBAVUTIL_VERSION_INT,
    .child_class_iterate = resample_child_class_iterate,
    .child_next       = resample_child_next,
};

static const AVFilterPad aresample_outputs[] = {
    {
        .name          = "default",
        .config_props  = config_output,
        .type          = AVMEDIA_TYPE_AUDIO,
    },
};

const FFFilter ff_af_aresample = {
    .p.name        = "aresample",
    .p.description = NULL_IF_CONFIG_SMALL("Resample audio data."),
    .p.priv_class  = &aresample_class,
    .preinit       = preinit,
    .activate      = activate,
    .uninit        = uninit,
    .priv_size     = sizeof(AResampleContext),
    FILTER_INPUTS(ff_audio_default_filterpad),
    FILTER_OUTPUTS(aresample_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
