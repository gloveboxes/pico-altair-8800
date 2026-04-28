#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)

void bt_keyboard_queue_init(void);
void bt_keyboard_init(void);
void bt_keyboard_poll(void);

bool bt_keyboard_try_dequeue_input(uint8_t* value);

void bt_keyboard_request_pairing(void);
void bt_keyboard_request_disconnect(void);
void bt_keyboard_request_clear_bonds(void);

bool bt_keyboard_is_ready(void);
bool bt_keyboard_is_connected(void);
bool bt_keyboard_has_bond(void);

#else

static inline void bt_keyboard_queue_init(void)
{
}

static inline void bt_keyboard_init(void)
{
}

static inline void bt_keyboard_poll(void)
{
}

static inline bool bt_keyboard_try_dequeue_input(uint8_t* value)
{
    (void)value;
    return false;
}

static inline void bt_keyboard_request_pairing(void)
{
}

static inline void bt_keyboard_request_disconnect(void)
{
}

static inline void bt_keyboard_request_clear_bonds(void)
{
}

static inline bool bt_keyboard_is_ready(void)
{
    return false;
}

static inline bool bt_keyboard_is_connected(void)
{
    return false;
}

static inline bool bt_keyboard_has_bond(void)
{
    return false;
}

#endif