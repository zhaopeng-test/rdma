
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_common.h"

#define PORT 18515
#define MSG_SIZE 1024

#define LOG(fmt, ...)  printf("[SERVER] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  printf("[SERVER][ERR] " fmt " (errno=%d:%s)\n", ##__VA_ARGS__, errno, strerror(errno))

int main() {
    LOG("Start");

    /* ---------- RDMA device ---------- */
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);

    struct ibv_qp_init_attr qpia = {
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
    struct ibv_qp *qp = ibv_create_qp(pd, &qpia);
    LOG("QP created qpn=%u", qp->qp_num);

    /* ---------- INIT ---------- */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .port_num = 1,
        .pkey_index = 0,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
    };
    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_PORT |
        IBV_QP_PKEY_INDEX |
        IBV_QP_ACCESS_FLAGS);
    LOG("QP -> INIT");

    /* ---------- TCP ---------- */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(sock, 1);
    int conn = accept(sock, NULL, NULL);
    LOG("TCP connected");

    /* ---------- exchange QP info ---------- */

    char *buf = calloc(1, MSG_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        ERR("ibv_reg_mr failed");
        return 1;
    }
    strcpy(buf, "Hello from Server via RDMA READ");
    sleep(1);
    
    struct qp_info local = {}, remote = {};
    local.qp_num = qp->qp_num;
    local.addr = (uintptr_t)buf;
    local.rkey = mr->rkey;

    union ibv_gid gid;
    ibv_query_gid(ctx, 1, 1, &gid);   // 使用 GID[1]（IPv4）
    memcpy(local.gid, &gid, 16);

    write(conn, &local, sizeof(local));
    read(conn, &remote, sizeof(remote));
    LOG("Remote qpn=%u", remote.qp_num);


    /* ---------- RTR ---------- */
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

    /* ---------- RTS ---------- */
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


    sleep(3);
    //server no recv CQ
    LOG("Server Received: %s", buf);
 
    return 0;
}
