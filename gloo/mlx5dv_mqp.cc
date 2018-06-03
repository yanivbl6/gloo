
#include "mlx5dv_mqp.h"

int hmca_bcol_cc_mq_destroy(struct ibv_qp *mq) {
    int rc;
    rc = ibv_destroy_qp(mq);
    if (rc) {
        return HCOLL_ERROR;
    }
    return rc;
}

struct ibv_qp* hmca_bcol_cc_mq_create(struct ibv_cq *cq, struct ibv_pd *pd, struct ibv_context *ctx, uint16_t send_wq_size){
			   
    int rc = HCOLL_SUCCESS;
    struct ibv_exp_qp_init_attr init_attr;
    struct ibv_qp_attr attr;
    struct ibv_qp *_mq;


    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.qp_context = NULL;
    init_attr.send_cq = cq;
    init_attr.recv_cq = cq;
    init_attr.srq = NULL;
    init_attr.cap.max_send_wr  = send_wq_size;
    init_attr.cap.max_recv_wr  = 0;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = 0;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.sq_sig_all = 0;
    init_attr.pd = pd;
    init_attr.comp_mask        = IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
    init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
        IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
        IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;
;
   _mq = ibv_exp_create_qp(ctx, &init_attr);

    if (NULL == _mq) {
        rc = HCOLL_ERROR;
    }

    if (rc == HCOLL_SUCCESS) {
        attr.qp_state        = IBV_QPS_INIT;
        attr.pkey_index      = 0;
        attr.port_num        = 1;
        attr.qp_access_flags = 0;

        rc = ibv_modify_qp(_mq, &attr,
                           IBV_QP_STATE              |
                           IBV_QP_PKEY_INDEX         |
                           IBV_QP_PORT               |
                           IBV_QP_ACCESS_FLAGS);
        if (rc) {
            rc = HCOLL_ERROR;
        }
    }

    if (rc == HCOLL_SUCCESS) {
        memset(&attr, 0, sizeof(attr));
        attr.qp_state              = IBV_QPS_RTR;
        attr.path_mtu              = IBV_MTU_1024;
        attr.dest_qp_num           = _mq->qp_num;
        attr.rq_psn                = 0;
        attr.max_dest_rd_atomic    = 1;
        attr.min_rnr_timer         = 12;
        attr.ah_attr.is_global     = 0;
        attr.ah_attr.dlid          = 0;
        attr.ah_attr.sl            = 0;
        attr.ah_attr.src_path_bits = 0;
        attr.ah_attr.port_num      = 1;

        rc = ibv_modify_qp(_mq, &attr,
                           IBV_QP_STATE              |
                           IBV_QP_AV                 |
                           IBV_QP_PATH_MTU           |
                           IBV_QP_DEST_QPN           |
                           IBV_QP_RQ_PSN             |
                           IBV_QP_MAX_DEST_RD_ATOMIC |
                           IBV_QP_MIN_RNR_TIMER);

        if (rc) {
            rc = HCOLL_ERROR;
        }
    }

    if (rc == HCOLL_SUCCESS) {
        attr.qp_state      = IBV_QPS_RTS;
        attr.timeout       = 14;
        attr.retry_cnt     = 7;
        attr.rnr_retry     = 7;
        attr.sq_psn        = 0;
        attr.max_rd_atomic = 1;
        rc = ibv_modify_qp(_mq, &attr,
                           IBV_QP_STATE              |
                           IBV_QP_TIMEOUT            |
                           IBV_QP_RETRY_CNT          |
                           IBV_QP_RNR_RETRY          |
                           IBV_QP_SQ_PSN             |
                           IBV_QP_MAX_QP_RD_ATOMIC);
        if (rc) {
            rc = HCOLL_ERROR;
        }
    }
    return _mq;
}

struct ibv_qp* rc_qp_create(struct ibv_cq *cq, struct ibv_pd *pd, struct ibv_context *ctx, uint16_t send_wq_size, uint16_t recv_rq_size, int slaveRecv, int slaveSend){
	struct ibv_exp_qp_init_attr init_attr;
	struct ibv_qp_attr attr;
	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.qp_context = NULL;
	init_attr.send_cq = cq;
	init_attr.recv_cq = cq;
	init_attr.cap.max_send_wr = send_wq_size;
	init_attr.cap.max_recv_wr = recv_rq_size;
	init_attr.cap.max_send_sge = 1;
	init_attr.cap.max_recv_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.pd = pd;
	init_attr.comp_mask        = IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
	init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
								 IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
								 IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;

	if (slaveSend){
		init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_SEND;
	}

	if (slaveRecv){
		init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_RECV;
	}

	struct ibv_qp *qp;
	return ibv_exp_create_qp(ctx, &init_attr);
}

int rc_qp_get_addr(struct ibv_qp *qp, peer_addr_t *addr)
{
	struct ibv_port_attr attr;
	if (ibv_query_port(qp->context, qp->ib_port, &attr)) {
		fprintf(stderr, "Couldn't get port info\n");
		return 1; // TODO: indicate error?
	}

	addr->lid = attr.lid;
	addr->qpn = qp->qp_num;
	addr->psn = 0x1234;
}

int rc_qp_connect(peer_addr_t *addr, struct ibv_qp *qp)
{
	struct ibv_qp_attr attr = {
			.qp_state		= IBV_QPS_RTR,
			.path_mtu		= mtu,
			.dest_qp_num		= dest->qpn,
			.rq_psn			= dest->psn,
			.max_dest_rd_atomic	= 1,
			.min_rnr_timer		= 12,
			.ah_attr		= {
					.is_global	= 0,
					.dlid		= dest->lid,
					.sl		= sl,
					.src_path_bits	= 0,
					.port_num	= port
			}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			IBV_QP_STATE              |
			IBV_QP_AV                 |
			IBV_QP_PATH_MTU           |
			IBV_QP_DEST_QPN           |
			IBV_QP_RQ_PSN             |
			IBV_QP_MAX_DEST_RD_ATOMIC |
			IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			IBV_QP_STATE              |
			IBV_QP_TIMEOUT            |
			IBV_QP_RETRY_CNT          |
			IBV_QP_RNR_RETRY          |
			IBV_QP_SQ_PSN             |
			IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

