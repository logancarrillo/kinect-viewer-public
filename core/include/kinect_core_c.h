#ifndef KINECT_CORE_C_H
#define KINECT_CORE_C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kv_engine kv_engine_t;

typedef enum kv_source_kind {
    KV_SOURCE_REAL = 0,
    KV_SOURCE_SYNTHETIC = 1,
    KV_SOURCE_REPLAY = 2
} kv_source_kind_t;

typedef enum kv_view_mode {
    KV_MODE_IR = 0,
    KV_MODE_COLOR = 1,
    KV_MODE_DEPTH = 2,
    KV_MODE_SIDE_BY_SIDE = 3,
    KV_MODE_POINT_CLOUD_3D = 4
} kv_view_mode_t;

typedef enum kv_point_color_source {
    KV_POINT_COLOR_DEPTH = 0,
    KV_POINT_COLOR_IR = 1,
    KV_POINT_COLOR_REGISTERED_RGB = 2
} kv_point_color_source_t;

typedef enum kv_sbs_layout {
    KV_SBS_IR_DEPTH = 0,
    KV_SBS_COLOR_DEPTH = 1,
    KV_SBS_COLOR_IR = 2
} kv_sbs_layout_t;

typedef enum kv_pipeline_kind {
    KV_PIPELINE_OPENGL = 0,
    KV_PIPELINE_OPENCL = 1,
    KV_PIPELINE_CPU = 2
} kv_pipeline_kind_t;

typedef enum kv_colormap_kind {
    KV_COLORMAP_TURBO = 0,
    KV_COLORMAP_VIRIDIS = 1,
    KV_COLORMAP_INFERNO = 2,
    KV_COLORMAP_GRAY = 3
} kv_colormap_kind_t;

typedef enum kv_exposure_mode {
    KV_EXPOSURE_AUTO = 0,
    KV_EXPOSURE_SEMI_AUTO = 1,
    KV_EXPOSURE_MANUAL = 2
} kv_exposure_mode_t;

typedef struct kv_display_params {
    kv_view_mode_t mode;
    kv_sbs_layout_t sbs_layout;

    /* IR mapping parameters */
    bool ir_auto_scale;
    float ir_low_percentile;    /* e.g. 0.01f (1%) */
    float ir_high_percentile;   /* e.g. 0.995f (99.5%) */
    float ir_ema_alpha;         /* e.g. 0.12f; 1.0f = instant */
    float ir_gamma;             /* e.g. 0.45f */
    float ir_manual_min;        /* [0..65535] when !ir_auto_scale */
    float ir_manual_max;        /* [0..65535] when !ir_auto_scale */

    /* Depth mapping parameters */
    kv_colormap_kind_t depth_colormap;
    float depth_min_mm;         /* default 500.0f */
    float depth_max_mm;         /* default 4500.0f */
    bool depth_invert;          /* true = near is warm/high LUT index */

    /* Synthetic latency clock overlay */
    bool clock_overlay;

    /* Orientation transforms */
    bool mirror_horizontal;
    bool rotate_180;

    /* Depth cut-off, Depth/Colour blend & 3D Point Cloud controls */
    bool cutoff_enabled;
    float cutoff_near_mm;       /* default 500.0f */
    float cutoff_far_mm;        /* default 2000.0f */
    bool depth_color_blend_enabled;
    float depth_color_blend;    /* [0.0f .. 1.0f] fade of depth colours over registered colour */
    kv_point_color_source_t point_color_source;
} kv_display_params_t;

typedef struct kv_ir_intrinsics {
    bool valid;
    float fx;
    float fy;
    float cx;
    float cy;
    float k1;
    float k2;
    float k3;
    float p1;
    float p2;
} kv_ir_intrinsics_t;

typedef struct kv_health_snapshot {
    double ir_fps;
    double depth_fps;
    double color_fps;

    double ir_drops_per_min;
    double depth_drops_per_min;
    double color_drops_per_min;

    double ir_drop_ratio_10s;
    double depth_drop_ratio_10s;
    bool drop_warning_active;

    double inter_frame_p50_ms;
    double inter_frame_p95_ms;
    double depth_processing_ms;

    uint32_t driver_errors;
    char last_driver_error[256];
    char usb_location[128];
} kv_health_snapshot_t;

typedef struct kv_color_controls {
    kv_exposure_mode_t mode;
    float auto_exposure_compensation; /* [-2.0, 2.0] */
    float semi_auto_pseudo_ms;        /* (0.0, 66.0] */
    float manual_integration_ms;      /* (0.0, 66.0] */
    float manual_analog_gain;         /* [1.0, 4.0] */
} kv_color_controls_t;

typedef struct kv_stats_snapshot {
    bool running;
    bool device_present;
    bool lock_blocked;
    kv_source_kind_t source_kind;
    kv_pipeline_kind_t active_pipeline;
    kv_view_mode_t active_mode;

    uint64_t ir_frames;
    uint64_t depth_frames;
    uint64_t color_frames;
    uint64_t rendered_frames;

    uint64_t ir_seq_drops;
    uint64_t depth_seq_drops;
    uint64_t color_seq_drops;

    double current_fps;
    double color_fps;
    double uptime_sec;

    /* Timing in milliseconds (stamped with mach_absolute_time) */
    double inter_frame_mean_ms;
    double inter_frame_p95_ms;
    double inter_frame_max_ms;
    double map_time_mean_ms;
    double map_time_max_ms;
    double arrival_to_map_done_ms;

    /* Numerical pixel metrics (numbers only: no image data) */
    float raw_min;
    float raw_max;
    float raw_mean;
    uint32_t saturated_pixels; /* IR >= 65535 */
    uint32_t invalid_pixels;   /* Depth <= 0 or NaN/Inf */
    float smoothed_scale_low;
    float smoothed_scale_high;
    uint8_t mapped_luma_min;
    uint8_t mapped_luma_max;
    float mapped_luma_mean;
    uint32_t histogram16[16];

    /* Color metadata from Frame + hardware readback */
    float color_exposure;
    float color_gain;
    float color_gamma;
    uint32_t readback_exposure_mode;
    float readback_integration_ms;
    float readback_analog_gain;
    int32_t color_minus_depth_ticks;

    uint32_t benign_driver_notices;
    uint32_t driver_errors;

    char serial[64];
    char firmware[64];
    char status_message[256];
} kv_stats_snapshot_t;

typedef struct kv_mapped_frame {
    uint64_t frame_seq;
    uint64_t arrival_mach_ns;
    uint64_t map_done_mach_ns;
    uint32_t device_timestamp_ticks;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    kv_view_mode_t mode;
    bool is_synthetic;
    const uint8_t* bgra_pixels; /* Owned by buffer passed in or snapshot */
} kv_mapped_frame_t;

/* Monotonic nanosecond clock via mach_absolute_time (macOS) / steady_clock */
uint64_t kv_monotonic_ns(void);

/* Initialize default parameter structs */
void kv_default_display_params(kv_display_params_t* out_params);
void kv_default_color_controls(kv_color_controls_t* out_controls);

/* Check if real Kinect USB hardware is currently visible on the USB bus (without opening it) */
bool kv_is_kinect_usb_present(void);

/* Check ~/KINECT-IN-USE status without opening the device.
 * Returns true if locked by someone else, writing owner description to out_owner. */
bool kv_is_locked_by_other(const char* lock_path, char* out_owner, size_t out_owner_cap);

/* Engine lifecycle */
kv_engine_t* kv_engine_create(const char* lock_file_path);
void kv_engine_destroy(kv_engine_t* engine);

/* Start capture. NOTE: On macOS with KV_PIPELINE_OPENGL, call from the main thread.
 * Never falls back silently if the requested pipeline fails. */
bool kv_engine_start(
    kv_engine_t* engine,
    kv_source_kind_t source,
    kv_pipeline_kind_t pipeline,
    const kv_display_params_t* initial_params,
    char* out_error,
    size_t out_error_cap
);

bool kv_engine_start_ex(
    kv_engine_t* engine,
    kv_source_kind_t source,
    kv_pipeline_kind_t pipeline,
    const kv_display_params_t* initial_params,
    bool force_enable_rgb,
    char* out_error,
    size_t out_error_cap
);

bool kv_engine_is_rgb_active(const kv_engine_t* engine);

/* Start in-app recording playback mode through the live mapping & inspection pipeline.
 * Releases any open hardware device and ~/KINECT-IN-USE lock first. */
bool kv_engine_start_replay(
    kv_engine_t* engine,
    const kv_display_params_t* initial_params,
    const kv_ir_intrinsics_t* ir_intrinsics
);

/* Push a single recorded 16-bit IR or Depth (mm) frame into an engine running in KV_SOURCE_REPLAY mode. */
bool kv_engine_push_replay_frame16(
    kv_engine_t* engine,
    bool is_depth,
    const uint16_t* pixels_u16,
    uint32_t width,
    uint32_t height,
    uint32_t frame_index,
    uint32_t device_ticks
);

void kv_engine_stop(kv_engine_t* engine);
bool kv_engine_is_running(const kv_engine_t* engine);

void kv_engine_set_display_params(kv_engine_t* engine, const kv_display_params_t* params);
void kv_engine_get_display_params(const kv_engine_t* engine, kv_display_params_t* out_params);

bool kv_engine_set_color_controls(kv_engine_t* engine, const kv_color_controls_t* controls, char* out_error, size_t out_error_cap);

/* Copy latest mapped BGRA8 frame if newer than last_seen_seq.
 * Resizes/writes into caller buffer if capacity >= required_bytes; always sets *out_required_bytes.
 * Returns true if a new frame was copied. */
bool kv_engine_copy_latest_frame(
    kv_engine_t* engine,
    uint64_t last_seen_seq,
    uint8_t* dst_bgra,
    size_t dst_cap,
    size_t* out_required_bytes,
    kv_mapped_frame_t* out_meta
);

/* Copy latest raw 16-bit frame (IR uint16 [0..65535] or Depth uint16 mm [0=invalid, 1..65535]) for lossless host recording. */
bool kv_engine_copy_latest_raw16(
    kv_engine_t* engine,
    bool want_depth,
    uint64_t last_seen_seq,
    uint16_t* dst_u16,
    size_t dst_u16_count,
    uint64_t* out_seq,
    uint64_t* out_arrival_ns,
    uint32_t* out_device_ticks,
    uint32_t* out_width,
    uint32_t* out_height
);

void kv_engine_get_stats(const kv_engine_t* engine, kv_stats_snapshot_t* out_stats);
void kv_engine_get_health(const kv_engine_t* engine, kv_health_snapshot_t* out_health);
bool kv_engine_get_ir_intrinsics(const kv_engine_t* engine, kv_ir_intrinsics_t* out_intrinsics);

/* Copy latest 512x424 float depth (mm; <=0/NaN/IR-saturated normalized to 0.0f) and 512x424 float IR [0..65535],
 * plus IR camera intrinsics from libfreenect2. Pass NULL for any buffer not needed. */
bool kv_engine_copy_inspection_buffers(
    const kv_engine_t* engine,
    float* dst_depth_mm,
    size_t dst_depth_count,
    float* dst_ir_raw,
    size_t dst_ir_count,
    uint32_t* out_width,
    uint32_t* out_height,
    kv_ir_intrinsics_t* out_intrinsics
);

/* Unproject a distorted depth pixel (col_x, row_y) with depth_mm > 0 into 3D camera coordinates (mm)
 * using libfreenect2's IR camera parameters. */
bool kv_unproject_depth_pixel_mm(
    const kv_ir_intrinsics_t* intrinsics,
    int col_x,
    int row_y,
    float depth_mm,
    float* out_x_mm,
    float* out_y_mm,
    float* out_z_mm
);

/* Compute real 3D Euclidean distance in mm between two pixels on a depth buffer using IR camera intrinsics.
 * Returns false if intrinsics are invalid, coordinates are out of bounds, or either endpoint has depth <= 0. */
bool kv_measure_3d_distance_mm(
    const kv_ir_intrinsics_t* intrinsics,
    const float* depth_mm_buf,
    uint32_t width,
    uint32_t height,
    int x1,
    int y1,
    int x2,
    int y2,
    float* out_distance_mm
);

/* Copy latest registered 512x424 undistorted depth (mm; <=0/NaN/outside-cutoff gated to 0.0f) and
 * 512x424x4 BGRA8 point colors (Depth colormap, IR, or libfreenect2 registered live Colour) for Metal 3D rendering. */
bool kv_engine_copy_point_cloud_inputs(
    kv_engine_t* engine,
    uint64_t last_seen_seq,
    float* dst_undistorted_depth_mm,
    size_t dst_depth_count,
    uint8_t* dst_point_bgra,
    size_t dst_bgra_bytes,
    uint64_t* out_seq,
    uint64_t* out_arrival_ns,
    kv_ir_intrinsics_t* out_intrinsics
);

/* Copy latest live 1920x1080 BGRA8 colour frame in memory only (for on-device Vision pose estimation).
 * Returns false in replay mode or when no new colour frame has arrived since last_seen_color_seq. */
bool kv_engine_copy_latest_color_bgra(
    kv_engine_t* engine,
    uint64_t last_seen_color_seq,
    uint8_t* dst_bgra,
    size_t dst_cap,
    uint64_t* out_color_seq,
    uint32_t* out_width,
    uint32_t* out_height
);

/* Map normalized [0..1] colour coordinates (origin top-left) through libfreenect2's Registration bigdepth &
 * color_depth_map into 512x424 Depth/IR pixel coordinates and 3D camera coordinates (meters). */
bool kv_engine_map_color_norm_to_depth_and_3d(
    kv_engine_t* engine,
    float norm_cx,
    float norm_cy,
    float* out_depth_x,
    float* out_depth_y,
    float* out_x_m,
    float* out_y_m,
    float* out_z_m
);

/* Decode embedded 64-bit clock barcode from top of a luma/BGRA buffer (for synthetic latency verification).
 * Expects width >= 512, height >= 16. luma_or_bgra_bpp = 1 (luma plane) or 4 (BGRA). */
bool kv_decode_clock_barcode(
    const uint8_t* row_base,
    uint32_t width,
    uint32_t height,
    uint32_t row_stride_bytes,
    uint32_t bytes_per_pixel,
    uint32_t* out_seq24,
    uint32_t* out_timestamp_low32_us
);

#ifdef __cplusplus
}
#endif

#endif /* KINECT_CORE_C_H */
