/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */


#pragma once

#include <stddef.h>
#include <string.h>
#include <alloca.h>

#include "gloo/algorithm.h"
#include "gloo/context.h"
#include "gloo/mlx5dv_mqp.h"
#include "gloo/mlx5dv_mgr.h"


namespace gloo {

#warning "ALLREDUCE_NEW was built"

#define IB_ACCESS_FLAGS (IBV_ACCESS_LOCAL_WRITE  | \
						 IBV_ACCESS_REMOTE_WRITE | \
						 IBV_ACCESS_REMOTE_READ)

#define RX_SIZE 16

typedef int rank_t;

typedef struct verb_ctx {
	struct ibv_context		  *context;
	struct ibv_exp_device_attr attrs;
	struct ibv_pd			  *pd;
	struct ibv_cq			  *cq;
	struct ibv_qp			  *umr_qp;
        struct ibv_comp_channel channel;

} verb_ctx_t;

typedef struct mem_registration {
	struct ibv_exp_mem_region *mem_reg;
	struct ibv_mr *umr_mr;
	unsigned mrs_cnt;
} mem_registration_t;

typedef struct rd_peer_info {
	uintptr_t  buf;
	uint32_t rkey;
	peer_addr_t addr;
} rd_peer_info_t;

typedef struct rd_peer {
	rank_t rank;
	qp_ctx *qp_cd;

	struct ibv_qp *qp;
	struct ibv_cq *cq;

	struct ibv_sge outgoing_buf;
	struct ibv_sge incoming_buf;
	struct ibv_sge remote_buf;
	rd_peer_info_t remote;

} rd_peer_t;

typedef struct rd_connections {
	struct ibv_mr result;


	struct ibv_qp *mgmt_qp;
        struct ibv_cq *mgmt_cq;

	qp_ctx *mgmt_qp_cd;

	struct ibv_qp *loopback_qp;
        struct ibv_cq *loopback_cq;

	qp_ctx *loopback_qp_cd;
	unsigned peers_cnt;
	rd_peer_t *peers;
} rd_connections_t;

template <typename T>
class AllreduceNew : public Algorithm {
public:
	AllreduceNew(
			const std::shared_ptr<Context>& context,
			const std::vector<T*>& ptrs,
			const int count,
			const ReductionFunction<T>* fn = ReductionFunction<T>::sum)
: Algorithm(context),
  ptrs_(ptrs),
  count_(count),
  bytes_(count_ * sizeof(T)),
  fn_(fn) {
		if (this->contextSize_ == 1) {
			return;
		}

		/* Step #1: Initialize verbs for all to use */
		init_verbs();

		/* Step #2: Register existing memory buffers with UMR */
		register_memory();

		/* Step #3: Connect to the (recursive-doubling) peers and pre-post operations */
		connect_and_prepare();
	}

	virtual ~AllreduceNew() {
		teardown();
		deregister_memory();
		fini_verbs();
	}

	int p2p_exchange(void* send_buf, void* recv_buf, size_t size, int peer){
		auto& pair = this->getPair(peer);
		auto slot = this->context_.nextSlot();
		auto sendBuf = pair->createSendBuffer(slot, send_buf, size);
		auto recvBuf = pair->createRecvBuffer(slot, recv_buf, size);
		sendBuf->send();
		sendBuf->waitSend();
		recvBuf->waitRecv();
	}

	void run() {
#if 0
		rd.mgmt_qp_cd->db();

		rd.mgmt_qp_cd->rearm();


		int res = 0;
		while (!res){
			res = rd.mgmt_qp_cd->poll();
		}
#endif
	}

	void init_verbs(char *ib_devname="", int port=1)
	{
		verb_ctx_t *ctx = &ibv_;
		int n;
		struct ibv_device **dev_list = ibv_get_device_list(&n);
		struct ibv_device* ib_dev;
		if (!dev_list) {
			perror("Failed to get IB devices list");
			return; // TODO indicate failure?
		}

		if (!ib_devname) {
			ib_dev = *dev_list;
			if (!ib_dev) {
				fprintf(stderr, "No IB devices found\n");
				return; // TODO indicate failure?
			}
		} else {
			int i;
			for (i = 0; dev_list[i]; ++i)
				if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
					break;
			ib_dev = dev_list[i];
			if (!ib_dev) {
				fprintf(stderr, "IB device %s not found\n", ib_devname);
				return; // TODO indicate failure?
			}
		}

		ctx->context = ibv_open_device(ib_dev);
		ibv_free_device_list(dev_list);
		if (!ctx->context) {
			fprintf(stderr, "Couldn't get context for %s\n",
					ibv_get_device_name(ib_dev));
			goto clean_buffer;
		}

		ctx->pd = ibv_alloc_pd(ctx->context);
		if (!ctx->pd) {
			fprintf(stderr, "Couldn't allocate PD\n");
			goto clean_comp_channel;
		}

		ctx->cq = ibv_create_cq(ctx->context, RX_SIZE, NULL,
				NULL, 0);
		if (!ctx->cq) {
			fprintf(stderr, "Couldn't create CQ\n");
			return; // TODO indicate failure?
		}

		if (ibv_exp_query_device(ctx->context ,&ctx->attrs)) {
			fprintf(stderr, "Couldn't query device attributes\n");
			return; // TODO indicate failure?
		}

		{
			struct ibv_exp_qp_init_attr attr = {
					.qp_context	  = ctx->context,
					.send_cq	  = ctx->cq,
					.recv_cq	  = ctx->cq,
					.srq		  = nullptr,
					.qp_type	  = IBV_QPT_UD,
					.comp_mask	  = IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS,
					.create_flags = IBV_EXP_QP_CREATE_UMR,
					.cap		  = {
							.max_send_wr  = 1,
							.max_recv_wr  = 0,
							.max_send_sge = 1,
							.max_recv_sge = 1
					}
			};

			ctx->umr_qp = ibv_exp_create_qp(ctx->context, &attr);
			if (!ctx->umr_qp)  {
				fprintf(stderr, "Couldn't create QP\n");
				return; // TODO indicate failure?
			}
		}

		{
			struct ibv_qp_attr attr;
				attr.qp_state        = IBV_QPS_INIT;
				attr.pkey_index      = 0;
				attr.port_num        = port;
				attr.qkey            = 0x11111111;

			if (ibv_modify_qp(ctx->umr_qp, &attr,
					IBV_QP_STATE              |
					IBV_QP_PKEY_INDEX         |
					IBV_QP_PORT               |
					IBV_QP_QKEY)) {
				fprintf(stderr, "Failed to modify QP to INIT\n");
				return; // TODO indicate failure?
			}
		}

		return; // SUCCESS!

		clean_qp:
		ibv_destroy_qp(ctx->umr_qp);

		clean_cq:
		ibv_destroy_cq(ctx->cq);

		clean_mr:
		ibv_dealloc_pd(ctx->pd);

		clean_comp_channel:
		ibv_close_device(ctx->context);

		return; // TODO indicate failure?
	}

	int fini_verbs()
	{
		verb_ctx_t *ctx = &ibv_;
		if (ibv_destroy_qp(ctx->qp)) {
			fprintf(stderr, "Couldn't destroy QP\n");
			return 0; // TODO indicate failure?
		}

		if (ibv_destroy_cq(ctx->cq)) {
			fprintf(stderr, "Couldn't destroy CQ\n");
			return 0; // TODO indicate failure?
		}

		if (ibv_dealloc_pd(ctx->pd)) {
			fprintf(stderr, "Couldn't deallocate PD\n");
			return 0; // TODO indicate failure?
		}

		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "Couldn't release context\n");
			return 0; // TODO indicate failure?
		}
		return 1;
	}

	struct ibv_mr *register_umr(struct ibv_exp_mem_region mem_reg, unsigned mem_reg_cnt,
			struct ibv_exp_mkey_list_container *umr_mkey)
	{
		struct ibv_exp_mr_init_attr umr_init_attr = {
				.max_klm_list_size	= mem_reg_cnt,
				.create_flags		= IBV_EXP_MR_INDIRECT_KLMS,
				.exp_access_flags	= IB_ACCESS_FLAGS
		};

		struct ibv_exp_create_mr_in umr_create_mr_in = {
				.pd		 = ibv_.pd,
				.attr		 = &umr_init_attr,
				.comp_mask = 0
		};

		struct ibv_mr *res_mr = ibv_exp_create_mr(&umr_create_mr_in);
		if (!res_mr) {
			return res_mr;
		}

		/* Create the UMR work request */
		struct ibv_exp_send_wr wr = {0}, *bad_wr;
		wr.exp_opcode						= IBV_EXP_WR_UMR_FILL;
		wr.exp_send_flags					= IBV_EXP_SEND_SIGNALED;
		wr.ext_op.umr.umr_type				= IBV_EXP_UMR_MR_LIST;
		wr.ext_op.umr.memory_objects		= umr_mkey;
		wr.ext_op.umr.modified_mr			= res_mr;
		wr.ext_op.umr.base_addr				= mem_reg[0].addr;
		wr.ext_op.umr.num_mrs				= mem_reg_cnt;
		wr.ext_op.umr.mem_list.mem_reg_list	= mem_.mem_reg;
		if (!umr_mkey) {
			wr.exp_send_flags 			   |= IBV_EXP_SEND_INLINE;
		}

		/* Post WR and wait for it to complete */
		if (ibv_exp_post_send(ibv_.qp, &wr, &bad_wr)) {
			return nullptr;
		}
		struct ibv_wc wc;
		for (;;) {
			int ret = ibv_poll_cq(ibv_.cq, 1, &wc);
			if (ret < 0) {
				return nullptr;
			}
			if (ret == 1) {
				if (wc.status != IBV_WC_SUCCESS) {
					return nullptr;
				}
				break;
			}
		}

		return res_mr;
	}

	void register_memory()
	{
		if (count_ > ibv_.attrs.umr_caps.max_klm_list_size) {
			return; // TODO: indicate error!
		}

		/* Register the user's buffers */
		int buf_idx;
		for (buf_idx = 0; buf_idx < count_; buf_idx++) {
			mem_.mem_reg[buf_idx].base_addr = ptrs_[buf_idx];
			mem_.mem_reg[buf_idx].length	= bytes_;
			mem_.mem_reg[buf_idx].mr		= ibv_reg_mr(ibv_.pd,
					ptrs_[buf_idx], bytes_, IB_ACCESS_FLAGS);
		}

		/* Step #2: Create a UMR memory region */
		struct ibv_exp_mkey_list_container *umr_mkey = nullptr;
		if (count > ibv_.attrs.umr_caps.max_send_wqe_inline_klms) {
			struct ibv_exp_mkey_list_container_attr list_container_attr = {
					.pd					= ibv_.pd,
					.mkey_list_type		= IBV_EXP_MKEY_LIST_TYPE_INDIRECT_MR,
					.max_klm_list_size	= count,
					.comp_mask 			= 0
			};
			umr_mkey = ibv_exp_alloc_mkey_list_memory(&list_container_attr);
			if (!umr_mkey) {
				return; // TODO: indicate error!
			}
		} else {
			umr_mkey = nullptr;
		}

		mem_.umr_mr = register_umr(mem_.mem_reg, count_, umr_mkey);
		if (!mem_.umr_mr) {
			return; // TODO: indicate error!
		}

		/* Cleanup */
		if (umr_mkey) {
			ibv_exp_dealloc_mkey_list_memory(umr_mkey);
		}
	}

	void deregister_memory()
	{
		int buf_idx;
		ibv_dereg_mr(mem_.umr_mr);
		for (buf_idx = 0; buf_idx < count_; buf_idx++) {
			ibv_dereg_mr(mem_.mem_reg[buf_idx].mr);
		}
	}

	void connect_and_prepare()
	{
		/* Create a single management QP */
		unsigned send_wq_size = 4; // FIXME: calc
		unsigned recv_rq_size = RX_SIZE; // FIXME: calc

		int inputs = ptrs_.size();

                rd_.mgmt_cq = ibv_create_cq(ctx->context, RX_SIZE, NULL,
                                ctx->channel, 0);

                if (!rd_.mgmt_cq) {
                        fprintf(stderr, "Couldn't create CQ\n");
                        return; // TODO indicate failure?
                }
		

		rd_.mgmt_qp = hmca_bcol_cc_mq_create(ibv_.cq,
				ibv_.pd, ibv_.context, send_wq_size);

		rd.mgmt_qp_cd = new qp_ctx(rd_.mgmt_qp, rd_.mgmt_cq); 


		qp_ctx* mqp = rd.mgmt_qp_cd;

		/* Create a loopback QP */

		rd_.loopback_cq = ibv_create_cq(ctx->context, RX_SIZE, NULL,
				NULL, 0);

		if (!rd_.loopback_cq) {
			fprintf(stderr, "Couldn't create CQ\n");
			return; // TODO indicate failure?
		}

		rd_.loopback_qp = rc_qp_create(rd_.loopback_cq ,
				ibv_.pd, ibv_.context, send_wq_size, recv_rq_size, 1, 1);
		peer_addr_t loopback_addr;
		rc_qp_get_addr(rd_.loopback_qp, &loopback_addr);
		rc_qp_connect(rd_.loopback_qp, &loopback_addr);


		rd_.loopback_qp_cd = new qp_ctx(rd_.loopback_qp, rd_.loopback_cq);



		/* Prepare the first (intra-node) VectorCalc WQE - loobpack */
		rd_.result.mr = ibv_reg_mr(ibv_.pd, nullptr, bytes_, IB_ACCESS_FLAGS);
		rd_.result.addr = rd_.result.mr->addr;
		rd_.result.len = bytes_;


		/* Calculate the number of recursive-doubling rounds */
		unsigned step_idx, step_count = 0;
		while ((1 << ++step_count) < contextSize_);
		rd_.peers_cnt = step_count;
		rd_.peers = malloc(step_count * sizeof(*rd_.peers));
		if (!rd_.peers) {
			return; // TODO: indicate error!
		}

		int loopback_wqes = step_count+1 + count_;
		mqp->cd_recv_enable(rd_.loopback_qp_cd, loopback_wqes);

		/* Establish a connection with each peer */

		for (step_idx = 0; step_idx < step_count; step_idx++) {
			/* calculate the rank of each peer */
			int leap = 1 << step_idx;
			if ((contextRank_ % (leap << 1)) >= leap) {
				leap *= -1;
			}
			rd_.peers[step_idx].rank = contextRank_ + leap;

			/* Create a QP and a buffer for this peer */

			rd_.peers[step_idx].cq = ibv_create_cq(ctx->context, RX_SIZE, NULL,
					NULL, 0);

			if (!rd_.peers[step_idx].cq) {
				fprintf(stderr, "Couldn't create CQ\n");
				return; // TODO indicate failure?
			}


			rd_.peers[step_idx].cq = rc_qp_create(rd_.peers[step_idx].cq,
					ibv_.pd, ibv_.context, send_wq_size, recv_rq_size, 1, 1);
			struct ibv_mr *mr = ibv_reg_mr(ibv_.pd, 0, bytes_, IB_ACCESS_FLAGS);
			rd_.peers[step_idx].incoming_buf.mr	  = mr;
			rd_.peers[step_idx].incoming_buf.addr = mr->addr;
			rd_.peers[step_idx].incoming_buf.len  = bytes_;

			/* Create a UMR for VectorCalc-ing each buffer with the result */
			struct ibv_exp_mem_region mem_reg[2] = {
					{
							.addr = rd_.result.addr,
							.len  = bytes_,
							.lkey = rd_.result.mr->lkey
					},
					{
							.addr = mr->addr,
							.len  = bytes_,
							.lkey = mr->lkey
					}
			};
			mr = register_umr(mem_reg, 2, nullptr);
			if (!mr) {
				return; // TODO: indicate error!
			}
			rd_.peers[step_idx].outgoing_buf.mr = mr;
			rd_.peers[step_idx].outgoing_buf.addr = rd_.result.addr;
			rd_.peers[step_idx].outgoing_buf.len = bytes_;

			/* Exchange the QP+buffer address with this peer */
			rd_peer_info_t info = {
			, rd_.peers[step_idx].buffer.mr->rkey
			};
			rc_qp_get_addr(rd_.peers[step_idx].qp, &info.addr);
			p2p_exchange(&info, rd_.peers[step_idx].remote,
					sizeof(info), rd_.peers[step_idx].rank);
			rc_qp_connect(rd_.peers[step_idx].qp,
					&rd_.peers[step_idx].remote.addr);


			rd_.peers[step_idx].qp_cd = new qp_ctx(rd_.peers[step_idx].qp, rd_.peers[step_idx].cq );

			/* Set the logic to trigger the next step */
			mqp->cd_recv_enable(rd_.peers[step_idx].qp_cd, 1);
		}

		/* Trigger the intra-node broadcast - loopback */
		struct ibv_sge sg = {
				.addr = ptrs_[0],
				.len = bytes_ * inputs ,
				.lkey = mem_.umr_mr->lkey
		};

		unsigned buf_idx;	
		rd_.loopback_qp_cd->reduce_write(&sg, &rd_.result, inputs,
				MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);

		mqp->cd_send_enable(rd_.loopback_qp_cd);
		mqp->cd_wait(rd_.loopback_qp, loopback_wqes);

		struct ibv_sge dummy;

		for (step_idx = 0; step_idx < step_count; step_idx++) {
			rd_.peers[step_idx].qp_cd->write(&rd_.result, &rd_.peers[step_idx].remote_buf);

			mqp->cd_send_enable(rd_.peers[step_idx].qp_cd);
			mqp->cd_wait(rd_.peers[step_idx].qp_cd);
			rd_.peers[step_idx].qp_cd->pad();

			rd_.loopback_qp_cd->reduce_write(&dummy, &rd_.result, 2,
                                MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);
			mqp->cd_send_enable(rd_.loopback_qp_cd);
			mqp->cd_wait(rd_.loopback_qp, loopback_wqes);
		}


		sg.len =  bytes_;
		for (buf_idx = 0; buf_idx < count_; buf_idx++) {
                        sg.addr = ptrs_[buf_idx];
                        sg.lkey = mem_.mem_reg[buf_idx].mr->lkey;
                        rd_.loopback_qp_cd->write(&rd_.result, &sg, 0);
                }

		mqp->cd_send_enable(rd_.peers[step_idx].qp_cd);
		mqp->cd_wait(rd_.peers[step_idx].qp_cd, loopback_wqes, inputs);
		mqp->pad(1);
		mqp->dup();
	}

	void teardown()
	{
		for (step_idx = 0; step_idx < rd_.peers_cnt; step_idx++) {
			delete rd_.peers[step_idx].qp_cd;
			ibv_destroy_qp(rd_.peers[step_idx].qp);
                        ibv_destroy_cq(rd_.peers[step_idx].cq);
			ibv_dereg_mr(rd_.peers[step_idx].incoming_buf.mr);
		}
		delete rd_.loopback_qp_dc;
		ibv_destroy_qp(rd_.loopback_qp);
		ibv_destroy_cq(rd_.loopback_cq);
		delete rd_.mgmt_qp_dc;
		ibv_destroy_qp(rd_.mgmt_qp);
                ibv_destroy_cq(rd_.mgmt_cq);
		ibv_dereg_mr(rd_.result.mr);
		free(rd_.peers);
	}

protected:
	std::vector<T*> ptrs_;
	const int count_;
	const int bytes_;
	verb_ctx_t ibv_;
	mem_registration_t mem_;
	rd_connections_t rd_;

	const ReductionFunction<T>* fn_;
};

} // namespace gloo
