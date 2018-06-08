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
#include <vector>

namespace gloo {

typedef int rank_t;


typedef struct mem_registration {
	Iov usr_vec;	
	UmrMem* umr_mem;		
} mem_registration_t;

typedef struct rd_peer_info {
	uintptr_t  buf;
	union {
		uint32_t rkey;
		uint32_t lkey;
	};
	peer_addr_t addr;
} rd_peer_info_t;


class rd_peer_t{
   public:

	rd_peer_t(){};
	~rd_peer_t(){
		delete(this->qp_cd);
                ibv_destroy_qp(this->qp);
                ibv_destroy_cq(this->cq);
                ibv_destroy_cq(this->scq);
		delete(this->incoming_buf);
                delete(this->outgoing_buf);
                delete(this->remote_buf);
	};

        rank_t rank;
        qp_ctx *qp_cd;
        cq_ctx *cq_cq;

        struct ibv_qp *qp;
        struct ibv_cq *cq;
        struct ibv_cq *scq;

        NetMem* outgoing_buf;
        NetMem* incoming_buf;
        RemoteMem* remote_buf;
};



typedef struct rd_connections {
	NetMem*        result;

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

		ibv_ = new verb_ctx_t(); 

                PRINT("Verbs initiated");

	
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
		delete(ibv_);
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
                        			float* buf = (float*) ((void*) rd_.peers[i].incoming_buf->sg()->addr);
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
		++mone;
	}

	void register_memory( )
	{
		/* Register the user's buffers */
		for (int buf_idx = 0; buf_idx < ptrs_.size(); buf_idx++) {
			mem_.usr_vec.push_back(new UsrMem(ptrs_[buf_idx], bytes_, ibv_) );
		}
		PRINT("User memory registered");
		mem_.umr_mem = new UmrMem(mem_.usr_vec, ibv_);
		if (!mem_.umr_mem) {
			throw "UMR failed";
		}
	}

	void deregister_memory()
	{
		int buf_idx;
		delete(mem_.umr_mem);
		freeIov(mem_.usr_vec);
	}

	void connect_and_prepare()
	{
		/* Create a single management QP */
		unsigned send_wq_size = 16;
		unsigned recv_rq_size = RX_SIZE; // 

		int inputs = ptrs_.size();

		unsigned step_idx, step_count = 0;
		while ((1 << ++step_count) < contextSize_);

		verb_ctx_t* ctx = (this->ibv_);	

		rd_.mgmt_cq = cd_create_cq(ctx->context, CX_SIZE , NULL,
				0, 0);

		if (!rd_.mgmt_cq) {
			PRINT("Couldn't create CQ\n");
			return; // TODO indicate failure?
		}


		PRINT("creating MGMT QP\n");

                int mgmt_wqes = step_count * 6 + 5 ;  //should find better way to do this


		rd_.mgmt_qp = hmca_bcol_cc_mq_create(rd_.mgmt_cq,
				ibv_->pd, ibv_->context, send_wq_size);
		rd_.mgmt_qp_cd = new qp_ctx(rd_.mgmt_qp, rd_.mgmt_cq, mgmt_wqes   , 0); 


		qp_ctx* mqp = rd_.mgmt_qp_cd;
		PRINT("created MGMT QP\n");

		/* Create a loopback QP */

		rd_.loopback_cq = cd_create_cq(ctx->context, CX_SIZE, NULL,
				NULL, 0);

		if (!rd_.loopback_cq) {
			throw ("Couldn't create CQ\n");
		}



		int loopback_cqes = step_count+1 + inputs;

                int loopback_wqes = (step_count+1)*2 + inputs;


		rd_.loopback_qp = rc_qp_create(rd_.loopback_cq ,
				ibv_->pd, ibv_->context, loopback_cqes  , recv_rq_size, 1, 1 );
		peer_addr_t loopback_addr;
		rc_qp_get_addr(rd_.loopback_qp, &loopback_addr);
		rc_qp_connect(&loopback_addr,rd_.loopback_qp);
		rd_.loopback_qp_cd = new qp_ctx(rd_.loopback_qp, rd_.loopback_cq, loopback_wqes   , loopback_cqes);
		PRINT("loopback connected");


		rd_.result =  new HostMem(bytes_, ibv_);

		rd_.peers_cnt = step_count;
		rd_.peers =  new rd_peer_t[step_count];
		if (!rd_.peers) {
			throw "malloc failed";
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
				throw("Couldn't create CQ\n");
			}

			rd_.peers[step_idx].scq = cd_create_cq(ctx->context, CX_SIZE, NULL,
					NULL, 0);

			if (!rd_.peers[step_idx].scq) {
				throw("Couldn't create SCQ\n");
			}


			rd_.peers[step_idx].qp = rc_qp_create(rd_.peers[step_idx].cq,
					ibv_->pd, ibv_->context, 4, RX_SIZE, 1, 1, rd_.peers[step_idx].scq );

			rd_.peers[step_idx].incoming_buf = new HostMem( bytes_, ibv_);

			PRINT("RC created for peer\n");

			/* Create a UMR for VectorCalc-ing each buffer with the result */
			PRINT("before UMR"); 
			Iov umr_iov{ rd_.result, rd_.peers[step_idx].incoming_buf };

			rd_.peers[step_idx].outgoing_buf = new UmrMem(umr_iov, ibv_);

                        PRINT("after UMR");



			/* Exchange the QP+buffer address with this peer */
			rd_peer_info_t local_info, remote_info;
			local_info.buf  = (uintptr_t) rd_.peers[step_idx].incoming_buf->sg()->addr;
			local_info.rkey = rd_.peers[step_idx].incoming_buf->sg()->lkey; //TODO: fix rkey, lkey mixure
			rc_qp_get_addr(rd_.peers[step_idx].qp, &local_info.addr);
			p2p_exchange((void*)&local_info, (void*)&remote_info,
					sizeof(local_info), (int) rd_.peers[step_idx].rank);

			PRINT("Exchanged");
			/* Connect to the remote peer */
			rd_.peers[step_idx].remote_buf = new RemoteMem(remote_info.buf, remote_info.rkey);  

			rc_qp_connect(&remote_info.addr, rd_.peers[step_idx].qp);

			rd_.peers[step_idx].qp_cd =
					new qp_ctx(rd_.peers[step_idx].qp, rd_.peers[step_idx].cq, 1 , 1 , rd_.peers[step_idx].scq, 1);

			/* Set the logic to trigger the next step */
			mqp->cd_recv_enable(rd_.peers[step_idx].qp_cd);

			PRINT("Rcv_enabled");
		}





		unsigned buf_idx;	
		rd_.loopback_qp_cd->reduce_write(mem_.umr_mem , rd_.result, inputs,
				MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);

		mqp->cd_send_enable(rd_.loopback_qp_cd);
		mqp->cd_wait(rd_.loopback_qp_cd);
		
		for (step_idx = 0; step_idx < step_count; step_idx++) {
			rd_.peers[step_idx].qp_cd->writeCmpl(rd_.result, rd_.peers[step_idx].remote_buf);
			mqp->cd_send_enable(rd_.peers[step_idx].qp_cd);
			mqp->cd_wait(rd_.peers[step_idx].qp_cd);
                        mqp->cd_wait_send(rd_.peers[step_idx].qp_cd);
			rd_.peers[step_idx].qp_cd->fin();
			rd_.loopback_qp_cd->reduce_write(rd_.peers[step_idx].outgoing_buf, rd_.result, 2,
					MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);
			mqp->cd_send_enable(rd_.loopback_qp_cd);
			mqp->cd_wait(rd_.loopback_qp_cd);
		}

		for (buf_idx = 0; buf_idx < inputs; buf_idx++) {
			rd_.loopback_qp_cd->write(rd_.result, mem_.usr_vec[buf_idx]);
		}
		rd_.loopback_qp_cd->fin();
		mqp->cd_send_enable(rd_.loopback_qp_cd);
		mqp->cd_wait(rd_.loopback_qp_cd);
		mqp->fin();
	}

	void teardown()
	{

		delete rd_.loopback_qp_cd;
		ibv_destroy_qp(rd_.loopback_qp);
		ibv_destroy_cq(rd_.loopback_cq);
		delete rd_.mgmt_qp_cd;
		ibv_destroy_qp(rd_.mgmt_qp);
		ibv_destroy_cq(rd_.mgmt_cq);

		delete(rd_.result);
		delete(rd_.peers);
	}

protected:
	std::vector<T*> ptrs_;
	const int count_;
	const int bytes_;
	verb_ctx_t* ibv_;
	mem_registration_t mem_;
	rd_connections_t rd_;
	const ReductionFunction<T>* fn_;
	int mone;
	int pipeline;
};

} // namespace gloo
