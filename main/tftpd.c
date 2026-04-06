/*
 * tftpd.c  —  TFTP Server Protocol Engine  (ESP-IDF / FreeRTOS)
 *
 * Changes from the previous version (see inline FIX/NOTE comments):
 *
 *  FIX-1   tftpd_open_listen: format specifier was %d for strerror() (char*).
 *  FIX-2   tftpd_open_session_sock: missing log on bind failure; was non-static.
 *  FIX-3   tftpd_build_oack: sprintf → snprintf with remaining-space tracking.
 *  FIX-4   tftpd_send_window: fopen/fclose per window call → persistent FILE*
 *              in session; fixes flash wear and latency on ESP32/LittleFS.
 *  FIX-5   All fopen modes: "r" → "rb", "w" → "wb", "r+" → "r+b" (binary).
 *  FIX-6   sendto / fwrite return values checked against expected size, not
 *              just checked for < 0.
 *  FIX-7   Packet-size guard before struct cast: ACK and DATA require >= 4 B;
 *              only checking len >= 2 before was undefined behaviour.
 *  FIX-8   OACK_WRQ → WRQ fallthrough removed; factored into a static helper
 *              tftpd_process_wrq_data() called explicitly from both cases.
 *  FIX-9   tftpd_make_path: rejects any filename containing ".." to block
 *              path-traversal attacks.
 *  FIX-10  tftpd_close_session: closes the persistent FILE* if still open.
 *  FIX-11  Removed duplicate forward declaration of tftpd_send_error_to.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>     /* atoi                */
#include <string.h>
#include <unistd.h>     /* close()             */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "tftpd.h"

static const char *TAG = "tftpd";

/* ── Forward declarations (internal linkage only) ───────────────────────── */
static int  tftpd_alloc_session(int is_write);
static int  tftpd_open_session_sock(void);                      /* FIX-2 */
static int  tftpd_build_oack(char *buf, size_t buf_size,        /* FIX-3 */
                              uint16_t blksize, uint8_t winsz);
static void tftpd_send_error(int idx, uint16_t code, const char *msg);
static void tftpd_send_error_to(int sock, struct sockaddr_in *to,
                                 uint16_t code, const char *msg);
static int  tftpd_send_ack(int idx, uint16_t block);
static int  tftpd_parse_request(const char *buf, int len,
                                 char *filename, uint16_t *blksize,
                                 uint8_t *winsz, int *has_opts);
static int  tftpd_make_path(const char *filename,               /* FIX-9 */
                             char *out, size_t out_size);
static int  tftpd_send_window(int idx);
static void tftpd_process_wrq_data(int idx,                     /* FIX-8 */
                                    const char *buf, ssize_t len);

/* ═══════════════════════════════════════════════════════════════════════════
 * RESOURCE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

int tftpd_open_listen(uint16_t port)
{
    struct sockaddr_in addr;
    int sock, opt;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %s", strerror(errno));  /* FIX-1 */
        return -1;
    }

    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() on port %u failed: %s",           /* FIX-1 */
                 (unsigned)port, strerror(errno));
        close(sock);
        return -1;
    }

    g_tftpd_listen_sock = sock;
    ESP_LOGI(TAG, "Listen socket open on UDP port %u (fd=%d)", (unsigned)port, sock);
    return 0;
}

void tftpd_close_listen(void)
{
    if (g_tftpd_listen_sock >= 0) {
        close(g_tftpd_listen_sock);
        g_tftpd_listen_sock = -1;
    }
}

/*
 * tftpd_open_session_sock  —  ephemeral UDP socket for one session.
 *
 * FIX-2: added bind-failure log; changed to static (no external caller).
 */
static int tftpd_open_session_sock(void)
{
    struct sockaddr_in addr;
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "session socket() failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;   /* lwIP assigns the ephemeral TID */

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "session bind() failed: %s", strerror(errno)); /* FIX-2 */
        close(sock);
        return -1;
    }

    return sock;
}

static int tftpd_alloc_session(int is_write)
{
    int i;

    if (is_write) {
        if (g_tftpd_num_read > 0 || g_tftpd_num_write > 0)
            return -1;
    } else {
        if (g_tftpd_num_write > 0)
            return -1;
        if (g_tftpd_num_read >= TFTPD_MAX_SESSIONS)
            return -1;
    }

    for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
        if (g_tftpd_sessions[i].state == TFTPD_STATE_FREE)
            return i;
    }

    return -1;
}

/*
 * tftpd_close_session  —  tear down a session and release all resources.
 *
 * FIX-10: closes the persistent FILE* if it is still open.
 */
void tftpd_close_session(int idx)
{
    tftpd_session_t *s = &g_tftpd_sessions[idx];

    if (s->state == TFTPD_STATE_FREE)
        return;

    /*
     * Decrement the correct direction counter using the is_write flag.
     *
     * BUG THAT WAS HERE: the old code used state to decide the counter:
     *   if (state == RRQ || state == OACK_RRQ || state == DALLY) → read--
     *   else → write--
     *
     * DALLY is only ever entered from WRQ (tftpd_process_wrq_data is the
     * sole place that sets it), so DALLY sessions are write sessions.
     * Counting DALLY as a read caused g_tftpd_num_read to underflow to 255
     * (uint8_t wrap) and left g_tftpd_num_write permanently at 1, blocking
     * every subsequent RRQ with "Max concurrent read sessions reached".
     *
     * FIX: use s->is_write, which is set once at session creation and never
     * changes, regardless of which DALLY / intermediate state is active.
     */
    if (!s->is_write) {
        if (g_tftpd_num_read > 0)
            g_tftpd_num_read--;
    } else {
        if (g_tftpd_num_write > 0)
            g_tftpd_num_write--;
    }

    /*
     * FIX-10 / BUG FIX: close the persistent FILE* under the FS mutex.
     *
     * The original code called fclose() without holding g_tftpd_fs_mutex.
     * fclose() flushes buffered data and updates LittleFS internal metadata,
     * which is NOT thread-safe if another task is concurrently calling fopen,
     * fread, or fwrite on the same LittleFS volume.  Always take the mutex
     * before touching LittleFS state — even for teardown paths.
     *
     * If the mutex cannot be acquired within 500 ms (which should never
     * happen in normal operation), fall back to an unprotected fclose so
     * the file handle is not leaked.
     */
    if (s->fp != NULL) {
        if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            fclose(s->fp);
            xSemaphoreGive(g_tftpd_fs_mutex);
        } else {
            ESP_LOGW("tftpd", "close_session: mutex timeout — fclose without lock");
            fclose(s->fp);
        }
        s->fp = NULL;
    }

    if (s->sock >= 0) {
        close(s->sock);
        s->sock = -1;
    }

    memset(s, 0, sizeof(*s));
    s->state = TFTPD_STATE_FREE;
    s->sock  = -1;
    /* s->fp is already NULL after memset */
}

void tftpd_close_all_sessions(void)
{
    int i;
    for (i = 0; i < TFTPD_MAX_SESSIONS; i++)
        tftpd_close_session(i);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PACKET HELPERS
 * ═══════════════════════════════════════════════════════════════════════════*/

/*
 * tftpd_make_path  —  build the full VFS path for a client filename.
 *
 * FIX-9: reject any filename that contains ".." (covers both leading and
 * embedded traversal attempts such as "a/../../etc/passwd").
 *
 * Returns 0 on success, -1 if the filename is rejected or the buffer is
 * too small.
 */
static int tftpd_make_path(const char *filename, char *out, size_t out_size)
{
    /* Strip leading slashes */
    while (*filename == '/')
        filename++;

    /* Reject any path that still contains ".." after stripping */
    if (strstr(filename, "..") != NULL) {
        ESP_LOGW(TAG, "tftpd_make_path: rejected path-traversal attempt: %s",
                 filename);
        return -1;
    }

    int n = snprintf(out, out_size, TFTPD_ROOT_PATH "/%s", filename);
    if (n < 0 || (size_t)n >= out_size) {
        ESP_LOGE(TAG, "tftpd_make_path: path too long");
        return -1;
    }

    return 0;
}

/*
 * tftpd_build_oack  —  build an OACK packet into a caller-supplied buffer.
 *
 * FIX-3: was using sprintf() with no bounds checking; now uses snprintf()
 * with a running "remaining space" guard.  Signature gains buf_size.
 *
 * Returns total packet length on success, -1 if the buffer is too small.
 */
static int tftpd_build_oack(char *buf, size_t buf_size,
                             uint16_t blksize, uint8_t winsz)
{
    tftp_oack_t *pkt = (tftp_oack_t *)buf;
    char        *p   = (char *)pkt->opts;
    char        *end = buf + buf_size;
    int          n;

    if (buf_size < sizeof(tftp_oack_t) + 4)    /* sanity: room for anything */
        return -1;

    pkt->opcode = htons(TFTP_OP_OACK);

#define OACK_APPEND(fmt, ...) \
    do { \
        n = snprintf(p, (size_t)(end - p), fmt, ##__VA_ARGS__); \
        if (n < 0 || p + n + 1 >= end) return -1; \
        p += n + 1; /* include the NUL terminator */ \
    } while (0)

    OACK_APPEND("blksize");
    OACK_APPEND("%u", (unsigned)blksize);

    if (winsz > 1) {
        OACK_APPEND("windowsize");
        OACK_APPEND("%u", (unsigned)winsz);
    }

#undef OACK_APPEND

    return (int)(p - buf);
}

/*
 * tftpd_send_error_to  —  fire-and-forget ERROR to an arbitrary address.
 *
 * FIX-6: check sendto return value and log on failure.
 */
static void tftpd_send_error_to(int sock, struct sockaddr_in *to,
                                 uint16_t code, const char *msg)
{
    char          buf[80];
    tftp_error_t *pkt  = (tftp_error_t *)buf;
    int           mlen = msg ? (int)strlen(msg) : 0;
    ssize_t       pkt_len;
    ssize_t       sent;

    /* Clamp message so it fits in buf */
    int max_mlen = (int)(sizeof(buf) - sizeof(tftp_error_t) - 1);
    if (mlen > max_mlen)
        mlen = max_mlen;

    pkt->opcode   = htons(TFTP_OP_ERROR);
    pkt->err_code = htons(code);
    if (mlen > 0)
        memcpy(pkt->msg, msg, mlen);
    pkt->msg[mlen] = '\0';

    pkt_len = (ssize_t)(sizeof(tftp_error_t) + mlen + 1);
    sent = sendto(sock, buf, (size_t)pkt_len, 0,
                  (struct sockaddr *)to, sizeof(*to));
    if (sent != pkt_len)                                        /* FIX-6 */
        ESP_LOGW(TAG, "send_error_to: sendto sent=%zd expected=%zd errno=%d",
                 sent, pkt_len, errno);
}

static void tftpd_send_error(int idx, uint16_t code, const char *msg)
{
    tftpd_session_t   *s = &g_tftpd_sessions[idx];
    struct sockaddr_in to;

    if (s->sock < 0)
        return;

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    tftpd_send_error_to(s->sock, &to, code, msg);
    tftpd_close_session(idx);
}

/*
 * tftpd_send_ack  —  send ACK(block) to a session's client.
 *
 * FIX-6: validate full datagram was sent.
 * Returns 0 on success, -1 on error.
 */
static int tftpd_send_ack(int idx, uint16_t block)
{
    tftpd_session_t   *s = &g_tftpd_sessions[idx];
    tftp_ack_t         pkt;
    struct sockaddr_in to;
    ssize_t            sent;

    pkt.opcode    = htons(TFTP_OP_ACK);
    pkt.block_num = htons(block);

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    sent = sendto(s->sock, &pkt, sizeof(pkt), 0,
                  (struct sockaddr *)&to, sizeof(to));
    if (sent != (ssize_t)sizeof(pkt)) {                         /* FIX-6 */
        ESP_LOGW(TAG, "send_ack: sendto sent=%zd expected=%zu errno=%d",
                 sent, sizeof(pkt), errno);
        return -1;
    }
    return 0;
}

/*
 * tftpd_parse_request  —  parse RRQ / WRQ; extract filename and options.
 *
 * Returns 0 on success, -1 on malformed packet.
 */
static int tftpd_parse_request(const char *buf, int len,
                                char *filename, uint16_t *blksize,
                                uint8_t *winsz, int *has_opts)
{
    const char *p         = buf + 2;    /* skip opcode */
    int         remaining = len - 2;
    size_t      field_len;

    *blksize  = TFTPD_DEFAULT_BLKSIZE;
    *winsz    = TFTPD_DEFAULT_WINSZ;
    *has_opts = 0;

    if (remaining <= 0)
        return -1;

    /* Filename */
    field_len = strnlen(p, remaining);
    if (field_len == (size_t)remaining || field_len == 0 ||
        field_len >= TFTPD_MAX_FILENAME)
        return -1;

    memcpy(filename, p, field_len);
    filename[field_len] = '\0';
    p         += field_len + 1;
    remaining -= (int)(field_len + 1);

    /* Mode string (skip) */
    field_len = strnlen(p, remaining);
    if (field_len == (size_t)remaining)
        return -1;
    p         += field_len + 1;
    remaining -= (int)(field_len + 1);

    /* Option–value pairs */
    while (remaining > 0) {
        const char    *opt_str, *val_str;
        unsigned long  val;

        field_len = strnlen(p, remaining);
        if (field_len == (size_t)remaining) break;
        opt_str = p;
        p         += field_len + 1;
        remaining -= (int)(field_len + 1);

        if (remaining <= 0) break;

        field_len = strnlen(p, remaining);
        if (field_len == (size_t)remaining) break;
        val_str = p;
        p         += field_len + 1;
        remaining -= (int)(field_len + 1);

        val = (unsigned long)atoi(val_str);

        if (strcasecmp(opt_str, "blksize") == 0) {
            if (val >= 8) {
                *blksize  = (uint16_t)(val > TFTPD_MAX_BLKSIZE
                                       ? TFTPD_MAX_BLKSIZE : val);
                *has_opts = 1;
            }
        } else if (strcasecmp(opt_str, "windowsize") == 0) {
            if (val >= 1) {
                *winsz    = (uint8_t)(val > TFTPD_MAX_WINSZ
                                      ? TFTPD_MAX_WINSZ : val);
                *has_opts = 1;
            }
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FILE I/O HELPERS
 * ═══════════════════════════════════════════════════════════════════════════*/

/*
 * tftpd_send_window  —  send up to windowsize DATA blocks for an RRQ session.
 *
 * FIX-4 / FIX-5:
 *   - The file is opened once (stored in s->fp) and reused on every call,
 *     including retransmits.  fopen() is only called when s->fp == NULL.
 *   - File mode changed to "rb" (binary read).
 *
 * FIX-6: sendto return value validated against expected datagram size.
 *
 * Returns 0 on success, -1 on error (session closed before returning).
 */
static int tftpd_send_window(int idx)
{
    tftpd_session_t   *s   = &g_tftpd_sessions[idx];
    char               pkt_buf[TFTPD_MAX_PKT_BUF];
    tftp_data_t       *pkt = (tftp_data_t *)pkt_buf;
    struct sockaddr_in to;
    char               fpath[TFTPD_MAX_FILENAME + sizeof(TFTPD_ROOT_PATH) + 2];
    int                i, read_len;
    ssize_t            sent;
    ssize_t            expected;

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    /* Acquire FS mutex before touching s->fp */
    if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        tftpd_send_error(idx, TFTP_ERR_ACCESS,
                         "File system temporarily unavailable");
        return -1;
    }

    /* FIX-4: open the file only on the first call */
    if (s->fp == NULL) {
        if (tftpd_make_path(s->filename, fpath, sizeof(fpath)) != 0) { /* FIX-9 */
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_ACCESS, "Invalid filename");
            return -1;
        }
        s->fp = fopen(fpath, "rb");                             /* FIX-5 */
        if (s->fp == NULL) {
            ESP_LOGE(TAG, "fopen(%s, rb) failed: %s", fpath, strerror(errno));
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_NOTFOUND, "File not found");
            return -1;
        }
    }

    /* Always seek to the window-start position (handles retransmits too) */
    if (fseek(s->fp, (long)s->file_pos_win, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek failed: %s", strerror(errno));
        xSemaphoreGive(g_tftpd_fs_mutex);
        tftpd_send_error(idx, TFTP_ERR_UNDEF, "File seek error");
        return -1;
    }

    s->bytes_this_win = 0;
    s->is_last        = 0;

    for (i = 0; i < (int)s->windowsize && !s->is_last; i++) {
        uint16_t block = (uint16_t)(s->window_start + i);

        pkt->opcode    = htons(TFTP_OP_DATA);
        pkt->block_num = htons(block);

        read_len = (int)fread(pkt->data, 1, s->blksize, s->fp);
        if (ferror(s->fp)) {
            ESP_LOGE(TAG, "fread error: %s", strerror(errno));
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "File read error");
            return -1;
        }

        s->bytes_this_win += (uint32_t)read_len;
        if (read_len < (int)s->blksize)
            s->is_last = 1;

        expected = (ssize_t)(sizeof(tftp_data_t) + read_len);
        sent = sendto(s->sock, pkt_buf, (size_t)expected, 0,
                      (struct sockaddr *)&to, sizeof(to));
        if (sent != expected) {                                 /* FIX-6 */
            ESP_LOGE(TAG, "session[%d] sendto sent=%zd expected=%zd errno=%d",
                     idx, sent, expected, errno);
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "Network send error");
            return -1;
        }

        s->last_sent = block;
    }

    xSemaphoreGive(g_tftpd_fs_mutex);

    /* Arm the retransmit timer */
    s->timeout_sec  = g_tftpd_cfg.timeout;
    s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

    return 0;
}

/*
 * tftpd_process_wrq_data  —  write one DATA block to the receive file.
 *
 * FIX-8: Extracted from tftpd_handle_session_pkt to eliminate the
 * OACK_WRQ → WRQ implicit fallthrough.  Both states call this function
 * explicitly after their own guard checks pass.
 *
 * FIX-4 / FIX-5: FILE* kept open in s->fp for sequential writes;
 * opened once in "wb" mode on block 1, then written sequentially.
 * fseek is used only on out-of-order/retransmit scenarios (detected by
 * the duplicate-block check before this function is called).
 *
 * FIX-6: fwrite return value checked against expected write length.
 */
static void tftpd_process_wrq_data(int idx, const char *buf, ssize_t len)
{
    tftpd_session_t *s        = &g_tftpd_sessions[idx];
    char             fpath[TFTPD_MAX_FILENAME + sizeof(TFTPD_ROOT_PATH) + 2];
    uint16_t         block;
    int              write_len;

    /* FIX-7: minimum packet size for DATA is sizeof(tftp_data_t) = 4 bytes */
    if (len < (ssize_t)sizeof(tftp_data_t)) {
        tftpd_send_error(idx, TFTP_ERR_ILLEGAL, "DATA packet too short");
        return;
    }

    block     = ntohs(((const tftp_data_t *)buf)->block_num);
    write_len = (int)len - (int)sizeof(tftp_data_t);
    if (write_len < 0)
        write_len = 0;

    /* Duplicate: client is retransmitting a block we already ACKed */
    if (block == (uint16_t)(s->expected_block - 1)) {
        tftpd_send_ack(idx, block);
        return;
    }

    /* Out-of-sequence: silently ignore */
    if (block != s->expected_block)
        return;

    /* Acquire FS mutex before touching s->fp */
    if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        tftpd_send_error(idx, TFTP_ERR_ACCESS,
                         "File system temporarily unavailable");
        return;
    }

    /* FIX-4: open the file only on the very first block */
    if (s->fp == NULL) {
        if (tftpd_make_path(s->filename, fpath, sizeof(fpath)) != 0) { /* FIX-9 */
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_ACCESS, "Invalid filename");
            return;
        }
        /* FIX-5: binary write mode */
        s->fp = fopen(fpath, "wb");                             /* FIX-5 */
        if (s->fp == NULL) {
            ESP_LOGE(TAG, "fopen(%s, wb) failed: %s", fpath, strerror(errno));
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_ACCESS, "Cannot open file for write");
            return;
        }
        s->file_created = 1;
    }

    /* Write the data payload (sequential — no fseek needed for normal flow) */
    if (write_len > 0) {
        const uint8_t *data_ptr = (const uint8_t *)(buf + sizeof(tftp_data_t));
        size_t written = fwrite(data_ptr, 1, (size_t)write_len, s->fp);
        if (written != (size_t)write_len) {                     /* FIX-6 */
            ESP_LOGE(TAG, "fwrite: wrote %zu of %d bytes: %s",
                     written, write_len, strerror(errno));
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_DISKFULL, "File write failed");
            return;
        }
        /* Flush to ensure the data reaches the LittleFS layer */
        fflush(s->fp);
    }

    xSemaphoreGive(g_tftpd_fs_mutex);

    s->expected_block++;
    s->timeout_sec  = g_tftpd_cfg.timeout;
    s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

    /*
     * RFC 7440 §3 — windowed WRQ ACK rule:
     *
     * With windowsize == 1 the server ACKs every DATA block (classic RFC 1350
     * behaviour, and `block % 1 == 0` is always true, so no special case needed).
     *
     * With windowsize > 1 the server must ACK only the LAST block of each
     * window, not every individual block.  Sending an ACK mid-window tells
     * the client "you may advance your window by N blocks", causing it to
     * duplicate-send every block inside the window — exactly the bug reported.
     *
     * The window boundary is when `block` is a multiple of `windowsize`.
     * The final block (short DATA, write_len < blksize) always gets an ACK
     * regardless of its position in the window.
     *
     * Edge cases:
     *  - windowsize == 1: `block % 1 == 0` → always true → ACK every block. ✓
     *  - Block number wrap-around (0xFFFF → 0x0000): a block at the wrap
     *    boundary (e.g. block 65535 with winsz 4) may not be a multiple of
     *    winsz, but it will be short if it is the last block of the file, so
     *    is_last_block handles it.  For non-final wrap-around blocks in the
     *    middle of a large file the client will simply wait until the next
     *    window boundary — this is compliant with RFC 7440.
     */
    {
        int is_last_block  = (write_len < (int)s->blksize);
        int is_window_end  = ((block % s->windowsize) == 0);

        if (is_last_block || is_window_end) {
            s->last_ack_sent = block;
            tftpd_send_ack(idx, block);
        }
        /* else: mid-window block — do NOT ACK; wait for window boundary */
    }

    /* Final block: enter dally state (ensures the last ACK is re-sent if lost) */
    if (write_len < (int)s->blksize) {
        ESP_LOGI(TAG, "session[%d] WRQ complete", idx);
        s->state        = TFTPD_STATE_DALLY;
        s->dally_remain = TFTPD_DALLY_TICKS;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EVENT HANDLERS  (called from tftpd_main_task)
 * ═══════════════════════════════════════════════════════════════════════════*/

void tftpd_handle_listen_pkt(void)
{
    tftpd_session_t   *s;
    char               buf[TFTPD_MAX_PKT_BUF + 32];
    char               filename[TFTPD_MAX_FILENAME];
    char               oack_buf[TFTPD_MAX_OACK_BUF];
    struct sockaddr_in cli_addr;
    socklen_t          cli_len = sizeof(cli_addr);
    ssize_t            len;
    uint16_t           opcode;
    uint16_t           blksize;
    uint8_t            winsz;
    uint8_t            is_write;
    int                has_opts;
    int                idx;
    int                sock;
    int                oack_len;
    struct sockaddr_in to;

    len = recvfrom(g_tftpd_listen_sock, buf, sizeof(buf), 0,
                   (struct sockaddr *)&cli_addr, &cli_len);
    if (len < 4)    /* minimum viable RRQ/WRQ is opcode(2)+filename(1)+NUL(1) */
        return;

    if (!g_tftpd_cfg.enabled)
        return;

    /* FIX-7: len >= 4 already guaranteed above before reading uint16_t */
    opcode = ntohs(*(const uint16_t *)buf);

    if (opcode != TFTP_OP_RRQ && opcode != TFTP_OP_WRQ) {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr,
                            TFTP_ERR_ILLEGAL,
                            "Only RRQ/WRQ allowed on this port");
        return;
    }

    is_write = (opcode == TFTP_OP_WRQ) ? 1 : 0;

    if (tftpd_parse_request(buf, (int)len, filename,
                             &blksize, &winsz, &has_opts) != 0) {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr,
                            TFTP_ERR_ILLEGAL, "Malformed request");
        return;
    }

    idx = tftpd_alloc_session(is_write);
    if (idx < 0) {
        const char *reason;
        if (is_write) {
            reason = "Write session already active";
        } else if (g_tftpd_num_write > 0) {
            /* Blocked because a WRQ (possibly in DALLY) is still counted.
             * This is the window between WRQ completion and DALLY expiry. */
            reason = "Write session active — try again shortly";
        } else {
            reason = "Max concurrent read sessions reached";
        }
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr,
                            TFTP_ERR_UNDEF, reason);
        return;
    }

    sock = tftpd_open_session_sock();
    if (sock < 0) {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr,
                            TFTP_ERR_UNDEF, "No socket resources");
        return;
    }

    /* Initialise session slot */
    s = &g_tftpd_sessions[idx];
    memset(s, 0, sizeof(*s));
    s->sock         = sock;
    s->fp           = NULL;
    s->is_write     = is_write;     /* set once; never changes for this session */
    s->client_ip    = cli_addr.sin_addr.s_addr;
    s->client_port  = cli_addr.sin_port;
    s->blksize      = blksize;
    s->windowsize   = winsz;
    s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);
    s->timeout_sec  = g_tftpd_cfg.timeout;
    strncpy(s->filename, filename, TFTPD_MAX_FILENAME - 1);
    s->filename[TFTPD_MAX_FILENAME - 1] = '\0';

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    if (!is_write) {
        /* ── RRQ: server sends file ────────────────────────────────── */
        g_tftpd_num_read++;
        s->window_start = 1;
        s->last_ack     = 0;
        s->file_pos_win = 0;

        if (has_opts) {
            oack_len = tftpd_build_oack(oack_buf, sizeof(oack_buf),
                                         blksize, winsz);            /* FIX-3 */
            if (oack_len > 0) {
                ssize_t sent = sendto(s->sock, oack_buf, (size_t)oack_len,
                                      0, (struct sockaddr *)&to, sizeof(to));
                if (sent != (ssize_t)oack_len)                       /* FIX-6 */
                    ESP_LOGW(TAG, "OACK sendto partial: sent=%zd", sent);
            }
            s->state = TFTPD_STATE_OACK_RRQ;
        } else {
            s->state = TFTPD_STATE_RRQ;
            if (tftpd_send_window(idx) != 0)
                return;
        }

        ESP_LOGI(TAG, "RRQ session[%d] client=%s:%u file=%s blksize=%u winsz=%u",
                 idx, inet_ntoa(cli_addr.sin_addr),
                 (unsigned)ntohs(cli_addr.sin_port),
                 filename, (unsigned)blksize, (unsigned)winsz);

    } else {
        /* ── WRQ: client sends file ────────────────────────────────── */
        g_tftpd_num_write++;
        s->expected_block = 1;
        s->last_ack_sent  = 0;  /* ACK(0) is the implicit start; updated each time we actually ACK */

        if (has_opts) {
            oack_len = tftpd_build_oack(oack_buf, sizeof(oack_buf),
                                         blksize, winsz);            /* FIX-3 */
            if (oack_len > 0) {
                ssize_t sent = sendto(s->sock, oack_buf, (size_t)oack_len,
                                      0, (struct sockaddr *)&to, sizeof(to));
                if (sent != (ssize_t)oack_len)                       /* FIX-6 */
                    ESP_LOGW(TAG, "OACK sendto partial: sent=%zd", sent);
            }
            s->state = TFTPD_STATE_OACK_WRQ;
        } else {
            s->state = TFTPD_STATE_WRQ;
            tftpd_send_ack(idx, 0);     /* ACK(0) invites first DATA block */
        }

        ESP_LOGI(TAG, "WRQ session[%d] client=%s:%u file=%s blksize=%u winsz=%u",
                 idx, inet_ntoa(cli_addr.sin_addr),
                 (unsigned)ntohs(cli_addr.sin_port),
                 filename, (unsigned)blksize, (unsigned)winsz);
    }
}

/*
 * tftpd_handle_session_pkt  —  process one packet on an active session socket.
 *
 * FIX-7: Added minimum-size guards before every struct cast.
 * FIX-8: OACK_WRQ no longer falls through to WRQ; both call
 *         tftpd_process_wrq_data() explicitly.
 */
void tftpd_handle_session_pkt(int idx)
{
    tftpd_session_t   *s = &g_tftpd_sessions[idx];
    char               buf[TFTPD_MAX_PKT_BUF + 32];
    struct sockaddr_in cli_addr;
    socklen_t          from_len = sizeof(cli_addr);
    ssize_t            len;
    uint16_t           opcode, block;

    len = recvfrom(s->sock, buf, sizeof(buf), 0,
                   (struct sockaddr *)&cli_addr, &from_len);

    /* FIX-7: need at least the 2-byte opcode */
    if (len < 2)
        return;

    /* TID validation (RFC 1350 §4) */
    if (cli_addr.sin_addr.s_addr != s->client_ip ||
        cli_addr.sin_port        != s->client_port) {
        tftpd_send_error_to(s->sock, &cli_addr,
                            TFTP_ERR_BADTID, "Unknown transfer ID");
        return;
    }

    opcode = ntohs(*(const uint16_t *)buf);

    if (opcode == TFTP_OP_ERROR) {
        ESP_LOGW(TAG, "session[%d] received ERROR from client — closing", idx);
        tftpd_close_session(idx);
        return;
    }

    /* DALLY: re-send last ACK if client retransmits any block from the final window */
    if (s->state == TFTPD_STATE_DALLY) {
        if (opcode == TFTP_OP_DATA &&
            len >= (ssize_t)sizeof(tftp_data_t)) {              /* FIX-7 */
            block = ntohs(((const tftp_data_t *)buf)->block_num);
            /*
             * Client may retransmit any block in the last window if it did
             * not receive our final ACK.  Re-send last_ack_sent (the final
             * window-boundary / short-block ACK) for any block <= it.
             */
            if (block <= s->last_ack_sent && block >= (uint16_t)(s->last_ack_sent - s->windowsize + 1))
                tftpd_send_ack(idx, s->last_ack_sent);
        }
        return;
    }

    switch (s->state) {

    case TFTPD_STATE_OACK_RRQ:
        if (opcode != TFTP_OP_ACK) break;
        /* FIX-7: ACK packet must be at least 4 bytes */
        if (len < (ssize_t)sizeof(tftp_ack_t)) break;
        block = ntohs(((const tftp_ack_t *)buf)->block_num);
        if (block != 0) break;
        s->state = TFTPD_STATE_RRQ;
        tftpd_send_window(idx);
        break;

    case TFTPD_STATE_RRQ:
        if (opcode != TFTP_OP_ACK) break;
        /* FIX-7 */
        if (len < (ssize_t)sizeof(tftp_ack_t)) break;
        block = ntohs(((const tftp_ack_t *)buf)->block_num);

        /* Ignore stale duplicates from before the current window */
        if (block < (uint16_t)(s->window_start - 1)) break;

        s->timeout_sec  = g_tftpd_cfg.timeout;
        s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

        if (block == s->last_sent) {
            if (s->is_last) {
                ESP_LOGI(TAG, "session[%d] RRQ complete", idx);
                tftpd_close_session(idx);
                return;
            }
            s->last_ack      = block;
            s->file_pos_win += s->bytes_this_win;
            s->window_start  = (uint16_t)(block + 1);
            tftpd_send_window(idx);
        } else if (block >= s->window_start && block < s->last_sent) {
            /* Partial ACK: retransmit from next unACKed block */
            uint16_t acked = (uint16_t)(block - (s->window_start - 1));
            s->file_pos_win += (uint32_t)acked * s->blksize;
            s->window_start  = (uint16_t)(block + 1);
            s->last_ack      = block;
            tftpd_send_window(idx);
        }
        break;

    case TFTPD_STATE_OACK_WRQ:
        /*
         * FIX-8: was an implicit fallthrough into TFTPD_STATE_WRQ.
         * Now explicitly validated here, then delegates to the shared helper.
         */
        if (opcode != TFTP_OP_DATA) break;
        if (len < (ssize_t)sizeof(tftp_data_t)) break;          /* FIX-7 */
        block = ntohs(((const tftp_data_t *)buf)->block_num);
        if (block != 1) break;          /* must be the first block */
        s->state = TFTPD_STATE_WRQ;     /* advance state before processing */
        tftpd_process_wrq_data(idx, buf, len);
        break;

    case TFTPD_STATE_WRQ:
        if (opcode != TFTP_OP_DATA) break;
        tftpd_process_wrq_data(idx, buf, len);
        break;

    default:
        break;
    }
}

void tftpd_handle_timer(void)
{
    int               i;
    tftpd_session_t  *s;
    struct sockaddr_in to;
    char               oack_buf[TFTPD_MAX_OACK_BUF];
    int                oack_len;

    for (i = 0; i < TFTPD_MAX_SESSIONS; i++) {
        s = &g_tftpd_sessions[i];

        if (s->state == TFTPD_STATE_FREE)
            continue;

        if (s->state == TFTPD_STATE_DALLY) {
            if (s->dally_remain > 0)
                s->dally_remain--;
            if (s->dally_remain == 0)
                tftpd_close_session(i);
            continue;
        }

        if (s->timeout_sec > 0)
            s->timeout_sec--;
        if (s->timeout_sec > 0)
            continue;

        /* Timeout expired */
        if (s->retry_remain == 0) {
            ESP_LOGW(TAG, "session[%d] transfer timeout — closing", i);
            tftpd_send_error(i, TFTP_ERR_UNDEF, "Transfer timeout");
            continue;
        }
        s->retry_remain--;
        s->timeout_sec = g_tftpd_cfg.timeout;

        ESP_LOGD(TAG, "session[%d] retransmit (retries_left=%u)",
                 i, (unsigned)s->retry_remain);

        memset(&to, 0, sizeof(to));
        to.sin_family      = AF_INET;
        to.sin_addr.s_addr = s->client_ip;
        to.sin_port        = s->client_port;

        switch (s->state) {
        case TFTPD_STATE_OACK_RRQ:
        case TFTPD_STATE_OACK_WRQ:
            oack_len = tftpd_build_oack(oack_buf, sizeof(oack_buf),    /* FIX-3 */
                                         s->blksize, s->windowsize);
            if (oack_len > 0) {
                ssize_t sent = sendto(s->sock, oack_buf, (size_t)oack_len,
                                      0, (struct sockaddr *)&to, sizeof(to));
                if (sent != (ssize_t)oack_len)                         /* FIX-6 */
                    ESP_LOGW(TAG, "OACK retransmit sendto partial=%zd", sent);
            }
            break;

        case TFTPD_STATE_RRQ:
            tftpd_send_window(i);
            break;

        case TFTPD_STATE_WRQ:
            /*
             * Re-send the last ACK that was actually transmitted.
             *
             * BUG (pre-fix): was `tftpd_send_ack(i, expected_block - 1)`.
             * With windowsize > 1, expected_block - 1 can be a mid-window
             * block that was never ACKed.  Sending that as a retransmit ACK
             * tells the client its window advanced to a block the server
             * hasn't truly confirmed, corrupting the transfer.
             * Using last_ack_sent always re-sends the correct last boundary.
             */
            tftpd_send_ack(i, s->last_ack_sent);
            break;

        default:
            break;
        }
    }
}
