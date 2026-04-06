/*
 * tftpd.h  —  TFTP Server: Common Definitions  (ESP-IDF / FreeRTOS)
 *
 * All global state is owned by the single TFTPDT task; no cross-task
 * locking is required for the TFTP session fields themselves.
 * g_tftpd_fs_mutex guards LittleFS file operations so that other tasks
 * that also use the VFS do not corrupt open file handles.
 */

#ifndef _TFTPD_H_
#define _TFTPD_H_

#include <stdint.h>
#include <stdio.h>              /* FILE* stored in tftpd_session_t */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_err.h"

/* ── Default configuration ──────────────────────────────────────────────── */
#define TFTPD_DEFAULT_PORT      69
#define TFTPD_DEFAULT_TIMEOUT   3       /* retransmit timeout (seconds)      */
#define TFTPD_DEFAULT_RETRY     3       /* max transmit attempts incl. first */
#define TFTPD_DEFAULT_BLKSIZE   512
#define TFTPD_DEFAULT_WINSZ     1

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define TFTPD_MAX_SESSIONS      3
#define TFTPD_MAX_BLKSIZE       1456    /* Ethernet MTU 1500 – 44 B headers  */
#define TFTPD_MAX_WINSZ         8
#define TFTPD_MAX_FILENAME      256
#define TFTPD_DALLY_TICKS       2       /* seconds in DALLY before closing   */
#define TFTPD_MAX_PKT_BUF       (4 + TFTPD_MAX_BLKSIZE)
#define TFTPD_MAX_OACK_BUF      128

/* VFS mount point for the TFTP file store (LittleFS) */
#define TFTPD_ROOT_PATH         "/littlefs"

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

    /* RRQ fields */
    uint16_t window_start;      /* first block# of the current window       */
    uint16_t last_sent;
    uint16_t last_ack;
    uint32_t file_pos_win;      /* file byte offset at window start          */
    uint32_t bytes_this_win;    /* bytes read+sent in the current window     */
    uint8_t  is_last;           /* 1 when a short block has been sent        */

    /* WRQ fields */
    uint16_t expected_block;
    uint8_t  file_created;
    /*
     * last_ack_sent: the block number of the most recent ACK actually
     * transmitted to the client.
     *
     * With windowsize == 1 this equals expected_block - 1 at all times.
     * With windowsize > 1 the server only ACKs at window boundaries (RFC 7440
     * §3), so expected_block - 1 can be mid-window and was never ACKed.
     * The retransmit path in tftpd_handle_timer must re-send the last ACK
     * that was truly sent — not an un-ACKed mid-window block number, which
     * would incorrectly advance the client's send pointer.
     */
    uint16_t last_ack_sent;

    /*
     * Direction flag: 0 = RRQ (server sends), 1 = WRQ (client sends).
     *
     * Used by tftpd_close_session() to decrement the correct counter even
     * when the session is in TFTPD_STATE_DALLY.  DALLY is only ever entered
     * from WRQ (after the final DATA block is ACKed), but the state value
     * alone does not encode direction — without this flag the close logic
     * mistakenly decrements g_tftpd_num_read instead of g_tftpd_num_write,
     * leaving g_tftpd_num_write permanently at 1 and blocking all future RRQs.
     */
    uint8_t  is_write;

    /* Retransmit control */
    uint8_t  retry_remain;
    uint8_t  timeout_sec;
    uint8_t  dally_remain;

    /*
     * Persistent file handle.
     *
     * FIX: keeps the file open across packets instead of reopening on every
     * window call.  On ESP32 / LittleFS each fopen() hits flash metadata;
     * reopening per-packet causes unnecessary flash wear and latency.
     *
     * Lifetime: opened on first data exchange, closed in tftpd_close_session.
     * Protected by g_tftpd_fs_mutex when performing actual I/O.
     */
    FILE    *fp;

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
/*
 * NOTE on thread safety: all fields below are accessed exclusively from the
 * TFTPDT task.  No additional locking is needed for the session array or
 * counters.  g_tftpd_fs_mutex guards FILE* operations so other VFS users
 * cannot race on open file handles.
 */
extern tftpd_config_t    g_tftpd_cfg;
extern tftpd_session_t   g_tftpd_sessions[TFTPD_MAX_SESSIONS];
extern int               g_tftpd_listen_sock;
extern uint8_t           g_tftpd_num_read;
extern uint8_t           g_tftpd_num_write;
extern SemaphoreHandle_t g_tftpd_fs_mutex;

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
