#include "tftpd.h"

static const char *TAG = "tftpd";

int tftpd_open_listen(uint16_t port)
{
    struct sockaddr_in serv_addr_in;
    int sock = -1, opt, ret_val;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket creation failed with error code : %d\n", strerror(errno));
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

void tftpd_close_listen(void)
{

}

void tftpd_close_session(int idx)
{

}
void tftpd_close_all_sessions(void)
{

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

    /* ── Acquire file-system mutex (replaces enter_filesys(OPEN_READ)) ──── */
    if (xSemaphoreTake(g_tftpd_fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        tftpd_send_error(idx, TFTP_ERR_ACCESS,
                "File system temporarily unavailable");
        return -1;
    }

    fp = fopen(fpath, "r");             /* replaces file_open(name, "r", NULL) */
    if (fp == NULL) {
        xSemaphoreGive(g_tftpd_fs_mutex); /* replaces exit_filesys(OPEN_READ) */
        tftpd_send_error(idx, TFTP_ERR_NOTFOUND, "File not found");
        return -1;
    }

    /* Seek to the window-start position */
    if (s->file_pos_win > 0) {
        /* replaces file_seek(fp, offset, 0 /SEEK_SET/) */
        if (fseek(fp, (long)s->file_pos_win, SEEK_SET) != 0) {
            fclose(fp);                 /* replaces file_close(fp) */
            xSemaphoreGive(tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "File seek error");
            return -1;
        }
    }

    s->bytes_this_win = 0;
    s->is_last        = 0;

    for (i = 0; i < s->windowsize && !s->is_last; i++) {
        uint16_t block = (uint16_t)(s->window_start + i);

        pkt->opcode    = htons(TFTP_OP_DATA);
        pkt->block_num = htons(block);

        /*
         * replaces: read_len = file_read(fp, buf, blksize)
         *
         * fread() returns items read (each 1 byte); ferror() distinguishes
         * EOF from a real I/O error.
         */
        read_len = (int)fread(pkt->data, 1, s->blksize, fp);
        if (ferror(fp)) {
            fclose(fp);
            xSemaphoreGive(tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "File read error");
            return -1;
        }

        s->bytes_this_win += (uint32_t)read_len;

        if (read_len < (int)s->blksize)
            s->is_last = 1;

        /* was: so_sendto(s->sock, pkt_buf, …) */
        sent = sendto(s->sock,
                pkt_buf,
                (size_t)(sizeof(tftp_data_t) + read_len),
                0,
                (struct sockaddr *)&to, sizeof(to));
        if (sent < 0) {
            fclose(fp);
            xSemaphoreGive(tftpd_fs_mutex);
            tftpd_send_error(idx, TFTP_ERR_UNDEF, "Network send error");
            return -1;
        }

        s->last_sent = block;
    }

    fclose(fp);                         /* replaces file_close(fp) */
    xSemaphoreGive(tftpd_fs_mutex);     /* replaces exit_filesys(OPEN_READ) */

    /* Arm the retransmit timer for this window */
    s->timeout_sec  = tftpd_cfg.timeout;
    s->retry_remain = (uint8_t)(tftpd_cfg.retry - 1);

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
    if (opcode != TFTP_OP_RRQ || opcode != TFTP_OP_WRQ)
    {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr, TFTP_ERR_ILLEGAL, "Only RRQ/WRQ allowed on this port\n");
        return;
    }

    is_write = (opcode == TFTP_OP_WRQ)? 1 : 0;

    if (tftpd_parse_request(buf, len, buf, &blksize,&winsz, &has_opts) != 0)
    {
        tftpd_send_error_to(g_tftpd_listen_sock, &cli_addr, TFTP_ERR_ILLEGAL, "Malformed request\n");
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

        ESP_LOGI(TAG, "RRQ session[%d] client=%s:%u file=%s blksize=%u winsz=%u",
                idx, inet_ntoa(cli_addr.sin_addr), (unsigned)ntohs(cli_addr.sin_port),
                filename, (unsigned)blksize, (unsigned)winsz);


    }
}

void tftpd_handle_session_pkt(int idx)
{

}

void tftpd_handle_timer(void)
{

}


