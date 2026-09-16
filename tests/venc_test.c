/*
 * HiKey960 Kirin 960 VPU (hi_vcodec) Hardware Video Encoder (VENC) Test Harness
 * Directly interfaces with /dev/hi_venc using Linux DMA-BUF Heaps and ioctl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>

#include "include/hi_type.h"
#include "include/hi_unf_common.h"
#include "include/hi_unf_video.h"
#include "include/hi_unf_venc.h"
#include "include/hi_drv_venc.h"
#include "include/drv_venc_ioctl.h"

#define VENC_DEV_PATH "/dev/hi_venc"

#define NUM_IN_BUFS  4
#define NUM_OUT_BUFS 4

#define VENC_MSG_CMD_DONE        (0x000a0002)
#define VENC_MSG_EMPTY_BUF_DONE  (0x000a0007)
#define VENC_MSG_FILL_BUF_DONE   (0x000a0008)

#ifndef OMXVENC_BUFFERFLAG_EOS
#define OMXVENC_BUFFERFLAG_EOS 0x00000001
#endif
#ifndef OMXVENC_BUFFERFLAG_ENDOFFRAME
#define OMXVENC_BUFFERFLAG_ENDOFFRAME 0x00000010
#endif
#ifndef OMXVENC_BUFFERFLAG_SYNCFRAME
#define OMXVENC_BUFFERFLAG_SYNCFRAME 0x00000020
#endif

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

static void fill_nv12_pattern(uint8_t *y_plane, uint8_t *uv_plane, int width, int height, int stride, int frame_idx) {
    for (int y = 0; y < height; y++) {
        uint8_t *py = y_plane + y * stride;
        for (int x = 0; x < width; x++) {
            int bar = (x * 8) / width;
            uint8_t val = (uint8_t)(((bar * 32) + (x + y + frame_idx * 4)) & 0xFF);
            py[x] = val;
        }
    }
    for (int y = 0; y < height / 2; y++) {
        uint8_t *puv = uv_plane + y * stride;
        for (int x = 0; x < width; x += 2) {
            puv[x] = (uint8_t)((128 + ((x * 127) / width) + frame_idx) & 0xFF);
            puv[x + 1] = (uint8_t)((128 + ((y * 127) / (height / 2)) - frame_idx) & 0xFF);
        }
    }
}

typedef struct {
    int venc_fd;
    HI_HANDLE hVencChn;
    int is_hevc;
    int width;
    int height;
    int stride;
    uint32_t in_len;
    uint32_t out_len;
    int fps;
    int bitrate;
    int gop;

    int in_fds[NUM_IN_BUFS];
    void *in_ptrs[NUM_IN_BUFS];
    venc_user_buf in_ubufs[NUM_IN_BUFS];
    int in_busy[NUM_IN_BUFS];

    int out_fds[NUM_OUT_BUFS];
    void *out_ptrs[NUM_OUT_BUFS];
    venc_user_buf out_ubufs[NUM_OUT_BUFS];
} venc_session_t;

static int venc_session_init(venc_session_t *s, int venc_fd, int is_hevc, int width, int height, int fps, int bitrate, int gop) {
    memset(s, 0, sizeof(*s));
    s->venc_fd = venc_fd;
    s->is_hevc = is_hevc;
    s->width = width;
    s->height = height;
    s->stride = (width + 15) & ~15;
    s->in_len = s->stride * height * 3 / 2;
    s->out_len = width * height * 2;
    if (s->out_len < 2 * 1024 * 1024) s->out_len = 2 * 1024 * 1024;
    s->fps = fps;
    s->bitrate = bitrate;
    s->gop = gop;

    VENC_INFO_CREATE_S create_info;
    memset(&create_info, 0, sizeof(create_info));
    create_info.stAttr.enVencType = is_hevc ? HI_UNF_VCODEC_TYPE_HEVC : HI_UNF_VCODEC_TYPE_H264;
    create_info.stAttr.enVencProfile = is_hevc ? 0 : HI_UNF_H264_PROFILE_HIGH;
    create_info.stAttr.enVencHevcProfile = 0;
    create_info.stAttr.u32Width = width;
    create_info.stAttr.u32Height = height;
    create_info.stAttr.u32StrmBufSize = s->out_len;
    create_info.stAttr.u32TargetBitRate = bitrate;
    create_info.stAttr.u32TargetFrmRate = fps;
    create_info.stAttr.u32InputFrmRate = fps;
    create_info.stAttr.u32Gop = gop;
    create_info.stAttr.u32MaxQp = 51;
    create_info.stAttr.u32MinQp = 10;
    create_info.stAttr.bQuickEncode = HI_FALSE;

    if (ioctl(venc_fd, CMD_VENC_CREATE_CHN, &create_info) < 0) {
        perror("ioctl CMD_VENC_CREATE_CHN");
        return -1;
    }
    s->hVencChn = create_info.hVencChn;

    // Allocate and map input buffers
    for (int i = 0; i < NUM_IN_BUFS; i++) {
        if (alloc_dma_buf(s->in_len, &s->in_fds[i], &s->in_ptrs[i]) < 0) {
            fprintf(stderr, "Failed to allocate DMA buffer for input %d\n", i);
            return -1;
        }
        memset(&s->in_ubufs[i], 0, sizeof(s->in_ubufs[i]));
        s->in_ubufs[i].dir = PORT_DIR_INPUT;
        s->in_ubufs[i].bufferaddr = (HI_U64)(uintptr_t)s->in_ptrs[i];
        s->in_ubufs[i].pmem_fd = s->in_fds[i];
        s->in_ubufs[i].share_fd = s->in_fds[i];
        s->in_ubufs[i].buffer_size = s->in_len;
        s->in_ubufs[i].strideY = s->stride;
        s->in_ubufs[i].strideC = s->stride;
        s->in_ubufs[i].offset_YC = s->stride * height;
        s->in_ubufs[i].picWidth = width;
        s->in_ubufs[i].picHeight = height;

        VENC_INFO_MAP_S map_info;
        map_info.hVencChn = s->hVencChn;
        map_info.VencMapBuffer = &s->in_ubufs[i];
        if (ioctl(venc_fd, CMD_VENC_KEN_MAP, &map_info) < 0) {
            perror("ioctl CMD_VENC_KEN_MAP input");
            return -1;
        }
    }

    // Allocate and map output buffers
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        if (alloc_dma_buf(s->out_len, &s->out_fds[i], &s->out_ptrs[i]) < 0) {
            fprintf(stderr, "Failed to allocate DMA buffer for output %d\n", i);
            return -1;
        }
        memset(&s->out_ubufs[i], 0, sizeof(s->out_ubufs[i]));
        s->out_ubufs[i].dir = PORT_DIR_OUTPUT;
        s->out_ubufs[i].bufferaddr = (HI_U64)(uintptr_t)s->out_ptrs[i];
        s->out_ubufs[i].pmem_fd = s->out_fds[i];
        s->out_ubufs[i].share_fd = s->out_fds[i];
        s->out_ubufs[i].buffer_size = s->out_len;
        s->out_ubufs[i].picWidth = width;
        s->out_ubufs[i].picHeight = height;

        VENC_INFO_MAP_S map_info;
        map_info.hVencChn = s->hVencChn;
        map_info.VencMapBuffer = &s->out_ubufs[i];
        if (ioctl(venc_fd, CMD_VENC_KEN_MAP, &map_info) < 0) {
            perror("ioctl CMD_VENC_KEN_MAP output");
            return -1;
        }
        printf("[VENC_TEST] Out buf %d: ptr=%p bufaddr=0x%llx phy=0x%llx kern=0x%llx fd=%d\n",
               i, s->out_ptrs[i], (unsigned long long)s->out_ubufs[i].bufferaddr,
               (unsigned long long)s->out_ubufs[i].bufferaddr_Phy,
               (unsigned long long)s->out_ubufs[i].kernelbufferaddr, s->out_fds[i]);
    }

    return 0;
}

static int venc_session_start(venc_session_t *s) {
    if (ioctl(s->venc_fd, CMD_VENC_START_RECV_PIC, &s->hVencChn) < 0) {
        perror("ioctl CMD_VENC_START_RECV_PIC");
        return -1;
    }

    // Queue all output buffers
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        VENC_INFO_QUEUE_FRAME_S qstream;
        memset(&qstream, 0, sizeof(qstream));
        qstream.hVencChn = s->hVencChn;
        qstream.stVencFrame_OMX = s->out_ubufs[i];
        if (ioctl(s->venc_fd, CMD_VENC_QUEUE_STREAM, &qstream) < 0) {
            perror("ioctl CMD_VENC_QUEUE_STREAM");
            return -1;
        }
    }
    return 0;
}

static int venc_session_destroy(venc_session_t *s) {
    ioctl(s->venc_fd, CMD_VENC_STOP_RECV_PIC, &s->hVencChn);

    for (int i = 0; i < NUM_IN_BUFS; i++) {
        if (s->in_fds[i] > 0) {
            VENC_INFO_MAP_S map_info;
            map_info.hVencChn = s->hVencChn;
            map_info.VencMapBuffer = &s->in_ubufs[i];
            ioctl(s->venc_fd, CMD_VENC_KEN_UMMAP, &map_info);
            munmap(s->in_ptrs[i], s->in_len);
            close(s->in_fds[i]);
            s->in_fds[i] = -1;
        }
    }

    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        if (s->out_fds[i] > 0) {
            VENC_INFO_MAP_S map_info;
            map_info.hVencChn = s->hVencChn;
            map_info.VencMapBuffer = &s->out_ubufs[i];
            ioctl(s->venc_fd, CMD_VENC_KEN_UMMAP, &map_info);
            munmap(s->out_ptrs[i], s->out_len);
            close(s->out_fds[i]);
            s->out_fds[i] = -1;
        }
    }

    VENC_INFO_CREATE_S destroy_info;
    memset(&destroy_info, 0, sizeof(destroy_info));
    destroy_info.hVencChn = s->hVencChn;
    ioctl(s->venc_fd, CMD_VENC_DESTROY_CHN, &destroy_info);
    return 0;
}

static int find_out_buf_idx(venc_session_t *s, HI_U64 addr) {
    for (int i = 0; i < NUM_OUT_BUFS; i++) {
        if (s->out_ubufs[i].bufferaddr == addr) return i;
    }
    return -1;
}

static int find_in_buf_idx(venc_session_t *s, HI_U64 addr) {
    for (int i = 0; i < NUM_IN_BUFS; i++) {
        if (s->in_ubufs[i].bufferaddr == addr) return i;
    }
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <codec: h264|hevc> <width> <height> <num_frames> <out_file|none> [options]\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  --fps <rate>          Target & input framerate (default 30)\n");
        fprintf(stderr, "  --bitrate <bps>       Target bitrate (default auto: 5M for 1080p, 20M for 4K)\n");
        fprintf(stderr, "  --gop <n>             GOP size (default 30)\n");
        fprintf(stderr, "  --input-yuv <file>    Raw NV12 input file (default: animated pattern)\n");
        fprintf(stderr, "  --stress <cycles>     Stress test: repeat create->encode->destroy N times\n");
        return 1;
    }

    const char *codec_str = argv[1];
    int width = atoi(argv[2]);
    int height = atoi(argv[3]);
    int num_frames = atoi(argv[4]);
    const char *out_file = argv[5];

    int is_hevc = 0;
    if (!strcmp(codec_str, "hevc") || !strcmp(codec_str, "h265")) is_hevc = 1;
    else if (strcmp(codec_str, "h264")) {
        fprintf(stderr, "Error: Unknown codec %s (only h264 or hevc supported)\n", codec_str);
        return 1;
    }

    int fps = 30;
    int bitrate = (width >= 3840) ? 20000000 : (width >= 1920 ? 5000000 : (width >= 1280 ? 3000000 : 1500000));
    int gop = 30;
    const char *input_yuv = NULL;
    int stress_cycles = 1;

    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gop") && i + 1 < argc) gop = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--input-yuv") && i + 1 < argc) input_yuv = argv[++i];
        else if (!strcmp(argv[i], "--stress") && i + 1 < argc) stress_cycles = atoi(argv[++i]);
    }

    printf("=================================================================\n");
    printf("[VENC_TEST] HiKey960 Kirin 960 Hardware Video Encoder Test\n");
    printf("[VENC_TEST] Codec: %s (%s)\n", is_hevc ? "HEVC / H.265" : "AVC / H.264", codec_str);
    printf("[VENC_TEST] Resolution: %dx%d @ %d fps, Bitrate: %d bps, GOP: %d\n", width, height, fps, bitrate, gop);
    printf("[VENC_TEST] Frames: %d, Output: %s, Stress Cycles: %d\n", num_frames, out_file, stress_cycles);
    printf("=================================================================\n");

    int venc_fd = open(VENC_DEV_PATH, O_RDWR);
    if (venc_fd < 0) {
        perror("Failed to open " VENC_DEV_PATH);
        return 1;
    }

    FILE *fin_yuv = NULL;
    if (input_yuv) {
        fin_yuv = fopen(input_yuv, "rb");
        if (!fin_yuv) {
            perror("Failed to open input YUV file");
            close(venc_fd);
            return 1;
        }
    }

    int test_failed = 0;
    for (int cycle = 1; cycle <= stress_cycles; cycle++) {
        if (stress_cycles > 1) {
            printf("\n--- Stress Cycle %d / %d ---\n", cycle, stress_cycles);
        }

        venc_session_t session;
        if (venc_session_init(&session, venc_fd, is_hevc, width, height, fps, bitrate, gop) < 0) {
            fprintf(stderr, "Session init failed in cycle %d\n", cycle);
            close(venc_fd);
            if (fin_yuv) fclose(fin_yuv);
            return 1;
        }

        if (venc_session_start(&session) < 0) {
            fprintf(stderr, "Session start failed in cycle %d\n", cycle);
            venc_session_destroy(&session);
            close(venc_fd);
            if (fin_yuv) fclose(fin_yuv);
            return 1;
        }

        FILE *fout = NULL;
        if (strcmp(out_file, "none") != 0 && cycle == 1) {
            fout = fopen(out_file, "wb");
            if (!fout) perror("Warning: Failed to open output file");
        }

        int frames_queued = 0;
        int frames_encoded = 0;
        int frames_recycled = 0;
        uint64_t total_bytes = 0;
        double t_start = get_time_sec();
        int eos_sent = 0;

        while (frames_encoded < num_frames) {
            // Feed input frames
            while (frames_queued < num_frames) {
                int in_idx = -1;
                for (int i = 0; i < NUM_IN_BUFS; i++) {
                    if (!session.in_busy[i]) {
                        in_idx = i;
                        break;
                    }
                }
                if (in_idx < 0) break; // All input buffers busy

                uint8_t *y_ptr = (uint8_t *)session.in_ptrs[in_idx];
                uint8_t *uv_ptr = y_ptr + session.stride * height;

                struct dma_buf_sync in_sync_start = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
                ioctl(session.in_fds[in_idx], DMA_BUF_IOCTL_SYNC, &in_sync_start);

                if (fin_yuv) {
                    size_t read_bytes = fread(y_ptr, 1, width * height, fin_yuv);
                    read_bytes += fread(uv_ptr, 1, width * height / 2, fin_yuv);
                    if (read_bytes < (size_t)(width * height * 3 / 2)) {
                        rewind(fin_yuv); // loop input
                    }
                } else {
                    fill_nv12_pattern(y_ptr, uv_ptr, width, height, session.stride, frames_queued);
                }

                struct dma_buf_sync in_sync_end = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE };
                ioctl(session.in_fds[in_idx], DMA_BUF_IOCTL_SYNC, &in_sync_end);

                session.in_ubufs[in_idx].data_len = session.in_len;
                session.in_ubufs[in_idx].timestamp = (HI_U64)(frames_queued * 1000000ULL / fps);
                session.in_ubufs[in_idx].flags = OMXVENC_BUFFERFLAG_ENDOFFRAME;
                if (frames_queued == num_frames - 1) {
                    session.in_ubufs[in_idx].bEOS = 1;
                    session.in_ubufs[in_idx].flags |= OMXVENC_BUFFERFLAG_EOS;
                    eos_sent = 1;
                }

                VENC_INFO_QUEUE_FRAME_S qframe;
                memset(&qframe, 0, sizeof(qframe));
                qframe.hVencChn = session.hVencChn;
                qframe.stVencFrame_OMX = session.in_ubufs[in_idx];

                if (ioctl(venc_fd, CMD_VENC_QUEUE_FRAME, &qframe) < 0) {
                    perror("ioctl CMD_VENC_QUEUE_FRAME");
                    break;
                }
                session.in_busy[in_idx] = 1;
                frames_queued++;
            }

            // Poll message queue
            VENC_INFO_GET_MSG_S getmsg;
            memset(&getmsg, 0, sizeof(getmsg));
            getmsg.hVencChn = session.hVencChn;

            int ret = ioctl(venc_fd, CMD_VENC_GET_MSG, &getmsg);
            if (ret == 0 && getmsg.msg_info_omx.msgcode != 0) {
                if (getmsg.msg_info_omx.msgcode == VENC_MSG_FILL_BUF_DONE) {
                    int out_idx = find_out_buf_idx(&session, getmsg.msg_info_omx.buf.bufferaddr);
                    uint32_t len = getmsg.msg_info_omx.buf.data_len;
                    uint32_t offset = getmsg.msg_info_omx.buf.offset;
                    uint32_t flags = getmsg.msg_info_omx.buf.flags;
                    printf("[VENC_TEST] MSG FILL_BUF_DONE: msg.bufaddr=0x%llx phy=0x%llx kern=0x%llx len=%u out_idx=%d\n",
                           (unsigned long long)getmsg.msg_info_omx.buf.bufferaddr,
                           (unsigned long long)getmsg.msg_info_omx.buf.bufferaddr_Phy,
                           (unsigned long long)getmsg.msg_info_omx.buf.kernelbufferaddr,
                           len, out_idx);
                    if (out_idx >= 0 && len > 0) {
                        struct dma_buf_sync sync_start = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
                        ioctl(session.out_fds[out_idx], DMA_BUF_IOCTL_SYNC, &sync_start);

                        uint8_t *src = (uint8_t *)session.out_ptrs[out_idx] + offset;
                        printf("[VENC_TEST] MSG: out_idx=%d len=%u offset=%u flags=0x%08x [", out_idx, len, offset, flags);
                        for (int b = 0; b < (len < 16 ? len : 16); b++) {
                            printf("%02x ", src[b]);
                        }
                        printf("]\n");

                        if (fout) {
                            fwrite(src, 1, len, fout);
                        }

                        struct dma_buf_sync sync_end = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
                        ioctl(session.out_fds[out_idx], DMA_BUF_IOCTL_SYNC, &sync_end);
                        total_bytes += len;

                        if (flags & OMXVENC_BUFFERFLAG_CODECCONFIG) {
                            printf("[VENC_TEST] Header / CodecConfig received (%u bytes, flags=0x%08x)\n", len, flags);
                            fflush(stdout);
                        } else {
                            frames_encoded++;
                            if (frames_encoded % 10 == 0 || frames_encoded == num_frames) {
                                double elapsed = get_time_sec() - t_start;
                                double cur_fps = elapsed > 0 ? frames_encoded / elapsed : 0;
                                printf("[VENC_TEST] Encoded frame %4d / %4d (%6u bytes, flags=0x%08x, Total: %8llu bytes, %.1f FPS)\n",
                                       frames_encoded, num_frames, len, flags, (unsigned long long)total_bytes, cur_fps);
                                fflush(stdout);
                            }
                        }

                        // Requeue output buffer
                        VENC_INFO_QUEUE_FRAME_S qstream;
                        memset(&qstream, 0, sizeof(qstream));
                        qstream.hVencChn = session.hVencChn;
                        qstream.stVencFrame_OMX = session.out_ubufs[out_idx];
                        ioctl(venc_fd, CMD_VENC_QUEUE_STREAM, &qstream);
                    }
                } else if (getmsg.msg_info_omx.msgcode == VENC_MSG_EMPTY_BUF_DONE) {
                    int in_idx = find_in_buf_idx(&session, getmsg.msg_info_omx.buf.bufferaddr);
                    if (in_idx >= 0) {
                        session.in_busy[in_idx] = 0;
                        frames_recycled++;
                    }
                }
            } else {
                usleep(500); // Wait for hardware interrupt / encoder processing
            }

            // Watchdog: prevent infinite hang if driver stops responding
            if (get_time_sec() - t_start > 30.0 && frames_encoded == 0) {
                fprintf(stderr, "[VENC_TEST] TIMEOUT: No frames encoded after 30s! Aborting.\n");
                break;
            }
        }

        double total_time = get_time_sec() - t_start;
        double avg_fps = total_time > 0 ? frames_encoded / total_time : 0;
        printf("[VENC_TEST] Cycle %d Complete: %d frames in %.2f s (%.1f FPS), %llu total bytes (%.1f kbps)\n",
               cycle, frames_encoded, total_time, avg_fps, (unsigned long long)total_bytes,
               total_time > 0 ? (total_bytes * 8.0 / total_time / 1000.0) : 0);

        if (frames_encoded < num_frames) {
            fprintf(stderr, "[VENC_TEST] FAILED: Only %d/%d frames encoded!\n", frames_encoded, num_frames);
            test_failed = 1;
        }

        if (fout) {
            fclose(fout);
            fout = NULL;
        }

        venc_session_destroy(&session);
        if (test_failed) break;
    }

    if (fin_yuv) fclose(fin_yuv);
    close(venc_fd);
    if (test_failed) {
        fprintf(stderr, "[VENC_TEST] Run ended with errors!\n");
        return 1;
    }
    printf("[VENC_TEST] All cycles completed successfully!\n");
    return 0;
}

