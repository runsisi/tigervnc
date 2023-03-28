#ifndef __RFB_H264RKMPPDECODER_H__
#define __RFB_H264RKMPPDECODER_H__

extern "C" {
#include <rockchip/rk_mpi.h>

#include <libswscale/swscale.h>
}

#include <rfb/H264DecoderContext.h>

namespace rfb {
    class H264RkmppDecoderContext : public H264DecoderContext {
        public:
        H264RkmppDecoderContext(const Rect &r) : H264DecoderContext(r) {}
        ~H264RkmppDecoderContext() { freeCodec(); }

        virtual void decode(const uint8_t* h264_buffer, uint32_t len, ModifiablePixelBuffer* pb);

        protected:
        virtual bool initCodec();
        virtual void freeCodec();

    private:
        MppCtx ctx;
        MppApi *mpi;
        MppDecCfg cfg;

        MppPacket packet;
        MppFrame frame;
        MppBufferGroup frm_grp;

        SwsContext *sws;
        uint8_t *swsBuffer;
    };
}

#endif
