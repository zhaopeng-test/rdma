#pragma once
#include <infiniband/verbs.h>
#include <stdint.h>

struct qp_info {
    uint32_t qp_num;
    uint32_t rkey;
    uint64_t addr;
    uint8_t  gid[16];
};