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
 */

#include <errno.h>      /* errno, EINTR                                      */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <sys/select.h>
#include <sys/socket.h>

#include "esp_log.h"

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
 *   - 1-second timeout drives retransmit / dally countdowns via
 *     tftpd_handle_timer() (replaces the hardware periodic timer from the
 *     original BDCOM design).
 * ═══════════════════════════════════════════════════════════════════════════*/
static void tftpd_main_task(void *param)
{
    fd_set         rdset;
    struct timeval tv;
    int            maxfd, nready, i;

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
            if (g_tftpd_listen_sock < 0)
                tftpd_open_listen(g_tftpd_cfg.port);
            continue;
        }

        /* ── 1-second timeout drives retransmit / dally timers ─────────── */
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        nready = select(maxfd + 1, &rdset, NULL, NULL, &tv);

        if (nready < 0) {
            if (errno != EINTR)   /* EINTR is benign; all others are real */
                ESP_LOGE(TAG, "select() error: %s", strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ── Timeout ────────────────────────────────────────────────────── */
        if (nready == 0) {
            tftpd_handle_timer();
            continue;
        }

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
