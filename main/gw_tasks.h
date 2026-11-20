/* The firmware's tasks in one table (wiki Mower-Gateway-Architecture §4.3):
 * name, priority and stack of every task the firmware creates, and the loop
 * counters the supervisor watches.
 *
 * Watched tasks call gw_task_alive() once per loop and never wait longer than
 * a few seconds at a time; the supervisor restarts the board when one has not
 * counted for CONFIG_GATEWAY_LIVE_STALE_S (not during an update). cell_wd is
 * not watched: it blocks for minutes inside esp_modem while it redials (the
 * supervisor's "server link down" step covers it); devcert waits on the
 * console; ota lives only for one update.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    /* watched: keep these first */
    GW_T_REMOTE_CMD,
    GW_T_TICK,
    GW_T_SIGNAL,
    GW_T_NTRIP,
    GW_T_WATCHED,
    /* not watched */
    GW_T_CELL = GW_T_WATCHED,
    GW_T_SUPERVISOR,
    GW_T_OTA,
    GW_T_DEVCERT,
    GW_T_COUNT
} gw_task_id_t;

/* xTaskCreate with the table's name, stack and priority. */
BaseType_t gw_task_create(gw_task_id_t id, TaskFunction_t fn, void *arg, TaskHandle_t *handle);

/* Counts one loop of a watched task; ignored for the others. */
void gw_task_alive(gw_task_id_t id);

/* Supervisor: the name of a watched task whose counter has not changed for
 * stale_ms, else NULL. Every task counts as alive while ota_writing. */
const char *gw_tasks_stale(int64_t now_ms, int64_t stale_ms, bool ota_writing);

/* Logs the free stack (high-water mark, bytes) of every running firmware task
 * and of the ESP-IDF tasks worth sizing. */
void gw_tasks_log_stacks(void);
