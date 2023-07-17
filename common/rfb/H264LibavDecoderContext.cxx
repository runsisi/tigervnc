/* Copyright (C) 2021 Vladimir Sukhonosov <xornet@xornet.org>
 * Copyright (C) 2021 Martins Mozeiko <martins.mozeiko@gmail.com>
 * All Rights Reserved.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */


extern "C" {
#include <libavutil/imgutils.h>
#include <libavcodec/version.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
}
#if LIBAVCODEC_VERSION_MAJOR > 57 || LIBAVCODEC_VERSION_MAJOR == 57 && LIBAVCODEC_VERSION_MINOR >= 37
#define FFMPEG_DECODE_VIDEO2_DEPRECATED
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 58
#define FFMPEG_INIT_PACKET_DEPRECATED
#endif

#include <xf86drm.h>

#include <rfb/Exception.h>
#include <rfb/LogWriter.h>
#include <rfb/PixelBuffer.h>
#include <rfb/H264LibavDecoderContext.h>

using namespace rfb;

static LogWriter vlog("H264LibavDecoderContext");

static int find_render_node(char *node, size_t maxlen) {
  int r = -ENOENT;
  drmDevice *devices[64];

  int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
  for (int i = 0; i < n; ++i) {
    drmDevice *dev = devices[i];
    if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
      continue;

    strncpy(node, dev->nodes[DRM_NODE_RENDER], maxlen);
    node[maxlen - 1] = '\0';
    r = 0;
    break;
  }

  drmFreeDevices(devices, n);
  return r;
}

bool H264LibavDecoderContext::initCodec() {
  os::AutoMutex lock(&mutex);

  sws = NULL;
  swsBuffer = NULL;
  h264WorkBuffer = NULL;
  h264WorkBufferLength = 0;

  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec)
  {
    vlog.error("Codec not found");
    return false;
  }

  parser = av_parser_init(codec->id);
  if (!parser)
  {
    vlog.error("Could not create H264 parser");
    return false;
  }

  avctx = avcodec_alloc_context3(codec);
  if (!avctx)
  {
    av_parser_close(parser);
    vlog.error("Could not allocate video codec context");
    return false;
  }

  char render_node[64];
  int r = find_render_node(render_node, sizeof(render_node));
  if (r < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not find render node");
  } else {
    // hwaccel
    AVBufferRef *hwdevice_ref = NULL;
    r = av_hwdevice_ctx_create(&hwdevice_ref, AV_HWDEVICE_TYPE_VAAPI, render_node, NULL, 0);
    if (r < 0) {
      av_log(NULL, AV_LOG_ERROR, "Device creation failed: %d\n", r);
    } else {
      avctx->hw_device_ctx = hwdevice_ref;
    }
  }

  frame = av_frame_alloc();
  if (!frame)
  {
    av_parser_close(parser);
    avcodec_free_context(&avctx);
    vlog.error("Could not allocate video frame");
    return false;
  }

  if (avcodec_open2(avctx, codec, NULL) < 0)
  {
    av_parser_close(parser);
    avcodec_free_context(&avctx);
    av_frame_free(&frame);
    vlog.error("Could not open codec");
    return false;
  }

  int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB32, rect.width(), rect.height(), 1);
  swsBuffer = new uint8_t[numBytes];

  initialized = true;
  return true;
}

void H264LibavDecoderContext::freeCodec() {
  os::AutoMutex lock(&mutex);

  if (!initialized)
    return;
  av_parser_close(parser);
  avcodec_free_context(&avctx);
  av_frame_free(&frame);
  delete[] swsBuffer;
  free(h264WorkBuffer);
  initialized = false;
}

// We need to reallocate buffer because AVPacket uses non-const pointer.
// We don't want to const_cast our buffer somewhere. So we would rather to maintain context's own buffer
// Also avcodec requires a right padded buffer
uint8_t* H264LibavDecoderContext::makeH264WorkBuffer(const uint8_t* buffer, uint32_t len)
{
  uint32_t reserve_len = len + len % AV_INPUT_BUFFER_PADDING_SIZE;

  if (!h264WorkBuffer || reserve_len > h264WorkBufferLength)
  {
    h264WorkBuffer = (uint8_t*)realloc(h264WorkBuffer, reserve_len);
    if (h264WorkBuffer == NULL) {
      throw Exception("H264LibavDecoderContext: Unable to allocate memory");
    }
    h264WorkBufferLength = reserve_len;
  }

  memcpy(h264WorkBuffer, buffer, len);
  memset(h264WorkBuffer + len, 0, h264WorkBufferLength - len);
  return h264WorkBuffer;
}

void H264LibavDecoderContext::decode(const uint8_t* h264_in_buffer,
                                     uint32_t len,
                                     ModifiablePixelBuffer* pb) {
  os::AutoMutex lock(&mutex);
  if (!initialized)
    return;
  uint8_t* h264_work_buffer = makeH264WorkBuffer(h264_in_buffer, len);

#ifdef FFMPEG_INIT_PACKET_DEPRECATED
  AVPacket *packet = av_packet_alloc();
#else
  AVPacket *packet = new AVPacket();
  av_init_packet(packet);
#endif

  int ret;
  int frames_received = 0;
  while (len)
  {
    ret = av_parser_parse2(parser, avctx, &packet->data, &packet->size, h264_work_buffer, len, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (ret < 0)
    {
      vlog.error("Error while parsing");
      break;
    }
    // We need to slap on tv to make it work here (don't ask me why)
    if (!packet->size && len == static_cast<uint32_t>(ret))
      ret = av_parser_parse2(parser, avctx, &packet->data, &packet->size, h264_work_buffer, len, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (ret < 0)
    {
      vlog.error("Error while parsing");
      break;
    }
    h264_work_buffer += ret;
    len -= ret;

    if (!ret)
    {
      packet->size = len;
      packet->data = h264_work_buffer;
      len = 0;
    }

    if (!packet->size)
      continue;

#ifndef FFMPEG_DECODE_VIDEO2_DEPRECATED
    int got_frame;
    ret = avcodec_decode_video2(avctx, frame, &got_frame, packet);
    if (ret < 0 || !got_frame)
    {
      vlog.error("Error during decoding");
      break;
    }
#else
    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0)
    {
      vlog.error("Error sending a packet to decoding");
      break;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0)
    {
      vlog.error("Error during decoding");
      break;
    }
#endif
    frames_received++;
  }

#ifdef FFMPEG_INIT_PACKET_DEPRECATED
  packet->size = 0;
  packet->data = NULL;
  av_packet_free(&packet);
#else
  delete packet;
#endif

  if (!frames_received)
    return;

  if (!frame->height)
    return;

  const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(avctx->pix_fmt);
  if (pixdesc && pixdesc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
    // hwaccel
    AVFrame *sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    int r = av_hwframe_transfer_data(sw_frame, frame, 0);
    if (r < 0) {
      fprintf(stderr, "Error transferring the data to system memory\n");
      av_frame_free(&sw_frame);
      return;
    }

    int size = av_image_get_buffer_size((AVPixelFormat)sw_frame->format, sw_frame->width, sw_frame->height, 1);
    r = av_image_copy_to_buffer(swsBuffer, size,
      (const uint8_t * const *)sw_frame->data,
      (const int *)sw_frame->linesize, (AVPixelFormat)sw_frame->format,
      sw_frame->width, sw_frame->height, 1);
    if (r < 0) {
      fprintf(stderr, "Can not copy image to buffer\n");
      av_frame_free(&sw_frame);
      return;
    }

    av_frame_free(&sw_frame);

    int stride;
    pb->getBuffer(rect, &stride);
    pb->imageRect(rect, swsBuffer, stride);
    return;
  }

  // w/o hwaccel
  sws = sws_getCachedContext(sws, frame->width, frame->height, avctx->pix_fmt,
                             frame->width, frame->height, AV_PIX_FMT_RGB32,
                             0, NULL, NULL, NULL);

  int stride;
  pb->getBuffer(rect, &stride);
  int dst_linesize = stride * pb->getPF().bpp/8;  // stride is in pixels, linesize is in bytes (stride x4). We need bytes

  sws_scale(sws, frame->data, frame->linesize, 0, frame->height, &swsBuffer, &dst_linesize);

  pb->imageRect(rect, swsBuffer, stride);
}
