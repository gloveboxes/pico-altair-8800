#include "bt_keyboard.h"

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "pico/util/queue.h"

#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

#ifndef BT_KEYBOARD_VERBOSE
#define BT_KEYBOARD_VERBOSE 0
#endif
#define BT_LOG(...) do { if (BT_KEYBOARD_VERBOSE) printf(__VA_ARGS__); } while (0)

#define BT_KEYBOARD_INPUT_QUEUE_DEPTH 64
#define BT_KEYBOARD_COMMAND_QUEUE_DEPTH 8
#define BT_KEYBOARD_HID_DESCRIPTOR_STORAGE_SIZE 768
#define BT_KEYBOARD_NAME_MAX_LEN 31
#define BT_KEYBOARD_NUM_KEYS 6
#define BT_KEYBOARD_TLV_TAG ((((uint32_t)'B') << 24) | (((uint32_t)'T') << 16) | (((uint32_t)'K') << 8) | 'B')

typedef enum
{
    BT_KEYBOARD_COMMAND_START_PAIRING = 1,
    BT_KEYBOARD_COMMAND_DISCONNECT,
    BT_KEYBOARD_COMMAND_CLEAR_BONDS,
} bt_keyboard_command_t;

typedef enum
{
    BT_KEYBOARD_STATE_OFF = 0,
    BT_KEYBOARD_STATE_WAITING_FOR_STACK,
    BT_KEYBOARD_STATE_IDLE,
    BT_KEYBOARD_STATE_SCANNING,
    BT_KEYBOARD_STATE_CONNECTING,
    BT_KEYBOARD_STATE_PAIRING,
    BT_KEYBOARD_STATE_CONNECTING_HID,
    BT_KEYBOARD_STATE_READY,
} bt_keyboard_state_t;

typedef struct
{
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} bt_keyboard_peer_t;

static queue_t bt_keyboard_input_queue;
static queue_t bt_keyboard_command_queue;
static bool bt_keyboard_queues_initialized = false;

static volatile bt_keyboard_state_t bt_keyboard_state = BT_KEYBOARD_STATE_OFF;
static volatile bool bt_keyboard_stack_ready = false;
static volatile bool bt_keyboard_connected = false;
static volatile bool bt_keyboard_has_bonded_peer = false;

static bt_keyboard_peer_t bt_keyboard_peer;
static char bt_keyboard_name[BT_KEYBOARD_NAME_MAX_LEN + 1] = {0};
static hci_con_handle_t bt_keyboard_connection_handle = HCI_CON_HANDLE_INVALID;
static uint16_t bt_keyboard_hids_cid = 0;
static hid_protocol_mode_t bt_keyboard_protocol_mode = HID_PROTOCOL_MODE_REPORT;
static uint8_t bt_keyboard_hid_descriptor_storage[BT_KEYBOARD_HID_DESCRIPTOR_STORAGE_SIZE];
static uint8_t bt_keyboard_last_keys[BT_KEYBOARD_NUM_KEYS];

static bool bt_keyboard_manual_pair_requested = false;
static bool bt_keyboard_manual_disconnect_requested = false;
static bool bt_keyboard_scan_after_disconnect = false;
static volatile bool bt_keyboard_store_pending = false;

static btstack_timer_source_t bt_keyboard_reconnect_timer;
static btstack_timer_source_t bt_keyboard_connect_timer;
static btstack_packet_callback_registration_t bt_keyboard_hci_event_registration;
static btstack_packet_callback_registration_t bt_keyboard_sm_event_registration;

static const btstack_tlv_t* bt_keyboard_tlv_impl = NULL;
static void* bt_keyboard_tlv_context = NULL;

static const uint8_t bt_keyboard_keytable_us_none[] = {
    0xff, 0xff, 0xff, 0xff, 'a',  'b',  'c',  'd',  'e',  'f',  'g',  'h',  'i',  'j', 'k', 'l', 'm', 'n',
    'o',  'p',  'q',  'r',  's',  't',  'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2', '3', '4', '5', '6',
    '7',  '8',  '9',  '0',  '\r', 0x1b, 0x08, '\t', ' ',  '-',  '=',  '[',  ']',  '\\', 0xff, ';', '\'', '`',
    ',',  '.',  '/',  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const uint8_t bt_keyboard_keytable_us_shift[] = {
    0xff, 0xff, 0xff, 0xff, 'A',  'B',  'C',  'D',  'E',  'F',  'G',  'H',  'I',  'J', 'K', 'L', 'M', 'N',
    'O',  'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@', '#', '$', '%', '^',
    '&',  '*',  '(',  ')',  '\r', 0x1b, 0x08, '\t', ' ',  '_',  '+',  '{',  '}',  '|',  0xff, ':', '"', '~',
    '<',  '>',  '?',  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void bt_keyboard_start_scan(void);
static void bt_keyboard_connect_stored_peer(void);
static void bt_keyboard_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void bt_keyboard_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void bt_keyboard_handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void bt_keyboard_reconnect_timeout(btstack_timer_source_t* timer);

static void bt_keyboard_enqueue_input(uint8_t value)
{
    uint8_t ascii = (uint8_t)(value & ASCII_MASK_7BIT);
    if (!queue_try_add(&bt_keyboard_input_queue, &ascii))
    {
        uint8_t discard = 0;
        if (queue_try_remove(&bt_keyboard_input_queue, &discard))
        {
            queue_try_add(&bt_keyboard_input_queue, &ascii);
        }
    }
}

static void bt_keyboard_copy_name_from_advertisement(const uint8_t* packet)
{
    memset(bt_keyboard_name, 0, sizeof(bt_keyboard_name));

    ad_context_t context;
    const uint8_t* data = gap_event_advertising_report_get_data(packet);
    const uint8_t data_len = gap_event_advertising_report_get_data_length(packet);

    for (ad_iterator_init(&context, data_len, data); ad_iterator_has_more(&context); ad_iterator_next(&context))
    {
        const uint8_t data_type = ad_iterator_get_data_type(&context);
        if (data_type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME &&
            data_type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME)
        {
            continue;
        }

        const uint8_t* field = ad_iterator_get_data(&context);
        size_t field_len = ad_iterator_get_data_len(&context);
        if (field_len > BT_KEYBOARD_NAME_MAX_LEN)
        {
            field_len = BT_KEYBOARD_NAME_MAX_LEN;
        }
        memcpy(bt_keyboard_name, field, field_len);
        bt_keyboard_name[field_len] = '\0';
        return;
    }
}

static void bt_keyboard_load_bonded_peer(void)
{
    btstack_tlv_get_instance(&bt_keyboard_tlv_impl, &bt_keyboard_tlv_context);
    if (!bt_keyboard_tlv_impl)
    {
        BT_LOG("Bluetooth keyboard: TLV not available, cannot load bond.\n");
        bt_keyboard_has_bonded_peer = false;
        return;
    }

    int stored_len = bt_keyboard_tlv_impl->get_tag(bt_keyboard_tlv_context, BT_KEYBOARD_TLV_TAG,
                                                   (uint8_t*)&bt_keyboard_peer, sizeof(bt_keyboard_peer));
    bt_keyboard_has_bonded_peer = stored_len == (int)sizeof(bt_keyboard_peer);

    if (bt_keyboard_has_bonded_peer)
    {
        BT_LOG("Bluetooth keyboard: loaded bond for %s (type=%d).\n",
               bd_addr_to_str(bt_keyboard_peer.addr), bt_keyboard_peer.addr_type);
    }
    else
    {
        BT_LOG("Bluetooth keyboard: no stored bond found (read %d bytes, expected %d).\n",
               stored_len, (int)sizeof(bt_keyboard_peer));
    }
}

static void bt_keyboard_store_bonded_peer(void)
{
    if (!bt_keyboard_tlv_impl)
    {
        btstack_tlv_get_instance(&bt_keyboard_tlv_impl, &bt_keyboard_tlv_context);
    }
    if (!bt_keyboard_tlv_impl)
    {
        BT_LOG("Bluetooth keyboard: TLV not available, cannot store bond.\n");
        return;
    }

    bt_keyboard_tlv_impl->store_tag(bt_keyboard_tlv_context, BT_KEYBOARD_TLV_TAG,
                                    (const uint8_t*)&bt_keyboard_peer, sizeof(bt_keyboard_peer));
    bt_keyboard_has_bonded_peer = true;

    // Verify the write persisted
    bt_keyboard_peer_t verify = {0};
    int vlen = bt_keyboard_tlv_impl->get_tag(bt_keyboard_tlv_context, BT_KEYBOARD_TLV_TAG,
                                             (uint8_t*)&verify, sizeof(verify));
    if (vlen == (int)sizeof(verify) && memcmp(&verify, &bt_keyboard_peer, sizeof(verify)) == 0)
    {
        BT_LOG("Bluetooth keyboard: bond stored for %s.\n", bd_addr_to_str(bt_keyboard_peer.addr));
    }
    else
    {
        printf("Bluetooth keyboard: WARNING bond verify failed (read %d bytes, expected %d).\n",
               vlen, (int)sizeof(verify));
    }
}

static void bt_keyboard_delete_bonded_peer(void)
{
    if (!bt_keyboard_tlv_impl)
    {
        btstack_tlv_get_instance(&bt_keyboard_tlv_impl, &bt_keyboard_tlv_context);
    }
    if (bt_keyboard_tlv_impl)
    {
        bt_keyboard_tlv_impl->delete_tag(bt_keyboard_tlv_context, BT_KEYBOARD_TLV_TAG);
    }

    memset(&bt_keyboard_peer, 0, sizeof(bt_keyboard_peer));
    memset(bt_keyboard_name, 0, sizeof(bt_keyboard_name));
    bt_keyboard_has_bonded_peer = false;
}

static bool bt_keyboard_advertisement_contains_hid_service(const uint8_t* packet)
{
    const uint8_t* data = gap_event_advertising_report_get_data(packet);
    const uint8_t data_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(data_len, data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void bt_keyboard_schedule_reconnect(uint32_t timeout_ms)
{
    btstack_run_loop_set_timer(&bt_keyboard_reconnect_timer, timeout_ms);
    btstack_run_loop_set_timer_handler(&bt_keyboard_reconnect_timer, bt_keyboard_reconnect_timeout);
    btstack_run_loop_add_timer(&bt_keyboard_reconnect_timer);
}

static void bt_keyboard_connect_timeout(btstack_timer_source_t* timer)
{
    (void)timer;
    if (bt_keyboard_state == BT_KEYBOARD_STATE_CONNECTING)
    {
        BT_LOG("Bluetooth keyboard: connection attempt timed out, cancelling.\n");
        gap_connect_cancel();
        bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;

        if (bt_keyboard_has_bonded_peer && !bt_keyboard_manual_pair_requested && !bt_keyboard_manual_disconnect_requested)
        {
            BT_LOG("Bluetooth keyboard: will retry in 5s.\n");
            bt_keyboard_schedule_reconnect(5000);
        }
    }
}

static void bt_keyboard_start_connect_timer(void)
{
    btstack_run_loop_set_timer(&bt_keyboard_connect_timer, 10000);
    btstack_run_loop_set_timer_handler(&bt_keyboard_connect_timer, bt_keyboard_connect_timeout);
    btstack_run_loop_add_timer(&bt_keyboard_connect_timer);
}

static void bt_keyboard_cancel_pending_connection(void)
{
    btstack_run_loop_remove_timer(&bt_keyboard_connect_timer);
    if (bt_keyboard_state == BT_KEYBOARD_STATE_CONNECTING)
    {
        gap_connect_cancel();
    }
}

static void bt_keyboard_connect_stored_peer(void)
{
    if (!bt_keyboard_stack_ready || !bt_keyboard_has_bonded_peer)
    {
        bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
        return;
    }

    BT_LOG("Bluetooth keyboard: connecting to bonded BLE device %s...\n", bd_addr_to_str(bt_keyboard_peer.addr));
    bt_keyboard_state = BT_KEYBOARD_STATE_CONNECTING;
    gap_connect(bt_keyboard_peer.addr, bt_keyboard_peer.addr_type);
    bt_keyboard_start_connect_timer();
}

static btstack_timer_source_t bt_keyboard_scan_timer;

static void bt_keyboard_scan_timeout(btstack_timer_source_t* timer)
{
    (void)timer;
    if (bt_keyboard_state == BT_KEYBOARD_STATE_SCANNING)
    {
        gap_stop_scan();
        bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
        BT_LOG("Bluetooth keyboard: scan timed out, no HID keyboard found.\n");
    }
}

static void bt_keyboard_start_scan(void)
{
    if (!bt_keyboard_stack_ready)
    {
        bt_keyboard_state = BT_KEYBOARD_STATE_WAITING_FOR_STACK;
        return;
    }

    // Cancel any in-flight connection attempt before scanning
    bt_keyboard_cancel_pending_connection();

    memset(bt_keyboard_name, 0, sizeof(bt_keyboard_name));
    BT_LOG("Bluetooth keyboard: scanning for BLE HID keyboards...\n");
    printf("Put the keyboard into pairing mode now.\n");
    bt_keyboard_state = BT_KEYBOARD_STATE_SCANNING;
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();

    // Auto-stop scan after 30 seconds to avoid permanently consuming radio time
    btstack_run_loop_set_timer(&bt_keyboard_scan_timer, 30000);
    btstack_run_loop_set_timer_handler(&bt_keyboard_scan_timer, bt_keyboard_scan_timeout);
    btstack_run_loop_add_timer(&bt_keyboard_scan_timer);
}

static uint8_t bt_keyboard_translate_usage(uint8_t usage, bool shift, bool ctrl)
{
    if (ctrl && usage >= 0x04 && usage <= 0x1d)
    {
        /* Ctrl+M (usage 0x10) → byte 28 (0x1C) to toggle CPU monitor,
         * matching the web terminal's Ctrl+M mapping.
         * Without this, Ctrl+M = 0x0D = Enter (indistinguishable). */
        if (usage == 0x10) return 28;
        return (uint8_t)CTRL_KEY((char)('A' + (usage - 0x04)));
    }

    switch (usage)
    {
        case 0x49:
            return (uint8_t)CTRL_KEY('O');
        case 0x4C:
            return (uint8_t)CTRL_KEY('G');
        case 0x4F:
            return (uint8_t)CTRL_KEY('D');
        case 0x50:
            return (uint8_t)CTRL_KEY('S');
        case 0x51:
            return (uint8_t)CTRL_KEY('X');
        case 0x52:
            return (uint8_t)CTRL_KEY('E');
        default:
            break;
    }

    if (usage < sizeof(bt_keyboard_keytable_us_none))
    {
        return shift ? bt_keyboard_keytable_us_shift[usage] : bt_keyboard_keytable_us_none[usage];
    }
    return 0xff;
}

static void bt_keyboard_handle_input_report(const uint8_t* descriptor_data, uint16_t descriptor_len,
                                            const uint8_t* report, uint16_t report_len)
{
    if (report_len < 1)
    {
        return;
    }

    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, descriptor_data, descriptor_len,
                            HID_REPORT_TYPE_INPUT, report, report_len);

    bool shift = false;
    bool ctrl = false;
    uint8_t new_keys[BT_KEYBOARD_NUM_KEYS] = {0};
    int new_key_count = 0;

    while (btstack_hid_parser_has_more(&parser))
    {
        uint16_t usage_page = 0;
        uint16_t usage = 0;
        int32_t value = 0;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);
        if (usage_page != 0x07)
        {
            continue;
        }

        switch (usage)
        {
            case 0xE0:
            case 0xE4:
                if (value)
                {
                    ctrl = true;
                }
                continue;
            case 0xE1:
            case 0xE5:
                if (value)
                {
                    shift = true;
                }
                continue;
            case 0x00:
                continue;
            default:
                break;
        }

        if (new_key_count < BT_KEYBOARD_NUM_KEYS)
        {
            new_keys[new_key_count++] = (uint8_t)usage;
        }

        for (int i = 0; i < BT_KEYBOARD_NUM_KEYS; ++i)
        {
            if (usage == bt_keyboard_last_keys[i])
            {
                usage = 0;
                break;
            }
        }

        if (usage == 0)
        {
            continue;
        }

        uint8_t translated = bt_keyboard_translate_usage((uint8_t)usage, shift, ctrl);
        if (translated != 0xff)
        {
            bt_keyboard_enqueue_input(translated);
        }
    }

    memcpy(bt_keyboard_last_keys, new_keys, sizeof(bt_keyboard_last_keys));
}

static void bt_keyboard_handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    (void)packet_type;
    (void)channel;
    (void)size;

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META)
    {
        return;
    }

    switch (hci_event_gattservice_meta_get_subevent_code(packet))
    {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
        {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status == ERROR_CODE_SUCCESS)
            {
                bt_keyboard_connected = true;
                bt_keyboard_state = BT_KEYBOARD_STATE_READY;
                bt_keyboard_manual_pair_requested = false;
                bt_keyboard_manual_disconnect_requested = false;
                bt_keyboard_scan_after_disconnect = false;
                bt_keyboard_has_bonded_peer = true;
                bt_keyboard_store_pending = true;  // Defer flash write to poll context
                printf("Bluetooth keyboard: connected and ready.");
                if (bt_keyboard_name[0] != '\0')
                {
                    printf(" Device: %s", bt_keyboard_name);
                }
                printf("\n");
            }
            else
            {
                bt_keyboard_connected = false;
                bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
                printf("Bluetooth keyboard: HID service discovery failed (0x%02x).\n", status);
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            bt_keyboard_connected = false;
            bt_keyboard_connection_handle = HCI_CON_HANDLE_INVALID;
            bt_keyboard_hids_cid = 0;
            memset(bt_keyboard_last_keys, 0, sizeof(bt_keyboard_last_keys));
            btstack_run_loop_remove_timer(&bt_keyboard_reconnect_timer);
            BT_LOG("Bluetooth keyboard: HID service disconnected.\n");
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
        {
            uint8_t si = gattservice_subevent_hid_report_get_service_index(packet);
            bt_keyboard_handle_input_report(
                hids_client_descriptor_storage_get_descriptor_data(bt_keyboard_hids_cid, si),
                hids_client_descriptor_storage_get_descriptor_len(bt_keyboard_hids_cid, si),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;
        }

        default:
            break;
    }
}

static void bt_keyboard_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
    {
        return;
    }

    switch (hci_event_packet_get_type(packet))
    {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING)
            {
                return;
            }

            bt_keyboard_stack_ready = true;
            bt_keyboard_load_bonded_peer();
            BT_LOG("Bluetooth keyboard: stack ready.\n");

            if (bt_keyboard_has_bonded_peer)
            {
                bt_keyboard_connect_stored_peer();
            }
            else
            {
                bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
            }

            if (bt_keyboard_manual_pair_requested)
            {
                bt_keyboard_start_scan();
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT:
            if (bt_keyboard_state != BT_KEYBOARD_STATE_SCANNING)
            {
                return;
            }
            if (!bt_keyboard_advertisement_contains_hid_service(packet))
            {
                return;
            }

            gap_stop_scan();
            btstack_run_loop_remove_timer(&bt_keyboard_scan_timer);
            gap_event_advertising_report_get_address(packet, bt_keyboard_peer.addr);
            bt_keyboard_peer.addr_type = gap_event_advertising_report_get_address_type(packet);
            bt_keyboard_copy_name_from_advertisement(packet);
            bt_keyboard_state = BT_KEYBOARD_STATE_CONNECTING;
            BT_LOG("Bluetooth keyboard: found %s%s%s at %s, connecting...\n",
                   bt_keyboard_name[0] != '\0' ? "'" : "",
                   bt_keyboard_name[0] != '\0' ? bt_keyboard_name : "HID keyboard",
                   bt_keyboard_name[0] != '\0' ? "'" : "",
                   bd_addr_to_str(bt_keyboard_peer.addr));
            gap_connect(bt_keyboard_peer.addr, bt_keyboard_peer.addr_type);
            bt_keyboard_start_connect_timer();
            break;

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
            {
                return;
            }

            btstack_run_loop_remove_timer(&bt_keyboard_connect_timer);
            bt_keyboard_connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            bt_keyboard_state = BT_KEYBOARD_STATE_PAIRING;
            BT_LOG("Bluetooth keyboard: link established, requesting pairing...\n");
            sm_request_pairing(bt_keyboard_connection_handle);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
        {
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            bt_keyboard_connected = false;
            bt_keyboard_connection_handle = HCI_CON_HANDLE_INVALID;
            bt_keyboard_hids_cid = 0;
            memset(bt_keyboard_last_keys, 0, sizeof(bt_keyboard_last_keys));

            BT_LOG("Bluetooth keyboard: link disconnected (reason=0x%02x).\n", reason);

            if (bt_keyboard_scan_after_disconnect)
            {
                bt_keyboard_scan_after_disconnect = false;
                bt_keyboard_start_scan();
            }
            else if (bt_keyboard_has_bonded_peer && !bt_keyboard_manual_pair_requested && !bt_keyboard_manual_disconnect_requested)
            {
                bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
                BT_LOG("Bluetooth keyboard: disconnected, retrying bonded device.\n");
                bt_keyboard_schedule_reconnect(2000);
            }
            else
            {
                bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
            }
            break;
        }

        default:
            break;
    }
}

static void bt_keyboard_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
    {
        return;
    }

    bool connect_to_hid = false;

    switch (hci_event_packet_get_type(packet))
    {
        case SM_EVENT_JUST_WORKS_REQUEST:
            BT_LOG("Bluetooth keyboard: Just Works pairing requested, accepting.\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            BT_LOG("Bluetooth keyboard: numeric comparison %" PRIu32 ", accepting.\n",
                   sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            printf("Bluetooth keyboard: enter passkey %" PRIu32 " on the keyboard, then press Enter.\n",
                   sm_event_passkey_display_number_get_passkey(packet));
            break;

        case SM_EVENT_PAIRING_COMPLETE:
            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS)
            {
                BT_LOG("Bluetooth keyboard: pairing complete.\n");
                connect_to_hid = true;
            }
            else
            {
                printf("Bluetooth keyboard: pairing failed (status=%u reason=%u).\n",
                       sm_event_pairing_complete_get_status(packet), sm_event_pairing_complete_get_reason(packet));
                bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
            }
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE:
            if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS)
            {
                BT_LOG("Bluetooth keyboard: re-encryption complete.\n");
                connect_to_hid = true;
            }
            else
            {
                printf("Bluetooth keyboard: re-encryption failed (status=%u), will retry.\n",
                       sm_event_reencryption_complete_get_status(packet));
                bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
            }
            break;

        default:
            break;
    }

    if (connect_to_hid)
    {
        uint8_t status;
        bt_keyboard_state = BT_KEYBOARD_STATE_CONNECTING_HID;
        status = hids_client_connect(bt_keyboard_connection_handle, bt_keyboard_handle_gatt_event,
                                     bt_keyboard_protocol_mode, &bt_keyboard_hids_cid);
        if (status != ERROR_CODE_SUCCESS)
        {
            printf("Bluetooth keyboard: failed to start HID service discovery (0x%02x).\n", status);
            bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
        }
    }
}

static void bt_keyboard_reconnect_timeout(btstack_timer_source_t* timer)
{
    (void)timer;
    bt_keyboard_connect_stored_peer();
}

void bt_keyboard_queue_init(void)
{
    if (bt_keyboard_queues_initialized)
    {
        return;
    }

    queue_init(&bt_keyboard_input_queue, sizeof(uint8_t), BT_KEYBOARD_INPUT_QUEUE_DEPTH);
    queue_init(&bt_keyboard_command_queue, sizeof(bt_keyboard_command_t), BT_KEYBOARD_COMMAND_QUEUE_DEPTH);
    bt_keyboard_queues_initialized = true;
}

void bt_keyboard_init(void)
{
    if (!bt_keyboard_queues_initialized)
    {
        bt_keyboard_queue_init();
    }

    if (bt_keyboard_state != BT_KEYBOARD_STATE_OFF)
    {
        return;
    }

    memset(&bt_keyboard_peer, 0, sizeof(bt_keyboard_peer));
    memset(bt_keyboard_name, 0, sizeof(bt_keyboard_name));
    memset(bt_keyboard_last_keys, 0, sizeof(bt_keyboard_last_keys));
    bt_keyboard_connection_handle = HCI_CON_HANDLE_INVALID;
    bt_keyboard_hids_cid = 0;
    bt_keyboard_stack_ready = false;
    bt_keyboard_connected = false;
    bt_keyboard_has_bonded_peer = false;
    bt_keyboard_manual_disconnect_requested = false;
    bt_keyboard_state = BT_KEYBOARD_STATE_WAITING_FOR_STACK;

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    gatt_client_init();

    // Minimal ATT server so the keyboard can do reverse GATT discovery
    att_db_util_init();
    att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
    {
        static const uint8_t dev_name[] = "Altair 8800";
        att_db_util_add_characteristic_uuid16(
            ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME,
            ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
            (uint8_t*)dev_name, sizeof(dev_name) - 1);
    }
    att_server_init(att_db_util_get_address(), NULL, NULL);

    // BLE HID client
    hids_client_init(bt_keyboard_hid_descriptor_storage, sizeof(bt_keyboard_hid_descriptor_storage));

    // Connection parameters tuned for a HID keyboard:
    // scan interval 60ms, scan window 30ms,
    // conn interval 15-30ms, slave latency 4, supervision timeout 4s
    gap_set_connection_parameters(0x0060, 0x0030, 0x000C, 0x0018, 4, 400, 0x0010, 0x0030);

    bt_keyboard_hci_event_registration.callback = bt_keyboard_packet_handler;
    hci_add_event_handler(&bt_keyboard_hci_event_registration);

    bt_keyboard_sm_event_registration.callback = bt_keyboard_sm_packet_handler;
    sm_add_event_handler(&bt_keyboard_sm_event_registration);

    printf("Bluetooth keyboard: enabling BLE HID support...\n");
    hci_power_control(HCI_POWER_ON);
}

void bt_keyboard_poll(void)
{
    // Process deferred flash writes outside interrupt context
    if (bt_keyboard_store_pending)
    {
        bt_keyboard_store_pending = false;
        bt_keyboard_store_bonded_peer();
    }

    bt_keyboard_command_t command;
    while (queue_try_remove(&bt_keyboard_command_queue, &command))
    {
        switch (command)
        {
            case BT_KEYBOARD_COMMAND_START_PAIRING:
                bt_keyboard_manual_pair_requested = true;
                bt_keyboard_manual_disconnect_requested = false;
                if (bt_keyboard_connected && bt_keyboard_connection_handle != HCI_CON_HANDLE_INVALID)
                {
                    bt_keyboard_scan_after_disconnect = true;
                    gap_disconnect(bt_keyboard_connection_handle);
                }
                else
                {
                    bt_keyboard_start_scan();
                }
                break;

            case BT_KEYBOARD_COMMAND_DISCONNECT:
                bt_keyboard_manual_pair_requested = false;
                bt_keyboard_manual_disconnect_requested = true;
                bt_keyboard_scan_after_disconnect = false;
                if (bt_keyboard_connection_handle != HCI_CON_HANDLE_INVALID)
                {
                    gap_disconnect(bt_keyboard_connection_handle);
                }
                else
                {
                    bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
                }
                break;

            case BT_KEYBOARD_COMMAND_CLEAR_BONDS:
                bt_keyboard_manual_pair_requested = false;
                bt_keyboard_manual_disconnect_requested = false;
                bt_keyboard_scan_after_disconnect = false;
                if (bt_keyboard_has_bonded_peer)
                {
                    gap_delete_bonding(bt_keyboard_peer.addr_type, bt_keyboard_peer.addr);
                }
                bt_keyboard_delete_bonded_peer();
                printf("Bluetooth keyboard: cleared stored bond information.\n");
                if (bt_keyboard_connection_handle != HCI_CON_HANDLE_INVALID)
                {
                    gap_disconnect(bt_keyboard_connection_handle);
                }
                else
                {
                    bt_keyboard_state = BT_KEYBOARD_STATE_IDLE;
                }
                break;

        }
    }
}

bool bt_keyboard_try_dequeue_input(uint8_t* value)
{
    return queue_try_remove(&bt_keyboard_input_queue, value);
}

void bt_keyboard_request_pairing(void)
{
    bt_keyboard_command_t command = BT_KEYBOARD_COMMAND_START_PAIRING;
    queue_add_blocking(&bt_keyboard_command_queue, &command);
}

void bt_keyboard_request_disconnect(void)
{
    bt_keyboard_command_t command = BT_KEYBOARD_COMMAND_DISCONNECT;
    queue_add_blocking(&bt_keyboard_command_queue, &command);
}

void bt_keyboard_request_clear_bonds(void)
{
    bt_keyboard_command_t command = BT_KEYBOARD_COMMAND_CLEAR_BONDS;
    queue_add_blocking(&bt_keyboard_command_queue, &command);
}

bool bt_keyboard_is_ready(void)
{
    return bt_keyboard_stack_ready;
}

bool bt_keyboard_is_connected(void)
{
    return bt_keyboard_connected;
}

bool bt_keyboard_has_bond(void)
{
    return bt_keyboard_has_bonded_peer;
}

#endif