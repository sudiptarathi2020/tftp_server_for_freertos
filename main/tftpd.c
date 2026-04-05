#include "tftpd.h"
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
static const char *TAG = "tftpd";


static void tftpd_send_error_to(int sock, struct sockaddr_in  *to, uint16_t code, const char *msg);
int tftpd_open_listen(uint16_t port);
static int tftpd_alloc_session(int is_write);
int tftpd_open_session_sock();
static int tftpd_build_oack(char *buf, uint16_t blksize, uint8_t winsz);
static void tftpd_send_error(int idx, uint16_t code, const char *msg);
static void tftpd_send_error_to(int sock, struct sockaddr_in  *to, uint16_t code, const char *msg);
void tftpd_close_listen(void);
void tftpd_close_session(int idx);
void tftpd_close_all_sessions(void);
static int tftpd_parse_request(char *buf, int len, char *filename, uint16_t *blksize, uint8_t *winsz, int *has_opts);
static void tftpd_make_path(const char *filename, char *out, size_t out_size);
static int tftpd_send_window(int idx);
void tftpd_handle_session_pkt(int idx);
void tftpd_handle_timer(void);


int tftpd_open_listen(uint16_t port)
{
    struct sockaddr_in serv_addr_in;
    int sock = -1, opt, ret_val;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket creation failed with error code : %s\n", strerror(errno));
        return -1;
    }

    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_addr_in, 0, sizeof(serv_addr_in));
    serv_addr_in.sin_family = AF_INET;
    serv_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr_in.sin_port = htons(port);

    ret_val = bind(sock, (struct sockaddr *)&serv_addr_in, sizeof(serv_addr_in));
    if (ret_val < 0)
    {
        ESP_LOGE(TAG, "socket bind failed to error code : %d\n", strerror(errno));
        close(sock);
        return -1;
    }

    g_tftpd_listen_sock = sock;
    ESP_LOGI(TAG, "listen socket created successfull\n");
    return 0;
}

static int tftpd_alloc_session(int is_write)
{
    int i;

    if (is_write) 
    {
        if (g_tftpd_num_read > 0 || g_tftpd_num_write > 0)
        {
            return -1;
        }
    }
    else 
    {
        if (g_tftpd_num_write > 0)
        {
            return -1;
        }

        if (g_tftpd_num_read >= TFTPD_MAX_SESSIONS)
        {
            return -1;
        }
    }

    for (i = 0; i < TFTPD_MAX_SESSIONS; i++) 
    {
        if (g_tftpd_sessions[i].state == TFTPD_STATE_FREE)
        {
            return i;
        }
    }

    return -1;
}


int tftpd_open_session_sock()
{
    struct sockaddr_in addr;
    int sock, rv;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;

    rv = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rv != 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static int tftpd_build_oack(char *buf, uint16_t blksize, uint8_t winsz)
{
    tftp_oack_t *pkt = (tftp_oack_t *)buf;
    char *p   = (char *)pkt->opts;

    pkt->opcode = htons(TFTP_OP_OACK);

    p += sprintf(p, "blksize")             + 1;
    p += sprintf(p, "%u", (unsigned)blksize) + 1;

    if (winsz > 1) {
        p += sprintf(p, "windowsize")         + 1;
        p += sprintf(p, "%u", (unsigned)winsz)  + 1;
    }

    return (int)(p - buf);
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

static void tftpd_send_error_to(int sock, struct sockaddr_in  *to, uint16_t code, const char *msg)
{
    char buf[80];
    tftp_error_t *tftp_err_pkt;
    tftp_err_pkt = (tftp_error_t *)buf;
    int mlen;

    mlen = msg ? (int)strlen(msg) : 0;
    if (mlen > (int)(sizeof(buf) - sizeof(tftp_error_t) - 1))
    {
        mlen = (int)(sizeof(buf) - sizeof(tftp_error_t) - 1);
    }

    tftp_err_pkt->opcode = htons(TFTP_OP_ERROR);
    tftp_err_pkt->err_code = htons(code);

    if (mlen > 0)
    {
        memcpy(tftp_err_pkt->msg, msg, mlen);
    }
    tftp_err_pkt->msg[mlen] = '\0';

    sendto(sock, buf, (size_t)(sizeof(tftp_error_t) + mlen + 1), 0, (struct sockaddr*)to, sizeof(*to));
}

static int tftpd_send_ack(int idx, uint16_t block)
{
    tftpd_session_t   *s = &g_tftpd_sessions[idx];
    tftp_ack_t         pkt;
    struct sockaddr_in to;

    pkt.opcode    = htons(TFTP_OP_ACK);
    pkt.block_num = htons(block);

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    return (int)sendto(s->sock, &pkt, sizeof(pkt),
            0, (struct sockaddr *)&to, sizeof(to));
}

void tftpd_close_listen(void)
{
    if (g_tftpd_listen_sock >= 0) {
        close(g_tftpd_listen_sock);   /* was: so_close() */
        g_tftpd_listen_sock = -1;
    }
}

void tftpd_close_session(int idx)
{
    tftpd_session_t *s = &g_tftpd_sessions[idx];
    if (s->state == TFTPD_STATE_FREE)
        return;

    if (s->state == TFTPD_STATE_RRQ || s->state == TFTPD_STATE_OACK_RRQ || s->state == TFTPD_STATE_DALLY)
    {
        if (g_tftpd_num_read > 0)
        {
            g_tftpd_num_read --;
        }
    }
    else
    {
        if (g_tftpd_num_write > 0)
        {
            g_tftpd_num_write --;
        }
    }

    if (s->sock >= 0)
    {
        close(s->sock);
        s->sock = -1;
    }

    memset(s, 0, sizeof(*s));
    s->state = TFTPD_STATE_FREE;
    s->sock = -1;
}

void tftpd_close_all_sessions(void)
{
    int i;
    for (i = 0; i< TFTPD_MAX_SESSIONS; i++)
    {
        tftpd_close_session(i);
    }
}

static int tftpd_parse_request(char *buf, int len, char *filename, uint16_t *blksize, uint8_t *winsz, int *has_opts)
{
    char *p = buf + 2;
    int remaining = len - 2;
    size_t field_len;

    *blksize  = TFTPD_DEFAULT_BLKSIZE;
    *winsz    = TFTPD_DEFAULT_WINSZ;
    *has_opts = 0;

    if (remaining <= 0) return -1;

    field_len = strnlen(p, remaining);
    if (field_len == remaining || field_len == 0 || field_len >= TFTPD_MAX_FILENAME)
        return -1;

    memcpy(filename, p, field_len);
    filename[field_len] = '\0';

    p += field_len + 1;
    remaining -= (field_len + 1);

    field_len = strnlen(p, remaining);
    if (field_len == remaining)
        return -1;

    p += field_len + 1;
    remaining -= (field_len + 1);

    while (remaining > 0) {
        char *opt_str, *val_str;
        unsigned long val;

        field_len = strnlen(p, remaining);
        if (field_len == remaining) 
            break;
        opt_str = p;

        p += field_len + 1;
        remaining -= (field_len + 1);
        if (remaining <= 0)
            break;

        field_len = strnlen(p, remaining);
        if (field_len == remaining)
            break;
        val_str = p;

        p += field_len + 1;
        remaining -= (field_len + 1);

        val = (unsigned long)atoi(val_str);

        if (strcasecmp(opt_str, "blksize") == 0) {
            if (val >= 8) {
                *blksize  = (uint16_t)(val > TFTPD_MAX_BLKSIZE ? TFTPD_MAX_BLKSIZE : val);
                *has_opts = 1;
            }
        } else if (strcasecmp(opt_str, "windowsize") == 0) {
            if (val >= 1) {
                *winsz    = (uint8_t)(val > TFTPD_MAX_WINSZ ? TFTPD_MAX_WINSZ : val);
                *has_opts = 1;
            }
        }
    }

    return 0;
}

static void tftpd_make_path(const char *filename, char *out, size_t out_size)
{
    while (*filename == '/')
        filename++;

    while (filename[0] == '.' && filename[1] == '.' &&
            (filename[2] == '/' || filename[2] == '\0')) {
        filename += 2;
        while (*filename == '/')
            filename++;
    }

    snprintf(out, out_size, TFTPD_ROOT_PATH "/%s", filename);
}

static int tftpd_send_window(int idx)
{
    tftpd_session_t   *s   = &g_tftpd_sessions[idx];
    char               pkt_buf[TFTPD_MAX_PKT_BUF];
    tftp_data_t       *pkt = (tftp_data_t *)pkt_buf;
    struct sockaddr_in to;
    FILE              *fp   = NULL;
    char               fpath[TFTPD_MAX_FILENAME + sizeof(TFTPD_ROOT_PATH) + 2];
    int                i, read_len;
    ssize_t            sent;

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = s->client_ip;
    to.sin_port        = s->client_port;

    tftpd_make_path(s->filename, fpath, sizeof(fpath));

    if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        tftpd_send_error(idx, TFTP_ERR_ACCESS,"File system temporarily unavailable");
        return -1;
    }

    fp = fopen(fpath, "r");             /* replaces file_open(name, "r", NULL) */
    if (fp == NULL) 
    {
        xSemaphoreGive(g_tftpd_fs_mutex); /* replaces exit_filesys(OPEN_READ) */
        tftpd_send_error(idx, TFTP_ERR_NOTFOUND, "File not found");
        return -1;
    }

    if (s->file_pos_win > 0) 
    {
        if (fseek(fp, (long)s->file_pos_win, SEEK_SET) != 0) 
        {
            fclose(fp);
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "File seek error");
            return -1;
        }
    }

    s->bytes_this_win = 0;
    s->is_last        = 0;

    for (i = 0; i < s->windowsize && !s->is_last; i++) 
    {
        uint16_t block = (uint16_t)(s->window_start + i);

        pkt->opcode    = htons(TFTP_OP_DATA);
        pkt->block_num = htons(block);

        read_len = (int)fread(pkt->data, 1, s->blksize, fp);
        if (ferror(fp)) 
        {
            fclose(fp);
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "File read error");
            return -1;
        }

        s->bytes_this_win += (uint32_t)read_len;

        if (read_len < (int)s->blksize)
            s->is_last = 1;

        sent = sendto(s->sock, pkt_buf, (size_t)(sizeof(tftp_data_t) + read_len), 0, (struct sockaddr *)&to, sizeof(to));
        if (sent < 0) 
        {
            fclose(fp);
            xSemaphoreGive(g_tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "Network send error");
            return -1;
        }

        s->last_sent = block;
    }

    fclose(fp);                      
    xSemaphoreGive(g_tftpd_fs_mutex); 

    s->timeout_sec  = g_tftpd_cfg.timeout;
    s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

    return 0;
}

void tftpd_handle_listen_pkt(void)
{
    tftpd_session_t *s;
    char buf[TFTPD_MAX_PKT_BUF + 32] = {0};
    char filename[TFTPD_MAX_FILENAME] = {0};
    char oack_buf[TFTPD_MAX_OACK_BUF] = {0};
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    ssize_t len = -1;
    uint16_t opcode;
    uint16_t blksize;
    uint8_t winsz;
    uint8_t is_write;
    int has_opts;
    int idx;
    int sock;
    int oack_len;
    struct sockaddr_in to;

    len = recvfrom(g_tftpd_listen_sock, buf, sizeof(buf),0, (struct sockaddr *)&cli_addr, &cli_len);
    if (len < 4)
    {
        return;
    }

    if (!g_tftpd_cfg.enabled)
    {
        return;
    }

    opcode = ntohs(*(uint16_t *)buf);
    if (opcode != TFTP_OP_RRQ && opcode != TFTP_OP_WRQ)
    {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr, TFTP_ERR_ILLEGAL, "Only RRQ/WRQ allowed on this port");
        return;
    }

    is_write = (opcode == TFTP_OP_WRQ)? 1 : 0;

    if (tftpd_parse_request(buf, len, filename, &blksize,&winsz, &has_opts) != 0)
    {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr, TFTP_ERR_ILLEGAL, "Malformed request");
        return;
    }

    idx = tftpd_alloc_session(is_write);
    if (idx < 0) {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr, TFTP_ERR_UNDEF, is_write ? "Write session already active" : "Max concurrent read sessions reached");
        return;
    }

    sock = tftpd_open_session_sock();
    if (sock < 0) {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr,
                TFTP_ERR_UNDEF, "No socket resources");
        return;
    }

    s = &g_tftpd_sessions[idx];
    memset(s, 0, sizeof(*s));
    s->sock         = sock;
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

    if (!is_write) 
    {
        g_tftpd_num_read++;
        s->window_start = 1;
        s->last_ack     = 0;
        s->file_pos_win = 0;

        if (has_opts) 
        {
            oack_len = tftpd_build_oack(oack_buf, blksize, winsz);
            sendto(s->sock, oack_buf, (size_t)oack_len,
                    0, (struct sockaddr *)&to, sizeof(to));
            s->state = TFTPD_STATE_OACK_RRQ;

        } 
        else 
        {
            s->state = TFTPD_STATE_RRQ;
            if (tftpd_send_window(idx) != 0)
                return;

        }

        ESP_LOGI(TAG, "RRQ session[%d] client=%s:%u file=%s blksize=%u winsz=%u", idx, inet_ntoa(cli_addr.sin_addr), (unsigned)ntohs(cli_addr.sin_port), filename, (unsigned)blksize, (unsigned)winsz);


    }
    else 
    {
        g_tftpd_num_write++;
        s->expected_block = 1;

        if (has_opts) 
        { 
            oack_len = tftpd_build_oack(oack_buf, blksize, winsz);
            sendto(s->sock, oack_buf, (size_t)oack_len, 0, (struct sockaddr *)&to, sizeof(to));
            s->state = TFTPD_STATE_OACK_WRQ;
        } 
        else 
        {
            s->state = TFTPD_STATE_WRQ;
            tftpd_send_ack(idx, 0);
        }

        ESP_LOGI(TAG, "WRQ session[%d] client=%s:%u file=%s blksize=%u winsz=%u", idx, inet_ntoa(cli_addr.sin_addr), (unsigned)ntohs(cli_addr.sin_port), filename, (unsigned)blksize, (unsigned)winsz);
    }

}

void tftpd_handle_session_pkt(int idx)
{
    tftpd_session_t *s = &g_tftpd_sessions[idx];
    char buf[TFTPD_MAX_PKT_BUF] = {0};
    struct sockaddr_in cli_addr;
    socklen_t from_len = sizeof(cli_addr);
    ssize_t len;
    uint16_t opcode, block;
    FILE *fp;
    char fpath[TFTPD_MAX_FILENAME + sizeof(TFTPD_ROOT_PATH) + 2] = {0};
    uint32_t offset;
    int write_len;

    len = recvfrom(s->sock, buf, sizeof(buf), 0, (struct sockaddr*)&cli_addr, &from_len);
    if (len < 2)
    {
        return;
    }

    if (cli_addr.sin_addr.s_addr != s->client_ip || cli_addr.sin_port != s->client_port)
    {
        tftpd_send_error_to (s->sock, &cli_addr, TFTP_ERR_BADTID, "Unknow transfer id");
        return;
    }

    opcode = ntohs(*(uint16_t *)buf);

    if (opcode == TFTP_OP_ERROR)
    {
        ESP_LOGW(TAG, "session[%d] received ERROR from client - closing" , idx);
        tftpd_close_session(idx);
        return;
    }

    if (s->state == TFTPD_STATE_DALLY)
    {
        if (opcode == TFTP_OP_DATA)
        {
            block = ntohs(((tftp_data_t *)buf)->block_num);
            if (block == s->expected_block - 1)
            {
                tftpd_send_ack(idx, block);
            }
        }
        return;
    }

    switch (s->state) 
    {

        case TFTPD_STATE_OACK_RRQ:
            if (opcode != TFTP_OP_ACK)
                break;
            block = ntohs(((tftp_ack_t *)buf)->block_num);
            if (block != 0)
                break;

            s->state = TFTPD_STATE_RRQ;
            tftpd_send_window(idx);
            break;

        case TFTPD_STATE_RRQ:
            if (opcode != TFTP_OP_ACK)
                break;

            block = ntohs(((tftp_ack_t *)buf)->block_num);

            if (block < (uint16_t)(s->window_start - 1))
                break;

            s->timeout_sec  = g_tftpd_cfg.timeout;
            s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

            if (block == s->last_sent) 
            {
                if (s->is_last) 
                {
                    ESP_LOGI(TAG, "session[%d] RRQ complete", idx);
                    tftpd_close_session(idx);
                    return;
                }
                s->last_ack      = block;
                s->file_pos_win += s->bytes_this_win;
                s->window_start  = (uint16_t)(block + 1);
                tftpd_send_window(idx);

            } 
            else if (block >= s->window_start && block <  s->last_sent) 
            {
                uint16_t acked_in_win = (uint16_t)(block - (s->window_start - 1));
                s->file_pos_win += (uint32_t)acked_in_win * s->blksize;
                s->window_start  = (uint16_t)(block + 1);
                s->last_ack      = block;
                tftpd_send_window(idx);
            }
            break;

        case TFTPD_STATE_OACK_WRQ:
            if (opcode != TFTP_OP_DATA)
                break;
            block = ntohs(((tftp_data_t *)buf)->block_num);
            if (block != 1)
                break;

            s->state = TFTPD_STATE_WRQ;

        case TFTPD_STATE_WRQ:
            if (opcode != TFTP_OP_DATA)
                break;

            block     = ntohs(((tftp_data_t *)buf)->block_num);
            write_len = (int)len - (int)sizeof(tftp_data_t);
            if (write_len < 0)
                write_len = 0;

            if (block == s->expected_block - 1) 
            {
                tftpd_send_ack(idx, block);
                break;
            }

            if (block != s->expected_block)
                break;

            offset = (uint32_t)(block - 1) * s->blksize;

            tftpd_make_path(s->filename, fpath, sizeof(fpath));

            if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) 
            {
                tftpd_send_error(idx, TFTP_ERR_ACCESS, "File system temporarily unavailable");
                return;
            }

            if (block == 1 && !s->file_created) 
            {
                fp = fopen(fpath, "w");
                s->file_created = 1;
            } 
            else 
            {
                fp = fopen(fpath, "r+");
            }

            if (fp == NULL) 
            {
                xSemaphoreGive(g_tftpd_fs_mutex);
                tftpd_send_error(idx, TFTP_ERR_ACCESS, "Cannot open file for write");
                return;
            }

            if (offset > 0) 
            {
                if (fseek(fp, (long)offset, SEEK_SET) != 0) 
                {
                    fclose(fp);
                    xSemaphoreGive(g_tftpd_fs_mutex);
                    tftpd_send_error(idx, TFTP_ERR_UNDEF, "File seek error");
                    return;
                }
            }

            if (write_len > 0) 
            {
                const uint8_t *data_ptr = (const uint8_t *)(buf + sizeof(tftp_data_t));

                if (fwrite(data_ptr, 1, (size_t)write_len, fp) != (size_t)write_len) 
                {
                    fclose(fp);
                    xSemaphoreGive(g_tftpd_fs_mutex);
                    tftpd_send_error(idx, TFTP_ERR_DISKFULL, "File write failed");
                    return;
                }
            }

            fclose(fp);
            xSemaphoreGive(g_tftpd_fs_mutex);

            s->expected_block++;
            s->timeout_sec  = g_tftpd_cfg.timeout;
            s->retry_remain = (uint8_t)(g_tftpd_cfg.retry - 1);

            tftpd_send_ack(idx, block);

            if (write_len < (int)s->blksize) 
            {
                ESP_LOGI(TAG, "session[%d] WRQ complete", idx);
                s->state        = TFTPD_STATE_DALLY;
                s->dally_remain = TFTPD_DALLY_TICKS;
            }
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

        /* ── Dally countdown ──────────────────────────────────────── */
        if (s->state == TFTPD_STATE_DALLY) {
            if (s->dally_remain > 0)
                s->dally_remain--;
            if (s->dally_remain == 0)
                tftpd_close_session(i);
            continue;
        }

        /* ── Retransmit countdown ─────────────────────────────────── */
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
                /* Retransmit the OACK */
                oack_len = tftpd_build_oack(oack_buf, s->blksize, s->windowsize);
                /* was: so_sendto(s->sock, oack_buf, …) */
                sendto(s->sock, oack_buf, (size_t)oack_len,
                        0, (struct sockaddr *)&to, sizeof(to));
                break;

            case TFTPD_STATE_RRQ:
                /* Retransmit the current window from window_start */
                tftpd_send_window(i);
                break;

            case TFTPD_STATE_WRQ:
                /* Retransmit the last ACK */
                tftpd_send_ack(i, (uint16_t)(s->expected_block - 1));
                break;

            default:
                break;
        }
    }
}
