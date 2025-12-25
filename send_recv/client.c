#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSG "Hello RDMA"
#define MSG_SIZE 64

int main() {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;

    char buf[MSG_SIZE] = MSG;

    dev_list = ibv_get_device_list(NULL);
    ctx = ibv_open_device(dev_list[0]);

    pd = ibv_alloc_pd(ctx);
    cq = ibv_create_cq(ctx, 1, NULL, NULL, 0);

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 1,
            .max_recv_wr = 1,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        }
    };

    qp = ibv_create_qp(pd, &qp_init);

    mr = ibv_reg_mr(pd, buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);

    /* QP state same as server (loopback) */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .port_num = 1,
        .pkey_index = 0,
    };
    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT);

    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = qp->qp_num;
    attr.rq_psn = 0;
    attr.ah_attr.port_num = 1;

    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV |
        IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN);

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;

    ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_SQ_PSN);

    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = MSG_SIZE,
        .lkey = mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 1,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad;

    ibv_post_send(qp, &wr, &bad);
    printf("Client sent message\n");

    return 0;
}

