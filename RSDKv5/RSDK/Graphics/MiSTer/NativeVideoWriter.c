#include "NativeVideoWriter.h"

/* Phase 9: runtime-configurable frame dimensions for dual-aspect support.
 * Defaults to 4:3 (320x240) so callers that never call
 * NativeVideoWriter_SetDims see the same behaviour as before Phase 9. */
int      nv_frame_width_runtime  = 320;
int      nv_frame_height_runtime = 240;
uint32_t nv_frame_bytes_runtime  = 320u * 240u * 2u;                /* 153,600 */
uint32_t nv_buf1_offset_runtime  = NV_BUF0_OFFSET + 320u * 240u * 2u; /* 0x25900 */

void NativeVideoWriter_SetDims(int width, int height) {
    if (width <= 0 || height <= 0) return;
    nv_frame_width_runtime  = width;
    nv_frame_height_runtime = height;
    nv_frame_bytes_runtime  = (uint32_t)width * (uint32_t)height * 2u;
    nv_buf1_offset_runtime  = NV_BUF0_OFFSET + nv_frame_bytes_runtime;
    /* Sanity: BUF1 + frame_bytes must not exceed REGION_SIZE.
     * 4:3:  153,600 * 2 + 0x100 = 0x25900 + 0x25800 = 0x4B100 (well under 0x80000)
     * 16:9: 203,520 * 2 + 0x100 = 0x31C00 + 0x31B00 = 0x63700 (under 0x80000)
     * If a future aspect overflows this, NV_DDR_REGION_SIZE must be bumped. */
}

#if defined(__linux__) && defined(PORT_MISTER)

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int mem_fd = -1;
static volatile uint8_t *ddr_base = NULL;
static uint32_t frame_counter = 0;
static int active_buf = 0;

bool NativeVideoWriter_Init(void) {
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("NativeVideoWriter: open /dev/mem");
        return false;
    }

    ddr_base = (volatile uint8_t *)mmap(NULL, NV_DDR_REGION_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, NV_DDR_PHYS_BASE);
    if (ddr_base == MAP_FAILED) {
        perror("NativeVideoWriter: mmap");
        ddr_base = NULL;
        close(mem_fd);
        mem_fd = -1;
        return false;
    }

    /* Phase 9: log the runtime dims so on-device verification can confirm
     * the active aspect made it through wrapper -> engine -> writer. */
    fprintf(stderr,
            "NativeVideoWriter_Init: width=%d height=%d frame_bytes=%u buf1_offset=0x%X region=0x%X\n",
            nv_frame_width_runtime, nv_frame_height_runtime,
            (unsigned)nv_frame_bytes_runtime, (unsigned)nv_buf1_offset_runtime,
            (unsigned)NV_DDR_REGION_SIZE);

    /* Clear both buffers and control word using runtime sizes / offsets. */
    memset((void *)(ddr_base + NV_BUF0_OFFSET), 0, nv_frame_bytes_runtime);
    memset((void *)(ddr_base + nv_buf1_offset_runtime), 0, nv_frame_bytes_runtime);
    volatile uint32_t *ctrl = (volatile uint32_t *)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = 0;
    frame_counter = 0;
    active_buf = 0;

    /* Clear feedback words so ARM doesn't read stale data from a previous run */
    volatile uint32_t *feedback = (volatile uint32_t *)(ddr_base + NV_FEEDBACK_OFFSET);
    *feedback = 0;
    volatile uint32_t *feedback_seq = (volatile uint32_t *)(ddr_base + NV_FEEDBACK_OFFSET + 4);
    *feedback_seq = 0;

    return true;
}

void NativeVideoWriter_Shutdown(void) {
    if (ddr_base) {
        /* Clear control word so FPGA detects stale frame and blanks output */
        volatile uint32_t *ctrl = (volatile uint32_t *)(ddr_base + NV_CTRL_OFFSET);
        *ctrl = 0;

        munmap((void *)ddr_base, NV_DDR_REGION_SIZE);
        ddr_base = NULL;
    }

    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

void NativeVideoWriter_WriteFrame(const void *pixels_rgb565, int width, int height, int pitch_bytes) {
    if (!ddr_base || width != nv_frame_width_runtime || height != nv_frame_height_runtime) {
        return;
    }

    uint32_t buf_offset = (active_buf == 0) ? NV_BUF0_OFFSET : nv_buf1_offset_runtime;
    volatile uint8_t *dst = ddr_base + buf_offset;
    const int line_bytes = nv_frame_width_runtime * 2;

    if (pitch_bytes == line_bytes) {
        /* Contiguous: single memcpy */
        memcpy((void *)dst, pixels_rgb565, nv_frame_bytes_runtime);
    } else {
        /* Row-by-row copy */
        const uint8_t *src = (const uint8_t *)pixels_rgb565;
        for (int y = 0; y < nv_frame_height_runtime; y++) {
            memcpy((void *)(dst + y * line_bytes), src + y * pitch_bytes, line_bytes);
        }
    }

    /*
     * Write ordering: on strongly-ordered/device memory (O_SYNC + MAP_SHARED),
     * ARM guarantees all prior writes (pixel data) complete before this write.
     * If this is ever changed to use cached memory + explicit flushes, a DSB
     * barrier must be inserted here before the control word write.
     * (Verbatim carry-over from 3sx.)
     */
    frame_counter++;
    volatile uint32_t *ctrl = (volatile uint32_t *)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (frame_counter << 2) | (active_buf & 1);

    /* Swap buffer for next frame */
    active_buf ^= 1;
}

bool NativeVideoWriter_IsActive(void) {
    return ddr_base != NULL;
}

uint32_t NativeVideoWriter_ReadFeedback(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *fb = (volatile uint32_t *)(ddr_base + NV_FEEDBACK_OFFSET);
    return *fb;
}

uint32_t NativeVideoWriter_ReadFeedbackSeq(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *seq = (volatile uint32_t *)(ddr_base + NV_FEEDBACK_OFFSET + 4);
    return *seq;
}

#else

/* Non-Linux or non-MiSTer build: writer is a compile-in but runtime no-op.
 * (The runtime dim variables and SetDims setter still link, so Mac-host TUs
 * can call them without failing.) */

bool NativeVideoWriter_Init(void) {
    return false;
}

void NativeVideoWriter_Shutdown(void) {
}

void NativeVideoWriter_WriteFrame(const void *pixels_rgb565, int width, int height, int pitch_bytes) {
    (void)pixels_rgb565;
    (void)width;
    (void)height;
    (void)pitch_bytes;
}

bool NativeVideoWriter_IsActive(void) {
    return false;
}

uint32_t NativeVideoWriter_ReadFeedback(void) {
    return 0;
}

uint32_t NativeVideoWriter_ReadFeedbackSeq(void) {
    return 0;
}

#endif
