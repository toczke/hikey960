/*
 * HiKey960 Kirin 960 VPU (hi_vcodec) Hardware Decoder Simple Test
 * Directly interfaces with /dev/hi_vdec via ioctl using Linux DMA-BUF Heaps
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
#include <linux/dma-heap.h>

#include "drv_omxvdec.h"

#ifndef VDEC_BUFFERFLAG_EOS
#define VDEC_BUFFERFLAG_EOS (0x00000001)
#endif
#ifndef VDEC_BUFFERFLAG_ENDOFFRAME
#define VDEC_BUFFERFLAG_ENDOFFRAME (0x00000010)
#endif
#ifndef VDEC_BUFFERFLAG_CODECCONFIG
#define VDEC_BUFFERFLAG_CODECCONFIG (0x00000080)
#endif

#define NUM_IN_BUFS  2
#define NUM_OUT_BUFS 16
#define IN_BUF_SIZE  (2 * 1024 * 1024)

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static int alloc_dma_buf(size_t len, int *out_fd, void **out_ptr) {
    const char *heaps[] = {
        "/dev/dma_heap/default_cma_region",
        "/dev/dma_heap/reserved",
        "/dev/dma_heap/system",
        NULL
    };

    for (int i = 0; heaps[i] != NULL; i++) {
        int heap_fd = open(heaps[i], O_RDWR | O_CLOEXEC);
        if (heap_fd < 0) continue;

        struct dma_heap_allocation_data heap_data = {
            .len = len,
            .fd = 0,
            .fd_flags = O_RDWR | O_CLOEXEC,
            .heap_flags = 0,
        };

        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap_data) == 0) {
            close(heap_fd);
            void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, heap_data.fd, 0);
            if (ptr == MAP_FAILED) {
                close(heap_data.fd);
                return -1;
            }
            *out_fd = heap_data.fd;
            *out_ptr = ptr;
            return 0;
        }
        close(heap_fd);
    }
    return -1;
}

struct nal_info {
    size_t offset;
    size_t size;
    uint8_t type;
};

static int parse_nals(const uint8_t *data, size_t len, struct nal_info *nals, int max_nals, int is_hevc) {
    int count = 0;
    size_t pos = 0;
    size_t prev_pos = 0;
    int prev_start_len = 0;

    while (pos + 3 < len && count < max_nals) {
        int start_len = 0;
        if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) {
            start_len = 3;
        } else if (pos + 4 < len && data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 1) {
            start_len = 4;
        }

        if (start_len > 0) {
            if (count > 0) {
                nals[count - 1].size = pos - prev_pos;
            }
            nals[count].offset = pos;
            uint8_t header = data[pos + start_len];
            if (is_hevc) {
                nals[count].type = (header >> 1) & 0x3F;
            } else {
                nals[count].type = header & 0x1F;
            }
            prev_pos = pos;
            prev_start_len = start_len;
            count++;
            pos += start_len;
        } else {
            pos++;
        }
    }
    if (count > 0) {
        nals[count - 1].size = len - prev_pos;
    }
    return count;
}

struct stream_feeder {
    int fd;
    int chan_id;
    OMXVDEC_BUF_DESC *in_bufs;
    void **in_maps;
    int *in_busy;
    const uint8_t *file_data;
    size_t file_size;
    struct nal_info *nals;
    int nal_count;
    int is_h264;
    int is_hevc;
    int nal_idx;
    size_t raw_pos;
    int eos_sent;
};

static int send_input_packet(struct stream_feeder *f, int buf_idx) {
    if (f->eos_sent) return 0;

    uint32_t flags = 0;
    size_t send_size = 0;
    OMXVDEC_IOCTL_MSG msg;

    if (f->nal_count > 0) {
        if (f->nal_idx >= f->nal_count) {
            f->in_bufs[buf_idx].data_len = 0;
            f->in_bufs[buf_idx].data_offset = 0;
            f->in_bufs[buf_idx].flags = VDEC_BUFFERFLAG_EOS;
            f->eos_sent = 1;
            memset(&msg, 0, sizeof(msg));
            msg.chan_num = f->chan_id;
            msg.in = &f->in_bufs[buf_idx];
            ioctl(f->fd, VDEC_IOCTL_EMPTY_INPUT_STREAM, &msg);
            f->in_busy[buf_idx] = 1;
            printf("[VDEC_TEST] Sent EOS packet to input buffer %d\n", buf_idx);
            fflush(stdout);
            return 1;
        }

        struct nal_info *n = &f->nals[f->nal_idx];
        memcpy(f->in_maps[buf_idx], f->file_data + n->offset, n->size);
        send_size = n->size;

        if (f->is_h264 && (n->type == 7 || n->type == 8)) {
            flags = VDEC_BUFFERFLAG_CODECCONFIG;
        } else if (f->is_hevc && (n->type == 32 || n->type == 33 || n->type == 34)) {
            flags = VDEC_BUFFERFLAG_CODECCONFIG;
        } else {
            flags = VDEC_BUFFERFLAG_ENDOFFRAME;
        }

        f->nal_idx++;
    } else {
        size_t chunk_size = 32768;
        if (f->raw_pos >= f->file_size) {
            f->in_bufs[buf_idx].data_len = 0;
            f->in_bufs[buf_idx].flags = VDEC_BUFFERFLAG_EOS;
            f->eos_sent = 1;
            memset(&msg, 0, sizeof(msg));
            msg.chan_num = f->chan_id;
            msg.in = &f->in_bufs[buf_idx];
            ioctl(f->fd, VDEC_IOCTL_EMPTY_INPUT_STREAM, &msg);
            f->in_busy[buf_idx] = 1;
            return 1;
        }
        if (f->raw_pos + chunk_size > f->file_size) {
            chunk_size = f->file_size - f->raw_pos;
        }
        memcpy(f->in_maps[buf_idx], f->file_data + f->raw_pos, chunk_size);
        send_size = chunk_size;
        f->raw_pos += chunk_size;
        flags = VDEC_BUFFERFLAG_ENDOFFRAME;
    }

    f->in_bufs[buf_idx].data_len = send_size;
    f->in_bufs[buf_idx].data_offset = 0;
    f->in_bufs[buf_idx].flags = flags;

    memset(&msg, 0, sizeof(msg));
    msg.chan_num = f->chan_id;
    msg.in = &f->in_bufs[buf_idx];
    if (ioctl(f->fd, VDEC_IOCTL_EMPTY_INPUT_STREAM, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_EMPTY_INPUT_STREAM");
        return -1;
    }
    f->in_busy[buf_idx] = 1;
    usleep(2000);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <stream.264/hevc> <codec: h264|hevc|mpeg2|mpeg4|vp8> <width> <height> [out.yuv] [--normal]\n", argv[0]);
        return 1;
    }

    const char *stream_file = argv[1];
    const char *codec_str = argv[2];
    int width = atoi(argv[3]);
    int height = atoi(argv[4]);
    const char *out_file = NULL;
    ePATH_MODE path_mode = PATH_MODE_NATIVE;

    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--normal")) {
            path_mode = PATH_MODE_NORMAL;
        } else if (!strcmp(argv[i], "--native")) {
            path_mode = PATH_MODE_NATIVE;
        } else if (!out_file) {
            out_file = argv[i];
        }
    }

    OMXVDEC_CODEC_TYPE codec_type = OMXVDEC_H264;
    int is_h264 = 0, is_hevc = 0;
    if (!strcmp(codec_str, "h264")) { codec_type = OMXVDEC_H264; is_h264 = 1; }
    else if (!strcmp(codec_str, "hevc")) { codec_type = OMXVDEC_HEVC; is_hevc = 1; }
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

    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data) {
        perror("malloc file_data");
        fclose(fp);
        return 1;
    }
    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        perror("fread file_data");
        free(file_data);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    FILE *fout = NULL;
    if (out_file) {
        fout = fopen(out_file, "wb");
        if (!fout) perror("fopen out_file");
    }

    int fd = open("/dev/hi_vdec", O_RDWR);
    if (fd < 0) {
        perror("open /dev/hi_vdec");
        free(file_data);
        if (fout) fclose(fout);
        return 1;
    }
    printf("[VDEC_TEST] Opened /dev/hi_vdec (fd=%d), path_mode=%s\n",
           fd, path_mode == PATH_MODE_NATIVE ? "NATIVE" : "NORMAL");
    fflush(stdout);

    uint32_t stride = (width + 127) & ~127;
    uint32_t height_align = (height + 31) & ~31;
    uint32_t out_len = (stride * height_align * 3) / 2;

    // 1. Channel Configuration & Creation
    OMXVDEC_CHAN_CFG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cfg_codec_type = codec_type;
    cfg.cfg_width = width;
    cfg.cfg_height = height;
    cfg.cfg_stride = stride;
    cfg.cfg_color_format = OMX_PIX_FMT_NV12;
    cfg.path_mode = path_mode;
    cfg.act_inbuf_size = IN_BUF_SIZE;
    cfg.act_inbuf_num = NUM_IN_BUFS;
    cfg.act_outbuf_num = NUM_OUT_BUFS;
    cfg.is_tvp = 0;

    int chan_id = -1;
    OMXVDEC_IOCTL_MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.in = &cfg;
    msg.out = &chan_id;

    if (ioctl(fd, VDEC_IOCTL_CHAN_CREATE, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_CREATE");
        close(fd);
        free(file_data);
        if (fout) fclose(fout);
        return 1;
    }
    printf("[VDEC_TEST] Channel created! chan_id=%d\n", chan_id);
    fflush(stdout);

    // 2. Allocate & Bind Input Buffers
    OMXVDEC_BUF_DESC in_bufs[NUM_IN_BUFS];
    int in_fds[NUM_IN_BUFS];
    void *in_maps[NUM_IN_BUFS];
    int in_busy[NUM_IN_BUFS];

    for (int i = 0; i < NUM_IN_BUFS; i++) {
        in_busy[i] = 0;
        if (alloc_dma_buf(IN_BUF_SIZE, &in_fds[i], &in_maps[i]) < 0) {
            fprintf(stderr, "Failed to allocate DMA-BUF for in_buf %d\n", i);
            goto cleanup;
        }

        memset(&in_bufs[i], 0, sizeof(in_bufs[i]));
        in_bufs[i].buffer_type = OMX_USE_NATIVE;
        in_bufs[i].dir = PORT_DIR_INPUT;
        in_bufs[i].buffer_len = IN_BUF_SIZE;
        in_bufs[i].bufferaddr = in_maps[i];
        in_bufs[i].share_fd = in_fds[i];
        in_bufs[i].max_frm_num = NUM_IN_BUFS;

        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &in_bufs[i];
        msg.out = &in_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_CHAN_BIND_BUFFER, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_CHAN_BIND_BUFFER input");
            goto cleanup;
        }
    }
    printf("[VDEC_TEST] %d input buffers bound.\n", NUM_IN_BUFS);
    fflush(stdout);

    // 3. Allocate & Bind Output Buffers
    OMXVDEC_BUF_DESC out_bufs[NUM_OUT_BUFS];
    int out_fds[NUM_OUT_BUFS];
    void *out_maps[NUM_OUT_BUFS];
    int out_busy[NUM_OUT_BUFS];

    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        out_busy[i] = 0;
        if (alloc_dma_buf(out_len, &out_fds[i], &out_maps[i]) < 0) {
            fprintf(stderr, "Failed to allocate DMA-BUF for out_buf %d\n", i);
            goto cleanup;
        }

        memset(&out_bufs[i], 0, sizeof(out_bufs[i]));
        out_bufs[i].buffer_type = OMX_USE_NATIVE;
        out_bufs[i].dir = PORT_DIR_OUTPUT;
        out_bufs[i].buffer_len = out_len;
        out_bufs[i].bufferaddr = out_maps[i];
        out_bufs[i].share_fd = out_fds[i];
        out_bufs[i].max_frm_num = NUM_OUT_BUFS;
        out_bufs[i].out_frame.stride = stride;

        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        msg.out = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_CHAN_BIND_BUFFER, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_CHAN_BIND_BUFFER output");
            goto cleanup;
        }
    }
    printf("[VDEC_TEST] %d output buffers bound (len=%u, stride=%u).\n", NUM_OUT_BUFS, out_len, stride);
    fflush(stdout);

    // 4. Start Channel
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    if (ioctl(fd, VDEC_IOCTL_CHAN_START, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_CHAN_START");
        goto cleanup;
    }
    printf("[VDEC_TEST] Channel started successfully!\n");
    fflush(stdout);

    // 5. Queue all output buffers to hardware
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_FILL_OUTPUT_FRAME");
            goto cleanup;
        }
        out_busy[i] = 1;
    }
    printf("[VDEC_TEST] %d output frames queued to hardware.\n", NUM_OUT_BUFS);
    fflush(stdout);

    struct nal_info nals[512];
    int nal_count = 0;
    if (is_h264 || is_hevc) {
        nal_count = parse_nals(file_data, file_size, nals, 512, is_hevc);
        printf("[VDEC_TEST] Parsed %d NAL units from stream.\n", nal_count);
        fflush(stdout);
    }

    struct stream_feeder feeder = {
        .fd = fd,
        .chan_id = chan_id,
        .in_bufs = in_bufs,
        .in_maps = in_maps,
        .in_busy = in_busy,
        .file_data = file_data,
        .file_size = file_size,
        .nals = nals,
        .nal_count = nal_count,
        .is_h264 = is_h264,
        .is_hevc = is_hevc,
        .nal_idx = 0,
        .raw_pos = 0,
        .eos_sent = 0,
    };

    double t_start = get_time_sec();

    for (int i = 0; i < NUM_IN_BUFS; i++) {
        if (!feeder.eos_sent) send_input_packet(&feeder, i);
    }

    OMXVDEC_MSG_INFO msg_info;
    int timeout_ms = 10000;
    int frames_decoded = 0;
    int idle_timeout_ms = 3000;

    while (timeout_ms > 0) {
        memset(&msg_info, 0, sizeof(msg_info));
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.out = &msg_info;

        int ret = ioctl(fd, VDEC_IOCTL_CHAN_GET_MSG, &msg);
        if (ret == 0) {
            if (msg_info.msgcode == VDEC_MSG_RESP_OUTPUT_DONE) {
                if (msg_info.status_code != 0) {
                    continue;
                }

                OMXVDEC_BUF_DESC *pout = &msg_info.msgdata.buf;
                int buf_idx = -1;
                for (int i = 0; i < NUM_OUT_BUFS; i++) {
                    if (out_bufs[i].phyaddr == pout->phyaddr) {
                        buf_idx = i;
                        break;
                    }
                }
                if (buf_idx < 0) continue;

                out_busy[buf_idx] = 0;

                if (pout->data_len > 0) {
                    frames_decoded++;
                    double t_now = get_time_sec();
                    printf("[VDEC_TEST] >>> FRAME #%d: data_len=%u, stride=%u, %ux%u in %.2f ms (%.2f FPS) <<<\n",
                           frames_decoded, pout->data_len, pout->out_frame.stride,
                           pout->out_frame.frame_width, pout->out_frame.frame_height,
                           (t_now - t_start) * 1000.0, frames_decoded / (t_now - t_start));
                    fflush(stdout);

                    if (fout) {
                        fwrite(out_maps[buf_idx], 1, pout->data_len, fout);
                    }
                }

                if (pout->flags & VDEC_BUFFERFLAG_EOS) {
                    printf("[VDEC_TEST] Output port received EOS!\n");
                    fflush(stdout);
                    break;
                }

                if (!out_busy[buf_idx]) {
                    memset(&msg, 0, sizeof(msg));
                    msg.chan_num = chan_id;
                    msg.in = &out_bufs[buf_idx];
                    if (ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg) == 0) {
                        out_busy[buf_idx] = 1;
                    }
                }
                idle_timeout_ms = 3000;
            } else if (msg_info.msgcode == VDEC_MSG_RESP_INPUT_DONE) {
                OMXVDEC_BUF_DESC *pin = &msg_info.msgdata.buf;
                for (int i = 0; i < NUM_IN_BUFS; i++) {
                    if (in_bufs[i].phyaddr == pin->phyaddr) {
                        in_busy[i] = 0;
                        if (!feeder.eos_sent) send_input_packet(&feeder, i);
                        break;
                    }
                }
            } else if (msg_info.msgcode == VDEC_MSG_RESP_START_DONE) {
                printf("[VDEC_TEST] Hardware confirm: START_DONE.\n");
                fflush(stdout);
            } else if (msg_info.msgcode == VDEC_EVT_REPORT_IMG_SIZE_CHG) {
                printf("[VDEC_TEST] Event: IMG_SIZE_CHG reported by decoder!\n");
                fflush(stdout);
            } else if (msg_info.msgcode == VDEC_EVT_REPORT_SEQ_INFO_CHG) {
                OMXVDEC_SEQ_INFO *s = &msg_info.msgdata.seq_info;
                printf("[VDEC_TEST] Event: SEQ_INFO_CHG reported! %ux%u stride=%u frame_size=%u min_num=%u max_num=%u\n",
                       s->dec_width, s->dec_height, s->stride, s->frame_size, s->min_frame_num, s->max_frame_num);
                fflush(stdout);
            }
        } else {
            usleep(2000);
            timeout_ms -= 2;
            if (feeder.eos_sent) {
                idle_timeout_ms -= 2;
                if (idle_timeout_ms <= 0) {
                    printf("[VDEC_TEST] Decoding complete (idle timeout after EOS).\n");
                    fflush(stdout);
                    break;
                }
            }
        }
    }

    double t_total = get_time_sec() - t_start;
    printf("[VDEC_TEST] SUMMARY: %d frames decoded in %.3f s -> %.2f FPS\n",
           frames_decoded, t_total, frames_decoded > 0 ? (frames_decoded / t_total) : 0.0);
    fflush(stdout);

cleanup:
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    ioctl(fd, VDEC_IOCTL_CHAN_STOP, &msg);

    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    ioctl(fd, VDEC_IOCTL_CHAN_RELEASE, &msg);

    for (int i = 0; i < NUM_IN_BUFS; i++) {
        if (in_maps[i] && in_maps[i] != MAP_FAILED) munmap(in_maps[i], IN_BUF_SIZE);
        if (in_fds[i] >= 0) close(in_fds[i]);
    }
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        if (out_maps[i] && out_maps[i] != MAP_FAILED) munmap(out_maps[i], out_len);
        if (out_fds[i] >= 0) close(out_fds[i]);
    }

    close(fd);
    free(file_data);
    if (fout) fclose(fout);
    printf("[VDEC_TEST] Cleaned up all resources successfully.\n");
    fflush(stdout);
    return frames_decoded > 0 ? 0 : 2;
}
