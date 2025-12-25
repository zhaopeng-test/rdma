#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_common.h"

#define PORT 18515
#define MSG_SIZE 1024

int main() {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1
        }
    };

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);

    /* INIT */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE
    };
    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

    /* TCP socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(sock, 1);
    int conn = accept(sock, NULL, NULL);

    /* 交换 QP 信息 */
    struct qp_info local, remote;
    local.qp_num = qp->qp_num;
    local.lid = 0;  // SoftRoCE lid 固定为 0
    write(conn, &local, sizeof(local));
    read(conn, &remote, sizeof(remote));

    /* RTR */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.port_num = 1;

    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV |
        IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC |
        IBV_QP_MIN_RNR_TIMER);

    /* RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;

    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);

    /* Buffer */
    char *buf = malloc(MSG_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(
        pd, buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);

    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = MSG_SIZE,
        .lkey = mr->lkey
    };

    struct ibv_recv_wr wr = {
        .sg_list = &sge,
        .num_sge = 1
    };
    struct ibv_recv_wr *bad;
    ibv_post_recv(qp, &wr, &bad);

    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) == 0);
    printf("Server received: %s\n", buf);

    return 0;
}

