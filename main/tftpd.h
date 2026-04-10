/*
 * tftpd.h  —  TFTP Server: Common Definitions  (ESP-IDF / FreeRTOS)
 *
 * Ported from the BDCOM/VxWorks version. All BDCOM platform types
 * (uint8/uint16/uint32, FCB_POINT, sys_* APIs) replaced with standard
 * C99 / POSIX / ESP-IDF equivalents.
 *
 * PORT-1  BDCOM uint8/uint16/uint32 → stdint.h uint8_t/uint16_t/uint32_t
 * PORT-2  FCB_POINT *fp / fp_mode → FILE *fp  (POSIX stdio, always binary).
 *         fp_mode dropped: fclose() works regardless of how the file was
 *         opened; there is no separate "exit_filesys" on ESP-IDF.
 * PORT-3  sys_msgq / TIMER_MSG_METHOD removed.  The ESP-IDF task uses a
 *         select() loop with an esp_timer_get_time() wall-clock tick instead.
 * PORT-4  tftpd_msg_send() removed; not applicable to the select() model.
 * PORT-5  BDCOM VFS root replaced with LittleFS mount point "/littlefs".
 * PORT-6  write_buf / write_buf_used retained, default capped to 16 KB.
 *         On ESP32 the total heap is ~200 KB; the BDCOM 128 KB is too large.
 *         The buffer reduces the number of LittleFS fwrite() calls, cutting
 *         flash page-program cycles proportionally.
 * PORT-7  write_offset retained — tracks the sequential byte position so a
 *         retransmitted block can be fseek()'d without re-opening the file.
 * PORT-8  g_tftpd_fs_mutex added — guards LittleFS VFS operations so other
 *         tasks sharing the same volume cannot race on open file handles.
 * PORT-9  Default blksize → 1456, windowsize → 4, matching BDCOM performance
 *         defaults and ESP-IDF LwIP MTU headroom.
 */

#ifndef _TFTPD_H_
#define _TFTPD_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_err.h"

/* ── VFS mount point for the TFTP file store (LittleFS) ─────────────────── */
#define TFTPD_ROOT_PATH         "/littlefs"

/* ── Default configuration ──────────────────────────────────────────────── */
#define TFTPD_DEFAULT_PORT      69
#define TFTPD_DEFAULT_TIMEOUT   3       /* retransmit timeout (seconds)      */
#define TFTPD_DEFAULT_RETRY     3       /* max retransmit attempts           */
#define TFTPD_DEFAULT_BLKSIZE   1456    /* PORT-9: max Ethernet (was 512)    */
#define TFTPD_DEFAULT_WINSZ     4       /* PORT-9: pipelined (was 1)         */

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define TFTPD_MAX_SESSIONS      3
#define TFTPD_MAX_BLKSIZE       1456    /* Ethernet MTU 1500 – 44 B headers  */
#define TFTPD_MAX_WINSZ         8
#define TFTPD_MAX_FILENAME      256
#define TFTPD_DALLY_TICKS       2       /* seconds in DALLY before closing   */
#define TFTPD_MAX_PKT_BUF       (4 + TFTPD_MAX_BLKSIZE)
#define TFTPD_MAX_OACK_BUF      128

/*
 * Write-buffer size for WRQ sessions.                                    PORT-6
 *
 * 16 KB default suits typical ESP32 heap budgets.  Raise to 64 KB on boards
 * with PSRAM.  Set to 0 to disable buffering (one fwrite() per DATA block).
 */
#ifndef TFTPD_WRITE_BUF_SIZE
#define TFTPD_WRITE_BUF_SIZE    (16 * 1024)
#endif

/* ── TFTP opcodes (RFC 1350 §5, RFC 2347 §2) ───────────────────────────── */
#define TFTP_OP_RRQ     1
#define TFTP_OP_WRQ     2
#define TFTP_OP_DATA    3
#define TFTP_OP_ACK     4
#define TFTP_OP_ERROR   5
#define TFTP_OP_OACK    6

/* ── TFTP error codes (RFC 1350 §5) ────────────────────────────────────── */
#define TFTP_ERR_UNDEF      0
#define TFTP_ERR_NOTFOUND   1
#define TFTP_ERR_ACCESS     2
#define TFTP_ERR_DISKFULL   3
#define TFTP_ERR_ILLEGAL    4
#define TFTP_ERR_BADTID     5
#define TFTP_ERR_EXISTS     6
#define TFTP_ERR_NOUSER     7
#define TFTP_ERR_OPTFAIL    8

/* ── Session states ─────────────────────────────────────────────────────── */
#define TFTPD_STATE_FREE        0
#define TFTPD_STATE_OACK_RRQ    1   /* OACK sent; awaiting ACK 0            */
#define TFTPD_STATE_RRQ         2   /* sending file to client               */
#define TFTPD_STATE_OACK_WRQ    3   /* OACK sent; awaiting DATA 1           */
#define TFTPD_STATE_WRQ         4   /* receiving file from client           */
#define TFTPD_STATE_DALLY       5   /* brief wait after last write ACK      */

/* ── Packed wire-format structs ─────────────────────────────────────────── */
typedef struct {
    uint16_t opcode;
    uint8_t  payload[0];
} __attribute__((packed)) tftp_req_t;

typedef struct {
    uint16_t opcode;
    uint16_t block_num;
    uint8_t  data[0];
} __attribute__((packed)) tftp_data_t;

typedef struct {
    uint16_t opcode;
    uint16_t block_num;
} __attribute__((packed)) tftp_ack_t;

typedef struct {
    uint16_t opcode;
    uint16_t err_code;
    uint8_t  msg[0];
} __attribute__((packed)) tftp_error_t;

typedef struct {
    uint16_t opcode;
    uint8_t  opts[0];
} __attribute__((packed)) tftp_oack_t;

/* ── Per-session state ──────────────────────────────────────────────────── */
typedef struct {
    uint8_t  state;             /* TFTPD_STATE_*                             */
    int      sock;              /* ephemeral session socket fd               */
    uint32_t client_ip;         /* client address (network byte order)       */
    uint16_t client_port;       /* client port    (network byte order)       */

    /* Negotiated transfer parameters */
    uint16_t blksize;
    uint8_t  windowsize;

    /*
     * Direction flag — set once at session creation, never changed.
     * Used by tftpd_close_session() to decrement the correct counter even
     * when the session is in TFTPD_STATE_DALLY (DALLY is a WRQ-only state
     * but its numeric value does not encode direction).
     */
    uint8_t  is_write;

    /* ── RRQ fields ─────────────────────────────────────────────────────── */
    uint16_t window_start;      /* first block# of the current window        */
    uint16_t last_sent;
    uint16_t last_ack;
    uint32_t file_pos_win;      /* file byte offset at window start           */
    uint32_t bytes_this_win;    /* bytes read + sent in the current window    */
    uint8_t  is_last;           /* 1 when the final (short) block was sent    */

    /* ── WRQ fields ─────────────────────────────────────────────────────── */
    uint16_t expected_block;
    uint8_t  file_created;

    /*
     * last_ack_sent — block number of the most recent ACK put on the wire.
     *
     * With windowsize == 1 this equals (expected_block - 1) at all times.
     * With windowsize > 1 (RFC 7440) the server ACKs only at window
     * boundaries, so (expected_block - 1) can be a mid-window block that
     * was never acknowledged.  The retransmit path in tftpd_handle_timer()
     * must re-send the last truly-sent ACK, not a block the client never saw
     * confirmed — doing so would silently advance the client's send window.
     */
    uint16_t last_ack_sent;

    /*
     * write_offset — byte offset of the next sequential fwrite().         PORT-7
     *
     * Advanced by write_len on each accepted in-order DATA block (regardless
     * of whether the write was buffered or flushed immediately).  On a
     * duplicate or retransmit, the file pointer is fseek()'d to the offset
     * of the duplicate block so the write overwrites correctly without
     * reopening the file.
     */
    uint32_t write_offset;

    /* ── Retransmit control ─────────────────────────────────────────────── */
    uint8_t  retry_remain;
    uint8_t  timeout_sec;
    uint8_t  dally_remain;

    /*
     * Persistent file handle.                                              PORT-2
     *
     * Opened once on the first data exchange, closed in tftpd_close_session().
     * Avoids per-packet fopen()/fclose() overhead; each fopen() on LittleFS
     * reads flash metadata.  Protected by g_tftpd_fs_mutex during I/O.
     */
    FILE    *fp;

    /*
     * Write buffer — accumulates WRQ DATA payloads before flushing.       PORT-6
     *
     * Flash page-program is slow; batching writes reduces the number of
     * actual flash operations roughly proportional to the buffer size.
     *
     * Lifecycle:
     *   allocated  — heap malloc in tftpd_handle_listen_pkt() on WRQ accept
     *                (only when TFTPD_WRITE_BUF_SIZE > 0)
     *   flushed    — when full, or on every window-boundary ACK, or on EOF
     *   freed      — tftpd_close_session()
     *
     * write_buf_used: number of valid bytes currently in the buffer.
     */
    uint8_t *write_buf;
    uint32_t write_buf_used;

    char     filename[TFTPD_MAX_FILENAME];
} tftpd_session_t;

/* ── Module configuration ───────────────────────────────────────────────── */
typedef struct {
    uint8_t  enabled;
    uint16_t port;
    uint8_t  timeout;
    uint8_t  retry;
} tftpd_config_t;

/* ── Globals (owned by TFTPDT task; defined in tftpd_task.c) ───────────── */
extern tftpd_config_t    g_tftpd_cfg;
extern tftpd_session_t   g_tftpd_sessions[TFTPD_MAX_SESSIONS];
extern int               g_tftpd_listen_sock;
extern uint8_t           g_tftpd_num_read;
extern uint8_t           g_tftpd_num_write;
extern SemaphoreHandle_t g_tftpd_fs_mutex;  /* PORT-8: guards all FILE* ops  */

/* ── Public API ─────────────────────────────────────────────────────────── */
void tftpd_init(void);

int  tftpd_open_listen(uint16_t port);
void tftpd_close_listen(void);
void tftpd_close_session(int idx);
void tftpd_close_all_sessions(void);
void tftpd_handle_listen_pkt(void);
void tftpd_handle_session_pkt(int idx);
void tftpd_handle_timer(void);

#endif /* _TFTPD_H_ */
