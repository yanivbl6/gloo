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

#include "mlx5dv_mgr.h"
#include "assert.h"



qp_ctx::qp_ctx(struct ibv_qp* qp, struct ibv_cq* cq){
        int ret;
        struct mlx5dv_obj dv_obj = {};

        this->qp = (struct mlx5dv_qp*)  malloc(sizeof(struct mlx5dv_qp));
	this->cq = (struct mlx5dv_cq*)  malloc(sizeof(struct mlx5dv_cq));
        this->qpn = qp->qp_num;

        dv_obj.qp.in = qp;
        dv_obj.qp.out = this->qp;
        dv_obj.cq.in = cq;
        dv_obj.cq.out = this->cq;

                //pingpong_context
	ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ  );
	this->write_cnt = 0;
	this->exe_cnt = 0;
	this->dbseg.qpn_ds = htobe32(qpn << 8);
	this->phase = 0;
	this->offset = (this->qp->sq.stride  * (this->qp->sq.wqe_cnt / 2)) / sizeof(uint32_t);
	this->cmpl_cnt = 0;
	this->poll_cnt = 0;
	volatile void* tar =  (volatile void*) this->cq->buf;

	printf("wqe_cnt = %d, stride = %d\n",this->qp->sq.wqe_cnt,this->qp->sq.stride);
        printf("cqn num = %d\n", this->cq->cqn);

}

qp_ctx::~qp_ctx(){
	free(this->qp);
	free(this->cq);
}


ValRearmTasks::ValRearmTasks(){
	size= 0;
	buf_size=4;
	ptrs = (uint32_t**) malloc(buf_size  * sizeof(uint32_t*));
}

ValRearmTasks::~ValRearmTasks(){
	free(ptrs);
}

void ValRearmTasks::add(uint32_t* ptr){
	++size;
	if (size > buf_size){
		uint32_t** tmp_ptrs = (uint32_t**) malloc((buf_size * 2)  * sizeof(uint32_t*));
		int i = 0;
		for (i = 0; i < buf_size; ++i){
			tmp_ptrs[i] = ptrs[i];
		}
		buf_size = buf_size *2;
		free(ptrs);
		ptrs = tmp_ptrs;
	}
	ptrs[size-1] = ptr;
}

void RearmTasks::add(uint32_t* ptr, int inc){
	//MapIt it =  map.find(inc);
	//if (it != map.end()){
	//	map[inc].add(ptr);
	//}
	map[inc].add(ptr);
}

int qp_ctx::poll(int x){
	uint32_t val = (this->poll_cnt + x - 1);
	volatile void* tar =  (volatile void*)  ((volatile char*) this->cq->buf  + (val & (this->cq->cqe_cnt-1))  * this->cq->cqe_size);
	volatile struct cqe64* cur_cqe = (volatile struct cqe64*) tar;
        uint8_t opcode = cur_cqe->op_own >> 4;

        if (opcode != 15 && !((cur_cqe->op_own & 1) ^ !!(val  & (this->cq->cqe_cnt)))){
                if (opcode==13 || opcode == 14){
                        printf("bad CQE: %X\n hw_syn = %X, vendor_syn = %X, syn = %X\n",cur_cqe->wqe_counter, cur_cqe->hw_syn,cur_cqe->vendor_syn,cur_cqe->syn);
                }
                this->cq->dbrec[0] = htobe32(val & 0xffffff);
                (this->poll_cnt) += x;

                return 1;
        } else {
                return 0;
        }
}


void qp_ctx::db(){
	struct mlx5_db_seg db;
	exe_cnt += (qp->sq.wqe_cnt/2);
        dbseg.opmod_idx_opcode = htobe32(exe_cnt << 8);
	udma_to_device_barrier();
	qp->dbrec[1] = htobe32(exe_cnt);
	pci_store_fence();
	*(uint64_t*) qp->bf.reg = *(uint64_t*) &(dbseg);
	pci_store_fence();
}

static inline void mlx5dv_set_remote_data_seg(struct mlx5_wqe_raddr_seg *seg,
			 uint64_t addr, uint32_t rkey)
{
	seg->raddr	= htobe64(addr);
	seg->rkey       = htonl(rkey);
	seg->reserved	= 0;
}

static void set_vectorcalc_seg(struct mlx5_wqe_vectorcalc_seg *vseg,
                               uint8_t op, uint8_t type, uint8_t chunks, uint16_t num_vectors )
{
                vseg->calc_operation = htobe32(op << 24);
                vseg->options = htobe32(type << 24 |
#if __BYTE_ORDER == __LITTLE_ENDIAN
                                                 1UL << 22 |
#elif __BYTE_ORDER == __BIG_ENDIAN
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif
                                                 chunks << 16 |
                                                 num_vectors);
}

static inline void cd_set_wait(struct mlx5_wqe_coredirect_seg  *seg,
                         uint32_t index, uint32_t number)
{
        seg->index    = htonl(index);
        seg->number   = htonl(number);
}

void qp_ctx::write(struct ibv_sge* local, struct ibv_sge* remote, int signal){
    struct mlx5_wqe_ctrl_seg *ctrl;
    struct mlx5_wqe_raddr_seg *rseg;
    struct mlx5_wqe_data_seg *dseg;
    const uint8_t ds = 3;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl = (struct mlx5_wqe_ctrl_seg*)  ((char*)  qp->sq.buf + qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), MLX5_OPCODE_RDMA_WRITE_IMM, 0, qpn, ((signal)?8:0)  , ds, 0, 0);
    rseg = (struct mlx5_wqe_raddr_seg*)(ctrl + 1);
    mlx5dv_set_remote_data_seg(rseg, remote->addr, remote->lkey);
    dseg = (struct mlx5_wqe_data_seg*)(rseg + 1);
    mlx5dv_set_data_seg(dseg, local->length, local->lkey, local->addr);
    write_cnt+=1;   
}

void qp_ctx::reduce_write(struct ibv_sge* local, struct ibv_sge* remote, uint16_t num_vectors, uint8_t op, uint8_t type){
    struct mlx5_wqe_ctrl_seg *ctrl; //1
    struct mlx5_wqe_raddr_seg *rseg; //1
    struct mlx5_wqe_vectorcalc_seg *vseg;  //2
    struct mlx5_wqe_data_seg *dseg; //1
    const uint8_t ds = 5;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl = (struct mlx5_wqe_ctrl_seg*) ( (char*) qp->sq.buf + qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), MLX5_OPCODE_RDMA_WRITE_IMM, 0xff, qpn, 0 , ds, 0, 0);
    rseg = (struct mlx5_wqe_raddr_seg*)(ctrl + 1);
    mlx5dv_set_remote_data_seg(rseg, remote->addr, remote->lkey);
    vseg = (struct mlx5_wqe_vectorcalc_seg*)(rseg + 1);
    set_vectorcalc_seg(vseg, op, type, 4, num_vectors);
    dseg = (struct mlx5_wqe_data_seg*)(vseg + 1);
    mlx5dv_set_data_seg(dseg, local->length, local->lkey, local->addr);
    write_cnt+=2;
}

#define CE 0

void qp_ctx::cd_send_enable(qp_ctx* slave_qp){
    struct mlx5_wqe_ctrl_seg *ctrl; //1
    struct mlx5_wqe_coredirect_seg *wseg; //1
    const uint8_t ds = 2;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl = (struct mlx5_wqe_ctrl_seg*) ( (char*)  qp->sq.buf + qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), 0x17, 0x00, qpn, CE , ds, 0, 0);
    wseg = (struct mlx5_wqe_coredirect_seg*)(ctrl + 1);
    cd_set_wait(wseg, slave_qp->write_cnt, slave_qp->qpn);
    this->tasks.add((uint32_t*) &(wseg->index),  slave_qp->qp->sq.wqe_cnt);

    write_cnt+=1;
}

void qp_ctx::cd_recv_enable(qp_ctx* slave_qp, uint32_t index){
    struct mlx5_wqe_ctrl_seg *ctrl; //1
    struct mlx5_wqe_coredirect_seg *wseg; //1
    const uint8_t ds = 2;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl = (struct mlx5_wqe_ctrl_seg*) ((char*)  qp->sq.buf + qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), 0x16, 0x00, qpn, CE  , ds, 0, 0);
    wseg = (struct mlx5_wqe_coredirect_seg*)(ctrl + 1);
    cd_set_wait(wseg, slave_qp->qp->rq.wqe_cnt, slave_qp->qpn);
    this->tasks.add((uint32_t*)  &(wseg->index), index);

    write_cnt+=1;
}

void qp_ctx::cd_wait(uint32_t cqe_num, uint32_t index, uint32_t inc ){
    struct mlx5_wqe_ctrl_seg *ctrl; //1
    struct mlx5_wqe_coredirect_seg *wseg; //1
    const uint8_t ds = 2;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl = (struct mlx5_wqe_ctrl_seg*)  ( (char*) qp->sq.buf + qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), 0x0f, 0x00, qpn, CE , ds, 0, 0);
    wseg = (struct mlx5_wqe_coredirect_seg*)(ctrl + 1);
    cd_set_wait(wseg, index, cqe_num);

    this->tasks.add((uint32_t*)  &(wseg->index), inc);

    write_cnt+=1;
}

void qp_ctx::cd_wait(qp_ctx* slave_qp, uint32_t increment, uint32_t index){
	this->cd_wait(slave_qp->cq->cqn, slave_qp->cmpl_cnt, increment);
	slave_qp->cmpl_cnt+= index;
}


void qp_ctx::nop(size_t num_pad, int signal){
    struct mlx5_wqe_ctrl_seg *ctrl; //1
    const uint8_t ds = (num_pad*(qp->sq.stride/ 16));
    int wqe_count = qp->sq.wqe_cnt;
    ctrl = (struct mlx5_wqe_ctrl_seg*)  ( (char*) qp->sq.buf + qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, write_cnt, 0x00, 0x00, qpn, (signal==1)?8:0 , ds, 0, 0);
    write_cnt+=num_pad;
}

void qp_ctx::pad(int half){
	int wqe_count = qp->sq.wqe_cnt;
	int pad_size = 8;
	int target_count = half?(wqe_count/2):(wqe_count);
	if (write_cnt + pad_size > target_count){
		printf("ERROR = wqe buffer exceeded! should be at least %d, is %d\n", write_cnt , target_count  );
	} else if (write_cnt + pad_size < target_count/2) {
		printf("ERROR = wqe buffer too big!\n");
	}

	while (write_cnt + pad_size < target_count ){
		this->nop(pad_size,0);
	}
	this->nop(target_count - write_cnt,half);
}

void qp_ctx::dup(){
	int wqe_count = qp->sq.wqe_cnt;
        void* first_half = qp->sq.buf;
	void* second_half = (void*) ((char*) qp->sq.buf + qp->sq.stride * (wqe_count / 2));
	memcpy(second_half, first_half, qp->sq.stride * (wqe_count / 2));
}

void qp_ctx::rearm(){
	this->tasks.exec(this->offset, this->phase);
	phase = !(phase);
}

void RearmTasks::exec(uint32_t offset, int phase){
	for (MapIt it = this->map.begin();  it != map.end()  ; ++it){
		it->second.exec(it->first, offset , phase);
	}
}

void ValRearmTasks::exec(uint32_t inc, uint32_t offset, int phase){
	if (!phase){
		for (int i=0; i < size; ++i){
			ptrs[i][offset] =  htonl(ntohl(ptrs[i][0]) + inc);	
		}
	} else {
                for (int i=0; i < size; ++i){
                        ptrs[i][0] =  htonl( ntohl( ptrs[i][offset]) + inc);
                }
	}
}

void qp_ctx::printSq(){
	printf("Sq: %dX%d\n", qp->sq.stride,  qp->sq.wqe_cnt);
	print_buffer(this->qp->sq.buf, qp->sq.stride * qp->sq.wqe_cnt);
}

void qp_ctx::printRq(){
        printf("Rq:\n");
        print_buffer(this->qp->rq.buf, qp->rq.stride * qp->rq.wqe_cnt);
}

void qp_ctx::printCq(){
        printf("Cq:\n");
        print_buffer(this->cq->buf, cq->cqe_size * cq->cqe_cnt);
}

void print_buffer(volatile void* buf, int count){
        printf("buffer:\n");
        int i = 0;
        for (i = 0; i< count/sizeof(int); ++i  ){
                if (i%16==0){
                        printf("\n");
                }
                printf("%08X  ", ntohl(((int*) buf)[i]));
        }
        printf("\n");
}


