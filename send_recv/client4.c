#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_common.h"

#define PORT 18515
#define MSG "Hello from Client"

#define LOG(fmt, ...)  printf("[CLIENT] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  printf("[CLIENT][ERR] " fmt " (errno=%d:%s)\n", ##__VA_ARGS__, errno, strerror(errno))

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <server_ip>\n", argv[0]);
        return 1;
    }

    LOG("Start");

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);

    struct ibv_qp_init_attr qpia = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 10, .max_send_sge = 1}
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qpia);
    LOG("QP created qpn=%u", qp->qp_num);

    /* INIT */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .port_num = 1,
        .pkey_index = 0,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE
    };
    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_PORT |
        IBV_QP_PKEY_INDEX |
        IBV_QP_ACCESS_FLAGS);
    LOG("QP -> INIT");

    /* TCP */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT)
    };
    inet_pton(AF_INET, argv[1], &addr.sin_addr);
    connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    LOG("TCP connected");

    /* exchange QP */
    struct qp_info local = {}, remote = {};
    local.qp_num = qp->qp_num;

    union ibv_gid gid;
    ibv_query_gid(ctx, 1, 1, &gid);
    memcpy(local.gid, &gid, 16);

    read(sock, &remote, sizeof(remote));
    write(sock, &local, sizeof(local));
    LOG("Remote qpn=%u", remote.qp_num);

    /* RTR */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;

    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = 1;
    attr.ah_attr.grh.hop_limit = 64;
    attr.ah_attr.grh.sgid_index = 1;
    memcpy(&attr.ah_attr.grh.dgid, remote.gid, 16);

    if (ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_AV |
        IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC |
        IBV_QP_MIN_RNR_TIMER)) {
        ERR("QP to RTR failed");
        return 1;
    }
    LOG("QP -> RTR");

    /* RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;

    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN |
        IBV_QP_MAX_QP_RD_ATOMIC);
    LOG("QP -> RTS");

    /* SEND */
    char *buf = strdup(MSG);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, 1024, 0);

    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = strlen(buf) + 1,
        .lkey = mr->lkey
    };
    struct ibv_send_wr wr = {
        .opcode = IBV_WR_SEND,
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED
    };
    struct ibv_send_wr *bad;
    ibv_post_send(qp, &wr, &bad);

    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) == 0);
    LOG("Send done");

    return 0;
}
