#ifndef RSDK_MISTER_NATIVE_VIDEO_WRITER_H
#define RSDK_MISTER_NATIVE_VIDEO_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writer constants — must match Phase 4 RTL exactly.
// (Values repeated as defines here for ARM-side consumers that want to check
//  the contract from test harnesses; also canonical source for the .c file.)
//
// Fragility note: NV_FRAME_BYTES is derived from NV_FRAME_WIDTH *
// NV_FRAME_HEIGHT * 2 via macro expansion. A downstream TU that redefines
// NV_FRAME_WIDTH (whether via #define or -D) before including this header
// would silently diverge NV_FRAME_BYTES. Phase 2 accepts this fragility to
// match 3sx's macro style. Phase 6 should switch these to
// static const uint32_t + _Static_assert once the toolchain surface
// guarantees C11.
#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00060000u   /* 384 KB — two 153,600 B buffers + ctrl + feedback + slack */
#define NV_CTRL_OFFSET      0x00000000u
#define NV_FEEDBACK_OFFSET  0x00000040u
#define NV_BUF0_OFFSET      0x00000100u   /* ctrl (4 B at 0x00) + feedback (8 B at 0x40-0x47) + 244 B pad */
#define NV_BUF1_OFFSET      0x00025900u   /* NV_BUF0_OFFSET + NV_FRAME_BYTES = 0x100 + 0x25800 */
#define NV_FRAME_WIDTH      320
#define NV_FRAME_HEIGHT     240
#define NV_FRAME_BYTES      (NV_FRAME_WIDTH * NV_FRAME_HEIGHT * 2)  /* 153,600 */

/// Initialize the DDR3 direct writer for FPGA native video output.
/// Maps /dev/mem at the native video buffer region (0x3A000000) and clears
/// both frame buffers. Returns true when the writer is ready. On Mac or any
/// non-PORT_MISTER Linux build, returns false unconditionally (stub).
bool    NativeVideoWriter_Init(void);

/// Release DDR3 mapping and close /dev/mem.
void    NativeVideoWriter_Shutdown(void);

/// Write one RGB565 frame (320x240) into the inactive DDR3 double-buffer,
/// then flip the control word so the FPGA reads from the freshly-written buffer.
/// @param pixels_rgb565  Source pixel data in RGB565 format.
/// @param width          Must be NV_FRAME_WIDTH (320).
/// @param height         Must be NV_FRAME_HEIGHT (240).
/// @param pitch_bytes    Row stride in bytes (NV_FRAME_WIDTH*2 = 640 if contiguous).
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
