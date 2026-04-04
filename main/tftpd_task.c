#include <string.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <sys/select.h>
#include <sys/socket.h>
#include "esp_log.h"

#include "tftpd.h"

tftpd_config_t g_tftpd_cfg;
tftpd_session_t g_tftpd_sessions[TFTPD_MAX_SESSIONS];
int g_tftpd_listen_sock = -1;
uint8_t g_tftpd_num_read = 0;
uint8_t g_tftpd_num_write = 0;

static const char *TAG = "tftpd_task";
SemaphoreHandle_t g_tftpd_fs_mutex;

void tftpd_init(void);
void tftpd_main_task(void *param);

void tftpd_init(void) {
    memset(&g_tftpd_cfg, 0, sizeof(g_tftpd_cfg));
    g_tftpd_cfg.enabled = 1;
    g_tftpd_cfg.port = TFTPD_DEFAULT_PORT;
    g_tftpd_cfg.timeout = TFTPD_DEFAULT_TIMEOUT;
    g_tftpd_cfg.retry = TFTPD_DEFAULT_RETRY;

    memset(g_tftpd_sessions, 0, sizeof(g_tftpd_sessions));
    for (int i = 0; i < TFTPD_MAX_SESSIONS; i++)
        g_tftpd_sessions[i].sock = -1;

    g_tftpd_listen_sock = -1;
    g_tftpd_num_read = 0;
    g_tftpd_num_write = 0;

    g_tftpd_fs_mutex = xSemaphoreCreateMutex();
    if (g_tftpd_fs_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed — aborting init");
        return;
    }

    BaseType_t rv = xTaskCreate(tftpd_main_task, "TFTPDT", 8192, NULL, 5, NULL);

    if (rv != pdPASS)
        ESP_LOGE(TAG, "xTaskCreate(TFTPDT) failed");
    else
        ESP_LOGI(TAG, "TFTPDT task created — TFTP server starting on port %u", (unsigned)g_tftpd_cfg.port);
}

void tftpd_main_task(void *param)
{
    fd_set rdset;
    struct timeval tv;
    int maxfd, ret_val, i;

    ESP_LOGI(TAG, "tftpd server starting at port %d\n", g_tftpd_cfg.port);

    ret_val = tftpd_open_listen(g_tftpd_cfg.port);
    if (ret_val < 0)
    {
        ESP_LOGE(TAG, "tftpd_open_listen failed\n");
        goto clean_up;
    }

    while (1)
    {
        FD_ZERO(&rdset);
        maxfd = -1;

        if (g_tftpd_listen_sock >= 0) {
            FD_SET(g_tftpd_listen_sock, &rdset);
            maxfd = g_tftpd_listen_sock;
        }

        for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
            if (g_tftpd_sessions[i].state != TFTPD_STATE_FREE && g_tftpd_sessions[i].sock  >= 0) {
                FD_SET(g_tftpd_sessions[i].sock, &rdset);
                if (g_tftpd_sessions[i].sock > maxfd)
                    maxfd = g_tftpd_sessions[i].sock;
            }
        }

        if (maxfd < 0) {
            ESP_LOGW(TAG, "No open sockets; retrying listen open in 1 s");
            vTaskDelay(pdMS_TO_TICKS(1000));
            tftpd_handle_timer();
            if (g_tftpd_listen_sock < 0)
                tftpd_open_listen(g_tftpd_cfg.port);
            continue;
        }

        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        ret_val = select(maxfd + 1, &rdset, NULL, NULL, &tv);

        if (ret_val < 0) {
            if (errno != EINTR)
                ESP_LOGE(TAG, "select() error: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (ret_val == 0) {
            tftpd_handle_timer();
            continue;
        }

        if (g_tftpd_listen_sock >= 0 &&
                FD_ISSET(g_tftpd_listen_sock, &rdset)) {
            tftpd_handle_listen_pkt();
        }

        for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
            if (g_tftpd_sessions[i].state != TFTPD_STATE_FREE && g_tftpd_sessions[i].sock  >= 0 && FD_ISSET(g_tftpd_sessions[i].sock, &rdset)) {
                tftpd_handle_session_pkt(i);
            }
        }
    }

    clean_up:
    vTaskDelete(NULL);

}

