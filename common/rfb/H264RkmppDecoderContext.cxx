extern "C" {
#include <unistd.h>
#include <libavutil/imgutils.h>
}

#include <rfb/LogWriter.h>
#include <rfb/PixelBuffer.h>
#include <rfb/H264RkmppDecoderContext.h>

using namespace rfb;

static LogWriter vlog("H264RkmppDecoderContext");

bool H264RkmppDecoderContext::initCodec()
{
    os::AutoMutex lock(&mutex);

    MPP_RET ret = MPP_OK;

    ret = mpp_packet_init(&packet, NULL, 0);
    if (ret) {
        vlog.error("mpp_packet_init failed");
        return false;
    }

    ret = mpp_create(&ctx, &mpi);
    if (ret) {
        vlog.error("mpp_create failed");
        mpp_packet_deinit(&packet);
        return false;
    }

    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret) {
        vlog.error("%p mpp_init failed", ctx);
        mpp_packet_deinit(&packet);
        mpp_destroy(ctx);
        return false;
    }

    mpp_dec_cfg_init(&cfg);
    ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        vlog.error("%p failed to get decoder cfg ret %d", ctx, ret);
        mpp_packet_deinit(&packet);
        mpp_destroy(ctx);
        mpp_dec_cfg_deinit(cfg);
        return false;
    }
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    if (ret) {
        vlog.error("%p failed to set split_parse ret %d", ctx, ret);
        mpp_packet_deinit(&packet);
        mpp_destroy(ctx);
        mpp_dec_cfg_deinit(cfg);
        return false;
    }
    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        vlog.error("%p failed to set cfg %p ret %d", ctx, cfg, ret);
        mpp_packet_deinit(&packet);
        mpp_destroy(ctx);
        mpp_dec_cfg_deinit(cfg);
        return false;
    }

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB32, rect.width(), rect.height(), 1);
    swsBuffer = new uint8_t[numBytes];

    initialized = true;
    frm_grp = NULL;
    return true;
}

void H264RkmppDecoderContext::freeCodec()
{
    os::AutoMutex lock(&mutex);

    if (!initialized) {
        return;
    }

    mpp_packet_deinit(&packet);
    mpi->reset(ctx);
    mpp_destroy(ctx);
    mpp_dec_cfg_deinit(cfg);
    mpp_buffer_group_put(frm_grp);

    delete[] swsBuffer;
    initialized = false;
}

void H264RkmppDecoderContext::decode(const uint8_t *h264_in_buffer, uint32_t len, ModifiablePixelBuffer *pb)
{
    os::AutoMutex lock(&mutex);

    int frames_received = 0;

    if (!initialized) {
        return;
    }

    mpp_packet_set_data(packet, (void *)h264_in_buffer);
    mpp_packet_set_size(packet, len);
    mpp_packet_set_pos(packet, (void *)h264_in_buffer);
    mpp_packet_set_length(packet, len);

    /* 将一个 packet 消耗完毕，并取得所有的 frame 才完成 */
    do {
        int pkt_done = 0;
        MPP_RET ret = MPP_OK;
        MppFrame  frame_new;

        if (!pkt_done) {
            ret = mpi->decode_put_packet(ctx, packet);
            if (ret == MPP_OK) {
                pkt_done = 1;
            }
        }

        /* 不断地获取 frame，直到所有 frame 均已获取 */
        do {
            ret = mpi->decode_get_frame(ctx, &frame_new);
            if (ret) {
                vlog.error("Error during decoding");
                break;
            }

            if (frame_new) {
                if (mpp_frame_get_info_change(frame_new)) {
                    RK_U32 width = mpp_frame_get_width(frame_new);
                    RK_U32 height = mpp_frame_get_height(frame_new);
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame_new);
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame_new);
                    RK_U32 buf_size = mpp_frame_get_buf_size(frame_new);

                    vlog.info("%p decode_get_frame get info changed found", ctx);
                    vlog.info("%p decoder require buffer w:h [%d:%d] stride [%d:%d] buf_size %d", ctx, width, height, hor_stride, ver_stride, buf_size);

                    if (NULL == frm_grp) {
                        /* If buffer group is not set create one and limit it */
                        ret = mpp_buffer_group_get_internal(&frm_grp, MPP_BUFFER_TYPE_ION);
                        if (ret) {
                            vlog.error("%p get mpp buffer group failed ret %d", ctx, ret);
                            break;
                        }

                        /* Set buffer to mpp decoder */
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp);
                        if (ret) {
                            vlog.error("%p set buffer group failed ret %d", ctx, ret);
                            break;
                        }
                    } else {
                        /* If old buffer group exist clear it */
                        ret = mpp_buffer_group_clear(frm_grp);
                        if (ret) {
                            vlog.error("%p clear buffer group failed ret %d", ctx, ret);
                            break;
                        }
                    }

                    /* Use limit config to limit buffer count to 24 with buf_size */
                    ret = mpp_buffer_group_limit_config(frm_grp, buf_size, 24);
                    if (ret) {
                        vlog.error("%p limit buffer group failed ret %d", ctx, ret);
                        break;
                    }

                    ret = mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret) {
                        vlog.error("%p info change ready failed ret %d", ctx, ret);
                        break;
                    }

                    mpp_frame_deinit(&frame_new);
                } else {
                    if (mpp_frame_get_errinfo(frame_new) || mpp_frame_get_discard(frame_new)) {
                        continue;
                    }

                    if (frame) {
                        mpp_frame_deinit(&frame);
                    }
                    frame = frame_new;
                    frames_received++;
                }
                continue;
            }
            break;
        } while (1);

        if (pkt_done) {
            break;
        }

        /*
         * why sleep here:
         * mpi->decode_put_packet will failed when packet in internal queue is
         * full, waiting the package is consumed. Usually hardware decode one
         * frame which resolution is 1080p needs 2 ms, so here we sleep 1ms
         * is enough.
         */
        usleep(1000);
    } while (1);

    if (!frames_received) {
        return;
    }

    unsigned char *data[2];
    int linesize[2];
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int h_stride = 0;
    unsigned int v_stride = 0;
    MppFrameFormat fmt  = MPP_FMT_YUV420SP;
    MppBuffer buffer = NULL;
    unsigned char *base = NULL;

    width = mpp_frame_get_width(frame);
    height = mpp_frame_get_height(frame);
    h_stride = mpp_frame_get_hor_stride(frame);
    v_stride = mpp_frame_get_ver_stride(frame);
    fmt = mpp_frame_get_fmt(frame);
    buffer = mpp_frame_get_buffer(frame);

    base = (unsigned char *)mpp_buffer_get_ptr(buffer);
    if (fmt != MPP_FMT_YUV420SP) {
        vlog.error("frame format is not NV12");
        return;
    }
    data[0] = base;
    data[1] = base + h_stride * v_stride;
    linesize[0] = h_stride;
    linesize[1] = h_stride;

    int stride;
    int dst_linesize;

    pb->getBuffer(rect, &stride);
    dst_linesize = stride * pb->getPF().bpp/8;  // stride is in pixels, linesize is in bytes (stride x4). We need bytes
    sws = sws_getCachedContext(sws, width, height, AV_PIX_FMT_NV12, width, height, AV_PIX_FMT_RGB32, 0, NULL, NULL, NULL);
    sws_scale(sws, data, linesize, 0, height, &swsBuffer, &dst_linesize);
    pb->imageRect(rect, swsBuffer, stride);
}
