/*
 * tftpd_task.c  —  TFTP Server: Task Entry and Initialization
 *                   ESP-IDF / FreeRTOS
 *
 * Ported from the BDCOM/VxWorks version.  All BDCOM platform APIs
 * (taskSpawn, sys_msgq_*, sys_add_timer, TIMER_MSG_METHOD, socket_register,
 * syslog) replaced with FreeRTOS / ESP-IDF equivalents.
 *
 * PORT-A  taskSpawn → xTaskCreate.
 * PORT-B  sys_msgq_create / sys_msgq_receive / TIMER_MSG_METHOD removed.
 *         Replaced with a select()-based main loop and an
 *         esp_timer_get_time() wall-clock tick for the 1-second timer.
 * PORT-C  socket_register() removed — not present in lwIP / ESP-IDF.
 *         The select() fd_set covers all open sockets instead.
 * PORT-D  syslog() → ESP_LOG* macros.
 * PORT-E  sys_add_timer / sys_start_timer removed.  The repeating 1-second
 *         tick is emulated by comparing esp_timer_get_time() against a
 *         stored baseline in each select() iteration.
 * PORT-F  tftpd_cfg.enabled defaults to 1 (auto-start); in the BDCOM
 *         version it started disabled and was enabled via CLI.
 * PORT-G  Show-running / CLI / version registration removed; not applicable
 *         to ESP-IDF.  Use the ESP-IDF console component if a CLI is needed.
 *
 * Timer-starvation fix (carried over from previous ESP-IDF revision):
 *   tftpd_handle_timer() is called based on wall-clock elapsed time, not
 *   only on select() timeout.  When DATA packets arrive continuously during
 *   a WRQ, select() never times out, so the old code never called the timer,
 *   DALLY never expired, and g_tftpd_num_write stayed at 1 forever.
 */

#include <errno.h>
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
tftpd_config_t    g_tftpd_cfg;
tftpd_session_t   g_tftpd_sessions[TFTPD_MAX_SESSIONS];
int               g_tftpd_listen_sock = -1;
uint8_t           g_tftpd_num_read    = 0;
uint8_t           g_tftpd_num_write   = 0;
SemaphoreHandle_t g_tftpd_fs_mutex    = NULL;

static void tftpd_main_task(void *param);

/* ═══════════════════════════════════════════════════════════════════════════
 * tftpd_init
 * ═══════════════════════════════════════════════════════════════════════════*/
void tftpd_init(void)
{
    int i;

    /* 1. Configuration defaults */
    memset(&g_tftpd_cfg, 0, sizeof(g_tftpd_cfg));
    g_tftpd_cfg.enabled = 1;                /* PORT-F: auto-start             */
    g_tftpd_cfg.port    = TFTPD_DEFAULT_PORT;
    g_tftpd_cfg.timeout = TFTPD_DEFAULT_TIMEOUT;
    g_tftpd_cfg.retry   = TFTPD_DEFAULT_RETRY;

    /* 2. Session array */
    memset(g_tftpd_sessions, 0, sizeof(g_tftpd_sessions));
    for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
        g_tftpd_sessions[i].sock      = -1;
        g_tftpd_sessions[i].fp        = NULL;
        g_tftpd_sessions[i].write_buf = NULL;
    }

    g_tftpd_listen_sock = -1;
    g_tftpd_num_read    = 0;
    g_tftpd_num_write   = 0;

    /* 3. LittleFS mutex (PORT-8 in tftpd.h) */
    g_tftpd_fs_mutex = xSemaphoreCreateMutex();
    if (g_tftpd_fs_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed — aborting init");
        return;
    }

    /*
     * 4. Spawn the TFTPDT task.                                           PORT-A
     *
     * Stack: 8192 words (~32 KB).  Priority 5 suits a network I/O task.
     * No message queue or timer handle needed — the task uses select() and
     * esp_timer_get_time() internally.                                    PORT-B
     */
    BaseType_t rv = xTaskCreate(tftpd_main_task, "TFTPDT",
                                8192, NULL, 5, NULL);
    if (rv != pdPASS)
        ESP_LOGE(TAG, "xTaskCreate(TFTPDT) failed");
    else
        ESP_LOGI(TAG, "TFTPDT task created — port %u  blksize %u  winsz %u",
                 (unsigned)g_tftpd_cfg.port,
                 (unsigned)TFTPD_DEFAULT_BLKSIZE,
                 (unsigned)TFTPD_DEFAULT_WINSZ);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * tftpd_main_task
 *
 * Replaces tftpd_main_process() from the BDCOM version.            PORT-B/C
 *
 * Architecture difference:
 *   BDCOM: message-queue driven.  A socket_register() callback posts a
 *          message to the queue; a TIMER_MSG_METHOD timer posts tick messages.
 *          tftpd_main_process() blocks on sys_msgq_receive() and switches
 *          on msg_buf[3].
 *
 *   ESP-IDF: select()-based.  All open sockets are multiplexed through a
 *            single select() call with a sub-second timeout.  The 1-second
 *            timer tick is emulated with esp_timer_get_time() so it fires
 *            exactly once per second regardless of socket activity — this
 *            fixes the DALLY-starvation bug that existed when the tick was
 *            tied only to the select() timeout.
 * ═══════════════════════════════════════════════════════════════════════════*/
static void tftpd_main_task(void *param)
{
    fd_set         rdset;
    struct timeval tv;
    int            maxfd, nready, i;

    /*
     * Wall-clock baseline for the 1-second timer tick.                  PORT-E
     * esp_timer_get_time() is monotonic and unaffected by SNTP/settimeofday.
     */
    int64_t last_timer_us = esp_timer_get_time();

    ESP_LOGI(TAG, "TFTPDT started — opening listen socket on port %u",
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
            /*
             * No open sockets — delayed retry.
             * Still run the timer so DALLY sessions from a previous pass
             * get cleaned up even if the listen socket disappeared.
             */
            ESP_LOGW(TAG, "No open sockets; retrying in 1 s");
            vTaskDelay(pdMS_TO_TICKS(1000));
            tftpd_handle_timer();
            last_timer_us = esp_timer_get_time();
            if (g_tftpd_listen_sock < 0)
                tftpd_open_listen(g_tftpd_cfg.port);
            continue;
        }

        /*
         * Cap select() timeout to the time remaining until the next 1-second
         * tick, so we never overshoot by more than one scheduler quantum.
         */
        {
            int64_t now_us    = esp_timer_get_time();
            int64_t remain_us = 1000000LL - (now_us - last_timer_us);
            if (remain_us <= 0) remain_us = 1;
            tv.tv_sec  = (long)(remain_us / 1000000LL);
            tv.tv_usec = (long)(remain_us % 1000000LL);
        }

        nready = select(maxfd + 1, &rdset, NULL, NULL, &tv);

        /*
         * ── Wall-clock 1-second timer tick ──────────────────────────────
         *
         * Check elapsed time after every select() return, not only on
         * timeout.  During an active WRQ the client sends DATA packets
         * continuously and select() never times out — checking only on
         * timeout would starve the DALLY countdown and leave
         * g_tftpd_num_write permanently at 1.
         */
        {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_timer_us >= 1000000LL) {
                tftpd_handle_timer();
                last_timer_us = now_us;
            }
        }

        if (nready < 0) {
            if (errno != EINTR)
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
