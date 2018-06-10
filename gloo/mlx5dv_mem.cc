/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "mlx5dv_mem.h"

RemoteMem::RemoteMem(uint64_t addr, uint32_t rkey){
        sge.addr = addr;
        sge.lkey = rkey;
}


HostMem::HostMem(size_t length,  verb_ctx_t* ctx){
        this->buf = malloc(length);

	if (!this->buf){
		throw "No Memory";
	}

        this->sge.addr =  (uint64_t) buf;
	this->mr = ibv_reg_mr(ctx->pd, this->buf  , length, IB_ACCESS_FLAGS);
	if (!this->mr){
		throw "Reg mr failed";
	}
	this->sge.length = length;
        this->sge.lkey = this->mr->lkey;
}


NetMem::~NetMem(){}

HostMem::~HostMem(){
        ibv_dereg_mr(this->mr);
	free(this->buf);
}

UsrMem::UsrMem(void* buf, size_t length,  verb_ctx_t* ctx){
        this->sge.addr =  (uint64_t) buf;
	this->mr = ibv_reg_mr(ctx->pd, buf, length, IB_ACCESS_FLAGS);

	if (!this->mr){
		throw "Reg MR failed";
	}
	this->sge.length = length;
        this->sge.lkey = this->mr->lkey;
}

UsrMem::~UsrMem(){
        ibv_dereg_mr(this->mr);
}

UmrMem::UmrMem(Iov &iov,  verb_ctx_t* ctx){
	//return;
	this->mr = register_umr(iov,ctx);
	this->sge.lkey = mr->lkey;
	this->sge.length = iov[0]->sg()->length;
	this->sge.addr = iov[0]->sg()->addr;
}

UmrMem::~UmrMem(){
        ibv_dereg_mr(this->mr);
}

void freeIov(Iov &iov){
	for (Iovit it = iov.begin(); it != iov.end(); ++it){
		delete(*it);
	}
	iov.clear();
}





PCX_ERROR(NotEnoughKLMs)
PCX_ERROR(NoUMRKey)
PCX_ERROR(CreateMRFailed)
PCX_ERROR(UMR_PollFailed)
PCX_ERROR(UMR_CompletionInError)


PCX_ERROR_RES(UMR_PostFailed)

PCX_ERROR(EmptyUMR)

struct ibv_mr *register_umr(Iov &iov, verb_ctx_t* ctx){

	unsigned mem_reg_cnt = iov.size();

        if (mem_reg_cnt > ctx->attrs.umr_caps.max_klm_list_size) {
		PERR(NotEnoughKLMs);
	}

        if (mem_reg_cnt == 0) {
                PERR(EmptyUMR);
        }


	struct ibv_exp_mkey_list_container *umr_mkey = nullptr;
	if (mem_reg_cnt > ctx->attrs.umr_caps.max_send_wqe_inline_klms) {
		struct ibv_exp_mkey_list_container_attr list_container_attr;
		list_container_attr.pd                          = ctx->pd;
		list_container_attr.mkey_list_type              = IBV_EXP_MKEY_LIST_TYPE_INDIRECT_MR;
		list_container_attr.max_klm_list_size           = mem_reg_cnt;
		list_container_attr.comp_mask                   = 0;
		umr_mkey = ibv_exp_alloc_mkey_list_memory(&list_container_attr);
		if (!umr_mkey) {
			PERR(NoUMRKey);
		}
	} else {
        	umr_mkey = NULL;
	} 

	struct ibv_exp_create_mr_in mrin;
	memset(&mrin, 0, sizeof(mrin));
	mrin.pd                     = ctx->pd;
	mrin.attr.create_flags      = IBV_EXP_MR_INDIRECT_KLMS;
	mrin.attr.exp_access_flags  = IB_ACCESS_FLAGS;
	mrin.attr.max_klm_list_size = mem_reg_cnt;
	struct ibv_mr *res_mr = ibv_exp_create_mr(&mrin);
	if (!res_mr) {
		PERR(CreateMRFailed);
	}

	int buf_idx = 0;
	struct ibv_exp_mem_region* mem_reg = (struct ibv_exp_mem_region*) malloc(mem_reg_cnt * sizeof(struct ibv_exp_mem_region));
	for (buf_idx = 0; buf_idx < mem_reg_cnt; ++buf_idx) {
		mem_reg[buf_idx].base_addr = iov[buf_idx]->sg()->addr;
		mem_reg[buf_idx].length    = iov[buf_idx]->sg()->length;
		mem_reg[buf_idx].mr        = iov[buf_idx]->getMr();
		printf("addr = %ld, len = %d, mr = %d\n", mem_reg[buf_idx].base_addr,
							 mem_reg[buf_idx].length, mem_reg[buf_idx].mr->lkey);
	}

	/* Create the UMR work request */
	struct ibv_exp_send_wr wr, *bad_wr;
	memset(&wr, 0, sizeof(wr));
	wr.exp_opcode                                   = IBV_EXP_WR_UMR_FILL;
	wr.exp_send_flags                               = IBV_EXP_SEND_SIGNALED;
	wr.ext_op.umr.umr_type                          = IBV_EXP_UMR_MR_LIST;
	wr.ext_op.umr.memory_objects            	= umr_mkey;
	wr.ext_op.umr.modified_mr                       = res_mr;
	wr.ext_op.umr.base_addr                         = iov[0]->sg()->addr;
	wr.ext_op.umr.num_mrs                           = mem_reg_cnt;
	wr.ext_op.umr.mem_list.mem_reg_list     	= mem_reg;
	if (!umr_mkey) {
		wr.exp_send_flags                          |= IBV_EXP_SEND_INLINE;
	}

	/* Post WR and wait for it to complete */
	
	if (int res = ibv_exp_post_send(ctx->umr_qp, &wr, &bad_wr)) {
		RES_ERR(UMR_PostFailed,res);
	}
	struct ibv_wc wc;
	for (;;) {
		int ret = ibv_poll_cq(ctx->umr_cq, 1, &wc);
		if (ret < 0) {
			PERR(UMR_PollFailed);
		}
		if (ret == 1) {
			if (wc.status != IBV_WC_SUCCESS) {
				PERR(UMR_CompletionInError);
			}
			break;
		}
	}

        if (umr_mkey) {
        	ibv_exp_dealloc_mkey_list_memory(umr_mkey);
        }


	free(mem_reg);

	return res_mr;
}
