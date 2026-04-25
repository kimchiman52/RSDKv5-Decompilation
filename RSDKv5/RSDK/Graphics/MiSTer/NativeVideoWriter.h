#ifndef RSDK_MISTER_NATIVE_VIDEO_WRITER_H
#define RSDK_MISTER_NATIVE_VIDEO_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Phase 9: dimensions are RUNTIME-configurable to support dual-aspect
// (4:3 320x240 / 16:9 424x240). Defaults match the 4:3 mode for backward
// compatibility if NativeVideoWriter_SetDims is not called.
//
// Address invariants (FPGA-side fixed for both aspects):
//   NV_DDR_PHYS_BASE   = 0x3A000000  (unchanged)
//   NV_DDR_REGION_SIZE = 0x00080000  (BUMPED from 0x60000 to fit 16:9 buffers:
//                                     2 * 203,520 + 256 = 407,296 B exceeds
//                                     the old 393,216 B region)
//   NV_CTRL_OFFSET     = 0x00000000  (unchanged)
//   NV_FEEDBACK_OFFSET = 0x00000040  (unchanged)
//   NV_BUF0_OFFSET     = 0x00000100  (unchanged - the FPGA reader hardcodes
//                                     CTRL_ADDR/BUF0_ADDR; only BUF1_ADDR is
//                                     aspect-keyed there)
//
// Runtime-set fields (derived from aspect):
//   nv_frame_width_runtime, nv_frame_height_runtime, nv_frame_bytes_runtime,
//   nv_buf1_offset_runtime
#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00080000u   /* 512 KB — fits 2 * 203,520 B (16:9) + ctrl + feedback */
#define NV_CTRL_OFFSET      0x00000000u
#define NV_FEEDBACK_OFFSET  0x00000040u
#define NV_BUF0_OFFSET      0x00000100u   /* ctrl (4 B at 0x00) + feedback (8 B at 0x40-0x47) + 244 B pad */

extern int      nv_frame_width_runtime;
extern int      nv_frame_height_runtime;
extern uint32_t nv_frame_bytes_runtime;
extern uint32_t nv_buf1_offset_runtime;

/// Configure the runtime dimensions. Should be called BEFORE
/// NativeVideoWriter_Init so the mmap covers the correct region.
/// If never called, defaults to 4:3 (320x240).
///   width=320,  height=240 -> 4:3  (frame_bytes=153,600, buf1_offset=0x25900)
///   width=424,  height=240 -> 16:9 (frame_bytes=203,520, buf1_offset=0x31C00)
void NativeVideoWriter_SetDims(int width, int height);

/// Initialize the DDR3 direct writer for FPGA native video output.
/// Maps /dev/mem at the native video buffer region (0x3A000000) and clears
/// both frame buffers. Returns true when the writer is ready. On Mac or any
/// non-PORT_MISTER Linux build, returns false unconditionally (stub).
bool    NativeVideoWriter_Init(void);

/// Release DDR3 mapping and close /dev/mem.
void    NativeVideoWriter_Shutdown(void);

/// Write one RGB565 frame into the inactive DDR3 double-buffer, then flip the
/// control word so the FPGA reads from the freshly-written buffer.
/// @param pixels_rgb565  Source pixel data in RGB565 format.
/// @param width          Must equal nv_frame_width_runtime (320 for 4:3,
///                       424 for 16:9 after NativeVideoWriter_SetDims).
/// @param height         Must equal nv_frame_height_runtime (240 for both modes).
/// @param pitch_bytes    Row stride in bytes (width*2 if contiguous).
///                       Renamed from 3sx's "pitch" to prevent callers from
///                       passing RSDK's ScreenInfo::pitch (uint16 pixels)
///                       directly without multiplying by sizeof(uint16).
void    NativeVideoWriter_WriteFrame(const void *pixels_rgb565, int width, int height, int pitch_bytes);

/// True if the DDR3 writer has been initialised and is ready for frames.
bool    NativeVideoWriter_IsActive(void);

/// Read the FPGA vsync feedback word from DDR3 (offset 0x40).
/// Feedback format: bits[31:8] = ARM timestamp (us, bottom 24 bits),
///                  bits[7:0]  = FPGA 8-bit frame counter.
/// Returns 0 if the writer is not initialized.
uint32_t NativeVideoWriter_ReadFeedback(void);

/// Read the feedback sequence number from DDR3 (offset 0x44).
uint32_t NativeVideoWriter_ReadFeedbackSeq(void);

/// Extract the FPGA frame counter from a feedback word.
static inline uint8_t  NV_FeedbackFrameCounter(uint32_t fb) { return (uint8_t)(fb & 0xFF); }

/// Extract the ARM timestamp (microseconds, bottom 24 bits) from a feedback word.
static inline uint32_t NV_FeedbackTimestampUs(uint32_t fb) { return fb >> 8; }

#ifdef __cplusplus
}
#endif

#endif /* RSDK_MISTER_NATIVE_VIDEO_WRITER_H */
