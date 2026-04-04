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

void tftpd_handle_listen_pkt(void)
{
    char buf[TFTPD_MAX_PKT_BUF + 32];
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    ssize_t len = -1;
    uint16_t opcode;
    uint8_t is_write;

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
}

void tftpd_handle_session_pkt(int idx)
{

}

void tftpd_handle_timer(void)
{

}


