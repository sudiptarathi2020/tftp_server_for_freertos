#ifndef _TFTPD_H_
#define _TFTPD_H_

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/socket.h>
#include "esp_log.h"
#include "esp_err.h"


/* defaults */
#define TFTPD_DEFAULT_PORT 69
#define TFTPD_DEFAULT_TIMEOUT 3
#define TFTPD_DEFAULT_RETRY 3
#define TFTPD_DEFAULT_BLKSIZE 512
#define TFTPD_DEFAULT_WINSZ 1

/* limits */
#define TFTPD_MAX_SESSIONS 3
#define TFTPD_MAX_BLKSIZE 1456
#define TFTPD_MAX_WINSZ 8
#define TFTPD_MAX_FILENAME 256
#define TFTPD_DALLY_TICKS 2
#define TFTPD_MAX_PKT_BUF (4 + TFTPD_MAX_BLKSIZE)
#define TFTPD_MAX_OACK_BUF 128

/* root path */
#define TFTPD_ROOT_PATH "/littlefs"

/* packet opcodes */
#define TFTP_OP_RRQ 1
#define TFTP_OP_WRQ 2
#define TFTP_OP_DATA 3
#define TFTP_OP_ACK 4
#define TFTP_OP_ERROR 5
#define TFTP_OP_OACK 6

/* error codes */
#define TFTP_ERR_UNDEF 0
#define TFTP_ERR_NOTFOUND 1
#define TFTP_ERR_ACCESS 2
#define TFTP_ERR_DISKFULL 3
#define TFTP_ERR_ILLEGAL 4
#define TFTP_ERR_BADTID 5
#define TFTP_ERR_EXISTS 6
#define TFTP_ERR_NOUSER 7
#define TFTP_ERR_OPTFAIL 8

/* session states */
#define TFTPD_STATE_FREE 0
#define TFTPD_STATE_OACK_RRQ 1
#define TFTPD_STATE_RRQ 2
#define TFTPD_STATE_OACK_WRQ 3
#define TFTPD_STATE_WRQ 4
#define TFTPD_STATE_DALLY 5

/* RRQ/WRQ request packet*/
typedef struct {
    uint16_t opcode;
    uint8_t payload[0];
} __attribute__((packed)) tftp_req_t;

/* data packets */
typedef struct {
    uint16_t opcode;
    uint16_t block_num;
    uint8_t data[0];
} __attribute__((packed)) tftp_data_t;

/* data ack packets */
typedef struct {
    uint16_t opcode;
    uint16_t block_num;
} __attribute__((packed)) tftp_ack_t;

/* error packet */
typedef struct {
    uint16_t opcode;
    uint16_t err_code;
    uint8_t msg[0];
} __attribute__((packed)) tftp_error_t;

/* option ack packet */
typedef struct {
    uint16_t opcode;
    uint8_t opts[0];
} __attribute__((packed)) tftp_oack_t;

/* sesstion struct to handle multiple session */
typedef struct {
    uint8_t state;
    int sock;
    uint32_t client_ip;
    uint16_t client_port;
    uint16_t blksize;
    uint8_t windowsize;
    uint16_t window_start;
    uint16_t last_sent;
    uint16_t last_ack;
    uint32_t file_pos_win;
    uint32_t bytes_this_win;
    uint8_t is_last;
    uint16_t expected_block;
    uint8_t file_created;
    uint8_t retry_remain;
    uint8_t timeout_sec;
    uint8_t dally_remain;
    char filename[TFTPD_MAX_FILENAME];
} tftpd_session_t;

/* gloabl configuration value struct */
typedef struct {
    uint8_t enabled;
    uint16_t port;
    uint8_t timeout;
    uint8_t retry;
} tftpd_config_t;

/* global values */
extern tftpd_config_t g_tftpd_cfg;
extern tftpd_session_t g_tftpd_sessions[TFTPD_MAX_SESSIONS];
extern int g_tftpd_listen_sock;
extern uint8_t g_tftpd_num_read;
extern uint8_t g_tftpd_num_write;

/* semaphore to protect file operations */
extern SemaphoreHandle_t g_tftpd_fs_mutex;

void tftpd_init(void);

int tftpd_open_listen(uint16_t port);
void tftpd_close_listen(void);
void tftpd_close_session(int idx);
void tftpd_close_all_sessions(void);
void tftpd_handle_listen_pkt(void);
void tftpd_handle_session_pkt(int idx);
void tftpd_handle_timer(void);

#endif  // _TFTPD_H_
