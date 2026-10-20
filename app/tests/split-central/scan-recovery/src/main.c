/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define CONFIG_BT_GATT_AUTO_DISCOVER_CCC 1
#define CONFIG_BT_USER_PHY_UPDATE 1
#define CONFIG_BT_SMP 1

/* F2: unchanged production scan/advertisement/reservation/cleanup functions.
 * Bluetooth APIs and the final peripheral-event handler are the stub boundaries.
 */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zmk/event_manager.h>
#include <zmk/split/transport/central.h>

#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_POSITION_QUEUE_SIZE 5
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS 1
#define CONFIG_ZMK_SPLIT_BLE_PREF_INT 6
#define CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY 30
#define CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT 400
#define CONFIG_ZMK_SPLIT_BLE 1
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_SPLIT_RUN_STACK_SIZE 512
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_SPLIT_RUN_QUEUE_SIZE 5
#define CONFIG_ZMK_BLE_THREAD_PRIORITY 5
#define CONFIG_ZMK_LOG_LEVEL 0

#undef SYS_INIT
#define SYS_INIT(fn, ...) static int (*const unused_init_##fn)(void) __unused = fn
#undef ZMK_SUBSCRIPTION
#define ZMK_SUBSCRIPTION(...)
#undef ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER
#define ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(name, ...)                                            \
    const struct zmk_split_transport_central name

static int scan_start_result, scan_stop_result, conn_create_result, addr_slot;
static unsigned int scan_start_calls, scan_stop_calls, conn_create_calls, conn_refs, conn_unrefs;
static bool explicit_scanning, advertise_during_start;
static const bt_addr_le_t peer = {.type = BT_ADDR_LE_RANDOM, .a = {{1, 2, 3, 4, 5, 6}}};
static struct bt_conn *const test_conn = (struct bt_conn *)0x1234;
static bool split_central_eir_found(const bt_addr_le_t *addr);

static void split_central_device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                                       struct net_buf_simple *ad);

static int test_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb) {
    scan_start_calls++;
    zassert_equal(param->type, BT_LE_SCAN_TYPE_PASSIVE);
    zassert_equal(cb, split_central_device_found);
    if (scan_start_result) {
        return scan_start_result;
    }
    zassert_false(explicit_scanning, "Duplicate underlying scan start");
    explicit_scanning = true;
    if (advertise_during_start) {
        advertise_during_start = false;
        split_central_eir_found(&peer);
    }
    return 0;
}

static int test_scan_stop(void) {
    scan_stop_calls++;
    /* Pinned Zephyr clears explicit-scan ownership before a controller stop error. */
    explicit_scanning = false;
    return scan_stop_result;
}

static int test_conn_create(const bt_addr_le_t *addr,
                            const struct bt_conn_le_create_param *create_param,
                            const struct bt_le_conn_param *param, struct bt_conn **conn) {
    ARG_UNUSED(create_param);
    conn_create_calls++;
    zassert_mem_equal(addr, &peer, sizeof(peer));
    zassert_false(explicit_scanning, "Connection attempted while explicit scan active");
    zassert_equal(param->interval_min, 6);
    zassert_equal(param->interval_max, 6);
    zassert_equal(param->latency, 30);
    zassert_equal(param->timeout, 400);
    zassert_is_null(*conn);
    if (conn_create_result) {
        return conn_create_result;
    }
    *conn = test_conn;
    conn_refs++;
    return 0;
}

static int test_put_peripheral_addr(const bt_addr_le_t *addr) {
    zassert_mem_equal(addr, &peer, sizeof(peer));
    return addr_slot;
}

static void test_conn_unref(struct bt_conn *conn) {
    zassert_equal(conn, test_conn);
    zassert_true(conn_refs > 0);
    conn_refs--;
    conn_unrefs++;
}

#define bt_le_scan_start test_scan_start
#define bt_le_scan_stop test_scan_stop
#define bt_conn_le_create test_conn_create
#define bt_conn_unref test_conn_unref
#define zmk_ble_put_peripheral_addr test_put_peripheral_addr
#include "../../../../src/split/bluetooth/central.c"
#undef zmk_ble_put_peripheral_addr
#undef bt_le_scan_start
#undef bt_le_scan_stop
#undef bt_conn_le_create
#undef bt_conn_unref

int zmk_split_transport_central_peripheral_event_handler(
    const struct zmk_split_transport_central *transport, uint8_t source,
    struct zmk_split_transport_peripheral_event ev) {
    ARG_UNUSED(transport);
    ARG_UNUSED(source);
    ARG_UNUSED(ev);
    return 0;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    struct k_work_sync sync;
    k_work_flush(&peripheral_event_work, &sync);
    memset(peripherals, 0, sizeof(peripherals));
    k_msgq_purge(&peripheral_event_msgq);
    is_enabled = true;
    is_scanning = false;
    explicit_scanning = false;
    advertise_during_start = false;
    scan_start_result = scan_stop_result = conn_create_result = addr_slot = 0;
    scan_start_calls = scan_stop_calls = conn_create_calls = conn_refs = conn_unrefs = 0;
}

static void finish_connection(void) {
    zassert_equal(peripherals[0].conn, test_conn);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
    zassert_ok(confirm_peripheral_slot_conn(test_conn));
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTED);
    zassert_ok(release_peripheral_slot(0));
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_is_null(peripherals[0].conn);
    zassert_equal(conn_refs, 0, "Connection reference leaked");
    zassert_equal(conn_unrefs, 1);
}

ZTEST(split_scan, test_failed_start_then_retry_reaches_scan_api) {
    scan_start_result = -EIO;
    zassert_equal(start_scanning(), -EIO);
    scan_start_result = 0;
    zassert_ok(start_scanning());
    zassert_equal(scan_start_calls, 2, "Retry was suppressed by stale scanning flag");
    zassert_true(explicit_scanning);
}

ZTEST(split_scan, test_repeated_start_errors_do_not_busy_retry_or_latch_state) {
    const int errors[] = {-EAGAIN, -ENOMEM, -EIO};
    for (int i = 0; i < ARRAY_SIZE(errors); i++) {
        scan_start_result = errors[i];
        zassert_equal(start_scanning(), errors[i]);
        zassert_equal(scan_start_calls, i + 1, "Unexpected immediate retry");
        zassert_false(is_scanning);
    }
    scan_start_result = 0;
    zassert_ok(start_scanning());
    zassert_true(explicit_scanning);
    zassert_equal(scan_start_calls, 4);
}

ZTEST(split_scan, test_host_already_running_is_success_without_restarting) {
    explicit_scanning = true;
    scan_start_result = -EALREADY;
    zassert_ok(start_scanning());
    zassert_true(is_scanning);
    zassert_ok(start_scanning());
    zassert_equal(scan_start_calls, 1);
}

ZTEST(split_scan, test_successful_scan_is_not_started_twice) {
    zassert_ok(start_scanning());
    zassert_ok(start_scanning());
    zassert_equal(scan_start_calls, 1);
    zassert_true(explicit_scanning);
}

ZTEST(split_scan, test_failed_stop_releases_reservation_and_can_connect_later) {
    zassert_ok(start_scanning());
    scan_stop_result = -EIO;
    zassert_false(split_central_eir_found(&peer));
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN,
                  "Stop failure leaked reservation");
    zassert_is_null(peripherals[0].conn);
    zassert_equal(conn_create_calls, 0);
    zassert_equal(scan_start_calls, 2, "Discovery not resumed after releasing reservation");
    zassert_true(explicit_scanning);
    scan_stop_result = 0;
    zassert_false(split_central_eir_found(&peer));
    zassert_equal(conn_create_calls, 1);
    finish_connection();
}

ZTEST(split_scan, test_stop_failure_followed_by_start_failure_remains_retryable) {
    zassert_ok(start_scanning());
    scan_stop_result = -EIO;
    scan_start_result = -ENOMEM;
    zassert_false(split_central_eir_found(&peer));
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_false(is_scanning);
    zassert_equal(scan_start_calls, 2);
    zassert_equal(conn_create_calls, 0);
    scan_start_result = scan_stop_result = 0;
    zassert_ok(start_scanning());
    zassert_equal(scan_start_calls, 3);
    zassert_false(split_central_eir_found(&peer));
    finish_connection();
}

ZTEST(split_scan, test_repeated_stop_errors_do_not_leak_or_busy_retry) {
    zassert_ok(start_scanning());
    scan_stop_result = -EIO;
    for (int i = 0; i < 3; i++) {
        zassert_false(split_central_eir_found(&peer));
        zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
        zassert_is_null(peripherals[0].conn);
        zassert_equal(scan_stop_calls, i + 1);
        zassert_equal(scan_start_calls, i + 2);
        zassert_equal(conn_refs, 0);
    }
    scan_stop_result = 0;
    zassert_false(split_central_eir_found(&peer));
    finish_connection();
}

ZTEST(split_scan, test_already_stopped_error_does_not_leak_reservation) {
    zassert_ok(start_scanning());
    explicit_scanning = false;
    scan_stop_result = -EALREADY;
    zassert_false(split_central_eir_found(&peer));
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_equal(conn_create_calls, 0);
    zassert_equal(scan_start_calls, 2);
}

ZTEST(split_scan, test_connection_create_failure_cleanup_still_recovers) {
    zassert_ok(start_scanning());
    conn_create_result = -ENOMEM;
    zassert_false(split_central_eir_found(&peer));
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_equal(conn_refs, 0);
    zassert_true(explicit_scanning);
    zassert_equal(scan_start_calls, 2);
    conn_create_result = 0;
    zassert_false(split_central_eir_found(&peer));
    finish_connection();
}

ZTEST(split_scan, test_successful_stop_and_connect_preserve_reference_ownership) {
    zassert_ok(start_scanning());
    zassert_false(split_central_eir_found(&peer));
    zassert_false(is_scanning);
    zassert_false(explicit_scanning);
    zassert_equal(scan_stop_calls, 1);
    zassert_equal(scan_start_calls, 1);
    finish_connection();
}

ZTEST(split_scan, test_disabled_central_does_not_scan) {
    is_enabled = false;
    zassert_ok(start_scanning());
    zassert_equal(scan_start_calls, 0);
}

ZTEST(split_scan, test_connected_slots_do_not_scan) {
    peripherals[0].conn = test_conn;
    peripherals[0].state = PERIPHERAL_SLOT_STATE_CONNECTED;
    zassert_ok(start_scanning());
    zassert_equal(scan_start_calls, 0);
}

ZTEST(split_scan, test_failed_reservation_leaves_scan_running) {
    zassert_ok(start_scanning());
    addr_slot = -ENOMEM;
    zassert_false(split_central_eir_found(&peer));
    zassert_equal(scan_stop_calls, 0);
    zassert_equal(conn_create_calls, 0);
    zassert_true(explicit_scanning);
}

ZTEST(split_scan, test_advertisement_during_start_does_not_restore_stopped_flag) {
    advertise_during_start = true;
    zassert_ok(start_scanning());
    zassert_false(is_scanning, "Start completion overwrote callback's stop state");
    zassert_equal(conn_create_calls, 1);
    finish_connection();
}

ZTEST(split_scan, test_advertisement_parser_recovers_after_failed_stop) {
    zassert_ok(start_scanning());
    scan_stop_result = -EIO;
    NET_BUF_SIMPLE_DEFINE(ad, 18);
    net_buf_simple_add_u8(&ad, 17);
    net_buf_simple_add_u8(&ad, BT_DATA_UUID128_ALL);
    net_buf_simple_add_mem(&ad, split_service_uuid.val, 16);
    split_central_device_found(&peer, -40, BT_GAP_ADV_TYPE_ADV_IND, &ad);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_equal(conn_create_calls, 0);
    zassert_equal(scan_start_calls, 2);
    scan_stop_result = 0;
    split_central_device_found(&peer, -40, BT_GAP_ADV_TYPE_ADV_DIRECT_IND, NULL);
    finish_connection();
}

ZTEST_SUITE(split_scan, NULL, NULL, before, NULL, NULL);
