#pragma once
#include <infiniband/verbs.h>
#include <stdint.h>

struct qp_info {
    uint32_t qp_num;
    //uint16_t lid;
   // union ibv_gid gid;
    uint8_t  gid[16];
};
