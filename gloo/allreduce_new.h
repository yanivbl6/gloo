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

#include "gloo/algorithm.h"
#include "gloo/context.h"
#include "gloo/testbuild.h"

namespace gloo {

#warning "ALLREDUCE_NEW was built"

#define DEFAULT_KNOMIAL_RADIX (2) /* Binomial tree */

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
		/* Step #1: figure out who my neighbors are */
		rank_t father, *children;
		unsigned children_cnt;
		if (calc_tree(DEFAULT_KNOMIAL_RADIX, &father,
				      &children, &children_cnt)) {
			return; // TODO: indicate failure
		}

		/* Step #2: Allocate buffers to talk to each neighbor */
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

		int g = power(5,3);
		printf("5^3 = %d\n",g);

		(*buf) = this->contextRank_*10;
		ret = p2p((void*) buf,sizeof(int),1,0);

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

	int calc_tree(unsigned knomial_radix, rank_t *father,
			      rank_t **children, unsigned *children_cnt)
	{
	    int ret;
	    rank_t child_count;
	    rank_t next_child;
	    rank_t next_father;
	    rank_t first_child			= 1;
	    rank_t first_father 		= 0;
	    rank_t my_rank 				= contextRank_;
	    rank_t node_count 			= contextSize_;
	    unsigned tree_radix 		= knomial_radix - 1;
	    unsigned children_alloced	= 0;
	    *father 					= -1;
	    *children_cnt 				= 0;
	    *children 					= 0;

	    /* Build the entire graph */
	    next_child = first_child;
	    while (next_child < node_count) {
	    	for (child_count = 0; child_count < tree_radix; child_count++) {
	    		for (next_father = first_father;
	    			 (next_father < first_child) && (next_child < node_count);
	    			 next_father++, next_child++) {
	    			if (next_child == my_rank) {
	    				*father = next_father;
	    			}

	    			if (next_father == my_rank) {
	    				if (children_cnt == children_alloced) {
	    					children_alloced += tree_radix;
	    					*children = realloc(*children, tree_radix * sizeof(**children));
	    					if (!*children)
	    						return -1;
	    				}
	    				(*children)[children_cnt++] = next_child;
	    			}
	    		}
	    	}

	    	first_child += (first_child - first_father) * tree_radix;
	    }
	    return 0;
	}
};

} // namespace gloo
