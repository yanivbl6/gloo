/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */


#pragma once

#define DEBUG
#define VALIDITY_CHECK
#define HANG_REPORTX

#include <stddef.h>
#include <string.h>
#include <alloca.h>

#include "gloo/algorithm.h"
#include "gloo/context.h"
#include "gloo/mlx5dv_mqp.h"
#include "gloo/mlx5dv_mgr.h"
#include "gloo/mlx5dv_mem.h"


#include <ctime>


namespace gloo {

typedef int rank_t;


typedef struct mem_registration {
	unsigned mem_reg_cnt;
	struct ibv_exp_mem_region *mem_reg;
	struct ibv_mr *umr_mr;
} mem_registration_t;

typedef struct rd_peer_info {
	uintptr_t  buf;
	union {
		uint32_t rkey;
		uint32_t lkey;
	};
	peer_addr_t addr;
} rd_peer_info_t;

typedef struct rd_peer {
	rank_t rank;
	qp_ctx *qp_cd;
	cq_ctx *cq_cq;

	struct ibv_qp *qp;
	struct ibv_cq *cq;
        struct ibv_cq *scq;


	struct ibv_sge outgoing_buf;
	struct ibv_mr* outgoing_mr;
	struct ibv_sge incoming_buf;
	struct ibv_mr* incoming_mr;
	struct ibv_sge remote_buf;
} rd_peer_t;



typedef struct rd_connections {
	struct ibv_sge result;
	struct ibv_mr* result_mr;


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

		pipeline = 1;

		/* Step #1: Initialize verbs for all to use */
		PRINT("starting AllreduceNew");
		init_verbs();
	
		/* Step #2: Register existing memory buffers with UMR */
		register_memory();
		PRINT("register_memory DONE");

		/* Step #3: Connect to the (recursive-doubling) peers and pre-post operations */
		connect_and_prepare();
		PRINT("connect_and_prepare DONE");
		mone = 0;
	}

	virtual ~AllreduceNew() {
		teardown();
		deregister_memory();
		fini_verbs();
	}

	int p2p_exchange(void* send_buf, void* recv_buf, size_t size, int peer){
		auto& pair = this->getPair(peer);
		auto slot = this->context_->nextSlot();

		auto sendBuf = pair->createSendBuffer(slot, send_buf, size);
		auto recvBuf = pair->createRecvBuffer(slot, recv_buf, size);
		sendBuf->send();
		sendBuf->waitSend();
		recvBuf->waitRecv();
	}

	void run() {



#ifdef VALIDITY_CHECK
		for (int i =0; i < ptrs_.size(); ++i){
			//fprintf(stderr, "Input %d:\n",i);
			float* buf = (float*)  ptrs_[i];
			for (int k =0; k< count_; ++k){
				buf[k] = ((float) k + i) + contextRank_ + mone;
			}
			//print_values(buf, count_);
		}
#endif
//		fprintf(stderr,"iteration: %d\n", mone);
		rd_.mgmt_qp_cd->db();
		rd_.mgmt_qp_cd->rearm();

		int res = 0;
		uint64_t count = 0;

//		clock_t begin = clock()*1E6;

		while (!res){
			res = rd_.loopback_qp_cd->poll();
//                        res = rd_.mgmt_qp_cd->poll();

			++count;
#ifdef HANG_REPORT
			if (count == 1000000000){

				fprintf(stderr,"iteration: %d\n", mone);
                                PRINT("managment qp:");
				rd_.mgmt_qp_cd->printCq();
				rd_.mgmt_qp_cd->printSq();


                                PRINT("loopback qp:");
				rd_.loopback_qp_cd->printCq();
                                rd_.loopback_qp_cd->printSq();


                                PRINT("rc qp:");
				rd_.peers[0].qp_cd->printCq();
				rd_.peers[0].qp_cd->printSq();
			}

#endif
		}
//		clock_t end = clock()*1E6;
//		double elapsed_us = double(end - begin) / CLOCKS_PER_SEC;

//		fprintf(stderr,"iteration: %d, time = %f\n", mone, elapsed_us );


#ifdef VALIDITY_CHECK

		unsigned step_count = 0;
		while ((1 << ++step_count) < contextSize_);

                for (int i =0; i < step_count; ++i){
                        //fprintf(stderr, "Incoming %d:\n",i);
                        float* buf = (float*) ((void*) rd_.peers[i].incoming_buf.addr);
                        //print_values(buf, count_);
                }

		for (int i =0; i < ptrs_.size(); ++i){
			//fprintf(stderr, "Output %d:\n",i);
			int err = 0;
			float* buf = (float*)  ptrs_[i];
			//print_values(buf, count_);
			for (int k =0; k< count_; ++k){
				int expected_base = ((k+mone)*2 + ptrs_.size() -1)*ptrs_.size()/2;
				int expected_max = ((k+mone+contextSize_-1)*2 + ptrs_.size() -1)*ptrs_.size()/2;
				float expected_result = (float) (expected_base + expected_max)*contextSize_/2;
				float result = buf[k];
				if (result != expected_result){
					fprintf(stderr,"ERROR: In Iteration %d\n expected: %.2f, got: %.2f\n", mone  ,expected_result, result);
					for (int i =0; i < ptrs_.size(); ++i){
						fprintf(stderr, "Input %d:\n",i);
						float buf[count_];
						for (int k =0; k< count_; ++k){
							buf[k] = ((float) k + i) + contextRank_ + mone;
						}
						print_values(buf, count_);
					}
					for (int i =0; i < step_count; ++i){
                                        	fprintf(stderr, "Incoming %d:\n",i);
                        			float* buf = (float*) ((void*) rd_.peers[i].incoming_buf.addr);
                        			print_values(buf, count_);
                			}
					fprintf(stderr, "Output %d:\n",i);
					print_values(buf, count_);
					//err = 1;
					break;
				}
			}
			if (err){
				break;
			}
		}
#endif
		mone = 1- mone;
	}

	void init_verbs(char *ib_devname=nullptr, int port=1)
	{
		verb_ctx_t *ctx = &ibv_;
		struct ibv_device **dev_list = ibv_get_device_list(nullptr);
		struct ibv_device* ib_dev;
		if (!dev_list) {
			perror("Failed to get IB devices list");
			return; // TODO indicate failure?
		}

		if (!ib_devname) {
			ib_dev = dev_list[0];
			if (!ib_dev) {
				PRINT("No IB devices found");
				return; // TODO indicate failure?
			}
		} else {
			int i;
			for (i = 0; dev_list[i]; ++i)
				if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
					break;
			ib_dev = dev_list[i];
			if (!ib_dev) {
				PRINT("IB device not found");
				return; // TODO indicate failure?
			}
		}

		PRINT(ibv_get_device_name(ib_dev));
		ctx->context = ibv_open_device(ib_dev);
		ibv_free_device_list(dev_list);
		if (!ctx->context) {
			PRINT("Couldn't get context");
		}

		ctx->pd = ibv_alloc_pd(ctx->context);
		if (!ctx->pd) {
			PRINT("Couldn't allocate PD");
			goto clean_comp_channel;
		}

		ctx->umr_cq = ibv_create_cq(ctx->context, CX_SIZE , NULL, NULL, 0);
		if (!ctx->umr_cq) {
			PRINT("Couldn't create CQ");
			return; // TODO indicate failure?
		}

		memset(&ctx->attrs, 0, sizeof(ctx->attrs));
		ctx->attrs.comp_mask = IBV_EXP_DEVICE_ATTR_UMR;
		if (ibv_exp_query_device(ctx->context ,&ctx->attrs)) {
			PRINT("Couldn't query device attributes");
			return; // TODO indicate failure?
		}

		{
			struct ibv_exp_qp_init_attr attr;
			memset(&attr, 0, sizeof(attr));
			attr.pd                 = ctx->pd;
			attr.send_cq		= ctx->umr_cq;
			attr.recv_cq		= ctx->umr_cq;
			attr.qp_type		= IBV_QPT_RC;
			attr.comp_mask		= IBV_EXP_QP_INIT_ATTR_PD |
						  IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS |
						  IBV_EXP_QP_INIT_ATTR_MAX_INL_KLMS;
			attr.exp_create_flags	= IBV_EXP_QP_CREATE_UMR;
			attr.cap.max_send_wr	= 1;
			attr.cap.max_recv_wr	= 0;
			attr.cap.max_send_sge	= 1;
			attr.cap.max_recv_sge	= 0;
			attr.max_inl_send_klms  = 4;

			ctx->umr_qp = ibv_exp_create_qp(ctx->context, &attr);
			if (!ctx->umr_qp)  {
				PRINT("Couldn't create UMR QP");
				return; // TODO indicate failure?
			}
		}

		{
			struct ibv_qp_attr qp_attr;
			memset(&qp_attr, 0, sizeof(qp_attr));
			qp_attr.qp_state	= IBV_QPS_INIT;
			qp_attr.pkey_index	= 0;
			qp_attr.port_num	= 1;
			qp_attr.qp_access_flags = 0;
			
			if (ibv_modify_qp(ctx->umr_qp, &qp_attr,
				IBV_QP_STATE |
				IBV_QP_PKEY_INDEX | 
				IBV_QP_PORT |
				IBV_QP_ACCESS_FLAGS)) {
				PRINT("Failed to INIT the UMR QP");
				return; // TODO: indicate failure?
			}
		}

		peer_addr_t my_addr;
		rc_qp_get_addr(ctx->umr_qp, &my_addr);
		rc_qp_connect(&my_addr, ctx->umr_qp);

		PRINT("init_verbs DONE");
		return; // SUCCESS!

		clean_qp:
		ibv_destroy_qp(ctx->umr_qp);

		clean_cq:
		ibv_destroy_cq(ctx->umr_cq);

		clean_mr:
		ibv_dealloc_pd(ctx->pd);

		clean_comp_channel:
		ibv_close_device(ctx->context);

		return; // TODO indicate failure?
	}

	int fini_verbs()
	{
		verb_ctx_t *ctx = &ibv_;
		if (ibv_destroy_qp(ctx->umr_qp)) {
			PRINT("Couldn't destroy QP");
			return 0; // TODO indicate failure?
		}

		if (ibv_destroy_cq(ctx->umr_cq)) {
			PRINT("Couldn't destroy CQ");
			return 0; // TODO indicate failure?
		}

		if (ibv_dealloc_pd(ctx->pd)) {
			PRINT("Couldn't deallocate PD");
			return 0; // TODO indicate failure?
		}

		if (ibv_close_device(ctx->context)) {
			PRINT("Couldn't release context");
			return 0; // TODO indicate failure?
		}
		return 1;
	}

	void register_memory()
	{
		int inputs = ptrs_.size();
		if (inputs > ibv_.attrs.umr_caps.max_klm_list_size) {
			return; // TODO: indicate error!
		}

		/* Register the user's buffers */
		int buf_idx;
		mem_.mem_reg = (struct ibv_exp_mem_region*) malloc(inputs * sizeof(struct ibv_exp_mem_region));
		for (buf_idx = 0; buf_idx < inputs; buf_idx++) {
			mem_.mem_reg[buf_idx].base_addr = (uint64_t)  ptrs_[buf_idx];
			mem_.mem_reg[buf_idx].length	= bytes_;
			mem_.mem_reg[buf_idx].mr	= ibv_reg_mr(ibv_.pd,
					ptrs_[buf_idx], bytes_, IB_ACCESS_FLAGS);
		}

		/* Step #2: Create a UMR memory region */
		struct ibv_exp_mkey_list_container *umr_mkey = nullptr;
		if (inputs > ibv_.attrs.umr_caps.max_send_wqe_inline_klms) {
			struct ibv_exp_mkey_list_container_attr list_container_attr;
			list_container_attr.pd				= ibv_.pd;
			list_container_attr.mkey_list_type		= IBV_EXP_MKEY_LIST_TYPE_INDIRECT_MR;
			list_container_attr.max_klm_list_size		= inputs;
			list_container_attr.comp_mask 			= 0;
			umr_mkey = ibv_exp_alloc_mkey_list_memory(&list_container_attr);
			if (!umr_mkey) {
				return; // TODO: indicate error!
			}
		} else {
			umr_mkey = nullptr;
		}

		mem_.umr_mr = register_umr(inputs, mem_.mem_reg, umr_mkey, ibv_);
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
		for (buf_idx = 0; buf_idx < ptrs_.size(); buf_idx++) {
			ibv_dereg_mr(mem_.mem_reg[buf_idx].mr);
		}
		free(mem_.mem_reg);
	}

	void connect_and_prepare()
	{
		/* Create a single management QP */
		unsigned send_wq_size = 16; // FIXME: calc
		unsigned recv_rq_size = RX_SIZE; // FIXME: calc

		int inputs = ptrs_.size();

		unsigned step_idx, step_count = 0;
		while ((1 << ++step_count) < contextSize_);

		verb_ctx_t* ctx = &(this->ibv_);	

		rd_.mgmt_cq = cd_create_cq(ctx->context, CX_SIZE , NULL,
				0, 0);

		if (!rd_.mgmt_cq) {
			PRINT("Couldn't create CQ\n");
			return; // TODO indicate failure?
		}


		PRINT("creating MGMT QP\n");

                int mgmt_wqes = step_count * 6 + 5 ;  //should find better way to do this


		rd_.mgmt_qp = hmca_bcol_cc_mq_create(rd_.mgmt_cq,
				ibv_.pd, ibv_.context, send_wq_size);
		rd_.mgmt_qp_cd = new qp_ctx(rd_.mgmt_qp, rd_.mgmt_cq, mgmt_wqes   , 0); 


		qp_ctx* mqp = rd_.mgmt_qp_cd;
		PRINT("created MGMT QP\n");

		/* Create a loopback QP */

		rd_.loopback_cq = cd_create_cq(ctx->context, CX_SIZE, NULL,
				NULL, 0);

		if (!rd_.loopback_cq) {
			PRINT("Couldn't create CQ\n");
			return; // TODO indicate failure?
		}



		int loopback_cqes = step_count+1 + inputs;

                int loopback_wqes = (step_count+1)*2 + inputs;


		rd_.loopback_qp = rc_qp_create(rd_.loopback_cq ,
				ibv_.pd, ibv_.context, loopback_cqes  , recv_rq_size, 1, 1 );
		peer_addr_t loopback_addr;
		rc_qp_get_addr(rd_.loopback_qp, &loopback_addr);
		rc_qp_connect(&loopback_addr,rd_.loopback_qp);
		rd_.loopback_qp_cd = new qp_ctx(rd_.loopback_qp, rd_.loopback_cq, loopback_wqes   , loopback_cqes);
		PRINT("loopback connected");


		/* Prepare the first (intra-node) VectorCalc WQE - loobpack */
		void* tmp  =  malloc(bytes_);
		rd_.result.addr =  (uint64_t) tmp;
		rd_.result_mr = ibv_reg_mr(ibv_.pd, tmp  , bytes_, IB_ACCESS_FLAGS);

		rd_.result.length = bytes_;
		rd_.result.lkey = rd_.result_mr->lkey;

		/* Calculate the number of recursive-doubling rounds */
		rd_.peers_cnt = step_count;
		rd_.peers = (rd_peer_t*)  malloc(step_count * sizeof(*rd_.peers));
		if (!rd_.peers) {
			return; // TODO: indicate error!
		}



		mqp->cd_recv_enable(rd_.loopback_qp_cd);

		/* Establish a connection with each peer */

		for (step_idx = 0; step_idx < step_count; step_idx++) {

			/* calculate the rank of each peer */
			int leap = 1 << step_idx;
			if ((contextRank_ % (leap << 1)) >= leap) {
				leap *= -1;
			}
			rd_.peers[step_idx].rank = contextRank_ + leap;

			/* Create a QP and a buffer for this peer */

			rd_.peers[step_idx].cq = cd_create_cq(ctx->context, CX_SIZE, NULL,
					NULL, 0);

			if (!rd_.peers[step_idx].cq) {
				PRINT("Couldn't create CQ\n");
				return; // TODO indicate failure?
			}

			rd_.peers[step_idx].scq = cd_create_cq(ctx->context, CX_SIZE, NULL,
					NULL, 0);

			if (!rd_.peers[step_idx].scq) {
				PRINT("Couldn't create SCQ\n");
				return; // TODO indicate failure?
			}


			rd_.peers[step_idx].qp = rc_qp_create(rd_.peers[step_idx].cq,
					ibv_.pd, ibv_.context, 4, RX_SIZE, 1, 1, rd_.peers[step_idx].scq );
			void *incoming_buf = malloc(bytes_);
			rd_.peers[step_idx].incoming_buf.addr   = (uint64_t) incoming_buf;
			struct ibv_mr *mr = ibv_reg_mr(ibv_.pd, incoming_buf, bytes_, IB_ACCESS_FLAGS);
			rd_.peers[step_idx].incoming_mr	      = mr;
			rd_.peers[step_idx].incoming_buf.length  = bytes_;
			PRINT("RC created for peer\n");
			uint32_t rkey = mr->rkey;

			/* Create a UMR for VectorCalc-ing each buffer with the result */
			struct ibv_exp_mem_region mem_reg[2];
			mem_reg[0].base_addr	= (uint64_t) rd_.result.addr;
			mem_reg[0].length	= bytes_;
			mem_reg[0].mr	= rd_.result_mr;
			mem_reg[1].base_addr	= (uint64_t) mr->addr;
			mem_reg[1].length	= bytes_;
			mem_reg[1].mr	= mr;

			PRINT("before UMR"); 
			mr = register_umr(2, mem_reg, nullptr, ibv_);
			if (!mr) {
				return; // TODO: indicate error!
			}
                        PRINT("after UMR");

			rd_.peers[step_idx].outgoing_mr = mr;
			rd_.peers[step_idx].outgoing_buf.addr = (uint64_t) rd_.result.addr;
			rd_.peers[step_idx].outgoing_buf.length = bytes_;
                        rd_.peers[step_idx].outgoing_buf.lkey = mr->lkey;


			/* Exchange the QP+buffer address with this peer */
			rd_peer_info_t local_info, remote_info;
			local_info.buf  = (uintptr_t) incoming_buf;
			local_info.rkey = rkey;
			rc_qp_get_addr(rd_.peers[step_idx].qp, &local_info.addr);
			p2p_exchange((void*)&local_info, (void*)&remote_info,
					sizeof(local_info), (int) rd_.peers[step_idx].rank);

			PRINT("Exchanged");
			/* Connect to the remote peer */
			rd_.peers[step_idx].remote_buf.addr = remote_info.buf;
			rd_.peers[step_idx].remote_buf.lkey = remote_info.rkey;
			rc_qp_connect(&remote_info.addr, rd_.peers[step_idx].qp);
			rd_.peers[step_idx].qp_cd =
					new qp_ctx(rd_.peers[step_idx].qp, rd_.peers[step_idx].cq, 1 , 1 , rd_.peers[step_idx].scq, 1);

			/* Set the logic to trigger the next step */
			mqp->cd_recv_enable(rd_.peers[step_idx].qp_cd);
			PRINT("Rcv_enabled");
		}




		/* Trigger the intra-node broadcast - loopback */
		struct ibv_sge sg;
		sg.addr = (uint64_t)  ptrs_[0];
		sg.length =  bytes_ ;// *  inputs; //  bytes_; // * inputs;
		sg.lkey = mem_.umr_mr->lkey;

		unsigned buf_idx;	
		rd_.loopback_qp_cd->reduce_write(&sg, &rd_.result, inputs,
				MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);

		mqp->cd_send_enable(rd_.loopback_qp_cd);
		mqp->cd_wait(rd_.loopback_qp_cd);
		
		for (step_idx = 0; step_idx < step_count; step_idx++) {
			rd_.peers[step_idx].qp_cd->writeCmpl(&rd_.result, &rd_.peers[step_idx].remote_buf);
			mqp->cd_send_enable(rd_.peers[step_idx].qp_cd);
			mqp->cd_wait(rd_.peers[step_idx].qp_cd);
                        mqp->cd_wait_send(rd_.peers[step_idx].qp_cd);
			rd_.peers[step_idx].qp_cd->fin();
			rd_.loopback_qp_cd->reduce_write(&rd_.peers[step_idx].outgoing_buf, &rd_.result, 2,
					MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);
			mqp->cd_send_enable(rd_.loopback_qp_cd);
			mqp->cd_wait(rd_.loopback_qp_cd);
		}

		sg.length =  bytes_;
		for (buf_idx = 0; buf_idx < inputs; buf_idx++) {
			sg.addr = (uint64_t) ptrs_[buf_idx];
			sg.lkey = mem_.mem_reg[buf_idx].mr->lkey;
			rd_.loopback_qp_cd->write(&rd_.result, &sg);
		}
		rd_.loopback_qp_cd->fin();
		mqp->cd_send_enable(rd_.loopback_qp_cd);
		mqp->cd_wait(rd_.loopback_qp_cd);
		mqp->fin();
//		mqp->printSq();
	}

	void teardown()
	{
		for (int step_idx = 0; step_idx < rd_.peers_cnt; step_idx++) {
			delete rd_.peers[step_idx].qp_cd;
			ibv_destroy_qp(rd_.peers[step_idx].qp);
			ibv_destroy_cq(rd_.peers[step_idx].cq);
			ibv_destroy_cq(rd_.peers[step_idx].scq);
			ibv_dereg_mr(rd_.peers[step_idx].incoming_mr);
			ibv_dereg_mr(rd_.peers[step_idx].outgoing_mr);
			free((void*) rd_.peers[step_idx].incoming_buf.addr);
		}
		delete rd_.loopback_qp_cd;
		ibv_destroy_qp(rd_.loopback_qp);
		ibv_destroy_cq(rd_.loopback_cq);
		delete rd_.mgmt_qp_cd;
		ibv_destroy_qp(rd_.mgmt_qp);
		ibv_destroy_cq(rd_.mgmt_cq);
		ibv_dereg_mr(rd_.result_mr);
		free((void*) rd_.result.addr);
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
	int mone;
	int pipeline;
};

} // namespace gloo
