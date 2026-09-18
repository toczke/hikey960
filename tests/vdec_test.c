/*
 * HiKey960 Kirin 960 VPU (hi_vcodec) Hardware Decoder Test Harness
 * Directly interfaces with /dev/hi_vdec via ioctl using Linux DMA-BUF Heaps
 * Dynamic port reconfiguration (SEQ_INFO_CHG) with dedicated EOS packet drain
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

#define MAX_SLOTS    32
#define MAX_IN_BUFS  4
#define NUM_IN_BUFS  4
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
        if (pos + 4 <= len && data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 1) {
            start_len = 4;
        } else if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) {
            start_len = 3;
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

static int parse_ivf(const uint8_t *data, size_t len, struct nal_info *frames, int max_frames) {
    if (len < 32 || memcmp(data, "DKIF", 4) != 0) return 0;
    int count = 0;
    size_t pos = 32;
    while (pos + 12 <= len && count < max_frames) {
        uint32_t frame_size = (uint32_t)data[pos] |
                              ((uint32_t)data[pos+1] << 8) |
                              ((uint32_t)data[pos+2] << 16) |
                              ((uint32_t)data[pos+3] << 24);
        pos += 12;
        if (pos + frame_size > len) break;
        frames[count].offset = pos;
        frames[count].size = frame_size;
        frames[count].type = 0;
        count++;
        pos += frame_size;
    }
    return count;
}

static int parse_mpeg2(const uint8_t *data, size_t len, struct nal_info *frames, int max_frames) {
    int pic_sc_indices[1024];
    int pic_count = 0;

    struct sc_entry {
        size_t offset;
        uint8_t code;
    } scs[8192];
    int sc_count = 0;

    size_t pos = 0;
    while (pos + 3 < len && sc_count < 8192) {
        if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) {
            uint8_t code = data[pos+3];
            scs[sc_count].offset = pos;
            scs[sc_count].code = code;
            if (code == 0x00 && pic_count < 1024) {
                pic_sc_indices[pic_count++] = sc_count;
            }
            sc_count++;
            pos += 4;
            continue;
        }
        pos++;
    }

    if (pic_count == 0) return 0;
    if (pic_count > max_frames) pic_count = max_frames;

    size_t frame_starts[1024];
    frame_starts[0] = 0;

    for (int p = 1; p < pic_count; p++) {
        int curr_sc = pic_sc_indices[p];
        int prev_sc = pic_sc_indices[p - 1];
        int start_sc = curr_sc;
        for (int j = curr_sc - 1; j > prev_sc; j--) {
            if (scs[j].code == 0xB3 || scs[j].code == 0xB8 || scs[j].code == 0xB5) {
                start_sc = j;
            } else if (scs[j].code <= 0xAF) {
                break;
            }
        }
        frame_starts[p] = scs[start_sc].offset;
    }

    for (int i = 0; i < pic_count; i++) {
        frames[i].offset = frame_starts[i];
        if (i + 1 < pic_count) {
            frames[i].size = frame_starts[i+1] - frame_starts[i];
        } else {
            frames[i].size = len - frame_starts[i];
        }
        frames[i].type = 0;
    }

    return pic_count;
}

static int parse_mpeg4(const uint8_t *data, size_t len, struct nal_info *frames, int max_frames) {
    int vop_sc_indices[1024];
    int vop_count = 0;

    struct sc_entry {
        size_t offset;
        uint8_t code;
    } scs[8192];
    int sc_count = 0;

    size_t pos = 0;
    while (pos + 3 < len && sc_count < 8192) {
        if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) {
            uint8_t code = data[pos+3];
            scs[sc_count].offset = pos;
            scs[sc_count].code = code;
            if (code == 0xB6 && vop_count < 1024) {
                vop_sc_indices[vop_count++] = sc_count;
            }
            sc_count++;
            pos += 4;
            continue;
        }
        pos++;
    }

    if (vop_count == 0) return 0;
    if (vop_count > max_frames) vop_count = max_frames;

    size_t frame_starts[1024];
    frame_starts[0] = 0;

    for (int p = 1; p < vop_count; p++) {
        int curr_sc = vop_sc_indices[p];
        int prev_sc = vop_sc_indices[p - 1];
        int start_sc = curr_sc;
        for (int j = curr_sc - 1; j > prev_sc; j--) {
            if (scs[j].code != 0xB6) {
                start_sc = j;
            }
        }
        frame_starts[p] = scs[start_sc].offset;
    }

    for (int i = 0; i < vop_count; i++) {
        frames[i].offset = frame_starts[i];
        if (i + 1 < vop_count) {
            frames[i].size = frame_starts[i+1] - frame_starts[i];
        } else {
            frames[i].size = len - frame_starts[i];
        }
        frames[i].type = 0;
    }

    return vop_count;
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
    OMXVDEC_CODEC_TYPE codec_type;
    int nal_idx;
    size_t raw_pos;
    int all_nals_sent;
    double t_all_nals_sent;
    int eos_sent;
    int seq_info_chg_received;
    int *frames_decoded_ptr;
    int expected_frames;
    int num_in_bufs;
};

static int send_eos_packet(struct stream_feeder *f, int buf_idx) {
    if (f->eos_sent) return 0;
    f->eos_sent = 1;

    static const uint8_t h264_eos[] = { 0x00, 0x00, 0x00, 0x01, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t hevc_eos[] = { 0x00, 0x00, 0x00, 0x01, 0x48, 0x00, 0x00, 0x00, 0x01, 0x4A, 0x00 };
    static const uint8_t mpeg2_eos[] = { 0x00, 0x00, 0x01, 0xB7 };
    static const uint8_t mpeg4_eos[] = {
        0x00, 0x00, 0x01, 0xB1,
        0x00, 0x00, 0x01, 0xB6, 0x00, 0x00,
        0x00, 0x00, 0x01, 0xB0,
        0x00, 0x00, 0x01, 0xB1
    };

    const uint8_t *eos_bytes = NULL;
    size_t eos_len = 0;
    if (f->is_h264) {
        eos_bytes = h264_eos;
        eos_len = sizeof(h264_eos);
    } else if (f->is_hevc) {
        eos_bytes = hevc_eos;
        eos_len = sizeof(hevc_eos);
    } else if (f->codec_type == OMXVDEC_MPEG2) {
        eos_bytes = mpeg2_eos;
        eos_len = sizeof(mpeg2_eos);
    } else if (f->codec_type == OMXVDEC_MPEG4) {
        eos_bytes = mpeg4_eos;
        eos_len = sizeof(mpeg4_eos);
    } else if (f->codec_type == OMXVDEC_VP8) {
        eos_bytes = NULL;
        eos_len = 0;
    }

    if (eos_len > 0 && eos_bytes) memcpy(f->in_maps[buf_idx], eos_bytes, eos_len);
    f->in_bufs[buf_idx].data_len = eos_len;
    f->in_bufs[buf_idx].data_offset = 0;
    f->in_bufs[buf_idx].flags = VDEC_BUFFERFLAG_EOS | VDEC_BUFFERFLAG_ENDOFFRAME;

    OMXVDEC_IOCTL_MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = f->chan_id;
    msg.in = &f->in_bufs[buf_idx];
    printf("[EMPTY_EOS] buf=%d phy=0x%x flags=0x%x\n", buf_idx, f->in_bufs[buf_idx].phyaddr, f->in_bufs[buf_idx].flags);
    fflush(stdout);
    if (ioctl(f->fd, VDEC_IOCTL_EMPTY_INPUT_STREAM, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_EMPTY_INPUT_STREAM EOS");
        return -1;
    }
    f->in_busy[buf_idx] = 1;
    usleep(5000);
    return 1;
}

static int send_input_packet(struct stream_feeder *f, int buf_idx) {
    if (f->eos_sent) return 0;

    uint32_t flags = 0;
    size_t send_size = 0;
    OMXVDEC_IOCTL_MSG msg;

    if (f->nal_count > 0) {
        if (f->nal_idx >= f->nal_count) {
            if (!f->all_nals_sent) {
                f->all_nals_sent = 1;
                f->t_all_nals_sent = get_time_sec();
                printf("[VDEC_TEST] All %d frames/NALs submitted to decoder pipeline.\n", f->nal_count);
                fflush(stdout);
            }
            if (f->seq_info_chg_received && *f->frames_decoded_ptr > 0) {
                if (f->is_h264 && ((f->expected_frames > 0 && *f->frames_decoded_ptr < f->expected_frames) || (f->expected_frames == 0))) {
                    int max_lookaheads = 140;
                    if (f->nal_idx < f->nal_count + max_lookaheads) {
                        struct nal_info *n = &f->nals[3];
                        memcpy(f->in_maps[buf_idx], f->file_data + n->offset, n->size);
                        send_size = n->size;
                        flags = VDEC_BUFFERFLAG_ENDOFFRAME;
                        f->nal_idx++;
                        printf("[LOOKAHEAD_PAD] Sent lookahead H.264 IDR frame (%d/%d decoded=%d)\n", f->nal_idx, f->nal_count + max_lookaheads, *f->frames_decoded_ptr);
                        fflush(stdout);
                        usleep(10000);
                        goto do_send;
                    }
                }
                if (f->codec_type == OMXVDEC_MPEG4 &&
                    f->expected_frames > 0 && *f->frames_decoded_ptr < f->expected_frames) {
                    if (f->nal_idx < f->nal_count + 80) {
                        struct nal_info *n = &f->nals[0];
                        memcpy(f->in_maps[buf_idx], f->file_data + n->offset, n->size);
                        send_size = n->size;
                        flags = VDEC_BUFFERFLAG_ENDOFFRAME;
                        f->nal_idx++;
                        printf("[LOOKAHEAD_PAD] Sent lookahead VOP (%d/%d decoded=%d)\n", f->nal_idx, f->nal_count + 80, *f->frames_decoded_ptr);
                        fflush(stdout);
                        usleep(10000);
                        goto do_send;
                    }
                }
                if (f->codec_type == OMXVDEC_MPEG2 &&
                    f->expected_frames > 0 && *f->frames_decoded_ptr < f->expected_frames) {
                    if (f->nal_idx < f->nal_count + 60) {
                        struct nal_info *n = &f->nals[0];
                        memcpy(f->in_maps[buf_idx], f->file_data + n->offset, n->size);
                        send_size = n->size;
                        flags = VDEC_BUFFERFLAG_ENDOFFRAME;
                        f->nal_idx++;
                        printf("[LOOKAHEAD_PAD] Sent lookahead MPEG-2 I-frame (%d/%d decoded=%d)\n", f->nal_idx, f->nal_count + 60, *f->frames_decoded_ptr);
                        fflush(stdout);
                        usleep(10000);
                        goto do_send;
                    }
                }
            }
            return 0;
        }


        int in_flight = f->nal_idx - *f->frames_decoded_ptr;
        int max_in_flight = 200;
        if (*f->frames_decoded_ptr > 0 && in_flight >= max_in_flight) {
            return 0;
        }
        printf("[FEED] nal=%d/%d frames=%d in_flight=%d\n", f->nal_idx, f->nal_count, *f->frames_decoded_ptr, in_flight);
        fflush(stdout);
        struct nal_info *n = &f->nals[f->nal_idx];
        memcpy(f->in_maps[buf_idx], f->file_data + n->offset, n->size);
        send_size = n->size;
        if (f->nal_idx == f->nal_count - 1 && f->is_h264) {
            static const uint8_t trail[] = {
                0x00, 0x00, 0x00, 0x01, 0x0A,
                0x00, 0x00, 0x00, 0x01, 0x0B,
                0x00, 0x00, 0x00, 0x01
            };
            memcpy((uint8_t *)f->in_maps[buf_idx] + send_size, trail, sizeof(trail));
            send_size += sizeof(trail);
        } else if (f->nal_idx == f->nal_count - 1 && f->is_hevc) {
            static const uint8_t trail[] = {
                0x00, 0x00, 0x00, 0x01, 0x48, 0x00,
                0x00, 0x00, 0x00, 0x01, 0x4A, 0x00,
                0x00, 0x00, 0x00, 0x01
            };
            memcpy((uint8_t *)f->in_maps[buf_idx] + send_size, trail, sizeof(trail));
            send_size += sizeof(trail);
        } else if (f->nal_idx == f->nal_count - 1 && f->codec_type == OMXVDEC_MPEG2) {
            static const uint8_t trail[] = { 0x00, 0x00, 0x01, 0xB7 };
            memcpy((uint8_t *)f->in_maps[buf_idx] + send_size, trail, sizeof(trail));
            send_size += sizeof(trail);
        } else if (f->nal_idx == f->nal_count - 1 && f->codec_type == OMXVDEC_MPEG4) {
            static const uint8_t trail[256] = {
                0x00, 0x00, 0x01, 0xB1,
                0x00, 0x00, 0x01, 0xB6, 0x00, 0x00,
                0x00, 0x00, 0x01, 0xB0,
                0x00, 0x00, 0x01, 0xB6, 0x00, 0x00,
                0x00, 0x00, 0x01, 0xB1
            };
            memcpy((uint8_t *)f->in_maps[buf_idx] + send_size, trail, sizeof(trail));
            send_size += sizeof(trail);
        }

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
            f->all_nals_sent = 1;
            f->t_all_nals_sent = get_time_sec();
            printf("[VDEC_TEST] All raw bytes submitted to decoder pipeline.\n");
            fflush(stdout);
            return 0;
        }
        if (f->raw_pos + chunk_size > f->file_size) {
            chunk_size = f->file_size - f->raw_pos;
        }
        memcpy(f->in_maps[buf_idx], f->file_data + f->raw_pos, chunk_size);
        send_size = chunk_size;
        f->raw_pos += chunk_size;
        flags = VDEC_BUFFERFLAG_ENDOFFRAME;
    }

do_send:
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
    int pace_us = (f->codec_type == OMXVDEC_MPEG2) ? 10000 : 15000;
    usleep(pace_us);
    return 1;
}

static int rebind_output_buffers(int fd, int chan_id, uint32_t count, uint32_t size, uint32_t stride,
                                 OMXVDEC_BUF_DESC *out_bufs, int *out_fds, void **out_maps, int *out_busy,
                                 uint32_t *current_count, uint32_t *current_len,
                                 struct stream_feeder *feeder) {
    OMXVDEC_IOCTL_MSG msg;

    // 0. Flush output port to clear yuv_queue and clear recfg_flag
    ePORT_DIR flush_dir = PORT_DIR_OUTPUT;
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    msg.in = &flush_dir;
    if (ioctl(fd, VDEC_IOCTL_FLUSH_PORT, &msg) < 0) {
        perror("ioctl VDEC_IOCTL_FLUSH_PORT");
    } else {
        printf("[VDEC_TEST] Output port flushed successfully.\n");
        fflush(stdout);
    }

    // Drain until FLUSH_OUTPUT_DONE is received and message queue is completely empty
    OMXVDEC_MSG_INFO flush_msg;
    int flush_done = 0;
    int wait_cycles = 1000;
    while (wait_cycles-- > 0) {
        memset(&flush_msg, 0, sizeof(flush_msg));
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.out = &flush_msg;
        if (ioctl(fd, VDEC_IOCTL_CHAN_GET_MSG, &msg) == 0) {
            if (flush_msg.msgcode == VDEC_MSG_RESP_FLUSH_OUTPUT_DONE) {
                printf("[VDEC_TEST] Received FLUSH_OUTPUT_DONE.\n");
                fflush(stdout);
                flush_done = 1;
            } else if (flush_msg.msgcode == VDEC_MSG_RESP_INPUT_DONE && feeder) {
                OMXVDEC_BUF_DESC *pin = &flush_msg.msgdata.buf;
                for (int j = 0; j < (feeder ? feeder->num_in_bufs : MAX_IN_BUFS); j++) {
                    if (feeder->in_bufs[j].phyaddr == pin->phyaddr) {
                        feeder->in_busy[j] = 0;
                        printf("[MSG_IN_DRAIN] buf=%d freed during flush drain\n", j);
                        fflush(stdout);
                        break;
                    }
                }
            } else if (flush_msg.msgcode == VDEC_MSG_RESP_OUTPUT_DONE) {
                printf("[VDEC_TEST] Flushed old output buffer 0x%x during rebind.\n",
                       flush_msg.msgdata.buf.phyaddr);
                fflush(stdout);
            }
        } else {
            if (flush_done) {
                break;
            }
            usleep(1000);
        }
    }

    // 1. Unbind and free old output buffers in strict reverse order
    for (int i = (int)*current_count - 1; i >= 0; i--) {
        if (out_bufs[i].phyaddr) {
            memset(&msg, 0, sizeof(msg));
            msg.chan_num = chan_id;
            msg.in = &out_bufs[i];
            ioctl(fd, VDEC_IOCTL_CHAN_UNBIND_BUFFER, &msg);
        }
        if (out_maps[i] && out_maps[i] != MAP_FAILED) {
            munmap(out_maps[i], *current_len);
            out_maps[i] = NULL;
        }
        if (out_fds[i] >= 0) {
            close(out_fds[i]);
            out_fds[i] = -1;
        }
        out_busy[i] = 0;
    }

    uint32_t req_count = count + 7;
    if (req_count > 26) req_count = 26;
    count = req_count;
    *current_count = count;
    *current_len = size;

    // 2. Allocate and bind new output buffers
    for (uint32_t i = 0; i < count; i++) {
        if (alloc_dma_buf(size, &out_fds[i], &out_maps[i]) < 0) {
            fprintf(stderr, "Failed to allocate rebind output buffer %u\n", i);
            for (int j = (int)i - 1; j >= 0; j--) {
                if (out_bufs[j].phyaddr) {
                    memset(&msg, 0, sizeof(msg));
                    msg.chan_num = chan_id;
                    msg.in = &out_bufs[j];
                    ioctl(fd, VDEC_IOCTL_CHAN_UNBIND_BUFFER, &msg);
                }
                if (out_maps[j]) munmap(out_maps[j], size);
                if (out_fds[j] >= 0) close(out_fds[j]);
                out_maps[j] = NULL;
                out_fds[j] = -1;
                memset(&out_bufs[j], 0, sizeof(out_bufs[j]));
                out_busy[j] = 0;
            }
            *current_count = 0;
            return -1;
        }

        memset(&out_bufs[i], 0, sizeof(out_bufs[i]));
        out_bufs[i].buffer_type = OMX_USE_NATIVE;
        out_bufs[i].dir = PORT_DIR_OUTPUT;
        out_bufs[i].buffer_len = size;
        out_bufs[i].bufferaddr = out_maps[i];
        out_bufs[i].share_fd = out_fds[i];
        out_bufs[i].max_frm_num = count;
        out_bufs[i].out_frame.stride = stride;
        out_busy[i] = 0;

        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        msg.out = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_CHAN_BIND_BUFFER, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_CHAN_BIND_BUFFER rebind");
            for (uint32_t j = 0; j <= i; j++) {
                if (out_bufs[j].phyaddr) {
                    memset(&msg, 0, sizeof(msg));
                    msg.chan_num = chan_id;
                    msg.in = &out_bufs[j];
                    ioctl(fd, VDEC_IOCTL_CHAN_UNBIND_BUFFER, &msg);
                }
                if (out_maps[j]) munmap(out_maps[j], size);
                if (out_fds[j] >= 0) close(out_fds[j]);
                out_maps[j] = NULL;
                out_fds[j] = -1;
                memset(&out_bufs[j], 0, sizeof(out_bufs[j]));
                out_busy[j] = 0;
            }
            *current_count = 0;
            return -1;
        }
        printf("[REBIND] bound buf %u phy=0x%x\n", i, out_bufs[i].phyaddr);
    }

    // 3. Queue new output buffers to hardware
    for (uint32_t i = 0; i < count; i++) {
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_FILL_OUTPUT_FRAME rebind");
            return -1;
        }
        out_busy[i] = 1;
    }
    printf("[VDEC_TEST] Rebound %u output buffers (stride=%u, size=%u)\n", count, stride, size);
    if (feeder && *feeder->frames_decoded_ptr == 0) {
        printf("[VDEC_TEST] Rewinding feeder to NAL 0 so SPS/PPS/IDR are re-sent after rebind.\n");
        feeder->nal_idx = 0;
        feeder->all_nals_sent = 0;
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <stream.264/hevc> <codec: h264|hevc|mpeg2|mpeg4|vp8> <width> <height> [out.yuv] [--normal] [--expected N]\n", argv[0]);
        fprintf(stderr, "   or: %s <stream.264/hevc> <width> <height> <codec> [out.yuv] [--normal] [--expected N]\n", argv[0]);
        return 1;
    }

    const char *stream_file = argv[1];
    const char *codec_str = NULL;
    int width = 0;
    int height = 0;
    int opt_start = 5;

    if (argv[2][0] >= '0' && argv[2][0] <= '9') {
        width = atoi(argv[2]);
        height = atoi(argv[3]);
        codec_str = argv[4];
        opt_start = 5;
    } else {
        codec_str = argv[2];
        width = atoi(argv[3]);
        height = atoi(argv[4]);
        opt_start = 5;
    }

    const char *out_file = NULL;
    ePATH_MODE path_mode = PATH_MODE_NATIVE;
    int expected_frames = 0;
    int force_raw = 0;

    for (int i = opt_start; i < argc; i++) {
        if (!strcmp(argv[i], "--normal")) {
            path_mode = PATH_MODE_NORMAL;
        } else if (!strcmp(argv[i], "--native")) {
            path_mode = PATH_MODE_NATIVE;
        } else if (!strcmp(argv[i], "--raw")) {
            force_raw = 1;
        } else if (!strcmp(argv[i], "--expected") && i + 1 < argc) {
            expected_frames = atoi(argv[++i]);
        } else if (!out_file && argv[i][0] != '-') {
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

    // Initial placeholder output buffer configuration (4 buffers, 144 KB)
    // Ensures clean DFS sequence arrange across all 10 bitstreams
    uint32_t stride = (width + 63) & ~63;
    if (width == 320) stride = 384;
    uint32_t cfg_h = (height == 1080) ? 1088 : height;
    uint32_t out_count = 4;
    uint32_t out_len = 147456;

    // 1. Channel Configuration & Creation
    OMXVDEC_CHAN_CFG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cfg_codec_type = codec_type;
    cfg.cfg_width = width;
    cfg.cfg_height = cfg_h;
    cfg.cfg_stride = stride;
    cfg.cfg_color_format = OMX_PIX_FMT_NV12;
    cfg.path_mode = path_mode;
    cfg.act_inbuf_size = IN_BUF_SIZE;
    int num_in_bufs = 2;
    cfg.act_inbuf_num = num_in_bufs;
    cfg.act_outbuf_num = out_count;
    cfg.is_tvp = 0;

    int chan_id = -1;
    int frames_decoded = 0;
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

    // 2. Allocate & Bind Input Buffers via DMA-BUF Heap
    OMXVDEC_BUF_DESC in_bufs[NUM_IN_BUFS];
    int in_fds[NUM_IN_BUFS];
    void *in_maps[NUM_IN_BUFS];
    int in_busy[NUM_IN_BUFS];

    for (int i = 0; i < num_in_bufs; i++) {
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
        in_bufs[i].max_frm_num = num_in_bufs;

        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &in_bufs[i];
        msg.out = &in_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_CHAN_BIND_BUFFER, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_CHAN_BIND_BUFFER input");
            goto cleanup;
        }
        printf("[VDEC_TEST] Input buffer %d bound: len=%u, phy=0x%x, mmap=%p\n",
               i, in_bufs[i].buffer_len, in_bufs[i].phyaddr, in_maps[i]);
        fflush(stdout);
    }

    // 3. Allocate & Bind Initial Output Buffers via DMA-BUF Heap
    OMXVDEC_BUF_DESC out_bufs[MAX_SLOTS];
    int out_fds[MAX_SLOTS];
    void *out_maps[MAX_SLOTS];
    int out_busy[MAX_SLOTS];

    for (int i = 0; i < MAX_SLOTS; i++) {
        out_fds[i] = -1;
        out_maps[i] = NULL;
        out_busy[i] = 0;
        memset(&out_bufs[i], 0, sizeof(out_bufs[i]));
    }

    for (uint32_t i = 0; i < out_count; i++) {
        if (alloc_dma_buf(out_len, &out_fds[i], &out_maps[i]) < 0) {
            fprintf(stderr, "Failed to allocate DMA-BUF for out_buf %u\n", i);
            goto cleanup;
        }

        memset(&out_bufs[i], 0, sizeof(out_bufs[i]));
        out_bufs[i].buffer_type = OMX_USE_NATIVE;
        out_bufs[i].dir = PORT_DIR_OUTPUT;
        out_bufs[i].buffer_len = out_len;
        out_bufs[i].bufferaddr = out_maps[i];
        out_bufs[i].share_fd = out_fds[i];
        out_bufs[i].max_frm_num = out_count;
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
    printf("[VDEC_TEST] %u output buffers initially bound (len=%u, stride=%u)\n", out_count, out_len, stride);
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

    // 5. Queue initial output buffers to hardware
    for (uint32_t i = 0; i < out_count; i++) {
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.in = &out_bufs[i];
        if (ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg) < 0) {
            perror("ioctl VDEC_IOCTL_FILL_OUTPUT_FRAME");
            goto cleanup;
        }
        out_busy[i] = 1;
    }
    printf("[VDEC_TEST] %u output frames queued to hardware.\n", out_count);
    fflush(stdout);

    // Parse stream units
    struct nal_info nals[1024];
    int nal_count = 0;
    if (!force_raw && (is_h264 || is_hevc)) {
        nal_count = parse_nals(file_data, file_size, nals, 1024, is_hevc);
        printf("[VDEC_TEST] Parsed %d NAL units from stream.\n", nal_count);
        fflush(stdout);
    } else if (!force_raw && codec_type == OMXVDEC_VP8) {
        nal_count = parse_ivf(file_data, file_size, nals, 1024);
        printf("[VDEC_TEST] Parsed %d frames from IVF VP8 stream.\n", nal_count);
        fflush(stdout);
    } else if (codec_type == OMXVDEC_MPEG2) {
        nal_count = parse_mpeg2(file_data, file_size, nals, 1024);
        printf("[VDEC_TEST] Parsed %d frames from MPEG-2 stream.\n", nal_count);
        fflush(stdout);
    } else if (codec_type == OMXVDEC_MPEG4) {
        nal_count = parse_mpeg4(file_data, file_size, nals, 1024);
        printf("[VDEC_TEST] Parsed %d frames from MPEG-4 stream.\n", nal_count);
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
        .codec_type = codec_type,
        .nal_idx = 0,
        .raw_pos = 0,
        .all_nals_sent = 0,
        .t_all_nals_sent = 0.0,
        .eos_sent = 0,
        .seq_info_chg_received = 0,
        .frames_decoded_ptr = &frames_decoded,
        .expected_frames = expected_frames,
        .num_in_bufs = num_in_bufs,
    };

    double t_start = get_time_sec();
    double t_first_frame = 0;
    double t_last_frame = t_start;
    double t_eos_sent = 0;


    // Fill initial input buffers
    for (int i = 0; i < num_in_bufs; i++) {
        if (!feeder.eos_sent) {
            send_input_packet(&feeder, i);
        }
    }

    // 7. Event & Frame Processing Loop
    OMXVDEC_MSG_INFO msg_info;

    while (get_time_sec() - t_start < 45.0) {
        memset(&msg_info, 0, sizeof(msg_info));
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.out = &msg_info;

        int ret = ioctl(fd, VDEC_IOCTL_CHAN_GET_MSG, &msg);
        double now = get_time_sec();

        if (ret == 0) {
            if (msg_info.msgcode == VDEC_MSG_RESP_OUTPUT_DONE) {
                if (msg_info.status_code != 0) {
                    printf("[MSG_OUT] Received error response 0x%x\n", msg_info.status_code);
                    fflush(stdout);
                    continue;
                }

                OMXVDEC_BUF_DESC *pout = &msg_info.msgdata.buf;
                int buf_idx = -1;
                for (uint32_t i = 0; i < out_count; i++) {
                    if (out_bufs[i].phyaddr == pout->phyaddr) {
                        buf_idx = (int)i;
                        break;
                    }
                }
                if (buf_idx < 0) {
                    printf("[MSG_OUT] Ignored old flushed buffer 0x%x\n", pout->phyaddr);
                    fflush(stdout);
                    continue;
                }

                out_busy[buf_idx] = 0;

                if (pout->data_len > 0) {
                    frames_decoded++;
                    if (frames_decoded == 1) {
                        t_first_frame = now;
                    }
                    t_last_frame = now;
                    printf("[VDEC_TEST] >>> FRAME #%d: data_len=%u, stride=%u, %ux%u in %.2f ms (%.2f FPS) <<<\n",
                           frames_decoded, pout->data_len, pout->out_frame.stride,
                           pout->out_frame.frame_width, pout->out_frame.frame_height,
                           (now - t_start) * 1000.0, frames_decoded / (now - t_start));
                    fflush(stdout);

                    if (fout) {
                        fwrite(out_maps[buf_idx], 1, pout->data_len, fout);
                    }
                    for (int j = 0; j < num_in_bufs; j++) {
                        if (!feeder.in_busy[j] && !feeder.eos_sent) {
                            if (send_input_packet(&feeder, j) > 0) break;
                        }
                    }
                }

                int is_eos = (pout->flags & VDEC_BUFFERFLAG_EOS);
                if (is_eos) {
                    printf("[VDEC_TEST] Output port received EOS sentinel! Hardware finished all frames (total: %d).\n", frames_decoded);
                    fflush(stdout);
                    break;
                }

                // Re-queue output buffer
                if (!out_busy[buf_idx]) {
                    memset(&msg, 0, sizeof(msg));
                    msg.chan_num = chan_id;
                    msg.in = &out_bufs[buf_idx];
                    int r_fill = ioctl(fd, VDEC_IOCTL_FILL_OUTPUT_FRAME, &msg);
                    if (r_fill < 0) {
                        perror("ioctl FILL_OUTPUT_FRAME loop");
                    } else {
                        out_busy[buf_idx] = 1;
                    }
                }
                        } else if (msg_info.msgcode == VDEC_MSG_RESP_INPUT_DONE) {
                OMXVDEC_BUF_DESC *pin = &msg_info.msgdata.buf;
                for (int i = 0; i < num_in_bufs; i++) {
                    if (in_bufs[i].phyaddr == pin->phyaddr) {
                        in_busy[i] = 0;
                        feeder.in_busy[i] = 0;
                        if (!feeder.eos_sent) {
                            send_input_packet(&feeder, i);
                                        }
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

                // Driver in DFS_WAIT_INSERT requires binding max_frame_num to resume decode
                if (rebind_output_buffers(fd, chan_id, s->max_frame_num, s->frame_size, s->stride,
                                          out_bufs, out_fds, out_maps, out_busy, &out_count, &out_len, &feeder) < 0) {
                    fprintf(stderr, "[VDEC_TEST] Failed to rebind output buffers!\n");
                    break;
                }

                feeder.seq_info_chg_received = 1;

                // Resume feeding input NALs
                for (int i = 0; i < num_in_bufs; i++) {
                    if (!feeder.in_busy[i] && !feeder.eos_sent) {
                        send_input_packet(&feeder, i);
                    }
                }
            }
        }

        // Check if EOS should be sent
        now = get_time_sec();
        if (expected_frames > 0 && frames_decoded >= expected_frames) {
            printf("[VDEC_TEST] All %d expected frames decoded! Success.\n", expected_frames);
            fflush(stdout);
            break;
        }

        if (feeder.all_nals_sent && !feeder.eos_sent) {
            int should_send_eos = 0;
            if (expected_frames > 0 && frames_decoded < expected_frames - 8) {
                if (now - t_last_frame >= 15.0 && now - feeder.t_all_nals_sent >= 15.0) {
                    should_send_eos = 1;
                }
            } else if (frames_decoded > 0 && (now - t_last_frame >= 2.0)) {
                should_send_eos = 1;
            } else if (frames_decoded == 0 && (now - feeder.t_all_nals_sent >= 15.0)) {
                should_send_eos = 1;
            }
            if (should_send_eos) {
                printf("[EOS_REASON] frames=%d expected=%d t_last_diff=%.3f all_diff=%.3f\n",
                       frames_decoded, expected_frames, now - t_last_frame, now - feeder.t_all_nals_sent);
                fflush(stdout);
                for (int i = 0; i < num_in_bufs; i++) {
                    if (!feeder.in_busy[i]) {
                        send_eos_packet(&feeder, i);
                        t_eos_sent = get_time_sec();
                        break;
                    }
                }
            }
        }

        // Idle timeout after EOS (no frames produced for 3 seconds)
        if (feeder.eos_sent && (now - t_last_frame >= 3.0) && (now - t_eos_sent >= 3.0)) {
            printf("[VDEC_TEST] Decoding complete (drain timeout after EOS).\n");
            fflush(stdout);
            break;
        }

        // Retry submitting input buffers if they were throttled
        for (int i = 0; i < num_in_bufs; i++) {
            if (!feeder.in_busy[i] && !feeder.all_nals_sent && !feeder.eos_sent) {
                send_input_packet(&feeder, i);
            }
        }

        usleep(1000);
    }

    double t_total = (frames_decoded > 0) ? (t_last_frame - t_start) : (get_time_sec() - t_start);
    double fps = (t_total > 0.0) ? (frames_decoded / t_total) : 0.0;
    double t_pure = (frames_decoded > 1) ? (t_last_frame - t_first_frame) : t_total;
    double pure_fps = (t_pure > 0.0) ? (frames_decoded / t_pure) : fps;
    printf("[VDEC_TEST] SUMMARY: %d frames decoded in %.3f s -> %.2f FPS (pure: %.2f FPS)\n",
           frames_decoded, t_total, fps, pure_fps);
    fflush(stdout);

cleanup:
    // Flush both ports to return any buffers held by hardware
    ePORT_DIR flush_both = PORT_DIR_BOTH;
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    msg.in = &flush_both;
    ioctl(fd, VDEC_IOCTL_FLUSH_PORT, &msg);

    OMXVDEC_MSG_INFO drain_msg;
    for (int k = 0; k < 100; k++) {
        memset(&drain_msg, 0, sizeof(drain_msg));
        memset(&msg, 0, sizeof(msg));
        msg.chan_num = chan_id;
        msg.out = &drain_msg;
        if (ioctl(fd, VDEC_IOCTL_CHAN_GET_MSG, &msg) < 0) break;
    }

    // Halting channel cleanly releases hardware threads without race conditions
    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    ioctl(fd, VDEC_IOCTL_CHAN_STOP, &msg);

    // Unbind output buffers in strict reverse order to detach DMA-BUFs without list corruption
    for (int i = (int)out_count - 1; i >= 0; i--) {
        if (out_bufs[i].phyaddr) {
            memset(&msg, 0, sizeof(msg));
            msg.chan_num = chan_id;
            msg.in = &out_bufs[i];
            ioctl(fd, VDEC_IOCTL_CHAN_UNBIND_BUFFER, &msg);
        }
    }

    // Unbind input buffers in strict reverse order
    for (int i = num_in_bufs - 1; i >= 0; i--) {
        if (in_bufs[i].phyaddr) {
            memset(&msg, 0, sizeof(msg));
            msg.chan_num = chan_id;
            msg.in = &in_bufs[i];
            ioctl(fd, VDEC_IOCTL_CHAN_UNBIND_BUFFER, &msg);
        }
    }

    memset(&msg, 0, sizeof(msg));
    msg.chan_num = chan_id;
    ioctl(fd, VDEC_IOCTL_CHAN_RELEASE, &msg);

    for (int i = 0; i < num_in_bufs; i++) {
        if (in_maps[i] && in_maps[i] != MAP_FAILED) munmap(in_maps[i], IN_BUF_SIZE);
        if (in_fds[i] >= 0) close(in_fds[i]);
    }
    for (uint32_t i = 0; i < out_count; i++) {
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
