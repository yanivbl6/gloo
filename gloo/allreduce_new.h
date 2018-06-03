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

typedef int rank_t;

typedef struct verb_ctx {
	struct ibv_context		  *context;
	struct ibv_exp_device_attr attrs;
	struct ibv_pd			  *pd;
	struct ibv_cq			  *cq;
	struct ibv_qp			  *qp;
} verb_ctx_t;

typedef struct mem_registration {
	struct ibv_exp_mem_region *mem_reg;
	struct ibv_mr *umr_mr;
	unsigned mrs_cnt;
} mem_registration_t;

typedef struct rd_peer_info {
	uintptr  buf;
	uint32_t rkey;
	uint16_t qpn;
} rd_peer_info_t;

typedef struct rd_peer {
	rank_t rank;
	struct ibv_sge buffer;
	struct ibv_qp *qp;
	rd_peer_info_t remote;
} rd_peer_t;

typedef struct rd_connections {
	struct ibv_qp *mgmt_qp;
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

    /* Step #3: Connect to the (recursive-doubling) peers */
    establish_connections();
  }

  virtual ~AllreduceNew() {
	deregister_memory();
	fini_verbs();

    if (inbox_ != nullptr) {
      free(inbox_);
    }
    if (outbox_ != nullptr) {
      free(outbox_);
    }
  }

//#define SLOT (this->context_->nextSlot())
#define SLOTBASE (636)
#define SLOT(x,y) (SLOTBASE + x*this->contextSize_+y)

//only 1 p2p is allowed per pair.

int p2p(void* buf, size_t size, uint32_t src_rank, uint32_t dst_rank){
    if (src_rank == this->contextRank_){
      auto& pair = this->getPair(dst_rank);
      auto slot = SLOT(src_rank,dst_rank);
      auto dataBuf = pair->createSendBuffer(slot, buf, size);
      dataBuf->send();
      dataBuf->waitSend();
    } else if (dst_rank == this->contextRank_) {
      auto& pair = this->getPair(src_rank);
      auto slot = SLOT(src_rank,dst_rank);
      auto dataBuf = pair->createRecvBuffer(slot, buf, size);
      dataBuf->waitRecv();
    }
    return 1;
  }

  void run() {

    // Reduce specified pointers into ptrs_[0]
    for (int i = 1; i < ptrs_.size(); i++) {
      fn_->call(ptrs_[0], ptrs_[i], count_);
    }

    // Intialize outbox with locally reduced values
    memcpy(outbox_, ptrs_[0], bytes_);

    int numRounds = this->contextSize_ - 1;
    for (int round = 0; round < numRounds; round++) {
      // Initiate write to inbox of node on the right
      sendDataBuf_->send();

      // Wait for inbox write from node on the left
      recvDataBuf_->waitRecv();

      // Reduce
      fn_->call(ptrs_[0], inbox_, count_);

      // Wait for outbox write to complete
      sendDataBuf_->waitSend();

      // Prepare for next round if necessary
      if (round < (numRounds - 1)) {
        memcpy(outbox_, inbox_, bytes_);
      }

      // Send notification to node on the left that
      // this node is ready for an inbox write.
      sendNotificationBuf_->send();

      // Wait for notification from node on the right
      recvNotificationBuf_->waitRecv();
    }

    // Broadcast ptrs_[0]
    for (int i = 1; i < ptrs_.size(); i++) {
      memcpy(ptrs_[i], ptrs_[0], bytes_);
    }
  }

  void init_verbs(char *ib_devname="", int port=1)
  {
	verb_ctx_t *ctx = &ibv_;

	struct ibv_device **dev_list = ibv_get_device_list(ib_devname);
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

	ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
				ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return; // TODO indicate failure?
	}

	if (ibv_exp_query_device(&ctx->attrs)) {
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

		ctx->qp = ibv_exp_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return; // TODO indicate failure?
		}
	}

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qkey            = 0x11111111
		};

		if (ibv_modify_qp(ctx->qp, &attr,
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
  	ibv_destroy_qp(ctx->qp);

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
		return; // TODO indicate failure?
  	}

  	if (ibv_destroy_cq(ctx->cq)) {
  		fprintf(stderr, "Couldn't destroy CQ\n");
		return; // TODO indicate failure?
  	}

  	if (ibv_dealloc_pd(ctx->pd)) {
  		fprintf(stderr, "Couldn't deallocate PD\n");
		return; // TODO indicate failure?
  	}

  	if (ibv_close_device(ctx->context)) {
  		fprintf(stderr, "Couldn't release context\n");
		return; // TODO indicate failure?
  	}
  }

  void register_memory()
  {
	  if (count_ > ibv_.attrs.umr_caps.max_klm_list_size) {
    	  return; // TODO: indicate error!
	  }

	  /* Step #1: Register the user's buffers */
	  int buf_idx;
	  for (buf_idx = 0; buf_idx < count_; buf_idx++) {
		  mem_.mem_reg[buf_idx].base_addr = ptrs_[buf_idx];
		  mem_.mem_reg[buf_idx].length	  = bytes_;
		  mem_.mem_reg[buf_idx].mr		  = ibv_reg_mr(ibv_.pd,
				  ptrs_[buf_idx], bytes_,
				  IBV_ACCESS_LOCAL_WRITE  |
                  IBV_ACCESS_REMOTE_WRITE |
				  IBV_ACCESS_REMOTE_READ);
	  }

      /* Step #2: Create a UMR memory region */
	  struct ibv_exp_mkey_list_container *umr_mkey = nullptr;
	  if (count > ibv_.attrs.umr_caps.max_send_wqe_inline_klms) {
		  struct ibv_exp_mkey_list_container_attr list_container_attr = {
				  .pd				 = ibv_.pd,
				  .mkey_list_type	 = IBV_EXP_MKEY_LIST_TYPE_INDIRECT_MR,
				  .max_klm_list_size = count,
				  .comp_mask 		 = 0
		  };
		  umr_mkey = ibv_exp_alloc_mkey_list_memory(&list_container_attr);
	      if (!umr_mkey) {
	    	  return; // TODO: indicate error!
	      }
	  } else {
		  umr_mkey = null_ptr;
	  }

      struct ibv_exp_mr_init_attr umr_init_attr = {
              .max_klm_list_size = count_,
			  .create_flags		 = IBV_EXP_MR_INDIRECT_KLMS,
              .exp_access_flags	 = IBV_ACCESS_LOCAL_WRITE  |
			                       IBV_ACCESS_REMOTE_WRITE |
								   IBV_ACCESS_REMOTE_READ
      };
      struct ibv_exp_create_mr_in umr_create_mr_in = {
              .pd		 = ibv_.pd,
              .attr		 = &umr_init_attr,
              .comp_mask = 0
      };
      mem_.umr_mr = ibv_exp_create_mr(&umr_create_mr_in);
      if (!mem_.umr_mr) {
    	  return; // TODO: indicate error!
      }

      /* Step 3: Create the UMR work request */
      struct ibv_exp_send_wr wr = {0}, *bad_wr;
      wr.exp_opcode							= IBV_EXP_WR_UMR_FILL;
      wr.exp_send_flags						= IBV_EXP_SEND_SIGNALED;
      wr.ext_op.umr.umr_type				= IBV_EXP_UMR_MR_LIST;
      wr.ext_op.umr.memory_objects			= mem_.umr_mkey;
      wr.ext_op.umr.modified_mr				= mem_.umr_mr;
      wr.ext_op.umr.base_addr				= ptrs_[0];
      wr.ext_op.umr.num_mrs					= count_;
      wr.ext_op.umr.mem_list.mem_reg_list	= mem_.mem_reg;
      if (!umr_mkey) {
    	  wr.exp_send_flags 			   |= IBV_EXP_SEND_INLINE;
      }

      /* Step 4: Post WR and wait for it to complete */
      if (ibv_exp_post_send(ibv_.qp, &wr, &bad_wr)) {
    	  return; // TODO: indicate error!
      }
      struct ibv_wc wc;
      for (;;) {
          int ret = ibv_poll_cq(ibv_.cq, 1, &wc);
          if (ret < 0) {
        	  return; // TODO: indicate error!
          }
          if (ret == 1) {
              if (wc.status != IBV_WC_SUCCESS) {
            	  return; // TODO: indicate error!
              }
              break;
          }
      }

      /* Step 5: Cleanup */
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

  void establish_connections()
  {
	  unsigned send_wq_size = 4; // TODO: calc
	  unsigned recv_rq_size = 4; // TODO: calc

	  /* Calculate the number of recursive-doubling rounds */
	  unsigned step_idx, step_count = 0;
	  while ((1 << ++step_count) < contextSize_);
	  rd_.peers_cnt = step_count;
	  rd_.peers = malloc(step_count * sizeof(*rd_.peers));
	  if (!rd_.peers) {
    	  return; // TODO: indicate error!
	  }

	  /* Establish a connection with each peer */
	  for (step_idx = 0; step_idx < step_count; step_idx++) {
		  /* calculate the rank of each peer */
		  int lefty = 0;
		  int leap = 1 << step_idx;
		  if ((contextRank_ % (leap << 1)) >= leap) {
			  leap *= -1;
			  lefty = 1;
		  }
		  rd_.peers[step_idx].rank = contextRank_ + leap;

		  /* Create a QP and a buffer for this peer */
		  rd_.peers[step_idx].qp = rc_qp_create(ibv_.cq,
			  ibv_.pd, ibv_.context, send_wq_size, recv_rq_size, 1, 1);
		  rd_.peers[step_idx].buffer.mr = ibv_reg_mr(ibv_.pd, 0, bytes_,
				  IBV_ACCESS_LOCAL_WRITE  |
                  IBV_ACCESS_REMOTE_WRITE |
				  IBV_ACCESS_REMOTE_READ);
		  rd_.peers[step_idx].buffer.addr = rd_.peers[step_idx].buffer.mr.addr;
		  rd_.peers[step_idx].buffer.len = bytes_;

		  /* Exchange the QP+buffer address with this peer */
		  rd_peer_info_t info = {
				  .buf = rd_.peers[step_idx].buffer.addr,
				  .rkey = rd_.peers[step_idx].buffer.mr->rkey,
				  .qpn = rd_.peers[step_idx].qp->qpn
		  };
		  if (lefty)
			  p2p(rd_.peers[step_idx].remote, sizeof(info), rd_.peers[step_idx].rank, contextRank_);
		  p2p(&info, sizeof(info), contextRank_, rd_.peers[step_idx].rank);
		  if (!lefty)
			  p2p(rd_.peers[step_idx].remote, sizeof(info), rd_.peers[step_idx].rank, contextRank_);


		  /* Set the logic to trigger the next step */
		  if (step_idx) {
			  //TODO: send/recv using "rd_.peers[step_idx].remote"
		  }
	  }

	  /* Create a single management QP */
	  rd_.mgmt_qp = hmca_bcol_cc_mq_create(ibv_.cq,
			  ibv_.pd, ibv_.context, send_wq_size);
  }

 protected:
  std::vector<T*> ptrs_;
  const int count_;
  const int bytes_;
  const ReductionFunction<T>* fn_;

  T** inbox_;
  T* outbox_;
  verb_ctx_t ibv_;
  mem_registration_t mem_;
  rd_connections_t rd_;
  std::unique_ptr<transport::Buffer> sendDataBuf_;
  std::unique_ptr<transport::Buffer> recvDataBuf_;

  int dummy_;
  std::unique_ptr<transport::Buffer> sendNotificationBuf_;
  std::unique_ptr<transport::Buffer> recvNotificationBuf_;
};

} // namespace gloo
