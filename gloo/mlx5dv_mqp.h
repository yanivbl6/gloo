#pragma once

#include <infiniband/verbs_exp.h>



enum cd_statuses{
	HCOLL_SUCCESS = 0,
	HCOLL_ERROR = 1
};

int hmca_bcol_cc_mq_destroy(struct ibv_qp *mq);
struct ibv_qp* hmca_bcol_cc_mq_create(struct ibv_cq *cq, struct ibv_pd *pd, 
			   struct ibv_context *ctx, uint16_t send_wq_size);

struct ibv_qp* rc_qp_create(struct ibv_cq *cq, struct ibv_pd *pd, struct ibv_context *ctx, uint16_t send_wq_size, uint16_t recv_rq_size, int slaveRecv, int slaveSend);
