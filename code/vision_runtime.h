#ifndef CODE_VISION_RUNTIME_H_
#define CODE_VISION_RUNTIME_H_

#include "zf_common_typedef.h"

// sys_tick is incremented every 5 ms. Preview never controls processing rate.
#ifndef VISION_RUN_PREVIEW_ENABLE
#define VISION_RUN_PREVIEW_ENABLE    1       // 1: enable moving preview at 5 Hz
#endif
#define VISION_RUN_PREVIEW_TICKS     40u
#define VISION_STOP_PREVIEW_TICKS    20u
#define VISION_STATUS_TICKS          100u

// VSYNC wrapper increments epoch BEFORE camera_vsync_handler() can restart DMA.
// Full-frame completion uses the camera library flag, not DMA request-enable bits.
// Main owns the completed source buffer only while memcpy is running.
extern volatile uint8 camera_copy_lock;
extern volatile uint32 vision_skipped_capture_count;
extern volatile uint32 camera_capture_epoch;
extern volatile uint32 camera_frame_count;
extern volatile uint32 vision_processed_count;
extern volatile uint32 vision_copy_reject_count;
extern volatile uint32 vision_result_reject_count;
extern volatile uint16 vision_camera_fps;
extern volatile uint16 vision_process_fps;
extern volatile uint32 vision_process_us;
extern volatile uint32 vision_result_interval_us;

#endif /* CODE_VISION_RUNTIME_H_ */
