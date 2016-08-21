/*
 *  tvheadend - Transcoding
 *
 *  Copyright (C) 2016 Tvheadend
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "vaapi.h"

#include <libavcodec/vaapi.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>

#include <va/va.h>


static AVBufferRef *hw_device_ref = NULL;


typedef struct tvh_vaapi_context_t {
    struct vaapi_context;
    VAEntrypoint entrypoint;
    enum AVPixelFormat io_format;
    enum AVPixelFormat sw_format;
    int width;
    int height;
    AVBufferRef *hw_device_ref;
    AVBufferRef *hw_frames_ref;
} TVHVAContext;


static int
tvhva_init()
{
    static const char *renderD_fmt = "/dev/dri/renderD%d";
    static const char *card_fmt = "/dev/dri/card%d";
    int i, dev_num;
    char device[32];

    //search for valid graphics device
    for (i = 0; i < 6; i++) {
        memset(device, 0, sizeof(device));
        if (i < 3) {
            dev_num = i + 128;
            snprintf(device, sizeof(device), renderD_fmt, dev_num);
        }
        else {
            dev_num = i - 3;
            snprintf(device, sizeof(device), card_fmt, dev_num);
        }
        tvhdebug(LS_VAAPI, "trying device: %s", device);
        if (av_hwdevice_ctx_create(&hw_device_ref, AV_HWDEVICE_TYPE_VAAPI,
                                   device, NULL, 0)) {
            tvhdebug(LS_VAAPI,
                     "failed to create a context for device: %s", device);
            continue;
        }
        else {
            tvhinfo(LS_VAAPI,
                    "succesful context creation for device: %s", device);
            return 0;
        }
    }
    tvherror(LS_VAAPI, "failed to find suitable VAAPI device");
    return -1;
}


static void
tvhva_done()
{
    if (hw_device_ref) {
        av_buffer_unref(&hw_device_ref);
        hw_device_ref = NULL;
    }
}


/* TVHVAContext ============================================================= */

static void
tvhva_context_destroy(TVHVAContext *self)
{
    if (self) {
        if (self->context_id != VA_INVALID_ID) {
            vaDestroyContext(self->display, self->context_id);
            self->context_id = VA_INVALID_ID;
        }
        if (self->config_id != VA_INVALID_ID) {
            vaDestroyConfig(self->display, self->config_id);
            self->config_id = VA_INVALID_ID;
        }
        self->display = NULL;
        if (self->hw_frames_ref) {
            av_buffer_unref(&self->hw_frames_ref);
            self->hw_frames_ref = NULL;
        }
        if (self->hw_device_ref) {
            av_buffer_unref(&self->hw_device_ref);
            self->hw_device_ref = NULL;
        }
        free(self);
        self = NULL;
    }
}


static VADisplay *
tvhva_context_display(TVHVAContext *self)
{
    AVHWDeviceContext *hw_device_ctx = NULL;

    if (!hw_device_ref && tvhva_init()) {
        return NULL;
    }
    if (!(self->hw_device_ref = av_buffer_ref(hw_device_ref))) {
        return NULL;
    }
    hw_device_ctx = (AVHWDeviceContext*)self->hw_device_ref->data;
    return ((AVVAAPIDeviceContext *)hw_device_ctx->hwctx)->display;
}


static VAProfile
tvhva_context_profile(TVHVAContext *self, AVCodecContext *avctx)
{
    VAStatus va_res = VA_STATUS_ERROR_UNKNOWN;
    VAProfile profile = VAProfileNone, check = VAProfileNone, *profiles = NULL;
    int i, j, profiles_max, profiles_len;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_MPEG2VIDEO:
            switch (avctx->profile) {
                case FF_PROFILE_UNKNOWN:
                case FF_PROFILE_MPEG2_MAIN:
                    check = VAProfileMPEG2Main;
                    break;
                case FF_PROFILE_MPEG2_SIMPLE:
                    check = VAProfileMPEG2Simple;
                    break;
                default:
                    break;
            }
            break;
        case AV_CODEC_ID_H264:
            switch (avctx->profile) {
                case FF_PROFILE_UNKNOWN:
                case FF_PROFILE_H264_HIGH:
                    check = VAProfileH264High;
                    break;
                case FF_PROFILE_H264_BASELINE:
                    check = VAProfileH264Baseline;
                    break;
                case FF_PROFILE_H264_CONSTRAINED_BASELINE:
                    check = VAProfileH264ConstrainedBaseline;
                    break;
                case FF_PROFILE_H264_MAIN:
                    check = VAProfileH264Main;
                    break;
                default:
                    break;
            }
            break;
        case AV_CODEC_ID_HEVC:
            switch (avctx->profile) {
                case FF_PROFILE_UNKNOWN:
                case FF_PROFILE_HEVC_MAIN:
                    check = VAProfileHEVCMain;
                    break;
                case FF_PROFILE_HEVC_MAIN_10:
                    check = VAProfileHEVCMain10;
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    if (check != VAProfileNone &&
        (profiles_max = vaMaxNumProfiles(self->display)) > 0 &&
        (profiles = calloc(profiles_max, sizeof(VAProfile)))) {
        for (j = 0; j < profiles_max; j++) {
            profiles[j] = VAProfileNone;
        }
        va_res = vaQueryConfigProfiles(self->display,
                                       profiles, &profiles_len);
        if (va_res == VA_STATUS_SUCCESS) {
            for (i = 0; i < profiles_len; i++) {
                if (profiles[i] == check) {
                    profile = check;
                    break;
                }
            }
        }
        free(profiles);
    }

    return profile;
}


static int
tvhva_context_check_profile(TVHVAContext *self, VAProfile profile)
{
    VAStatus va_res = VA_STATUS_ERROR_UNKNOWN;
    VAEntrypoint *entrypoints = NULL;
    int res = -1, i, entrypoints_max, entrypoints_len;

    if ((entrypoints_max = vaMaxNumEntrypoints(self->display)) > 0 &&
        (entrypoints = calloc(entrypoints_max, sizeof(VAEntrypoint)))) {
        va_res = vaQueryConfigEntrypoints(self->display, profile,
                                          entrypoints, &entrypoints_len);
        if (va_res == VA_STATUS_SUCCESS) {
            for (i = 0; i < entrypoints_len; i++) {
                if (entrypoints[i] == self->entrypoint) {
                    res = 0;
                    break;
                }
            }
        }
        free(entrypoints);
    }
    return res;
}


static unsigned int
tvhva_get_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
        //case AV_PIX_FMT_YUV420P: // the cake is a lie
        case AV_PIX_FMT_NV12:
            return VA_RT_FORMAT_YUV420;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_YUYV422:
            return VA_RT_FORMAT_YUV422;
        case AV_PIX_FMT_GRAY8:
            return VA_RT_FORMAT_YUV400;
        default:
            return 0;
    }
}


static int
tvhva_context_config(TVHVAContext *self, VAProfile profile, unsigned int format)
{
    VAStatus va_res = VA_STATUS_ERROR_UNKNOWN;
    VAConfigAttrib attrib = { VAConfigAttribRTFormat, VA_ATTRIB_NOT_SUPPORTED };

    // vaCreateConfig
    va_res = vaGetConfigAttributes(self->display, profile, self->entrypoint,
                                   &attrib, 1);
    if (va_res != VA_STATUS_SUCCESS) {
        tvherror(LS_VAAPI, "vaGetConfigAttributes: %s", vaErrorStr(va_res));
        return -1;
    }
    if (attrib.value == VA_ATTRIB_NOT_SUPPORTED || !(attrib.value & format)) {
        tvherror(LS_VAAPI, "unsupported VA_RT_FORMAT");
        return -1;
    }
    attrib.value = format;
    va_res = vaCreateConfig(self->display, profile, self->entrypoint,
                            &attrib, 1, &self->config_id);
    if (va_res != VA_STATUS_SUCCESS) {
        tvherror(LS_VAAPI, "vaCreateConfig: %s", vaErrorStr(va_res));
        return -1;
    }
    return 0;
}


static int
tvhva_context_check_constraints(TVHVAContext *self)
{
    AVVAAPIHWConfig *va_config = NULL;
    AVHWFramesConstraints *hw_constraints = NULL;
    enum AVPixelFormat sw_format = AV_PIX_FMT_NONE;
    const AVPixFmtDescriptor *sw_desc = NULL, *io_desc = NULL;
    int i, ret = 0;

    if (!(va_config = av_hwdevice_hwconfig_alloc(self->hw_device_ref))) {
        tvherror(LS_VAAPI, "failed to allocate hwconfig");
        return AVERROR(ENOMEM);
    }
    va_config->config_id = self->config_id;

    hw_constraints =
        av_hwdevice_get_hwframe_constraints(self->hw_device_ref, va_config);
    if (!hw_constraints) {
        tvherror(LS_VAAPI, "failed to get constraints");
        av_freep(&va_config);
        return -1;
    }

    if (self->io_format != AV_PIX_FMT_NONE) {
        for (i = 0; hw_constraints->valid_sw_formats[i] != AV_PIX_FMT_NONE; i++) {
            sw_format = hw_constraints->valid_sw_formats[i];
            if (sw_format == self->io_format) {
                self->sw_format = sw_format;
                break;
            }
        }
        if (self->sw_format == AV_PIX_FMT_NONE &&
            (io_desc = av_pix_fmt_desc_get(self->io_format))) {
            for (i = 0; hw_constraints->valid_sw_formats[i] != AV_PIX_FMT_NONE; i++) {
                sw_format = hw_constraints->valid_sw_formats[i];
                if ((sw_desc = av_pix_fmt_desc_get(sw_format)) &&
                    sw_desc->nb_components == io_desc->nb_components &&
                    sw_desc->log2_chroma_w == io_desc->log2_chroma_w &&
                    sw_desc->log2_chroma_h == io_desc->log2_chroma_h) {
                    self->sw_format = sw_format;
                    break;
                }
            }
        }
    }
    if (self->sw_format == AV_PIX_FMT_NONE) {
        tvherror(LS_VAAPI, "VAAPI hardware does not support pixel format: %s",
                 av_get_pix_fmt_name(self->io_format));
        ret = AVERROR(EINVAL);
        goto end;
    }

    // Ensure the picture size is supported by the hardware.
    if (self->width < hw_constraints->min_width ||
        self->height < hw_constraints->min_height ||
        self->width > hw_constraints->max_width ||
        self->height > hw_constraints->max_height) {
        tvherror(LS_VAAPI, "VAAPI hardware does not support image "
                 "size %dx%d (constraints: width %d-%d height %d-%d).",
                 self->width, self->height,
                 hw_constraints->min_width, hw_constraints->max_width,
                 hw_constraints->min_height, hw_constraints->max_height);
        ret = AVERROR(EINVAL);
    }

end:
    av_hwframe_constraints_free(&hw_constraints);
    av_freep(&va_config);
    return ret;
}


static int
tvhva_context_setup(TVHVAContext *self, AVCodecContext *avctx)
{
    VAProfile profile = VAProfileNone;
    unsigned int format = 0;
    VAStatus va_res = VA_STATUS_ERROR_UNKNOWN;
    AVHWFramesContext *hw_frames_ctx = NULL;
    AVVAAPIFramesContext *va_frames = NULL;

    if (!(self->display = tvhva_context_display(self))) {
        return -1;
    }
    if ((profile = tvhva_context_profile(self, avctx)) == VAProfileNone ||
        tvhva_context_check_profile(self, profile)) {
        tvherror(LS_VAAPI, "unsupported codec: %s and/or profile: %s",
                 avctx->codec->name,
                 av_get_profile_name(avctx->codec, avctx->profile));
        return -1;
    }
    if (!(format = tvhva_get_format(self->io_format))) {
        tvherror(LS_VAAPI, "unsupported pixel format: %s",
                 av_get_pix_fmt_name(self->io_format));
        return -1;
    }

    if (tvhva_context_config(self, profile, format) ||
        tvhva_context_check_constraints(self)) {
        return -1;
    }

    if (!(self->hw_frames_ref = av_hwframe_ctx_alloc(self->hw_device_ref))) {
        tvherror(LS_VAAPI, "failed to create VAAPI frame context.");
        return AVERROR(ENOMEM);
    }
    hw_frames_ctx = (AVHWFramesContext*)self->hw_frames_ref->data;
    hw_frames_ctx->format = AV_PIX_FMT_VAAPI;
    hw_frames_ctx->sw_format = self->sw_format;
    hw_frames_ctx->width = self->width;
    hw_frames_ctx->height = self->height;
    hw_frames_ctx->initial_pool_size = 32;

    if (av_hwframe_ctx_init(self->hw_frames_ref) < 0) {
        tvherror(LS_VAAPI, "failed to initialise VAAPI frame context");
        return -1;
    }

    // vaCreateContext
    if (self->entrypoint == VAEntrypointVLD) { // decode only
        va_frames = hw_frames_ctx->hwctx;
        va_res = vaCreateContext(self->display, self->config_id,
                                 self->width, self->height,
                                 VA_PROGRESSIVE,
                                 va_frames->surface_ids, va_frames->nb_surfaces,
                                 &self->context_id);
        if (va_res != VA_STATUS_SUCCESS) {
            tvherror(LS_VAAPI, "vaCreateContext: %s", vaErrorStr(va_res));
            return -1;
        }
    }

    if (!(avctx->hw_frames_ctx = av_buffer_ref(self->hw_frames_ref))) {
        return AVERROR(ENOMEM);
    }

    avctx->sw_pix_fmt = self->sw_format;
    avctx->thread_count = 1;

    return 0;
}


static TVHVAContext *
tvhva_context_create(AVCodecContext *avctx, VAEntrypoint entrypoint)
{
    TVHVAContext *self = NULL;

    if (!(self = calloc(1, sizeof(TVHVAContext)))) {
        tvherror(LS_VAAPI, "failed to allocate vaapi context");
        return NULL;
    }
    self->display = NULL;
    self->config_id = VA_INVALID_ID;
    self->context_id = VA_INVALID_ID;
    self->entrypoint = entrypoint;
    if (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P) {
        self->io_format = AV_PIX_FMT_NV12;
    }
    else {
        self->io_format = avctx->sw_pix_fmt;
    }
    self->sw_format = AV_PIX_FMT_NONE;
    self->width = avctx->coded_width;
    self->height = avctx->coded_height;
    self->hw_device_ref = NULL;
    self->hw_frames_ref = NULL;
    if (tvhva_context_setup(self, avctx)) {
        tvhva_context_destroy(self);
        return NULL;
    }
    return self;
}


/* decoding ================================================================= */

static int
vaapi_get_buffer2(AVCodecContext *avctx, AVFrame *avframe, int flags)
{
    if (!(avctx->codec->capabilities & AV_CODEC_CAP_DR1)) {
        return avcodec_default_get_buffer2(avctx, avframe, flags);
    }
    return av_hwframe_get_buffer(avctx->hw_frames_ctx, avframe, 0);
}


int
vaapi_decode_setup_context(AVCodecContext *avctx)
{
    if (!(avctx->hwaccel_context =
          tvhva_context_create(avctx, VAEntrypointVLD))) {
        return -1;
    }

    avctx->get_buffer2 = vaapi_get_buffer2;
    avctx->thread_safe_callbacks = 0;

    return 0;
}


void
vaapi_decode_close_context(AVCodecContext *avctx)
{
    /*if (avctx->hw_frames_ctx) {
        av_buffer_unref(&avctx->hw_frames_ctx);
        avctx->hw_frames_ctx = NULL;
    }*/
    tvhva_context_destroy(avctx->hwaccel_context);
}


/* encoding ================================================================= */

int
vaapi_encode_setup_context(AVCodecContext *avctx)
{
    TVHVAContext *hwaccel_context = NULL;

    if (!(hwaccel_context =
          tvhva_context_create(avctx, VAEntrypointEncSlice))) {
        return -1;
    }
    if (!(avctx->opaque = av_buffer_ref(hwaccel_context->hw_device_ref))) {
        tvhva_context_destroy(hwaccel_context);
        return AVERROR(ENOMEM);
    }
    tvhva_context_destroy(hwaccel_context);
    return 0;
}


void
vaapi_encode_close_context(AVCodecContext *avctx)
{
    /*if (avctx->hw_frames_ctx) {
        av_buffer_unref(&avctx->hw_frames_ctx);
        avctx->hw_frames_ctx = NULL;
    }*/
    if (avctx->opaque) {
        AVBufferRef *hw_device_ctx = avctx->opaque;
        av_buffer_unref(&hw_device_ctx);
        avctx->opaque = NULL;
    }
}


/* module =================================================================== */

void
vaapi_done()
{
    tvhva_done();
}
