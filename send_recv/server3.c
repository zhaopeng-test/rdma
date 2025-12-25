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

#define LOGI(fmt, ...) printf("[SERVER][INFO] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("[SERVER][ERR ] " fmt " (errno=%d:%s)\n", \
                             ##__VA_ARGS__, errno, strerror(errno))

#define CHECK_RET(ret, msg) \
    do { if (ret) { LOGE("%s failed", msg); exit(1); } } while (0)

#define CHECK_PTR(ptr, msg) \
    do { if (!(ptr)) { LOGE("%s failed", msg); exit(1); } } while (0)

static void dump_gid(const char *tag, union ibv_gid *gid) {
    printf("[SERVER][INFO] %s GID = ", tag);
    for (int i = 0; i < 16; i++)
        printf("%02x%s", gid->raw[i], (i == 15) ? "" : ":");
    printf("\n");
}

int main() {
    int ret;

    /* ===== verbs init ===== */
    LOGI("Get IB device list");
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    CHECK_PTR(dev_list, "ibv_get_device_list");

    LOGI("Open device %s", ibv_get_device_name(dev_list[0]));
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    CHECK_PTR(ctx, "ibv_open_device");

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    CHECK_PTR(pd, "ibv_alloc_pd");

    struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    CHECK_PTR(cq, "ibv_create_cq");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1
        }
    };

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init);
    CHECK_PTR(qp, "ibv_create_qp");
    LOGI("QP created, qp_num=%u", qp->qp_num);

    /* ===== QP INIT ===== */
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .port_num        = 1,
        .pkey_index      = 0,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE
    };

    ret = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PORT |
        IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS);
    CHECK_RET(ret, "QP to INIT");
    LOGI("QP -> INIT");

    /* ===== query local GID ===== */
    union ibv_gid local_gid;
    ret = ibv_query_gid(ctx, 1, 0, &local_gid);
    CHECK_RET(ret, "ibv_query_gid");
    dump_gid("Local", &local_gid);

    /* ===== TCP listen ===== */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_RET(sock < 0, "socket");

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(sock, 1);

    LOGI("Waiting for client...");
    int conn = accept(sock, NULL, NULL);
    CHECK_RET(conn < 0, "accept");
    LOGI("Client connected");

    /* ===== exchange QP info ===== */
    struct qp_info local = {0}, remote = {0};
    local.qp_num = qp->qp_num;
    local.gid    = local_gid;

    write(conn, &local, sizeof(local));
    read(conn, &remote, sizeof(remote));

    LOGI("Remote qp_num=%u", remote.qp_num);
    dump_gid("Remote", &remote.gid);

    /* ===== RTR (RoCE v2 必须 GRH) ===== */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = IBV_MTU_1024;
    attr.dest_qp_num        = remote.qp_num;
    attr.rq_psn             = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;

    attr.ah_attr.is_global      = 1;
    attr.ah_attr.port_num       = 1;
    attr.ah_attr.grh.hop_limit  = 1;
    attr.ah_attr.grh.dgid       = remote.gid;
    attr.ah_attr.grh.sgid_index = 0;

    ret = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV |
        IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC |
        IBV_QP_MIN_RNR_TIMER);
    CHECK_RET(ret, "QP to RTR");
    LOGI("QP -> RTR");

    /* ===== RTS ===== */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;

    ret = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN |
        IBV_QP_MAX_QP_RD_ATOMIC);
    CHECK_RET(ret, "QP to RTS");
    LOGI("QP -> RTS");

    /* ===== RECV ===== */
    char *buf = calloc(1, MSG_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_PTR(mr, "ibv_reg_mr");

    struct ibv_sge sge = {
        .addr   = (uintptr_t)buf,
        .length = MSG_SIZE,
        .lkey   = mr->lkey
    };

    struct ibv_recv_wr wr = {
        .sg_list = &sge,
        .num_sge = 1
    };

    struct ibv_recv_wr *bad;
    ibv_post_recv(qp, &wr, &bad);
    LOGI("Posted RECV, waiting for data...");

    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) == 0);

    LOGI("RECV completed, status=%s, len=%u",
         ibv_wc_status_str(wc.status), wc.byte_len);

    LOGI("Message: %s", buf);
    return 0;
}

