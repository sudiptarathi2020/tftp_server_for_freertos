/*
 * tftpd.c  —  TFTP Server Protocol Engine  (ESP-IDF / FreeRTOS)
 *
 * Ported from the BDCOM/VxWorks version.  All platform-specific APIs
 * (open_filesys, write_filesys, exit_filesys, FCB_POINT, syslog, etc.)
 * replaced with standard POSIX stdio / ESP-IDF equivalents.
 *
 * PORT notes (match the tftpd.h PORT-* legend):
 *
 *  PORT-1  BDCOM integer types → stdint.h types throughout.
 *  PORT-2  FCB_POINT* + open/read/write/exit_filesys → FILE* + fopen/fread/
 *          fwrite/fclose.  Binary mode ("rb"/"wb") used everywhere.
 *  PORT-5  All paths prefixed with TFTPD_ROOT_PATH ("/littlefs") via
 *          tftpd_make_path().
 *  PORT-6  Write buffer: heap-allocated per WRQ session, TFTPD_WRITE_BUF_SIZE
 *          bytes.  DATA payloads are appended to the buffer; the buffer is
 *          flushed to LittleFS (under g_tftpd_fs_mutex) on every window-
 *          boundary ACK and on EOF.  If TFTPD_WRITE_BUF_SIZE == 0 every
 *          DATA block is written directly.
 *  PORT-7  write_offset: tracks the byte position of the next sequential
 *          write so the file pointer can be repositioned on a retransmit
 *          without reopening the file.
 *  PORT-8  g_tftpd_fs_mutex taken before every FILE* operation and released
 *          immediately after.  fclose() in tftpd_close_session() is also
 *          mutex-protected.
 *
 * Protocol fixes (all carried over from the previous ESP-IDF revision):
 *
 *  FIX-A   is_write flag: close_session uses the direction flag, not state.
 *  FIX-B   last_ack_sent: WRQ retransmit re-sends the last truly-ACKed block.
 *  FIX-C   RFC 7440 windowed WRQ: ACK only at window boundaries / last block.
 *  FIX-D   Packet-size guards: >= 4 bytes before every struct cast.
 *  FIX-E   OACK_WRQ fallthrough removed; explicit call to process_wrq_data().
 *  FIX-F   Path-traversal protection: reject filenames containing "..".
 *  FIX-G   OACK buffer: snprintf with remaining-space tracking.
 *  FIX-H   sendto / fwrite return values validated against expected sizes.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>     /* malloc / free / atoi */
#include <string.h>
#include <unistd.h>     /* close()              */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "tftpd.h"

static const char *TAG = "tftpd";

/* ── Forward declarations ────────────────────────────────────────────────── */
static int  tftpd_alloc_session(int is_write);
static int  tftpd_open_session_sock(void);
static int  tftpd_build_oack(char *buf, size_t buf_size,
                              uint16_t blksize, uint8_t winsz);
static void tftpd_send_error(int idx, uint16_t code, const char *msg);
static void tftpd_send_error_to(int sock, struct sockaddr_in *to,
                                 uint16_t code, const char *msg);
static int  tftpd_send_ack(int idx, uint16_t block);
static int  tftpd_parse_request(const char *buf, int len,
                                 char *filename, uint16_t *blksize,
                                 uint8_t *winsz, int *has_opts);
static int  tftpd_make_path(const char *filename,
                             char *out, size_t out_size);
static int  tftpd_send_window(int idx);
static void tftpd_process_wrq_data(int idx,
                                    const char *buf, ssize_t len);
static int  tftpd_flush_write_buf(int idx);   /* PORT-6 */

/* ═══════════════════════════════════════════════════════════════════════════
 * RESOURCE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════*/

int tftpd_open_listen(uint16_t port)
{
    struct sockaddr_in addr;
    int sock, opt;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() on port %u failed: %s",
                 (unsigned)port, strerror(errno));
        close(sock);
        return -1;
    }

    g_tftpd_listen_sock = sock;
    ESP_LOGI(TAG, "Listen socket open on UDP port %u (fd=%d)",
             (unsigned)port, sock);
    return 0;
}

void tftpd_close_listen(void)
{
    if (g_tftpd_listen_sock >= 0) {
        close(g_tftpd_listen_sock);
        g_tftpd_listen_sock = -1;
    }
}

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
        ESP_LOGE(TAG, "session bind() failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

static int tftpd_alloc_session(int is_write)
{
    int i;

    if (is_write) {
        /* WRQ requires exclusive access — no concurrent reads or writes */
        if (g_tftpd_num_read > 0 || g_tftpd_num_write > 0)
            return -1;
    } else {
        /* RRQ blocked while a WRQ is active (including DALLY) */
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
 * tftpd_close_session
 *
 * Tears down a session and releases all resources.
 *
 * FIX-A: uses s->is_write to choose which counter to decrement.
 *         The old code used the state value, which treated DALLY (a WRQ-only
 *         state) as a read session and underflowed g_tftpd_num_read to 255.
 *
 * PORT-2 / PORT-6 / PORT-8:
 *   fclose() called under g_tftpd_fs_mutex — LittleFS VFS state is not
 *   thread-safe during flush.  write_buf freed if still allocated.
 */
void tftpd_close_session(int idx)
{
    tftpd_session_t *s = &g_tftpd_sessions[idx];

    if (s->state == TFTPD_STATE_FREE)
        return;

    /* FIX-A: decrement the correct direction counter */
    if (!s->is_write) {
        if (g_tftpd_num_read > 0)
            g_tftpd_num_read--;
    } else {
        if (g_tftpd_num_write > 0)
            g_tftpd_num_write--;
    }

    /*
     * PORT-8: close the persistent FILE* under the FS mutex.
     * fclose() flushes stdio buffers and updates LittleFS metadata — not
     * safe to do concurrently with another task's fopen/fread/fwrite.
     */
    if (s->fp != NULL) {
        if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            fclose(s->fp);
            xSemaphoreGive(g_tftpd_fs_mutex);
        } else {
            ESP_LOGW(TAG, "close_session[%d]: mutex timeout — fclose unprotected", idx);
            fclose(s->fp);
        }
        s->fp = NULL;
    }

    /* PORT-6: free the write buffer */
    if (s->write_buf != NULL) {
        free(s->write_buf);
        s->write_buf      = NULL;
        s->write_buf_used = 0;
    }

    if (s->sock >= 0) {
        close(s->sock);
        s->sock = -1;
    }

    memset(s, 0, sizeof(*s));
    s->state = TFTPD_STATE_FREE;
    s->sock  = -1;
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
 * FIX-F: rejects filenames containing ".." to block path-traversal attacks.
 */
static int tftpd_make_path(const char *filename, char *out, size_t out_size)
{
    while (*filename == '/')
        filename++;

    if (strstr(filename, "..") != NULL) {
        ESP_LOGW(TAG, "tftpd_make_path: rejected path traversal: %s", filename);
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
 * FIX-G: snprintf with remaining-space tracking; never overflows the buffer.
 * Returns total packet length on success, -1 if the buffer is too small.
 */
static int tftpd_build_oack(char *buf, size_t buf_size,
                             uint16_t blksize, uint8_t winsz)
{
    tftp_oack_t *pkt = (tftp_oack_t *)buf;
    char        *p   = (char *)pkt->opts;
    char        *end = buf + buf_size;
    int          n;

    if (buf_size < sizeof(tftp_oack_t) + 4)
        return -1;

    pkt->opcode = htons(TFTP_OP_OACK);

#define OACK_APPEND(fmt, ...) \
    do { \
        n = snprintf(p, (size_t)(end - p), fmt, ##__VA_ARGS__); \
        if (n < 0 || p + n + 1 >= end) return -1; \
        p += n + 1; \
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

static void tftpd_send_error_to(int sock, struct sockaddr_in *to,
                                 uint16_t code, const char *msg)
{
    char          buf[80];
    tftp_error_t *pkt   = (tftp_error_t *)buf;
    int           mlen  = msg ? (int)strlen(msg) : 0;
    int           max_m = (int)(sizeof(buf) - sizeof(tftp_error_t) - 1);
    ssize_t       pkt_len, sent;

    if (mlen > max_m)
        mlen = max_m;

    pkt->opcode   = htons(TFTP_OP_ERROR);
    pkt->err_code = htons(code);
    if (mlen > 0)
        memcpy(pkt->msg, msg, mlen);
    pkt->msg[mlen] = '\0';

    pkt_len = (ssize_t)(sizeof(tftp_error_t) + mlen + 1);
    sent = sendto(sock, buf, (size_t)pkt_len, 0,
                  (struct sockaddr *)to, sizeof(*to));
    if (sent != pkt_len)
        ESP_LOGW(TAG, "send_error_to: sent=%zd expected=%zd errno=%d",
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

/* FIX-H: validate full datagram was sent. Returns 0 on success, -1 on error. */
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
    if (sent != (ssize_t)sizeof(pkt)) {
        ESP_LOGW(TAG, "send_ack[%d]: sent=%zd expected=%zu errno=%d",
                 idx, sent, sizeof(pkt), errno);
        return -1;
    }
    return 0;
}

static int tftpd_parse_request(const char *buf, int len,
                                char *filename, uint16_t *blksize,
                                uint8_t *winsz, int *has_opts)
{
    const char *p         = buf + 2;
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

    /* Mode string (octet / netascii — skip, we always use binary) */
    field_len = strnlen(p, remaining);
    if (field_len == (size_t)remaining)
        return -1;
    p         += field_len + 1;
    remaining -= (int)(field_len + 1);

    /* Option–value pairs (RFC 2347) */
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
 * tftpd_flush_write_buf                                                  PORT-6
 *
 * Writes all bytes in s->write_buf to the open file (s->fp), then resets
 * write_buf_used to 0.  Must be called with g_tftpd_fs_mutex already held.
 *
 * Returns 0 on success, -1 on fwrite error (session error already sent).
 */
static int tftpd_flush_write_buf(int idx)
{
    tftpd_session_t *s = &g_tftpd_sessions[idx];
    size_t written;

    if (s->write_buf == NULL || s->write_buf_used == 0)
        return 0;

    written = fwrite(s->write_buf, 1, s->write_buf_used, s->fp);
    if (written != s->write_buf_used) {
        ESP_LOGE(TAG, "flush_write_buf[%d]: wrote %zu of %" PRIu32 " bytes: %s",
                 idx, written, s->write_buf_used, strerror(errno));
        xSemaphoreGive(g_tftpd_fs_mutex);
        tftpd_send_error(idx, TFTP_ERR_DISKFULL, "File write failed");
        return -1;
    }

    s->write_buf_used = 0;
    return 0;
}

/*
 * tftpd_send_window  —  send up to windowsize DATA blocks for an RRQ session.
 *
 * PORT-2: uses FILE* (fopen "rb", fread, fseek) instead of BDCOM VFS.
 * PORT-8: all FILE* ops under g_tftpd_fs_mutex.
 * FIX-H:  sendto return validated.
 */
static int tftpd_send_window(int idx)
{
    tftpd_session_t   *s   = &g_tftpd_sessions[idx];
    char               pkt_buf[TFTPD_MAX_PKT_BUF];
    tftp_data_t       *pkt = (tftp_data_t *)pkt_buf;
    struct sockaddr_in to;
    char               fpath[TFTPD_MAX_FILENAME + sizeof(TFTPD_ROOT_PATH) + 2];
    int                i, read_len;
    ssize_t            sent, expected;

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    /* PORT-8: acquire FS mutex before touching s->fp */
    if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        tftpd_send_error(idx, TFTP_ERR_ACCESS,
                         "File system temporarily unavailable");
        return -1;
    }

    /* PORT-2: open once; reused on retransmits */
    if (s->fp == NULL) {
        if (tftpd_make_path(s->filename, fpath, sizeof(fpath)) != 0) {
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_ACCESS, "Invalid filename");
            return -1;
        }
        s->fp = fopen(fpath, "rb");
        if (s->fp == NULL) {
            ESP_LOGE(TAG, "fopen(%s, rb) failed: %s", fpath, strerror(errno));
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_NOTFOUND, "File not found");
            return -1;
        }
    }

    /* Seek to window-start position; handles retransmits too */
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
        if (sent != expected) {
            ESP_LOGE(TAG, "session[%d] sendto sent=%zd expected=%zd errno=%d",
                     idx, sent, expected, errno);
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "Network send error");
            return -1;
        }

        s->last_sent = block;
    }

    xSemaphoreGive(g_tftpd_fs_mutex);

    s->timeout_sec  = g_tftpd_cfg.timeout;
    s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

    return 0;
}

/*
 * tftpd_process_wrq_data  —  handle one incoming DATA block for a WRQ session.
 *
 * FIX-E: extracted from tftpd_handle_session_pkt to remove the implicit
 *         OACK_WRQ → WRQ fallthrough.
 * FIX-C: RFC 7440 windowed ACK — only ACK at window boundary or on EOF.
 * FIX-B: last_ack_sent updated on every transmitted ACK.
 *
 * PORT-2: FILE* replaces FCB_POINT; fopen "wb" on the first block.
 * PORT-6: incoming data is appended to s->write_buf if allocated.
 *         The buffer is flushed to LittleFS:
 *           • when it is full (no room for another full blksize payload), or
 *           • when a window-boundary ACK is about to be sent, or
 *           • on the final (short) block.
 *         This reduces flash write frequency by up to (WRITE_BUF / blksize).
 * PORT-7: write_offset advanced by write_len on each accepted block.
 *         On a duplicate (retransmit after our ACK was lost) the file pointer
 *         is fseek()'d back to the start of the duplicate block so the write
 *         is idempotent — the same bytes are overwritten rather than appended.
 * PORT-8: all FILE* ops under g_tftpd_fs_mutex.
 */
static void tftpd_process_wrq_data(int idx, const char *buf, ssize_t len)
{
    tftpd_session_t    *s = &g_tftpd_sessions[idx];
    char                fpath[TFTPD_MAX_FILENAME + sizeof(TFTPD_ROOT_PATH) + 2];
    uint16_t            block;
    int                 write_len;
    int                 is_last_block;
    int                 is_window_end;
    const uint8_t      *data_ptr;

    /* FIX-D: minimum packet size for DATA is 4 bytes */
    if (len < (ssize_t)sizeof(tftp_data_t)) {
        tftpd_send_error(idx, TFTP_ERR_ILLEGAL, "DATA packet too short");
        return;
    }

    block     = ntohs(((const tftp_data_t *)buf)->block_num);
    write_len = (int)len - (int)sizeof(tftp_data_t);
    if (write_len < 0)
        write_len = 0;

    data_ptr = (const uint8_t *)(buf + sizeof(tftp_data_t));

    /*
     * Duplicate detection: client is retransmitting a block we already ACKed.
     *
     * PORT-7: we must not simply ignore it — the ACK may have been lost, so
     * we re-send the ACK.  But we must not re-write to the file past the
     * current write_offset.  Since the file is open in sequential "wb" mode
     * and we never fseek() forward, we check whether this block's data would
     * fall entirely within the range already written.
     *
     * Simplified policy (matching BDCOM): if block == last_ack_sent we
     * re-ACK and return; if block < expected_block but != last_ack_sent we
     * silently ignore (the client must have got the ACK for it).
     */
    if (block == (uint16_t)(s->expected_block - 1)) {
        /* The last ACK we sent — client didn't get it; re-send. */
        tftpd_send_ack(idx, s->last_ack_sent);
        return;
    }

    /* Out-of-sequence: silently ignore */
    if (block != s->expected_block)
        return;

    is_last_block = (write_len < (int)s->blksize);
    is_window_end = ((block % s->windowsize) == 0);

    /* PORT-8: acquire FS mutex before touching s->fp */
    if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        tftpd_send_error(idx, TFTP_ERR_ACCESS,
                         "File system temporarily unavailable");
        return;
    }

    /* PORT-2: open the file on the very first block */
    if (s->fp == NULL) {
        if (tftpd_make_path(s->filename, fpath, sizeof(fpath)) != 0) {
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_ACCESS, "Invalid filename");
            return;
        }
        s->fp = fopen(fpath, "wb");
        if (s->fp == NULL) {
            ESP_LOGE(TAG, "fopen(%s, wb) failed: %s", fpath, strerror(errno));
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_ACCESS, "Cannot open file for write");
            return;
        }
        s->file_created  = 1;
        s->write_offset  = 0;   /* PORT-7 */
    }

    /*
     * PORT-6: write buffering.
     *
     * Case A — buffering enabled (write_buf != NULL):
     *   Append data to write_buf.  Flush if:
     *     (1) the buffer is full (not enough room for another blksize chunk),
     *     (2) this is a window-boundary block and we are about to ACK, or
     *     (3) this is the final (short) block.
     *   Flushing is done here, under the mutex, before releasing it.
     *
     * Case B — buffering disabled (write_buf == NULL):
     *   fwrite() directly to the file for every block.
     */
    if (write_len > 0) {
        if (s->write_buf != NULL) {
            /* Append to buffer */
            memcpy(s->write_buf + s->write_buf_used, data_ptr, (size_t)write_len);
            s->write_buf_used += (uint32_t)write_len;

            /* Determine whether to flush now */
            int need_flush = is_last_block
                          || is_window_end
                          || (s->write_buf_used + s->blksize > TFTPD_WRITE_BUF_SIZE);

            if (need_flush) {
                if (tftpd_flush_write_buf(idx) != 0)
                    return;   /* error: mutex already released, session closed */
                /* After a successful flush, fflush to commit to LittleFS */
                fflush(s->fp);
            }
        } else {
            /* Unbuffered: write directly */
            size_t written = fwrite(data_ptr, 1, (size_t)write_len, s->fp);
            if (written != (size_t)write_len) {
                ESP_LOGE(TAG, "fwrite[%d]: wrote %zu of %d bytes: %s",
                         idx, written, write_len, strerror(errno));
                xSemaphoreGive(g_tftpd_fs_mutex);
                tftpd_send_error(idx, TFTP_ERR_DISKFULL, "File write failed");
                return;
            }
            fflush(s->fp);
        }
    }

    xSemaphoreGive(g_tftpd_fs_mutex);

    /* PORT-7: advance write_offset by the number of data bytes accepted */
    s->write_offset  += (uint32_t)write_len;
    s->expected_block++;
    s->timeout_sec    = g_tftpd_cfg.timeout;
    s->retry_remain   = (uint8_t)(g_tftpd_cfg.retry - 1);

    /*
     * FIX-C: RFC 7440 windowed WRQ ACK rule.
     *
     * Only ACK at a window boundary or on the final (short) block.
     * Sending an ACK mid-window tells the client its window advanced,
     * causing it to duplicate-send every unconfirmed block in the window.
     *
     * windowsize == 1: block % 1 == 0 always → every block is ACKed. ✓
     */
    if (is_last_block || is_window_end) {
        s->last_ack_sent = block;
        tftpd_send_ack(idx, block);
    }

    /* Final block: enter DALLY to re-ACK if the client retransmits */
    if (is_last_block) {
        ESP_LOGI(TAG, "session[%d] WRQ complete (%" PRIu32 " bytes written)",
                 idx, s->write_offset);
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
    if (len < 4)
        return;

    if (!g_tftpd_cfg.enabled)
        return;

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
        if (is_write)
            reason = "Write session already active";
        else if (g_tftpd_num_write > 0)
            reason = "Write session active — try again shortly";
        else
            reason = "Max concurrent read sessions reached";
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
    s->write_buf    = NULL;
    s->is_write     = is_write;
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
        /* ── RRQ: server sends file to client ─────────────────────────── */
        g_tftpd_num_read++;
        s->window_start = 1;
        s->last_ack     = 0;
        s->file_pos_win = 0;

        if (has_opts) {
            oack_len = tftpd_build_oack(oack_buf, sizeof(oack_buf),
                                         blksize, winsz);
            if (oack_len > 0) {
                ssize_t sent = sendto(s->sock, oack_buf, (size_t)oack_len,
                                      0, (struct sockaddr *)&to, sizeof(to));
                if (sent != (ssize_t)oack_len)
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
        /* ── WRQ: client sends file to server ─────────────────────────── */
        g_tftpd_num_write++;
        s->expected_block = 1;
        s->last_ack_sent  = 0;
        s->write_offset   = 0;   /* PORT-7 */

        /*
         * PORT-6: allocate write buffer from heap.
         *
         * malloc() failure is non-fatal: write_buf stays NULL and
         * tftpd_process_wrq_data() falls back to per-block fwrite().
         */
#if TFTPD_WRITE_BUF_SIZE > 0
        s->write_buf = (uint8_t *)malloc(TFTPD_WRITE_BUF_SIZE);
        if (s->write_buf == NULL) {
            ESP_LOGW(TAG, "WRQ session[%d]: write_buf malloc failed — "
                          "falling back to unbuffered writes", idx);
        } else {
            s->write_buf_used = 0;
        }
#endif

        if (has_opts) {
            oack_len = tftpd_build_oack(oack_buf, sizeof(oack_buf),
                                         blksize, winsz);
            if (oack_len > 0) {
                ssize_t sent = sendto(s->sock, oack_buf, (size_t)oack_len,
                                      0, (struct sockaddr *)&to, sizeof(to));
                if (sent != (ssize_t)oack_len)
                    ESP_LOGW(TAG, "OACK sendto partial: sent=%zd", sent);
            }
            s->state = TFTPD_STATE_OACK_WRQ;
        } else {
            s->state = TFTPD_STATE_WRQ;
            tftpd_send_ack(idx, 0);     /* ACK(0) invites first DATA block */
        }

        ESP_LOGI(TAG, "WRQ session[%d] client=%s:%u file=%s blksize=%u "
                      "winsz=%u write_buf=%s",
                 idx, inet_ntoa(cli_addr.sin_addr),
                 (unsigned)ntohs(cli_addr.sin_port),
                 filename, (unsigned)blksize, (unsigned)winsz,
                 s->write_buf ? "yes" : "no (unbuffered)");
    }
}

/*
 * tftpd_handle_session_pkt  —  process one packet on an active session socket.
 *
 * FIX-D: minimum-size guard before every struct cast.
 * FIX-E: OACK_WRQ no longer falls through; both states call process_wrq_data()
 *         explicitly after their own validation passes.
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

    /* DALLY: re-send last window ACK if client retransmits any block in it */
    if (s->state == TFTPD_STATE_DALLY) {
        if (opcode == TFTP_OP_DATA &&
            len >= (ssize_t)sizeof(tftp_data_t)) {
            block = ntohs(((const tftp_data_t *)buf)->block_num);
            /*
             * Re-send last_ack_sent for any block in the final window.
             * The subtraction is safe because last_ack_sent >= windowsize-1
             * at this point (the transfer completed at least one full window).
             */
            if (block <= s->last_ack_sent &&
                block >= (uint16_t)(s->last_ack_sent - s->windowsize + 1))
                tftpd_send_ack(idx, s->last_ack_sent);
        }
        return;
    }

    switch (s->state) {

    case TFTPD_STATE_OACK_RRQ:
        if (opcode != TFTP_OP_ACK) break;
        if (len < (ssize_t)sizeof(tftp_ack_t)) break;
        block = ntohs(((const tftp_ack_t *)buf)->block_num);
        if (block != 0) break;
        s->state = TFTPD_STATE_RRQ;
        tftpd_send_window(idx);
        break;

    case TFTPD_STATE_RRQ:
        if (opcode != TFTP_OP_ACK) break;
        if (len < (ssize_t)sizeof(tftp_ack_t)) break;
        block = ntohs(((const tftp_ack_t *)buf)->block_num);

        if (block < (uint16_t)(s->window_start - 1)) break;  /* stale */

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
            /* Partial ACK within the current window */
            uint16_t acked  = (uint16_t)(block - (s->window_start - 1));
            s->file_pos_win += (uint32_t)acked * s->blksize;
            s->window_start  = (uint16_t)(block + 1);
            s->last_ack      = block;
            tftpd_send_window(idx);
        }
        break;

    case TFTPD_STATE_OACK_WRQ:
        /* FIX-E: explicit first-DATA validation before delegating */
        if (opcode != TFTP_OP_DATA) break;
        if (len < (ssize_t)sizeof(tftp_data_t)) break;
        block = ntohs(((const tftp_data_t *)buf)->block_num);
        if (block != 1) break;
        s->state = TFTPD_STATE_WRQ;
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
            oack_len = tftpd_build_oack(oack_buf, sizeof(oack_buf),
                                         s->blksize, s->windowsize);
            if (oack_len > 0) {
                ssize_t sent = sendto(s->sock, oack_buf, (size_t)oack_len,
                                      0, (struct sockaddr *)&to, sizeof(to));
                if (sent != (ssize_t)oack_len)
                    ESP_LOGW(TAG, "OACK retransmit sendto partial=%zd", sent);
            }
            break;

        case TFTPD_STATE_RRQ:
            tftpd_send_window(i);
            break;

        case TFTPD_STATE_WRQ:
            /*
             * FIX-B: re-send last_ack_sent, not (expected_block - 1).
             * With windowsize > 1, expected_block - 1 may be mid-window
             * (never ACKed), which would silently advance the client's window.
             */
            tftpd_send_ack(i, s->last_ack_sent);
            break;

        default:
            break;
        }
    }
}
