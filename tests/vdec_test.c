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

#define NUM_IN_BUFS  2
#define NUM_OUT_BUFS 4

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
    cfg.act_inbuf_num = NUM_IN_BUFS;
    cfg.act_outbuf_num = NUM_OUT_BUFS;

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

    // 2. Allocate Input Buffers
    OMXVDEC_BUF_DESC in_bufs[NUM_IN_BUFS];
    void *in_maps[NUM_IN_BUFS];
    for (int i = 0; i < NUM_IN_BUFS; i++) {
        memset(&in_bufs[i], 0, sizeof(in_bufs[i]));
        in_bufs[i].buffer_type = OMX_ALLOCATE_DRV;
        in_bufs[i].dir = PORT_DIR_INPUT;
        in_bufs[i].buffer_len = 2 * 1024 * 1024;
        in_bufs[i].max_frm_num = NUM_IN_BUFS;

        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &in_bufs[i];
        msg.out = &in_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_CHAN_ALLOC_BUFFER, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_CHAN_ALLOC_BUFFER input");
            goto cleanup;
        }

        int map_fd = (in_bufs[i].share_fd >= 0) ? in_bufs[i].share_fd : fd;
        off_t map_off = (in_bufs[i].share_fd >= 0) ? 0 : in_bufs[i].phyaddr;
        in_maps[i] = mmap(NULL, in_bufs[i].buffer_len, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, map_off);
        if (in_maps[i] == MAP_FAILED) {
            perror("mmap in_buf");
            goto cleanup;
        }
        printf("[VDEC_TEST] Input buffer %d allocated: len=%u, share_fd=%d, phy=0x%x, mmapped=%p\n",
               i, in_bufs[i].buffer_len, in_bufs[i].share_fd, in_bufs[i].phyaddr, in_maps[i]);
    }

    // 3. Allocate Output Buffers
    uint32_t out_len = width * height * 3 / 2;
    OMXVDEC_BUF_DESC out_bufs[NUM_OUT_BUFS];
    void *out_maps[NUM_OUT_BUFS];
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        memset(&out_bufs[i], 0, sizeof(out_bufs[i]));
        out_bufs[i].buffer_type = OMX_ALLOCATE_DRV;
        out_bufs[i].dir = PORT_DIR_OUTPUT;
        out_bufs[i].buffer_len = out_len;
        out_bufs[i].max_frm_num = NUM_OUT_BUFS;

        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        msg.out = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_CHAN_ALLOC_BUFFER, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_CHAN_ALLOC_BUFFER output");
            goto cleanup;
        }

        int map_fd = (out_bufs[i].share_fd >= 0) ? out_bufs[i].share_fd : fd;
        off_t map_off = (out_bufs[i].share_fd >= 0) ? 0 : out_bufs[i].phyaddr;
        out_maps[i] = mmap(NULL, out_bufs[i].buffer_len, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, map_off);
        if (out_maps[i] == MAP_FAILED) {
            perror("mmap out_buf");
            goto cleanup;
        }
        printf("[VDEC_TEST] Output buffer %d allocated: len=%u, share_fd=%d, phy=0x%x, mmapped=%p\n",
               i, out_bufs[i].buffer_len, out_bufs[i].share_fd, out_bufs[i].phyaddr, out_maps[i]);
    }

    // 4. Start Channel
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    if (ioctl(fd, VDEC_IOCTL_CHAN_START, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_START");
        goto cleanup;
    }
    printf("[VDEC_TEST] Channel started!\n");

    // 5. Queue all output buffers to hardware
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_FILL_OUTPUT_FRAME");
            goto cleanup;
        }
    }
    printf("[VDEC_TEST] %d output frames queued for decode.\n", NUM_OUT_BUFS);

    // 6. Fill input buffer with stream and submit to hardware
    size_t bytes_read = fread(in_maps[0], 1, file_size > in_bufs[0].buffer_len ? in_bufs[0].buffer_len : file_size, fp);
    in_bufs[0].data_len = bytes_read;
    in_bufs[0].data_offset = 0;
    in_bufs[0].flags = 0;

    double t_start = get_time_sec();
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    msg.in = &in_bufs[0];
    if (ioctl(fd, VDEC_IOCTL_EMPTY_INPUT_STREAM, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_EMPTY_INPUT_STREAM");
        goto cleanup;
    }
    printf("[VDEC_TEST] Submitted %zu bytes of %s to hardware VPU!\n", bytes_read, codec_str);

    // 7. Wait for VDEC message
    OMXVDEC_MSG_INFO msg_info;
    int timeout_ms = 5000;
    int got_frame = 0;
    while (timeout_ms > 0) {
        memset(&msg_info, 0, sizeof(msg_info));
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.out = &msg_info;

        if (ioctl(fd, VDEC_IOCTL_CHAN_GET_MSG, &msg) == 0) {
            printf("[VDEC_TEST] Received msgcode=0x%x status=%u\n", msg_info.msgcode, msg_info.status_code);
            if (msg_info.msgcode == VDEC_MSG_RESP_OUTPUT_DONE) {
                double t_end = get_time_sec();
                printf("[VDEC_TEST] SUCCESS! Hardware decoded output frame received in %.3f ms (FPS: %.2f)!\n",
                       (t_end - t_start) * 1000.0, 1.0 / (t_end - t_start));
                got_frame = 1;

                if (out_file) {
                    FILE *fout = fopen(out_file, "wb");
                    if (fout) {
                        fwrite(out_maps[0], 1, out_len, fout);
                        fclose(fout);
                        printf("[VDEC_TEST] Saved decoded NV12 frame to %s (%u bytes)\n", out_file, out_len);
                    }
                }
                break;
            } else if (msg_info.msgcode == VDEC_MSG_RESP_INPUT_DONE) {
                printf("[VDEC_TEST] Input stream buffer consumed by hardware.\n");
            } else if (msg_info.msgcode == VDEC_MSG_RESP_START_DONE) {
                printf("[VDEC_TEST] Hardware confirm: START_DONE.\n");
            }
        }
        usleep(5000);
        timeout_ms -= 5;
    }

    if (!got_frame) {
        printf("[VDEC_TEST] Message wait timed out or awaiting additional slices.\n");
    }

    // 8. Stop Channel
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    ioctl(fd, VDEC_IOCTL_CHAN_STOP, &msg);

cleanup:
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    ioctl(fd, VDEC_IOCTL_CHAN_RELEASE, &msg);
    close(fd);
    fclose(fp);
    printf("[VDEC_TEST] Test harness finished.\n");
    return got_frame ? 0 : 2;
}
