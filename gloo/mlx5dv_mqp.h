#pragma once


#include <infiniband/verbs_exp.h>

#define GID_INDEX 3

enum cd_statuses{
	PCOLL_SUCCESS = 0,
	PCOLL_ERROR = 1
};

typedef struct peer_addr {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
} peer_addr_t;

int hmca_bcol_cc_mq_destroy(struct ibv_qp *mq);
struct ibv_qp* hmca_bcol_cc_mq_create(struct ibv_cq *cq, struct ibv_pd *pd, 
			   struct ibv_context *ctx, uint16_t send_wq_size);

struct ibv_qp* rc_qp_create(struct ibv_cq *cq, struct ibv_pd *pd, struct ibv_context *ctx, uint16_t send_wq_size, uint16_t recv_rq_size, int slaveRecv, int slaveSend, struct ibv_cq *s_cq = NULL );
int rc_qp_get_addr(struct ibv_qp *qp, peer_addr_t *addr);
int rc_qp_connect(peer_addr_t *addr, struct ibv_qp *qp);

struct ibv_cq* cd_create_cq(struct ibv_context *context, int cqe, void *cq_context, struct ibv_comp_channel *channel, int comp_vector);
