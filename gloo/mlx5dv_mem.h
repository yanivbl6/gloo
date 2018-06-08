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
#pragma once

//#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
extern "C"{
#include <infiniband/mlx5dv.h>
}

#include "mlx5dv_ctx.h"

#include <infiniband/verbs_exp.h>

#include <vector>

class NetMem{
public:
	NetMem(){};
	virtual ~NetMem()=0;
	struct ibv_sge* sg(){return &sge;};
        struct ibv_mr* getMr(){return mr;};

protected:
	struct ibv_sge sge;
        struct ibv_mr*  mr;
};


typedef std::vector<NetMem*> Iov;
typedef Iov::iterator Iovit;

void freeIov(Iov &iov);

class HostMem: public NetMem{
public:
	HostMem(size_t length,  verb_ctx_t* ctx);
	~HostMem();
private:
	void* buf;
};


class UsrMem: public NetMem{
public:
	UsrMem(void* buf ,size_t length,  verb_ctx_t* ctx);
	~UsrMem();
};



struct ibv_mr *register_umr(Iov &iov,
                verb_ctx_t* ctx); 



class UmrMem: public NetMem{
public:
	UmrMem(Iov& mem_reg, verb_ctx_t* ctx);
	~UmrMem();
};

class RemoteMem: public NetMem{
public:
        RemoteMem(uint64_t addr, uint32_t rkey);
	~RemoteMem(){};
};


class pipelined_memory{
public:
	pipelined_memory(size_t length, size_t depth);
	~pipelined_memory();	


public:


        struct ibv_sge outgoing_buf;
        struct ibv_mr* outgoing_mr;
        struct ibv_sge incoming_buf;
        struct ibv_mr* incoming_mr;



private:
	size_t length;
	size_t depth;
	struct ibv_qp* umr_qp;
	struct ibv_cq* umr_cq;

};

