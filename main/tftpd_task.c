/*
 * tftpd_task.c  —  TFTP Server: Task Entry and Initialization
 *                   ESP-IDF / FreeRTOS
 *
 * Changes:
 *   - Added missing #include <errno.h> (errno used in select() error path).
 *   - Removed redundant forward declarations (tftpd_init / tftpd_main_task
 *     defined in the same translation unit; no prior declaration needed).
 *   - Removed duplicate #include <string.h>.
 *   - tftpd_main_task made static (not called from outside this file).
 *
 * BUG FIX — Timer starvation:
 *   Previously tftpd_handle_timer() was called ONLY when select() returned 0
 *   (i.e. a full 1-second idle period with no incoming packets).  During an
 *   active WRQ the client sends DATA packets continuously, so select() never
 *   times out, tftpd_handle_timer() is never called, and the DALLY countdown
 *   never advances — the write session stays locked indefinitely.  When the
 *   client finally stops sending, the server's retry/timeout machinery then
 *   burns the full (retry × timeout) = 3 × 3 = 9 s before logging
 *   "transfer timeout" and releasing the session.
 *
 *   Fix: use esp_timer_get_time() to track wall-clock elapsed time.
 *   tftpd_handle_timer() is now called once per second regardless of socket
 *   activity, ensuring DALLY expiry and retransmit timeouts work correctly
 *   even while data is flowing.
 */

#include <errno.h>      /* errno, EINTR                                      */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <sys/select.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_timer.h"  /* esp_timer_get_time() — monotonic µs since boot   */

#include "tftpd.h"

static const char *TAG = "tftpd_task";

/* ── Module globals (extern-declared in tftpd.h) ───────────────────────── */
tftpd_config_t   g_tftpd_cfg;
tftpd_session_t  g_tftpd_sessions[TFTPD_MAX_SESSIONS];
int              g_tftpd_listen_sock = -1;
uint8_t          g_tftpd_num_read    = 0;
uint8_t          g_tftpd_num_write   = 0;
SemaphoreHandle_t g_tftpd_fs_mutex   = NULL;

/* ── Static task (not part of the public API) ───────────────────────────── */
static void tftpd_main_task(void *param);

/* ═══════════════════════════════════════════════════════════════════════════
 * tftpd_init
 * ═══════════════════════════════════════════════════════════════════════════*/
void tftpd_init(void)
{
    int i;

    /* 1. Configuration defaults */
    memset(&g_tftpd_cfg, 0, sizeof(g_tftpd_cfg));
    g_tftpd_cfg.enabled = 1;
    g_tftpd_cfg.port    = TFTPD_DEFAULT_PORT;
    g_tftpd_cfg.timeout = TFTPD_DEFAULT_TIMEOUT;
    g_tftpd_cfg.retry   = TFTPD_DEFAULT_RETRY;

    /* 2. Session array */
    memset(g_tftpd_sessions, 0, sizeof(g_tftpd_sessions));
    for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
        g_tftpd_sessions[i].sock = -1;
        g_tftpd_sessions[i].fp   = NULL;
    }

    g_tftpd_listen_sock = -1;
    g_tftpd_num_read    = 0;
    g_tftpd_num_write   = 0;

    /* 3. File-system mutex */
    g_tftpd_fs_mutex = xSemaphoreCreateMutex();
    if (g_tftpd_fs_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed — aborting init");
        return;
    }

    /* 4. Spawn the TFTPDT task
     *    Stack: 8192 words ≈ 32 KiB (matches original design).
     *    Priority 5 is reasonable for a network I/O task on ESP-IDF.
     */
    BaseType_t rv = xTaskCreate(tftpd_main_task, "TFTPDT",
                                8192, NULL, 5, NULL);
    if (rv != pdPASS)
        ESP_LOGE(TAG, "xTaskCreate(TFTPDT) failed");
    else
        ESP_LOGI(TAG, "TFTPDT task created — listening on port %u",
                 (unsigned)g_tftpd_cfg.port);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * tftpd_main_task
 *
 * select()-based main loop:
 *   - Multiplexes the listen socket and all active session sockets.
 *   - Wall-clock 1-second tick (via esp_timer_get_time) drives retransmit /
 *     dally countdowns via tftpd_handle_timer().  The tick fires every second
 *     regardless of whether select() returned a timeout or active socket(s),
 *     which was the root cause of the "WRQ hangs for many seconds" bug.
 * ═══════════════════════════════════════════════════════════════════════════*/
static void tftpd_main_task(void *param)
{
    fd_set         rdset;
    struct timeval tv;
    int            maxfd, nready, i;

    /*
     * Wall-clock reference for the 1-second timer tick.
     * esp_timer_get_time() returns monotonic microseconds since boot;
     * it is not affected by SNTP or settimeofday().
     */
    int64_t last_timer_us = esp_timer_get_time();

    ESP_LOGI(TAG, "TFTPDT started, opening listen socket on port %u",
             (unsigned)g_tftpd_cfg.port);

    if (tftpd_open_listen(g_tftpd_cfg.port) < 0) {
        ESP_LOGE(TAG, "tftpd_open_listen failed — task exiting");
        goto task_exit;
    }

    while (1) {

        /* ── Build fd_set from all currently open sockets ─────────────── */
        FD_ZERO(&rdset);
        maxfd = -1;

        if (g_tftpd_listen_sock >= 0) {
            FD_SET(g_tftpd_listen_sock, &rdset);
            maxfd = g_tftpd_listen_sock;
        }

        for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
            if (g_tftpd_sessions[i].state != TFTPD_STATE_FREE &&
                g_tftpd_sessions[i].sock  >= 0) {
                FD_SET(g_tftpd_sessions[i].sock, &rdset);
                if (g_tftpd_sessions[i].sock > maxfd)
                    maxfd = g_tftpd_sessions[i].sock;
            }
        }

        if (maxfd < 0) {
            /* No open sockets — back-off, run the timer, try to reopen */
            ESP_LOGW(TAG, "No open sockets; retrying listen open in 1 s");
            vTaskDelay(pdMS_TO_TICKS(1000));
            tftpd_handle_timer();
            last_timer_us = esp_timer_get_time();
            if (g_tftpd_listen_sock < 0)
                tftpd_open_listen(g_tftpd_cfg.port);
            continue;
        }

        /*
         * Cap the select() timeout at the remaining time until the next
         * 1-second timer tick so we never overshoot by more than one
         * scheduler quantum.
         */
        {
            int64_t now_us    = esp_timer_get_time();
            int64_t remain_us = 1000000LL - (now_us - last_timer_us);
            if (remain_us <= 0) remain_us = 1;   /* don't block if overdue */
            tv.tv_sec  = (long)(remain_us / 1000000LL);
            tv.tv_usec = (long)(remain_us % 1000000LL);
        }

        nready = select(maxfd + 1, &rdset, NULL, NULL, &tv);

        /*
         * ── Wall-clock 1-second timer tick ──────────────────────────────
         *
         * FIX: call tftpd_handle_timer() based on elapsed real time, NOT
         * only when select() returns 0.  When packets arrive continuously
         * (active WRQ, client retransmitting in DALLY, etc.) select() never
         * times out, so the old code starved the timer: DALLY never expired,
         * g_tftpd_num_write stayed at 1, and new sessions were blocked until
         * the client's own timeout (potentially 9 s) caused it to give up.
         */
        {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_timer_us >= 1000000LL) {
                tftpd_handle_timer();
                last_timer_us = now_us;
            }
        }

        if (nready < 0) {
            if (errno != EINTR)   /* EINTR is benign; all others are real */
                ESP_LOGE(TAG, "select() error: %s", strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (nready == 0)
            continue;   /* pure timeout — timer already handled above */

        /* ── Listen socket: new RRQ or WRQ ─────────────────────────────── */
        if (g_tftpd_listen_sock >= 0 &&
            FD_ISSET(g_tftpd_listen_sock, &rdset)) {
            tftpd_handle_listen_pkt();
        }

        /* ── Session sockets: ACK / DATA / ERROR from client ───────────── */
        for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
            if (g_tftpd_sessions[i].state != TFTPD_STATE_FREE &&
                g_tftpd_sessions[i].sock  >= 0 &&
                FD_ISSET(g_tftpd_sessions[i].sock, &rdset)) {
                tftpd_handle_session_pkt(i);
            }
        }
    }

task_exit:
    tftpd_close_all_sessions();
    tftpd_close_listen();
    vTaskDelete(NULL);
}
