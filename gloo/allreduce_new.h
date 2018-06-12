/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#define PIPELINE_DEPTH 1

#include <alloca.h>
#include <stddef.h>
#include <string.h>

#include "gloo/algorithm.h"
#include "gloo/context.h"
#include "gloo/mlx5dv_mem.h"
#include "gloo/mlx5dv_mgr.h"
#include "gloo/mlx5dv_mqp.h"

#include <ctime>
#include <vector>

namespace gloo {


typedef struct mem_registration {
  Iov usr_vec;
  UmrMem *umr_mem;
  TempMem *tmpMem;
} mem_registration_t;

int p2p_exchange(void* comm, volatile void* send_buf , volatile void* recv_buf , size_t size , uint32_t peer, uint32_t tag){
   std::shared_ptr<Context>* ctx = static_cast<std::shared_ptr<Context>*>(comm);
  auto &pair = (*ctx)->getPair(peer);
  auto sendBuf = pair->createSendBuffer(tag, (void*) send_buf, size);
  auto recvBuf = pair->createRecvBuffer(tag, (void*) recv_buf, size);
  sendBuf->send();
  sendBuf->waitSend();
  recvBuf->waitRecv();
}

class rd_peer_t {
public:
  rd_peer_t()
      : outgoing_buf(NULL), incoming_buf(NULL){};
  ~rd_peer_t() {
    delete (qp);
    delete (this->incoming_buf);
    delete (this->outgoing_buf);
  };

  DoublingQp* qp;
  NetMem *outgoing_buf;
  NetMem *incoming_buf;
};

typedef struct rd_connections {
  NetMem *result;

  CommGraph* graph;
  LoopbackQp* lqp;

  unsigned peers_cnt;
  rd_peer_t *peers;
} rd_connections_t;

template <typename T> class AllreduceNew : public Algorithm {
public:
  AllreduceNew(const std::shared_ptr<Context> &context,
               const std::vector<T *> &ptrs, const int count,
               const ReductionFunction<T> *fn = ReductionFunction<T>::sum)
      : Algorithm(context), ptrs_(ptrs), count_(count),
        bytes_(count_ * sizeof(T)), fn_(fn) {
    if (this->contextSize_ == 1) {
      return;
    }

    /* Step #1: Initialize verbs for all to use */
    PRINT("starting AllreduceNew");

    ibv_ = new verb_ctx_t();

    PRINT("Verbs initiated");

    /* Step #2: Register existing memory buffers with UMR */
    register_memory();
    PRINT("register_memory DONE");

    /* Step #3: Connect to the (recursive-doubling) peers and pre-post
     * operations */
    connect_and_prepare();
    PRINT("connect_and_prepare DONE");
    mone = 0;
  }

  virtual ~AllreduceNew() {
    teardown();
    deregister_memory();
    delete (ibv_);
  }

  void run() {
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
    //		fprintf(stderr,"iteration: %d\n", mone);
    rd_.graph->mqp->qp->db();
    rd_.graph->mqp->qp->rearm();

    int res = 0;
    uint64_t count = 0;

    //		clock_t begin = clock()*1E6;

    while (!res) {
      res = rd_.lqp->qp->poll();

      ++count;
#ifdef HANG_REPORT

      unsigned step_count = 0;
      while ((1 << ++step_count) < contextSize_)
        ;

      if (count == 1000000000) {

        fprintf(stderr, "iteration: %d\n", mone);
        fprintf(stderr, "managment qp:");
        rd_.graph->mqp->print();

        fprintf(stderr, "loopback qp:");
        rd_.lqp->print();
        for (int k = 0; k < step_count; ++k) {
          fprintf(stderr, "rc qp %d:", k);
          rd_.peers[k].qp->print();
        }
      }

#endif
    }
//		clock_t end = clock()*1E6;
//		double elapsed_us = double(end - begin) / CLOCKS_PER_SEC;

//		fprintf(stderr,"iteration: %d, time = %f\n", mone, elapsed_us );

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
          for (int i = 0; i < step_count; ++i) {
            fprintf(stderr, "Incoming %d:\n", i);
            float *buf =
                (float *)((void *)rd_.peers[i].incoming_buf->sg()->addr);
            print_values(buf, count_);
          }
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
    ++mone;
  }

  void register_memory() {

    unsigned step_idx, step_count = 0;
    while ((1 << ++step_count) < contextSize_)
      ;

    pipeline = PIPELINE_DEPTH;
    while (step_count % pipeline) {
      --pipeline;
    }

    /* Register the user's buffers */
    for (int buf_idx = 0; buf_idx < ptrs_.size(); buf_idx++) {
      mem_.usr_vec.push_back(new UsrMem(ptrs_[buf_idx], bytes_, ibv_));
    }
    mem_.umr_mem = new UmrMem(mem_.usr_vec, ibv_);

    mem_.tmpMem = new TempMem(bytes_, pipeline, ibv_);
  }

  void deregister_memory() {

    delete mem_.tmpMem;

    PRINT("Freeing UMR");
    int buf_idx;
    delete (mem_.umr_mem);
    PRINT("Freeing user memory");
    freeIov(mem_.usr_vec);
  }

  void connect_and_prepare() {
    int inputs = ptrs_.size();
    unsigned step_idx, step_count = 0;
    while ((1 << ++step_count) < contextSize_)
      ;

    verb_ctx_t *ctx = (this->ibv_);

    /* Create a single management QP */
    rd_.graph = new CommGraph(ctx);
    CommGraph* sess = rd_.graph;
    PRINT("created MGMT QP");

    /* Create a loopback QP */
    rd_.lqp = new LoopbackQp(sess);
    LoopbackQp* lqp = rd_.lqp;
    PRINT("loopback connected");

    rd_.result = new HostMem(bytes_, ibv_);

    rd_.peers_cnt = step_count;
    rd_.peers = new rd_peer_t[step_count];
    if (!rd_.peers) {
      throw "malloc failed";
    }
    /* Establish a connection with each peer */

    for (step_idx = 0; step_idx < step_count; step_idx++) {



      /* calculate the rank of each peer */
      int leap = 1 << step_idx;
      if ((contextRank_ % (leap << 1)) >= leap) {
        leap *= -1;
      }
      uint32_t mypeer = contextRank_ + leap;

      uint32_t slot = this->context_->nextSlot();
      
      rd_.peers[step_idx].incoming_buf = new RefMem(mem_.tmpMem->next());

      rd_.peers[step_idx].qp = new DoublingQp(sess, &p2p_exchange , (void*) &(this->context_),
                       mypeer, slot, rd_.peers[step_idx].incoming_buf);
      PRINT("Creating RC QP - Done");


      Iov umr_iov{rd_.result, rd_.peers[step_idx].incoming_buf}; 
      rd_.peers[step_idx].outgoing_buf = new UmrMem(umr_iov, ibv_); 
    }

    PRINT("Creating all RC QPs - Done");


    lqp->reduce_write(mem_.umr_mem, rd_.result, inputs,
                                     MLX5DV_VECTOR_CALC_OP_ADD,
                                     MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);

    sess->wait(lqp);
    for (step_idx = 0; step_idx < step_count; step_idx++) {
      if (step_idx >= pipeline) {
        sess->wait(rd_.peers[step_idx].qp);
      }
      rd_.peers[step_idx].qp->writeCmpl(rd_.result);
      sess->wait(rd_.peers[step_idx].qp);
      sess->wait_send(rd_.peers[step_idx].qp);
      lqp->reduce_write(rd_.peers[step_idx].outgoing_buf,
                                       rd_.result, 2, MLX5DV_VECTOR_CALC_OP_ADD,
                                       MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32);

      sess->wait(lqp);
      rd_.peers[(step_idx + pipeline) % step_count].qp->sendCredit();
    }

    for (uint32_t buf_idx = 0; buf_idx < inputs; buf_idx++) {
      lqp->write(rd_.result, mem_.usr_vec[buf_idx]);
    }

    sess->wait(lqp);

    for (step_idx = 0; step_idx < pipeline; step_idx++) {
      sess->wait(rd_.peers[step_idx].qp);
    }

    PRINT("Graph building - Done");
    rd_.graph->finish();
    if (0){
      unsigned step_count = 0;
      while ((1 << ++step_count) < contextSize_)
        ;

      fprintf(stderr, "managment qp:\n");
      rd_.graph->mqp->print();
      fprintf(stderr, "loopback qp:\n");
      rd_.lqp->print();
      for (int k = 0; k < step_count; ++k) {
        fprintf(stderr, "rc qp %d:\n", k);
        rd_.peers[k].qp->print();
      }
      
    }
  }

  void teardown() {
    delete (rd_.lqp);
    delete (rd_.graph);
    delete (rd_.result);
    delete[](rd_.peers);
    PRINT("Teardown completed");
  }

protected:
  std::vector<T *> ptrs_;
  const int count_;
  const int bytes_;
  verb_ctx_t *ibv_;
  mem_registration_t mem_;
  rd_connections_t rd_;
  const ReductionFunction<T> *fn_;
  int mone;
  int pipeline;
};




} // namespace gloo
