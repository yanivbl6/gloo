/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found inpqp* LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#define RING_PIPELINE_DEPTH 1

#include <alloca.h>
#include <stddef.h>
#include <string.h>

#include "gloo/algorithm.h"
#include "gloo/context.h"
#include "third-party/pcx/pcx_mem.h"
#include "third-party/pcx/qps.h"

#include <ctime>
#include <vector>

namespace gloo {

typedef struct mem_registration_ring {
  Iop usr_vec;  
  PipeMem *usr_mem;
  PipeMem *tmpMem;
} mem_registration_ring_t;

int ring_exchange(void *comm, volatile void *send_buf, volatile void *recv_buf,
                 size_t size, uint32_t peer, uint32_t tag) {
  std::shared_ptr<Context> *ctx = static_cast<std::shared_ptr<Context> *>(comm);
  auto &pair = (*ctx)->getPair(peer);
  auto sendBuf = pair->createSendBuffer(tag, (void *)send_buf, size);
  auto recvBuf = pair->createRecvBuffer(tag, (void *)recv_buf, size);
  sendBuf->send();
  sendBuf->waitSend();
  recvBuf->waitRecv();
}

class step_ctx {
public:
  step_ctx() : outgoing_buf(NULL), umr_iov(){};
  ~step_ctx() {
    delete (this->outgoing_buf);
    freeIov(umr_iov);
  };
  Iov     umr_iov;
  NetMem *outgoing_buf;
};

typedef struct rd_connections_ring {

  CommGraph *graph;
  LoopbackQp *lqp;
  RingPair   *pqp;

  unsigned iters_cnt;
  step_ctx *iters;
} rd_connections_ring_t;

template <typename T> class PcxAllreduceRing : public Algorithm {
public:
  PcxAllreduceRing(const std::shared_ptr<Context> &context,
               const std::vector<T *> &ptrs, const int count,
               const ReductionFunction<T> *fn = ReductionFunction<T>::sum)
      : Algorithm(context), ptrs_(ptrs), count_(count),
        bytes_(count_ * sizeof(T)), fn_(fn) {
    if (this->contextSize_ == 1) {
      return;
    }

    /* Step #1: Initialize verbs for all to use */
    PRINT("starting PcxAllreduceRing");
    ibv_ = VerbCtx::getInstance();
    PRINT("Verbs initiated");

    /* Step #2&3: Connect to the (recursive-doubling) iters and pre-post
     * operations */
    connect_and_prepare();
    PRINT("connect_and_prepare DONE");
    mone = 0;
  }

  virtual ~PcxAllreduceRing() {
    teardown();
    deregister_memory();
    VerbCtx::remInstance();
  }

  void run() {
    debug_write_input();
    rd_.graph->mqp->qp->db();
    rd_.graph->mqp->qp->rearm();

    int res = 0;
    uint64_t count = 0;

    while (!res) {
      res = rd_.lqp->qp->poll();

      ++count;
      debug_hang_report(count);
    }
    debug_check_output();
    ++mone;
  }

  void deregister_memory() {
    delete(mem_.tmpMem);
    PRINT("Freeing UMR");
    int buf_idx;
    PRINT("Freeing user memory");
    freeIop(mem_.usr_vec);
  }

  void connect_and_prepare() {
    int inputs = ptrs_.size();
    unsigned step_idx, step_count = 0;
    step_count = contextSize_;

    VerbCtx *ctx = (this->ibv_);

    /* Create a single management QP */
    rd_.graph = new CommGraph(ctx); // does lock
    CommGraph *sess = rd_.graph;
    PRINT("created MGMT QP");

    /* Step #2: Register existing memory buffers with UMR */

    pipeline = RING_PIPELINE_DEPTH;
    while (step_count % pipeline) {
      --pipeline;
    }

    pipeline = step_count*2;

    pieceSize = bytes_ / step_count;

    for (int buf_idx = 0; buf_idx < inputs; buf_idx++) {
      mem_.usr_vec.push_back(new PipeMem((void*) ptrs_[buf_idx], pieceSize , (size_t) step_count, ibv_));
    }

    int temp_type = PCX_MEMORY_TYPE_MEMIC;
    temp_type = PCX_MEMORY_TYPE_HOST;

    mem_.tmpMem = new PipeMem(pieceSize, pipeline, ibv_,   temp_type );


    /* Create a loopback QP */
    rd_.lqp = new LoopbackQp(sess);
    LoopbackQp *lqp = rd_.lqp;
    PRINT("loopback connected");

    rd_.iters_cnt = step_count;
    rd_.iters = new step_ctx[step_count];
    if (!rd_.iters) {
      throw "malloc failed";
    }
    /* Establish a connection with each peer */

    uint32_t myRank = contextRank_;
    uint32_t slot1 = this->context_->nextSlot();
    uint32_t slot2 = this->context_->nextSlot();

    rd_.pqp = new RingPair(sess, &ring_exchange, (void *)&(this->context_), myRank, step_count , slot1 , slot2 , mem_.tmpMem); 
    PRINT("RC ring Qps created");

    RingQp* right = rd_.pqp->right;
    RingQp* left = rd_.pqp->left;

    for (step_idx = 0; step_idx < step_count; step_idx++) {
      size_t piece = (step_count + myRank - step_idx) % step_count;
      for (int k = 0; k < inputs; ++k) {
        rd_.iters[step_idx].umr_iov.push_back(new RefMem((*mem_.usr_vec[k])[piece]));
      }
      if (step_idx > 0){
      	rd_.iters[step_idx].umr_iov.push_back(new RefMem(mem_.tmpMem->next()));
      }
      rd_.iters[step_idx].outgoing_buf = new UmrMem(rd_.iters[step_idx].umr_iov, ibv_);
    }
    PRINT("UMR registeration done");

    int credits = pipeline;

    if (credits>1){
      right->reduce_write(rd_.iters[0].outgoing_buf, 0, inputs, MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);
      --credits;
    } else {
      right->reduce_write_cmpl(rd_.iters[0].outgoing_buf, 0, inputs, MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);
      sess->wait_send(right);
      left->sendCredit();
      sess->wait(right);
      credits = pipeline;
    }
    sess->wait(left);

    PRINT("initial send");   
 
    for (step_idx = 1; step_idx < step_count; step_idx++) {
      if (credits==1){
        right->reduce_write_cmpl(rd_.iters[step_idx].outgoing_buf, step_idx, (inputs+1) , MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32); 
        sess->wait_send(right);
        left->sendCredit();
        sess->wait(right);
        credits = pipeline;
      } else {
        right->reduce_write(rd_.iters[step_idx].outgoing_buf, step_idx, (inputs+1) , MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);
        --credits;
      }
      sess->wait(left);
    }

    PRINT("reduce-scatter");

    //done with the alreduce_scatter
    size_t last_frag = (step_count-1);

    for (step_idx = 0; step_idx < step_count; step_idx++) {
      RefMem newVal((*mem_.tmpMem)[last_frag]);

      size_t piece = (step_idx + myRank) % step_count;
      if (credits==1){
        right->writeCmpl(&newVal, step_count + step_idx );
        for (uint32_t buf_idx = 0; buf_idx < inputs; buf_idx++) {
          lqp->write(&newVal, rd_.iters[step_idx].umr_iov[buf_idx]);
        }
        sess->wait_send(right);
        sess->wait(lqp);
        left->sendCredit();
        sess->wait(right); //for credit
        credits = pipeline;
      } else {
        right->write(&newVal, step_count + step_idx);
        for (uint32_t buf_idx = 0; buf_idx < inputs; buf_idx++) {
          lqp->write(&newVal, rd_.iters[step_idx].umr_iov[buf_idx]);
        }
      }
      sess->wait(left); //for data
      ++last_frag;
    }

    PRINT("allgather");

    if (credits != pipeline){
      left->sendCredit();
      sess->wait(right);
      PRINT("credit passing");
    }

    PRINT("Graph building - Done");
    rd_.graph->finish(); // unlocks
  }

  void teardown() {
    delete (rd_.lqp);
    delete (rd_.graph);
    delete (rd_.pqp);
    delete[](rd_.iters);
    PRINT("Teardown completed");
  }

  void debug_write_input() {
#ifdef VALIDITY_CHECK
    for (int i = 0; i < ptrs_.size(); ++i) {
      // fprintf(stderr, "Input %d:\n",i);
      float *buf = (float *)ptrs_[i];
      for (int k = 0; k < count_; ++k) {
        buf[k] = ((float)k + i) + contextRank_ + mone;
      }
      // print_values(buf, count_);
    }
#endif
  }

  void debug_hang_report(uint64_t &count) {
#ifdef HANG_REPORT
    if (count == 1000000000) {
      fprintf(stderr, "iteration: %d\n", mone);
      fprintf(stderr, "poll cnt: %d\n", rd_.lqp->qp->get_poll_cnt());
      fprintf(stderr, "managment qp:\n");
      rd_.graph->mqp->print();
      fprintf(stderr, "loopback qp:\n");
      rd_.lqp->print();
      fprintf(stderr, "right qp:\n");
      rd_.pqp->right->print();
      fprintf(stderr, "left qp:\n");
      rd_.pqp->left->print();

    }
#endif
  }

  void debug_check_output() {
#ifdef VALIDITY_CHECK

    unsigned step_count = 0;
    while ((1 << ++step_count) < contextSize_)
      ;

    for (int i = 0; i < ptrs_.size(); ++i) {
      // fprintf(stderr, "Output %d:\n",i);
      int err = 0;
      float *buf = (float *)ptrs_[i];
      // print_values(buf, count_);
      for (int k = 0; k < count_; ++k) {
        int expected_base =
            ((k + mone) * 2 + ptrs_.size() - 1) * ptrs_.size() / 2;
        int expected_max =
            ((k + mone + contextSize_ - 1) * 2 + ptrs_.size() - 1) *
            ptrs_.size() / 2;
        float expected_result =
            (float)(expected_base + expected_max) * contextSize_ / 2;
        float result = buf[k];
        if (result != expected_result) {
          fprintf(stderr,
                  "ERROR: In Iteration %d\n expected: %.2f, got: %.2f\n", mone,
                  expected_result, result);
          for (int i = 0; i < ptrs_.size(); ++i) {
            fprintf(stderr, "Input %d:\n", i);
            float buf[count_];
            for (int k = 0; k < count_; ++k) {
              buf[k] = ((float)k + i) + contextRank_ + mone;
            }
            print_values(buf, count_);
          }
          mem_.tmpMem->print();
          fprintf(stderr, "Output %d:\n", i);
          print_values(buf, count_);
          // err = 1;
          break;
        }
      }
      if (err) {
        break;
      }
    }
#endif
  }

protected:
  std::vector<T *> ptrs_;
  const int count_;
  const int bytes_;
  VerbCtx *ibv_;
  mem_registration_ring_t mem_;
  rd_connections_ring_t rd_;
  const ReductionFunction<T> *fn_;
  int mone;
  int pipeline;
  size_t pieceSize;
};

} // namespace gloo
