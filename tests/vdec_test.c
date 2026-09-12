/*
 * HiKey960 Kirin 960 VPU (hi_vcodec) Hardware Decoder Test Harness
 * Directly interfaces with /dev/hi_vdec via ioctl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>

#include "drv_omxvdec.h"

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <stream.264/hevc> <codec: h264|hevc|mpeg2|mpeg4|vp8> <width> <height> [out.yuv]\n", argv[0]);
        return 1;
    }

    const char *stream_file = argv[1];
    const char *codec_str = argv[2];
    int width = atoi(argv[3]);
    int height = atoi(argv[4]);
    const char *out_file = (argc >= 6) ? argv[5] : NULL;

    OMXVDEC_CODEC_TYPE codec_type = OMXVDEC_H264;
    if (!strcmp(codec_str, "h264")) codec_type = OMXVDEC_H264;
    else if (!strcmp(codec_str, "hevc")) codec_type = OMXVDEC_HEVC;
    else if (!strcmp(codec_str, "mpeg2")) codec_type = OMXVDEC_MPEG2;
    else if (!strcmp(codec_str, "mpeg4")) codec_type = OMXVDEC_MPEG4;
    else if (!strcmp(codec_str, "vp8")) codec_type = OMXVDEC_VP8;
    else if (!strcmp(codec_str, "vc1")) codec_type = OMXVDEC_VC1;
    else {
        fprintf(stderr, "Unknown codec: %s\n", codec_str);
        return 1;
    }

    FILE *fp = fopen(stream_file, "rb");
    if (!fp) {
        perror("fopen stream");
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int fd = open("/dev/hi_vdec", O_RDWR);
    if (fd < 0) {
        perror("open /dev/hi_vdec");
        fclose(fp);
        return 1;
    }
    printf("[VDEC_TEST] Opened /dev/hi_vdec successfully (fd=%d)\n", fd);

    // 1. Channel Configuration & Creation
    OMXVDEC_CHAN_CFG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cfg_codec_type = codec_type;
    cfg.cfg_width = width;
    cfg.cfg_height = height;
    cfg.cfg_stride = width;
    cfg.cfg_color_format = OMX_PIX_FMT_NV12;
    cfg.path_mode = PATH_MODE_NORMAL;
    cfg.act_inbuf_size = 2 * 1024 * 1024;
    cfg.act_inbuf_num = 2;
    cfg.act_outbuf_num = 4;

    int chan_id = -1;
    OMXVDEC_IOCTL_MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.in = &cfg;
    msg.out = &chan_id;

    if (ioctl(fd, VDEC_IOCTL_CHAN_CREATE, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_CREATE");
        close(fd);
        fclose(fp);
        return 1;
    }
    printf("[VDEC_TEST] Channel created successfully! chan_id=%d\n", chan_id);

    // 2. Allocate Input Buffer
    OMXVDEC_BUF_DESC in_buf;
    memset(&in_buf, 0, sizeof(in_buf));
    in_buf.buffer_type = OMX_ALLOCATE_DRV;
    in_buf.dir = PORT_DIR_INPUT;
    in_buf.buffer_len = 2 * 1024 * 1024;

    msg.chan_num = chan_id;
    msg.in = &in_buf;
    msg.out = &in_buf;
    if (ioctl(fd, VDEC_IOCTL_CHAN_ALLOC_BUFFER, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_ALLOC_BUFFER input");
        goto cleanup;
    }
    printf("[VDEC_TEST] Input buffer allocated: len=%u, share_fd=%d, phy=0x%x\n",
           in_buf.buffer_len, in_buf.share_fd, in_buf.phyaddr);

    void *in_map = mmap(NULL, in_buf.buffer_len, PROT_READ | PROT_WRITE, MAP_SHARED, in_buf.share_fd, 0);
    if (in_map == MAP_FAILED) {
        perror("mmap in_buf");
        goto cleanup;
    }

    // 3. Allocate Output Buffer
    uint32_t out_len = width * height * 3 / 2;
    OMXVDEC_BUF_DESC out_buf;
    memset(&out_buf, 0, sizeof(out_buf));
    out_buf.buffer_type = OMX_ALLOCATE_DRV;
    out_buf.dir = PORT_DIR_OUTPUT;
    out_buf.buffer_len = out_len;

    msg.in = &out_buf;
    msg.out = &out_buf;
    if (ioctl(fd, VDEC_IOCTL_CHAN_ALLOC_BUFFER, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_ALLOC_BUFFER output");
        goto cleanup;
    }
    printf("[VDEC_TEST] Output buffer allocated: len=%u, share_fd=%d, phy=0x%x\n",
           out_buf.buffer_len, out_buf.share_fd, out_buf.phyaddr);

    void *out_map = mmap(NULL, out_buf.buffer_len, PROT_READ | PROT_WRITE, MAP_SHARED, out_buf.share_fd, 0);
    if (out_map == MAP_FAILED) {
        perror("mmap out_buf");
        goto cleanup;
    }

    // 4. Start Channel
    msg.in = NULL;
    msg.out = NULL;
    if (ioctl(fd, VDEC_IOCTL_CHAN_START, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_START");
        goto cleanup;
    }
    printf("[VDEC_TEST] Channel started!\n");

    // 5. Fill input buffer with elementary stream and empty to hardware
    size_t bytes_read = fread(in_map, 1, file_size > in_buf.buffer_len ? in_buf.buffer_len : file_size, fp);
    in_buf.data_len = bytes_read;
    in_buf.data_offset = 0;
    in_buf.flags = 0;

    // Queue output frame to receive decoded data
    msg.in = &out_buf;
    msg.out = NULL;
    ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg);

    // Send input stream to hardware
    double t_start = get_time_sec();
    msg.in = &in_buf;
    msg.out = NULL;
    if (ioctl(fd, VDEC_IOCTL_EMPTY_INPUT_STREAM, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_EMPTY_INPUT_STREAM");
        goto cleanup;
    }
    printf("[VDEC_TEST] Submitted %zu bytes of %s to hardware VPU!\n", bytes_read, codec_str);

    // 6. Wait for VDEC message
    OMXVDEC_MSG_INFO msg_info;
    memset(&msg_info, 0, sizeof(msg_info));
    msg.in = NULL;
    msg.out = &msg_info;

    int timeout_ms = 5000;
    int got_frame = 0;
    while (timeout_ms > 0) {
        if (ioctl(fd, VDEC_IOCTL_CHAN_GET_MSG, &msg) == 0) {
            if (msg_info.msgcode == VDEC_MSG_RESP_OUTPUT_DONE) {
                double t_end = get_time_sec();
                printf("[VDEC_TEST] SUCCESS! Hardware decoded output frame received in %.3f ms!\n",
                       (t_end - t_start) * 1000.0);
                got_frame = 1;

                if (out_file) {
                    FILE *fout = fopen(out_file, "wb");
                    if (fout) {
                        fwrite(out_map, 1, out_len, fout);
                        fclose(fout);
                        printf("[VDEC_TEST] Saved decoded NV12 frame to %s (%u bytes)\n", out_file, out_len);
                    }
                }
                break;
            } else if (msg_info.msgcode == VDEC_MSG_RESP_INPUT_DONE) {
                printf("[VDEC_TEST] Input stream buffer consumed by hardware.\n");
            }
        }
        usleep(5000);
        timeout_ms -= 5;
    }

    if (!got_frame) {
        printf("[VDEC_TEST] Message wait timed out or awaiting additional slices.\n");
    }

    // 7. Stop Channel
    msg.in = NULL;
    msg.out = NULL;
    ioctl(fd, VDEC_IOCTL_CHAN_STOP, &msg);

cleanup:
    msg.in = NULL;
    msg.out = NULL;
    ioctl(fd, VDEC_IOCTL_CHAN_RELEASE, &msg);
    close(fd);
    fclose(fp);
    printf("[VDEC_TEST] Test harness finished.\n");
    return got_frame ? 0 : 2;
}
