#ifndef YETTY_CORE_EVENT_H
#define YETTY_CORE_EVENT_H

#include <stdint.h>
#include <yetty/core/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum yetty_core_event_type {
    YETTY_EVENT_NONE = 0,
    /* Input events */
    YETTY_EVENT_KEY_DOWN,
    YETTY_EVENT_KEY_UP,
    YETTY_EVENT_CHAR,
    YETTY_EVENT_MOUSE_DOWN,
    YETTY_EVENT_MOUSE_UP,
    YETTY_EVENT_MOUSE_MOVE,
    YETTY_EVENT_MOUSE_DRAG,
    YETTY_EVENT_SCROLL,
    /* Focus events */
    YETTY_EVENT_SET_FOCUS,
    /* Resize */
    YETTY_EVENT_RESIZE,
    /* Poll */
    YETTY_EVENT_POLL_READABLE,
    YETTY_EVENT_POLL_WRITABLE,
    /* Timer */
    YETTY_EVENT_TIMER,
    /* Context menu */
    YETTY_EVENT_CONTEXT_MENU_ACTION,
    /* Card-local events */
    YETTY_EVENT_CARD_MOUSE_DOWN,
    YETTY_EVENT_CARD_MOUSE_UP,
    YETTY_EVENT_CARD_MOUSE_MOVE,
    YETTY_EVENT_CARD_SCROLL,
    YETTY_EVENT_CARD_KEY_DOWN,
    YETTY_EVENT_CARD_CHAR,
    /* Tree manipulation */
    YETTY_EVENT_CLOSE,
    YETTY_EVENT_SPLIT_PANE,
    /* Clipboard */
    YETTY_EVENT_COPY,
    YETTY_EVENT_PASTE,
    /* Command mode */
    YETTY_EVENT_COMMAND_KEY,
    /* Cursor shape */
    YETTY_EVENT_SET_CURSOR,
    /* Card repack */
    YETTY_EVENT_CARD_BUFFER_REPACK,
    YETTY_EVENT_CARD_TEXTURE_REPACK,
    /* Frame rate */
    YETTY_EVENT_SET_FRAME_RATE,
    /* Render */
    YETTY_EVENT_RENDER
};

struct yetty_core_event_key {
    int key;
    int mods;
    int scancode;
};

struct yetty_core_event_char {
    uint32_t codepoint;
    int mods;
};

struct yetty_core_event_mouse {
    float x;
    float y;
    int button;
    int mods;
};

struct yetty_core_event_scroll {
    float x;
    float y;
    float dx;
    float dy;
    int mods;
};

struct yetty_core_event_set_focus {
    yetty_core_object_id object_id;
};

struct yetty_core_event_resize {
    float width;
    float height;
};

struct yetty_core_event_poll {
    int fd;
};

struct yetty_core_event_timer {
    int timer_id;
};

struct yetty_core_event_context_menu_action {
    yetty_core_object_id object_id;
    int row;
    int col;
    char action[32];
};

struct yetty_core_event_card_mouse {
    yetty_core_object_id target_id;
    float x;
    float y;
    int button;
};

struct yetty_core_event_card_scroll {
    yetty_core_object_id target_id;
    float x;
    float y;
    float dx;
    float dy;
    int mods;
};

struct yetty_core_event_card_key {
    yetty_core_object_id target_id;
    int key;
    int mods;
    int scancode;
};

struct yetty_core_event_card_char {
    yetty_core_object_id target_id;
    uint32_t codepoint;
    int mods;
};

struct yetty_core_event_close {
    yetty_core_object_id object_id;
};

struct yetty_core_event_split_pane {
    yetty_core_object_id object_id;
    uint8_t orientation;
};

struct yetty_core_event_command_key {
    int key;
    uint32_t codepoint;
    int mods;
};

struct yetty_core_event_set_cursor {
    int shape;
};

struct yetty_core_event_card_repack {
    yetty_core_object_id target_id;
};

struct yetty_core_event_set_frame_rate {
    uint32_t fps;
};

struct yetty_core_event {
    enum yetty_core_event_type type;
    union {
        struct yetty_core_event_key key;
        struct yetty_core_event_char chr;
        struct yetty_core_event_mouse mouse;
        struct yetty_core_event_scroll scroll;
        struct yetty_core_event_set_focus set_focus;
        struct yetty_core_event_resize resize;
        struct yetty_core_event_poll poll;
        struct yetty_core_event_timer timer;
        struct yetty_core_event_context_menu_action ctx_menu;
        struct yetty_core_event_card_mouse card_mouse;
        struct yetty_core_event_card_scroll card_scroll;
        struct yetty_core_event_card_key card_key;
        struct yetty_core_event_card_char card_char;
        struct yetty_core_event_close close;
        struct yetty_core_event_split_pane split_pane;
        struct yetty_core_event_command_key cmd_key;
        struct yetty_core_event_set_cursor set_cursor;
        struct yetty_core_event_card_repack card_repack;
        struct yetty_core_event_set_frame_rate set_frame_rate;
    };
    void *payload;  /* optional heap-allocated data (copy/paste text) */
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_CORE_EVENT_H */
