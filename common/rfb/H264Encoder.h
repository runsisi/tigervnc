//
// Created by runsisi on 23-3-28.
//

#ifndef __RFB_H264ENCODER_H__
#define __RFB_H264ENCODER_H__

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/buffer.h>
#include <libavutil/pixdesc.h>
#include <libavutil/dict.h>
}

extern "C" {
#include <vec.h>
}

#include <xf86drm.h>
#include <libdrm/drm_fourcc.h>

#include <rfb/Encoder.h>
#include <rfb/Palette.h>
#include <rfb/PixelBuffer.h>
#include <rfb/SConnection.h>
#include <rfb/SMsgWriter.h>
#include <rfb/encodings.h>
#include <rfb/kmsgrab.h>

namespace rfb {

using namespace kmsgrab;

class H264Encoder : public Encoder {
public:
  explicit H264Encoder(SConnection* conn) : Encoder(conn, encodingH264,
      EncoderFlags(EncoderUseNativePF | EncoderLossy)) {
    frame_in_ = av_frame_alloc();
    frame_filtered_ = av_frame_alloc();
    pkt_out_ = av_packet_alloc();

    vec_init(&current_packet_, 65536);
  }

  ~H264Encoder() override {
    deinit();

    av_frame_free(&frame_in_);
    av_frame_free(&frame_filtered_);
    av_packet_free(&pkt_out_);

    vec_destroy(&current_packet_);
  }

  bool isSupported() override {
    return conn->client.supportsEncoding(encodingH264);
  }

  void writeRect(const PixelBuffer* /*pb*/, const Palette& /*palette*/) override {
    int r;

    if (!kmsgrab_ctx_) {
      r = init();
      if (r < 0) {
        return;
      }
    }

    r = encode();
    if (r < 0) {
      deinit();
      return;
    }

    Rect rect;
    rect.setXYWH(0, 0, width_, height_);

    conn->writer()->startRect(rect, encodingH264);

    // send
    rdr::OutStream* os = conn->getOutStream();
    os->writeU32(current_packet_.len);
    os->writeU32(0);
    os->writeBytes((const uint8_t*)current_packet_.data, current_packet_.len);
    vec_clear(&current_packet_);
  }

  void writeSolidRect(int /*width*/, int /*height*/,
      const PixelFormat& /*pf*/,
      const uint8_t* /*colour*/) override {

  }

private:
  int encode() {
    int r = kmsgrab_read_frame(kmsgrab_ctx_, frame_in_);
    if (r < 0) {
      return r;
    }

    r = av_buffersrc_add_frame_flags(filter_in_, frame_in_,
        AV_BUFFERSRC_FLAG_KEEP_REF);
    if (r < 0) {
      goto free_frame_in;
    }

    r = av_buffersink_get_frame(filter_out_, frame_filtered_);
    if (r < 0) {
      goto free_frame_in;
    }

    r = avcodec_send_frame(codec_ctx_, frame_filtered_);
    if (r < 0) {
      goto free_frame_filtered;
    }

    while (1) {
      r = avcodec_receive_packet(codec_ctx_, pkt_out_);
      if (r < 0) {
        goto free_pkt;
      }

      vec_append(&current_packet_, pkt_out_->data, pkt_out_->size);

      pkt_out_->stream_index = 0;
    }

free_pkt:
    av_packet_unref(pkt_out_);
free_frame_filtered:
    av_frame_unref(frame_filtered_);
free_frame_in:
    av_frame_unref(frame_in_);
    return r == AVERROR(EAGAIN) ? 0 : r;
  }

  static int find_control_node(char *node, size_t maxlen) {
    int r = -ENOENT;
    drmDevice *devices[64];

    int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
    for (int i = 0; i < n; ++i) {
      drmDevice *dev = devices[i];
      if (!(dev->available_nodes & (1 << DRM_NODE_PRIMARY)))
        continue;

      strncpy(node, dev->nodes[DRM_NODE_PRIMARY], maxlen);
      node[maxlen - 1] = '\0';
      r = 0;
      break;
    }

    drmFreeDevices(devices, n);
    return r;
  }

  int init() {
    KMSGrabContext *kctx = &kmsgrab_ctx_instance_;
    memset(kctx, 0, sizeof(*kctx));
    kctx->format = AV_PIX_FMT_NONE;
    kctx->drm_format_modifier = DRM_FORMAT_MOD_INVALID;

    int r = find_control_node(kctx->device_path, sizeof(kctx->device_path));
    if (r < 0) {
      return r;
    }

    r = kmsgrab_read_header(kctx);
    if (r < 0) {
      return r;
    }

    {
      next_frame_should_be_keyframe_ = true;

      width_ = kctx->width;
      height_ = kctx->height;
      format_ = kctx->format;
      timebase_ = (AVRational){1, 1000000};
      sample_aspect_ratio_ = (AVRational){1, 1};

      const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
      if (!codec) {
        goto free_kmsctx;
      }

      // filter
      filter_graph_ = avfilter_graph_alloc();
      if (!filter_graph_) {
        r = -ENOMEM;
        goto free_kmsctx;
      }

      /* Placeholder values are used to pacify input checking and the real
       * values are set below.
       */
      r = avfilter_graph_create_filter(&filter_in_,
          avfilter_get_by_name("buffer"), "in",
          "width=1:height=1:pix_fmt=drm_prime:time_base=1/1", NULL,
          filter_graph_);
      if (r < 0) {
        goto free_graph;
      }

      AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
      if (!params) {
        r = -ENOMEM;
        goto free_graph;
      }

      params->format = AV_PIX_FMT_DRM_PRIME;
      params->width = width_;
      params->height = height_;
      params->sample_aspect_ratio = sample_aspect_ratio_;
      params->time_base = timebase_;
      params->hw_frames_ctx = kctx->frames_ref;

      r = av_buffersrc_parameters_set(filter_in_, params);
      av_free(params);
      if (r < 0) {
        goto free_graph;
      }

      r = avfilter_graph_create_filter(&filter_out_,
          avfilter_get_by_name("buffersink"), "out", NULL,
          NULL, filter_graph_);
      if (r < 0) {
        goto free_graph;
      }

      AVFilterInOut *inputs;
      inputs = avfilter_inout_alloc();
      if (!inputs) {
        r = -ENOMEM;
        goto free_graph;
      }

      inputs->name = av_strdup("in");
      inputs->filter_ctx = filter_in_;
      inputs->pad_idx = 0;
      inputs->next = NULL;

      AVFilterInOut *outputs;
      outputs = avfilter_inout_alloc();
      if (!outputs) {
        avfilter_inout_free(&inputs);
        r = -ENOMEM;
        goto free_graph;
      }

      outputs->name = av_strdup("out");
      outputs->filter_ctx = filter_out_;
      outputs->pad_idx = 0;
      outputs->next = NULL;

      r = avfilter_graph_parse(filter_graph_,
          "hwmap=mode=direct:derive_device=vaapi"
          ",scale_vaapi=format=nv12:mode=fast",
          outputs, inputs, NULL);
      if (r < 0) {
        goto free_graph;
      }

      for (unsigned int i = 0; i < filter_graph_->nb_filters; ++i) {
        filter_graph_->filters[i]->hw_device_ctx = av_buffer_ref(kctx->device_ref);
        if (!filter_graph_->filters[i]->hw_device_ctx) {
          r = -ENOMEM;
          goto free_graph;
        }
      }

      r = avfilter_graph_config(filter_graph_, NULL);
      if (r < 0) {
        goto free_graph;
      }

      // codec
      codec_ctx_ = avcodec_alloc_context3(codec);
      if (!codec_ctx_) {
        r = -ENOMEM;
        goto free_graph;
      }

      struct AVCodecContext *c = codec_ctx_;
      c->width = width_;
      c->height = height_;
      c->time_base = timebase_;
      c->sample_aspect_ratio = sample_aspect_ratio_;
      c->pix_fmt = AV_PIX_FMT_VAAPI;
      c->gop_size = INT32_MAX; /* We'll select key frames manually */
      c->max_b_frames = 0; /* B-frames are bad for latency */
      c->global_quality = 1;

      /* open-h264 requires baseline profile, so we use constrained
       * baseline.
       */
      c->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;

      codec_ctx_->hw_frames_ctx = av_buffer_ref(filter_out_->inputs[0]->hw_frames_ctx);
      if (!codec_ctx_->hw_frames_ctx) {
        r = -ENOMEM;
        goto free_codec;
      }

      AVDictionary *opts = NULL;
      av_dict_set_int(&opts, "async_depth", 1, 0);

      r = avcodec_open2(codec_ctx_, codec, &opts);
      av_dict_free(&opts);
      if (r < 0) {
        goto free_codec;
      }
    }

    kmsgrab_ctx_ = kctx;

    return 0;

free_codec:
      avcodec_free_context(&codec_ctx_);
free_graph:
      avfilter_graph_free(&filter_graph_);
free_kmsctx:
      kmsgrab_read_close(kctx);
    return r;
  }

  void deinit() {
    if (kmsgrab_ctx_) {
      avcodec_free_context(&codec_ctx_);
      avfilter_graph_free(&filter_graph_);
      kmsgrab_read_close(kmsgrab_ctx_);
      kmsgrab_ctx_ = NULL;
    }
  }

private:
  KMSGrabContext kmsgrab_ctx_instance_;
  KMSGrabContext *kmsgrab_ctx_ = NULL;

  AVFrame *frame_in_ = NULL;
  AVFrame *frame_filtered_ = NULL;
  AVPacket *pkt_out_ = NULL;

  vec current_packet_;

  int width_;
  int height_;
  AVPixelFormat format_;

  AVRational timebase_;
  AVRational sample_aspect_ratio_;

  AVCodecContext* codec_ctx_;

  AVFilterGraph* filter_graph_;
  AVFilterContext* filter_in_;
  AVFilterContext* filter_out_;

  bool next_frame_should_be_keyframe_;
  bool current_frame_is_keyframe_;
};

} // namespace rfb

#endif // __RFB_H264ENCODER_H__
