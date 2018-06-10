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

#include "mlx5dv_ctx.h"
verb_ctx_t::~verb_ctx_t() {
  if (ibv_destroy_qp(this->umr_qp)) {
    throw("Couldn't destroy QP");
  }

  if (ibv_destroy_cq(this->umr_cq)) {
    throw("Couldn't destroy CQ");
  }

  if (ibv_dealloc_pd(this->pd)) {
    throw("Couldn't deallocate PD");
  }

  if (ibv_close_device(this->context)) {
    throw("Couldn't release context");
  }
}

// verb_ctx_t::verb_ctx_t(char *ib_devname){
verb_ctx_t::verb_ctx_t() {
  char *ib_devname = NULL;

  struct ibv_device **dev_list = ibv_get_device_list(nullptr);
  struct ibv_device *ib_dev;
  if (!dev_list) {
    throw("Failed to get IB devices list");
  }

  if (!ib_devname) {
    ib_dev = dev_list[0];
    if (!ib_dev) {
      throw("No IB devices found");
    }
  } else {
    int i;
    for (i = 0; dev_list[i]; ++i)
      if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
        break;
    ib_dev = dev_list[i];
    if (!ib_dev) {
      throw("IB device not found");
    }
  }

  PRINT(ibv_get_device_name(ib_dev));
  this->context = ibv_open_device(ib_dev);
  ibv_free_device_list(dev_list);
  if (!this->context) {
    throw "Couldn't get context";
  }

  this->pd = ibv_alloc_pd(this->context);
  if (!this->pd) {
    PRINT("Couldn't allocate PD");
    goto clean_comp_channel;
  }

  this->channel = NULL; // TODO

  this->umr_cq = ibv_create_cq(this->context, CX_SIZE, NULL, NULL, 0);
  if (!this->umr_cq) {
    throw "Couldn't create CQ";
  }
  memset(&this->attrs, 0, sizeof(this->attrs));
  this->attrs.comp_mask = IBV_EXP_DEVICE_ATTR_UMR;
  if (ibv_exp_query_device(this->context, &this->attrs)) {
    throw "Couldn't query device attributes";
  }

  {
    struct ibv_exp_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.pd = this->pd;
    attr.send_cq = this->umr_cq;
    attr.recv_cq = this->umr_cq;
    attr.qp_type = IBV_QPT_RC;
    attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD |
                     IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS |
                     IBV_EXP_QP_INIT_ATTR_MAX_INL_KLMS;
    attr.exp_create_flags = IBV_EXP_QP_CREATE_UMR;
    attr.cap.max_send_wr = 1;
    attr.cap.max_recv_wr = 0;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 0;
    attr.max_inl_send_klms = 4;

    this->umr_qp = ibv_exp_create_qp(this->context, &attr);
    if (!this->umr_qp) {
      throw("Couldn't create UMR QP");
    }
  }

  {
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = 0;

    if (ibv_modify_qp(this->umr_qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                                  IBV_QP_PORT |
                                                  IBV_QP_ACCESS_FLAGS)) {
      throw("Failed to INIT the UMR QP");
    }
  }

  return; // SUCCESS!

clean_qp:
  ibv_destroy_qp(this->umr_qp);

clean_cq:
  ibv_destroy_cq(this->umr_cq);

clean_mr:
  ibv_dealloc_pd(this->pd);

clean_comp_channel:
  ibv_close_device(this->context);

  throw "Failed to create QP";
}
