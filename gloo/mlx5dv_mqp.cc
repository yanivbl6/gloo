
#include "mlx5dv_mqp.h"

CommGraph::CommGraph(verb_ctx_t *vctx) : ctx(vctx), mqp(NULL), iq() {
  mqp = new ManagementQp(this);
}

void CommGraph::wait(PcxQp *slave_qp) { mqp->cd_wait(slave_qp); }

void CommGraph::wait_send(PcxQp *slave_qp) { mqp->cd_wait_send(slave_qp); }

void CommGraph::fin() {
  for (GraphQpsIt it = qps.begin(); it != qps.end(); ++it) {
    (*it)->init();
  }
  while (!iq.empty()) {
    LambdaInstruction &instruction = iq.front();
    instruction();
    iq.pop();
  }
  for (GraphQpsIt it = qps.begin(); it != qps.end(); ++it) {
    (*it)->fin();
  }
}

CommGraph::~CommGraph() { delete (mqp); }

void PcxQp::fin() { this->qp->fin(); }

void PcxQp::sendCredit() {
  ++wqe_count;
  ++pair->cqe_count;
  LambdaInstruction lambda = [this]() { qp->sendCredit(); };
  graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void PcxQp::write(NetMem *local, NetMem *remote) {
  ++wqe_count;
  ++this->pair->cqe_count;
  LambdaInstruction lambda = [this, local, remote]() {
    this->qp->write(local, remote);
  };
  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void PcxQp::writeCmpl(NetMem *local, NetMem *remote) {
  ++wqe_count;
  ++this->pair->cqe_count;
  if (has_scq) {
    ++scqe_count;
  } else {
    ++cqe_count;
  }
  LambdaInstruction lambda = [this, local, remote]() {
    this->qp->writeCmpl(local, remote);
  };
  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void PcxQp::reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                         uint8_t op, uint8_t type) {
  wqe_count += 2;
  ++this->pair->cqe_count;
  LambdaInstruction lambda = [this, local, remote, num_vectors, op, type]() {
    this->qp->reduce_write(local, remote, num_vectors, op, type);
  };
  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void PcxQp::cd_send_enable(PcxQp *slave_qp) {
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp]() {
    this->qp->cd_send_enable(slave_qp->qp);
  };
  this->graph->enqueue(lambda);
}

void PcxQp::cd_recv_enable(PcxQp *slave_qp) { // call from PcxQp constructor
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp]() {
    this->qp->cd_recv_enable(slave_qp->qp);
  };
  this->graph->enqueue(lambda);
  ++recv_enables;
}

void PcxQp::cd_wait(PcxQp *slave_qp) {
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp]() {
    this->qp->cd_wait(slave_qp->qp);
  };
  this->graph->enqueue(lambda);
}

void PcxQp::cd_wait_send(PcxQp *slave_qp) {
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp]() {
    this->qp->cd_wait_send(slave_qp->qp);
  };
  this->graph->enqueue(lambda);
}

PcxQp::PcxQp(CommGraph *cgraph)
    : wqe_count(0), cqe_count(0), scqe_count(0), recv_enables(0),
      initiated(false), graph(cgraph), ctx(cgraph->ctx) {
  cgraph->regQp(this);
}

PcxQp::~PcxQp() {}

PCX_ERROR(CreateCQFailed);

ManagementQp::ManagementQp(CommGraph *cgraph) : PcxQp(cgraph) {
  this->pair = this;
  this->has_scq = false;
}

void ManagementQp::init() {
  this->ibcq = cd_create_cq(ctx, cqe_count, NULL);
  if (!this->ibcq) {
    PERR(CreateCQFailed);
  }
  this->ibqp = create_management_qp(this->ibcq, ctx, wqe_count);
  this->qp = new qp_ctx(this->ibqp, this->ibcq, wqe_count, cqe_count);
  initiated = true;
}

ManagementQp::~ManagementQp() {
  delete (qp);
  ibv_destroy_qp(ibqp);
  ibv_destroy_cq(ibcq);
  this->ibscq = NULL;
}

LoopbackQp::LoopbackQp(CommGraph *cgraph) : PcxQp(cgraph) {
  this->pair = this;
  this->has_scq = false;
  cgraph->mqp->cd_recv_enable(this);
}

void LoopbackQp::init() {
  ibcq = cd_create_cq(ctx, cqe_count, NULL);
  if (!ibcq) {
    PERR(CreateCQFailed);
  }

  ibqp = rc_qp_create(ibcq, ctx, wqe_count, cqe_count);
  peer_addr_t loopback_addr;
  rc_qp_get_addr(ibqp, &loopback_addr);
  rc_qp_connect(&loopback_addr, ibqp);
  qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count);
  ibscq = NULL;
  initiated = true;
}

LoopbackQp::~LoopbackQp() {
  delete (qp);
  ibv_destroy_qp(ibqp);
  ibv_destroy_cq(ibcq);
}

DoublingQp::DoublingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm,
                       uint32_t peer, uint32_t tag, NetMem *incomingBuffer)
    : PcxQp(cgraph), incoming(incomingBuffer) {
  this->pair = this;
  this->has_scq = true;
  cgraph->mqp->cd_recv_enable(this);
  using namespace std::placeholders;
  exchange = std::bind(func, comm, _1, _2, _3, peer, tag);
}

void DoublingQp::init() {
  ibcq = cd_create_cq(ctx, cqe_count, NULL);
  if (!ibcq) {
    PERR(CreateCQFailed);
  }

  ibscq = cd_create_cq(ctx, scqe_count, NULL);
  if (!ibscq) {
    PERR(CreateCQFailed);
  }

  ibqp = rc_qp_create(ibcq, ctx, wqe_count, cqe_count, ibscq);

  rd_peer_info_t local_info, remote_info;
  local_info.buf = (uintptr_t)incoming->sg()->addr;
  local_info.rkey = incoming->getMr()->rkey;
  rc_qp_get_addr(ibqp, &local_info.addr);
  exchange((void *)&local_info, (void *)&remote_info, sizeof(local_info));
  this->remote = new RemoteMem(remote_info.buf, remote_info.rkey);

  qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count, ibscq, scqe_count);
  initiated = true;
}

DoublingQp::~DoublingQp() {
  delete (qp);
  ibv_destroy_qp(ibqp);
  ibv_destroy_cq(ibcq);
}

struct ibv_cq *cd_create_cq(verb_ctx_t *verb_ctx, int cqe, void *cq_context,
                            struct ibv_comp_channel *channel, int comp_vector) {
  struct ibv_cq *cq =
      ibv_create_cq(verb_ctx->context, cqe, cq_context, channel, comp_vector);

  struct ibv_exp_cq_attr attr;
  attr.cq_cap_flags = IBV_EXP_CQ_IGNORE_OVERRUN;
  attr.comp_mask = IBV_EXP_CQ_ATTR_CQ_CAP_FLAGS;

  int res = ibv_exp_modify_cq(cq, &attr, IBV_EXP_CQ_CAP_FLAGS);
  if (!res) {
  }

  return cq;
}

struct ibv_qp *create_management_qp(struct ibv_cq *cq, verb_ctx_t *verb_ctx,
                                    uint16_t send_wq_size) {

  int rc = PCOLL_SUCCESS;
  struct ibv_exp_qp_init_attr init_attr;
  struct ibv_qp_attr attr;
  struct ibv_qp *_mq;

  memset(&init_attr, 0, sizeof(init_attr));
  init_attr.qp_context = NULL;
  init_attr.send_cq = cq;
  init_attr.recv_cq = cq;
  init_attr.srq = NULL;
  init_attr.cap.max_send_wr = send_wq_size;
  init_attr.cap.max_recv_wr = 0;
  init_attr.cap.max_send_sge = 1;
  init_attr.cap.max_recv_sge = 1;
  init_attr.cap.max_inline_data = 0;
  init_attr.qp_type = IBV_QPT_RC;
  init_attr.sq_sig_all = 0;
  init_attr.pd = verb_ctx->pd;
  init_attr.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
  init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
                               IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
                               IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;
  ;
  _mq = ibv_exp_create_qp(verb_ctx->context, &init_attr);

  if (NULL == _mq) {
    rc = PCOLL_ERROR;
  }

  if (rc == PCOLL_SUCCESS) {
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = 0;

    rc = ibv_modify_qp(_mq, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                       IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (rc) {
      rc = PCOLL_ERROR;
    }
  }

  if (rc == PCOLL_SUCCESS) {
    union ibv_gid gid;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = _mq->qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = GID_INDEX;
    attr.ah_attr.dlid = 0;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    if (ibv_query_gid(verb_ctx->context, 1, GID_INDEX, &gid)) {
      fprintf(stderr, "can't read sgid of index %d\n", GID_INDEX);
      rc = PCOLL_ERROR;
    }

    attr.ah_attr.grh.dgid = gid;

    rc = ibv_modify_qp(_mq, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                       IBV_QP_MAX_DEST_RD_ATOMIC |
                                       IBV_QP_MIN_RNR_TIMER);

    if (rc) {
      rc = PCOLL_ERROR;
    }
  }

  if (rc == PCOLL_SUCCESS) {
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    rc = ibv_modify_qp(_mq, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
                                       IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                       IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (rc) {
      rc = PCOLL_ERROR;
    }
  }
  return _mq;
}

PCX_ERROR(InitQPFailed);

struct ibv_qp *rc_qp_create(struct ibv_cq *cq, verb_ctx_t *verb_ctx,
                            uint16_t send_wq_size, uint16_t recv_rq_size,
                            struct ibv_cq *s_cq, int slaveRecv, int slaveSend) {
  struct ibv_exp_qp_init_attr init_attr;
  struct ibv_qp_attr attr;
  memset(&init_attr, 0, sizeof(init_attr));
  init_attr.qp_context = NULL;
  init_attr.send_cq = (s_cq == NULL) ? cq : s_cq;
  init_attr.recv_cq = cq;
  init_attr.cap.max_send_wr = send_wq_size;
  init_attr.cap.max_recv_wr = recv_rq_size;
  init_attr.cap.max_send_sge = 1;
  init_attr.cap.max_recv_sge = 1;
  init_attr.qp_type = IBV_QPT_RC;
  init_attr.pd = verb_ctx->pd;
  init_attr.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
  init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
                               IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
                               IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;

  if (slaveSend) {
    init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_SEND;
  }

  if (slaveRecv) {
    init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_RECV;
  }

  struct ibv_qp *qp = ibv_exp_create_qp(verb_ctx->context, &init_attr);

  struct ibv_qp_attr qp_attr;
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_INIT;
  qp_attr.pkey_index = 0;
  qp_attr.port_num = 1;
  qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

  if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
    PERR(InitQPFailed);
  }

  return qp;
}
