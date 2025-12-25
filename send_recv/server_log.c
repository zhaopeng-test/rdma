#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_common.h"

#define PORT 18515
#define MSG_SIZE 1024

#define LOG_INFO(fmt, ...) \
    fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)

#define LOG_ERR(fmt, ...) \
    fprintf(stderr, "[ERROR] " fmt " (errno=%d: %s)\n", \
            ##__VA_ARGS__, errno, strerror(errno))

#define CHECK_NULL(ptr, msg) \
    if (!(ptr)) { LOG_ERR("%s failed", msg); exit(EXIT_FAILURE); }

#define CHECK_RET(ret, msg) \
    if ((ret)) { LOG_ERR("%s failed, ret=%d", msg, ret); exit(EXIT_FAILURE); }

int main() {
    int ret;

    LOG_INFO("Get IB device list");
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    CHECK_NULL(dev_list, "ibv_get_device_list");

    LOG_INFO("Open IB device: %s", ibv_get_device_name(dev_list[0]));
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "ibv_open_device");

    LOG_INFO("Alloc PD");
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "ibv_alloc_pd");

    LOG_INFO("Create CQ");
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    CHECK_NULL(cq, "ibv_create_cq");

    LOG_INFO("Create QP");
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
    CHECK_NULL(qp, "ibv_create_qp");

    LOG_INFO("QP created, qp_num=%u", qp->qp_num);

    /* INIT */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE
    };
    ret = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    CHECK_RET(ret, "QP to INIT");
    LOG_INFO("QP -> INIT");

    /* TCP socket */
    LOG_INFO("Create TCP socket");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_RET(sock < 0, "socket");

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    LOG_INFO("Bind TCP port %d", PORT);
    CHECK_RET(bind(sock, (struct sockaddr*)&addr, sizeof(addr)), "bind");

    LOG_INFO("Listen TCP");
    CHECK_RET(listen(sock, 1), "listen");

    LOG_INFO("Waiting for client...");
    int conn = accept(sock, NULL, NULL);
    CHECK_RET(conn < 0, "accept");
    LOG_INFO("Client connected");

    /* 交换 QP 信息 */
    struct qp_info local = {0}, remote = {0};
    local.qp_num = qp->qp_num;
    local.lid = 0; // SoftRoCE

    LOG_INFO("Send local QP info (qp_num=%u)", local.qp_num);
    write(conn, &local, sizeof(local));

    LOG_INFO("Receive remote QP info");
    read(conn, &remote, sizeof(remote));
    LOG_INFO("Remote qp_num=%u", remote.qp_num);

    /* RTR */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.port_num = 1;

    ret = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV |
        IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
        IBV_QP_MIN_RNR_TIMER);
    CHECK_RET(ret, "QP to RTR");
    LOG_INFO("QP -> RTR");

    /* RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;

    ret = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    CHECK_RET(ret, "QP to RTS");
    LOG_INFO("QP -> RTS");

    /* Buffer */
    LOG_INFO("Allocate buffer & register MR");
    char *buf = calloc(1, MSG_SIZE);
    CHECK_NULL(buf, "malloc");

    struct ibv_mr *mr = ibv_reg_mr(
        pd, buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr, "ibv_reg_mr");

    LOG_INFO("MR registered, lkey=0x%x", mr->lkey);

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

    LOG_INFO("Post RECV");
    CHECK_RET(ibv_post_recv(qp, &wr, &bad), "ibv_post_recv");

    LOG_INFO("Waiting for CQ event...");
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) == 0);

    if (wc.status != IBV_WC_SUCCESS) {
        LOG_ERR("WC error, status=%s", ibv_wc_status_str(wc.status));
        exit(EXIT_FAILURE);
    }

    LOG_INFO("RECV completed, byte_len=%u", wc.byte_len);
    printf("Server received: %s\n", buf);

    return 0;
}

