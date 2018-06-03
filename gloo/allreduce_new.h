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

	/* Step #1: Calculate the neighbors for each stage of recursive doubling */
	unsigned step_idx, step_count = 0;
	while ((1 << ++step_count) < contextRank_);
	rank_t *neighbors = alloca(step_count * sizeof(rank_t));
	for (step_idx = 0; step_idx < step_count; step_idx++) {
		int leap = 1 << step_idx;
		if ((contextRank_ % (leap << 1)) >= leap) {
			leap *= -1;
		}
		neighbors[step_idx] = contextRank_ + leap;
	}

	/* Step #2: Create the buffers for each step */
    inbox_ = static_cast<T*>(malloc(bytes_));
    outbox_ = static_cast<T*>(malloc(bytes_));

    if (this->contextSize_ == 1) {
      return;
    }


    int* buf = static_cast<int*>(malloc(sizeof(int)));


    (*buf) = this->contextRank_;
    int ret = p2p((void*) buf,sizeof(int),0,1);

    auto& leftPair = this->getLeftPair();
    auto& rightPair = this->getRightPair();
    auto slot = this->context_->nextSlot();
     
    (*buf) = this->contextRank_*10;
    ret = p2p((void*) buf,sizeof(int),1,0);



    if (count == 64){
	    uint32_t vec[64];
	    rearm_tasks tasks;
	    print_buffer((void*) &vec, 64);
	    for (int i = 0; i< 32; ++i){
	      vec[i] = i;
	      tasks.add(&vec[i], i%3);    
	    }
	    tasks.exec(32*sizeof(uint32_t), 0);
	    print_buffer((void*) &vec, 64);
	    tasks.exec(32*sizeof(uint32_t), 1);
            print_buffer((void*) &vec, 64);
    }
    
    


    // Buffer to send to (rank+1).
    sendDataBuf_ = rightPair->createSendBuffer(slot, outbox_, bytes_);

    // Buffer that (rank-1) writes to.
    recvDataBuf_ = leftPair->createRecvBuffer(slot, inbox_, bytes_);

    // Dummy buffers for localized barrier.
    // Before sending to the right, we only need to know that the node
    // on the right is done using the inbox that's about to be written
    // into. No need for a global barrier.
    auto notificationSlot = this->context_->nextSlot();
    sendNotificationBuf_ =
      leftPair->createSendBuffer(notificationSlot, &dummy_, sizeof(dummy_));
    recvNotificationBuf_ =
      rightPair->createRecvBuffer(notificationSlot, &dummy_, sizeof(dummy_));
  }




  virtual ~AllreduceNew() {
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

 protected:
  std::vector<T*> ptrs_;
  const int count_;
  const int bytes_;
  const ReductionFunction<T>* fn_;

  T* inbox_;
  T* outbox_;
  std::unique_ptr<transport::Buffer> sendDataBuf_;
  std::unique_ptr<transport::Buffer> recvDataBuf_;

  int dummy_;
  std::unique_ptr<transport::Buffer> sendNotificationBuf_;
  std::unique_ptr<transport::Buffer> recvNotificationBuf_;
};

} // namespace gloo
