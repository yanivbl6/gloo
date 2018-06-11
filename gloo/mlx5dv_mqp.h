#pragma once

#include "mlx5dv_ctx.h"
#include "mlx5dv_mgr.h"

#include <queue>
#include <functional>



enum cd_statuses { PCOLL_SUCCESS = 0, PCOLL_ERROR = 1 };



typedef std::function<void()> LambdaInstruction;
typedef std::queue<LambdaInstruction> InsQueue;


class PcxQp;
class ManagementQp;

class CommGraph{
public:
  CommGraph(verb_ctx_t *vctx);
  ~CommGraph();

  void enqueue(LambdaInstruction& ins){
    iq.push(std::move(ins));
  }

  void wait(PcxQp* slave_qp);
  void wait_send(PcxQp* slave_qp);

  verb_ctx_t* ctx;
  ManagementQp* mqp;
private:
  InsQueue iq;
};


class PcxQp{
public:
  PcxQp(CommGraph *cgraph): wqe_count(0), cqe_count(0), scqe_count(0), finished(false), graph(cgraph), ctx(cgraph->ctx){};
  virtual ~PcxQp() = 0;
  virtual void fin() = 0;


  void sendCredit();
  void write(NetMem *local, NetMem *remote);
  void writeCmpl(NetMem *local, NetMem *remote);
  void reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type);

  void cd_send_enable(PcxQp* slave_qp);
  void cd_recv_enable(PcxQp* slave_qp);
  void cd_wait(PcxQp* slave_qp);
  void cd_wait_send(PcxQp* slave_qp);

  int wqe_count;
  int cqe_count;
  int scqe_count;
protected:
  CommGraph* graph;
  struct ibv_qp* ibqp;
  struct ibv_cq* ibcq;
  struct ibv_cq* ibscq;
  qp_ctx* qp;
  PcxQp* pair;
  bool finished;
  bool has_scq;
  verb_ctx_t* ctx;
};

class ManagementQp: public PcxQp{
public:
  ManagementQp(CommGraph *cgraph);
  ~ManagementQp();
  void fin();

private:
    
};

int hmca_bcol_cc_mq_destroy(struct ibv_qp *mq);
struct ibv_qp *create_management_qp(struct ibv_cq *cq, struct ibv_pd *pd,
                                      struct ibv_context *ctx,
                                      uint16_t send_wq_size);

struct ibv_qp *rc_qp_create(struct ibv_cq *cq, struct ibv_pd *pd,
                            struct ibv_context *ctx, uint16_t send_wq_size,
                            uint16_t recv_rq_size, int slaveRecv, int slaveSend,
                            struct ibv_cq *s_cq = NULL);


struct ibv_cq *cd_create_cq(struct ibv_context *context, int cqe,
                            void *cq_context = NULL, struct ibv_comp_channel *channel = NULL,
                            int comp_vector = 0);
