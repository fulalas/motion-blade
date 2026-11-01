#ifndef MB_ENGINE_STATE_H
#define MB_ENGINE_STATE_H

#include "framing/bar_tracker.h"
#include "framing/border.h"
#include "motionblade/engine.h"

enum { FRAME_RING = 4, MAX_QUEUE = 16 };

typedef struct geometry {
    mb_rect active;
    mb_video_format format;
    mb_chroma_layout chroma;
    int level_count;
    int base_level;
    bool normalized_luma;
    mb_grid grids[MB_MAX_LEVELS];
} geometry;

typedef struct frame_slot {
    mb_frame_planes planes;
    mb_image *levels[MB_MAX_LEVELS];
} frame_slot;

typedef struct motion_fields {
    mb_buffer *forward[MB_MAX_LEVELS];
    mb_buffer *backward[MB_MAX_LEVELS];
    mb_buffer *previous_forward[MB_MAX_LEVELS];
    mb_buffer *previous_backward[MB_MAX_LEVELS];
    mb_buffer *scratch[MB_MAX_LEVELS];
    mb_buffer *older_backward;
    mb_buffer *stats;
    mb_buffer *previous_stats;
    mb_buffer *older_stats;
    mb_buffer *history;
    mb_buffer *cross_keys;
} motion_fields;

typedef struct selection_buffers {
    mb_buffer *keys;
    mb_buffer *extra_keys;
    mb_buffer *selection[2];
    mb_buffer *cells;
    mb_buffer *fallback;
    mb_buffer *offsets;
    mb_buffer *offsets_median;
    mb_buffer *global_motion;
    mb_buffer *panel_flags;
    mb_buffer *panel_spread;
} selection_buffers;

typedef struct pair_state {
    int frame_count;
    int newest;
    bool motion_ready;
    bool has_previous;
    bool has_older;
    bool end_of_stream;
    bool history_stale;
} pair_state;

typedef enum output_kind {
    OUTPUT_HOST,
    OUTPUT_DEVICE,
} output_kind;

typedef struct queued_output {
    mb_ticket ticket;
    output_kind kind;
    mb_rect area;
    int block_count;
} queued_output;

typedef struct output_queue {
    queued_output entries[MAX_QUEUE];
    int count;
} output_queue;

typedef struct framing_state {
    bar_tracker tracker;
    border_template border;
    int border_version;
    int device_border_version;
    int push_count;
    unsigned device_ticks;
    bool flags_pending;
    bool device_input;
} framing_state;

typedef struct debug_fields {
    mb_buffer *forward;
    mb_buffer *backward;
    mb_buffer *selection;
} debug_fields;

struct mb_engine {
    mb_backend backend;
    mb_logger logger;
    mb_params params;
    mb_video_format source;
    geometry geo;
    frame_slot frames[FRAME_RING];
    motion_fields fields;
    selection_buffers sel;
    mb_frame_planes output;
    pair_state pair;
    output_queue queue;
    framing_state framing;
    debug_fields debug;
    bool healthy;
};

#endif
