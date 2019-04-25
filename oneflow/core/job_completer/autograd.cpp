#include "oneflow/core/job_completer/autograd.h"
#include "oneflow/core/job/job_builder.h"
#include "oneflow/core/job_completer/clone_grad.h"
#include "oneflow/core/operator/variable_op.h"

namespace oneflow {

namespace {

const TrainConf& GetTrainConf() {
  const JobDesc* job_desc = Global<JobDesc>::Get();
  if (job_desc->IsTrain()) {
    return job_desc->other_conf().train_conf();
  } else if (job_desc->IsPredict()
             && job_desc->other_conf().predict_conf().has_tmp_split_fw_bw_train_conf()) {
    return job_desc->other_conf().predict_conf().tmp_split_fw_bw_train_conf();
  } else {
    UNIMPLEMENTED();
  }
}

bool AnyLbiWithDiffLbi(const OpEdge* op_edge) {
  const Operator& src_op = op_edge->src_node()->op();
  const Operator& dst_op = op_edge->dst_node()->op();
  auto IsOutputBlobModifierRequiresGrad = [&](const LogicalBlobId& lbi) {
    return src_op.OutputBlobModifier4Obn(op_edge->lbi2obn().at(lbi)).requires_grad();
  };
  auto IsInputBlobModifierRequiresGrad = [&](const LogicalBlobId& lbi) {
    const auto& ibns = op_edge->lbi2ibns().at(lbi);
    for (const std::string& ibn : ibns) {
      if (dst_op.InputBlobModifier4Ibn(ibn).requires_grad()) { return true; }
    }
    CHECK_GT(ibns.size(), 0);
    return false;
  };
  for (const LogicalBlobId& lbi : op_edge->lbis()) {
    if (IsOutputBlobModifierRequiresGrad(lbi) && IsInputBlobModifierRequiresGrad(lbi)) {
      return true;
    }
  }
  CHECK_GT(op_edge->lbis().size(), 0);
  return false;
}

void CheckNotReachableAmongOpNodes(const OpGraph& op_graph, const std::list<OpNode*>& op_nodes) {
  auto IsReachable = op_graph.MakePredicatorIsReachable();
  for (OpNode* src_node : op_nodes) {
    for (OpNode* dst_node : op_nodes) {
      if (src_node == dst_node) { continue; }
      CHECK(!IsReachable(src_node, dst_node));
    }
  }
}

void GetLossOpNodes(const OpGraph& op_graph, std::list<OpNode*>* loss_op_nodes) {
  const auto& train_conf = GetTrainConf();
  HashSet<std::string> loss_op_names;
  for (const std::string& loss_lbn : train_conf.loss_lbn()) {
    loss_op_names.emplace(GenLogicalBlobId(loss_lbn).op_name());
  }
  op_graph.ForEachNode([&](OpNode* op_node) {
    if (loss_op_names.find(op_node->op().op_name()) != loss_op_names.end()) {
      loss_op_nodes->push_back(op_node);
    }
  });
  CHECK_GT(loss_op_nodes->size(), 0);
}

void GetLossOpNodesAndAscendants(const OpGraph& op_graph, HashSet<OpNode*>* op_nodes) {
  std::list<OpNode*> starts;
  GetLossOpNodes(op_graph, &starts);
  auto ForEachNextNode = [&](OpNode* op_node, const std::function<void(OpNode*)>& Handler) {
    for (OpEdge* edge : op_node->in_edges()) {
      if (AnyLbiWithDiffLbi(edge)) { Handler(edge->src_node()); }
    }
  };
  op_graph.BfsForEachNode(starts, ForEachNextNode,
                          [&](OpNode* op_node) { op_nodes->emplace(op_node); });
}

std::function<bool(OpNode*)> MakePredicatorNeedBackwardOp(const OpGraph& op_graph) {
  auto var_op_nodes_and_descendants = std::make_shared<HashSet<OpNode*>>();
  GetVariableOpNodesAndDescendants(op_graph, var_op_nodes_and_descendants.get());
  auto loss_op_nodes_and_ascendants = std::make_shared<HashSet<OpNode*>>();
  GetLossOpNodesAndAscendants(op_graph, loss_op_nodes_and_ascendants.get());
  return [var_op_nodes_and_descendants, loss_op_nodes_and_ascendants](OpNode* op_node) {
    if (var_op_nodes_and_descendants->find(op_node) == var_op_nodes_and_descendants->end()) {
      return false;
    }
    if (loss_op_nodes_and_ascendants->find(op_node) == loss_op_nodes_and_ascendants->end()) {
      return false;
    }
    for (const auto& ibn : op_node->op().input_bns()) {
      if (op_node->op().InputBlobModifier4Ibn(ibn).requires_grad()) { return true; }
    }
    for (const auto& obn : op_node->op().output_bns()) {
      if (op_node->op().OutputBlobModifier4Obn(obn).requires_grad()) { return true; }
    }
    return false;
  };
}

std::function<bool(const LogicalBlobId&, const std::string&)> MakePredicatorHasDiff4LbiOpName(
    const OpGraph& op_graph, const std::function<bool(OpNode*)>& NeedBackwardOp) {
  auto lbis2ops_with_in_diff = std::make_shared<HashMap<LogicalBlobId, HashSet<std::string>>>();
  op_graph.ForEachEdge([&](OpEdge* edge) {
    if (NeedBackwardOp(edge->src_node()) && NeedBackwardOp(edge->dst_node())) {
      for (const auto& lbi : edge->lbis()) {
        if (edge->src_node()->op().HasOutDiff4Lbi(lbi)) {
          (*lbis2ops_with_in_diff)[lbi].emplace(edge->dst_node()->op().op_name());
        }
      }
    }
  });
  return [lbis2ops_with_in_diff](const LogicalBlobId& lbi, const std::string& op_name) {
    if (lbis2ops_with_in_diff->find(lbi) == lbis2ops_with_in_diff->end()) { return false; }
    const auto& op_names = lbis2ops_with_in_diff->at(lbi);
    return op_names.find(op_name) != op_names.end();
  };
}

void GenerateOnesAsDiffLbi(const LogicalBlobId& lbi, std::vector<OperatorConf>* op_confs,
                           LogicalBlobId* out_diff_lbi) {
  OperatorConf mul_zero_op;
  mul_zero_op.set_name(lbi.op_name() + "_" + lbi.blob_name() + "_grad_stage0");
  ScalarMulOpConf* mul_zero_op_conf = mul_zero_op.mutable_scalar_mul_conf();
  mul_zero_op_conf->set_in(GenLogicalBlobName(lbi));
  mul_zero_op_conf->set_out("out");
  mul_zero_op_conf->set_int_operand(0);
  op_confs->push_back(mul_zero_op);

  OperatorConf add_one_op;
  add_one_op.set_name(lbi.op_name() + "_" + lbi.blob_name() + "_grad_stage1");
  ScalarAddOpConf* add_one_op_conf = add_one_op.mutable_scalar_add_conf();
  add_one_op_conf->set_in(mul_zero_op.name() + "/out");
  add_one_op_conf->set_out("out");
  add_one_op_conf->set_int_operand(1);
  op_confs->push_back(add_one_op);

  out_diff_lbi->set_op_name(add_one_op.name());
  out_diff_lbi->set_blob_name("out");
}

void BuildTotalLossInstanceNumIdOpConf(
    const OpGraph& op_graph, JobBuilder* job_builder,
    const HashMap<LogicalBlobId, LogicalBlobId>& lbi2diff_lbi,
    const LogicalBlobId& total_loss_instance_num_lbi,
    std::function<const LogicalBlobId&(const ParallelDesc&)>* LossInstanceNum4ParallelDesc) {
  HashMap<ParallelDesc, int32_t> parallel_desc2optimizer_node_cnt;
  op_graph.ForEachNode([&](OpNode* op_node) {
    const VariableOp* var_op = dynamic_cast<const VariableOp*>(&op_node->op());
    if (var_op == nullptr) { return; }
    if (lbi2diff_lbi.find(var_op->BnInOp2Lbi(var_op->SoleObn())) == lbi2diff_lbi.end()) { return; }
    ++parallel_desc2optimizer_node_cnt[op_node->parallel_desc()];
  });
  auto parallel_desc2total_loss_instance_num_lbi =
      std::make_shared<HashMap<ParallelDesc, LogicalBlobId>>();
  for (const auto& pair : parallel_desc2optimizer_node_cnt) {
    if (pair.second == 1) {
      parallel_desc2total_loss_instance_num_lbi->emplace(pair.first, total_loss_instance_num_lbi);
    } else if (pair.second > 1) {
      OperatorConf id_op_conf;
      id_op_conf.set_name(std::string("System-TotalLossInstanceNum-Identity_") + NewUniqueId());
      auto* id_conf = id_op_conf.mutable_tuple_identity_conf();
      id_conf->add_in(GenLogicalBlobName(total_loss_instance_num_lbi));
      id_conf->add_out("out");
      (*id_op_conf.mutable_sbp_signature_hint()->mutable_bn_in_op2sbp_parallel())["out_0"]
          .mutable_broadcast_parallel();
      job_builder->AddOps(pair.first.parallel_conf(), {id_op_conf});
      parallel_desc2total_loss_instance_num_lbi->emplace(
          pair.first, GenLogicalBlobId(id_op_conf.name() + "/out"));
    } else {
      UNIMPLEMENTED();
    }
  }
  *LossInstanceNum4ParallelDesc = [parallel_desc2total_loss_instance_num_lbi](
                                      const ParallelDesc& parallel_desc) -> const LogicalBlobId& {
    return parallel_desc2total_loss_instance_num_lbi->at(parallel_desc);
  };
}

}  // namespace

void GetVariableOpNodesAndDescendants(const OpGraph& op_graph, HashSet<OpNode*>* op_nodes) {
  std::list<OpNode*> starts;
  op_graph.ForEachNode([&](OpNode* op_node) {
    const auto& op_conf = op_node->op().op_conf();
    if (op_conf.has_variable_conf()) { starts.push_back(op_node); }
  });
  auto ForEachNextNode = [&](OpNode* op_node, const std::function<void(OpNode*)>& Handler) {
    for (OpEdge* edge : op_node->out_edges()) {
      if (AnyLbiWithDiffLbi(edge)) { Handler(edge->dst_node()); }
    }
  };
  op_graph.BfsForEachNode(starts, ForEachNextNode,
                          [&](OpNode* op_node) { op_nodes->emplace(op_node); });
}

void GenerateBackwardOpConfWrapperStruct::Call(
    const Operator& op, std::vector<OperatorConf>* op_confs,
    const std::function<LogicalBlobId*(const std::string&)>& DiffLbi4BnInOp,
    const std::function<const BlobDesc&(const std::string&)>& LogicalBlobDesc4BnInOp) const {
  if (func_) {
    (*func_)(op, op_confs, DiffLbi4BnInOp, LogicalBlobDesc4BnInOp);
  } else if (naive_func_) {
    (*naive_func_)(op, op_confs, DiffLbi4BnInOp);
  } else {
    UNIMPLEMENTED();
  }
}

void GenerateBackwardOpConfIf(
    const Operator& op, std::vector<OperatorConf>* op_confs,
    const std::function<LogicalBlobId*(const std::string&)>& DiffLbi4BnInOp,
    const std::function<const BlobDesc&(const std::string&)>& LogicalBlobDesc4BnInOp) {
  auto* obj = NewObj<GenerateBackwardOpConfWrapperStruct>(op.op_conf().op_type_case());
  obj->Call(op, op_confs, DiffLbi4BnInOp, LogicalBlobDesc4BnInOp);
}

void AutoGrad(const OpGraph& op_graph, Job* job,
              HashMap<std::string, HashMap<std::string, LogicalBlobId>>* op_name2ibn2in_diff_lbi,
              HashMap<LogicalBlobId, LogicalBlobId>* lbi2diff_lbi) {
  CHECK(lbi2diff_lbi->empty());
  auto NeedBackwardOp = MakePredicatorNeedBackwardOp(op_graph);
  std::list<OpNode*> loss_nodes;
  GetLossOpNodes(op_graph, &loss_nodes);
  CheckNotReachableAmongOpNodes(op_graph, loss_nodes);
  for (OpNode* loss_node : loss_nodes) { CHECK(NeedBackwardOp(loss_node)); }
  JobBuilder job_builder(job);

  // generate ones lbi as loss's diff
  HashMap<LogicalBlobId, LogicalBlobId>* lbi2out_diff_lbi = lbi2diff_lbi;
  for (const std::string& loss_lbn : GetTrainConf().loss_lbn()) {
    const LogicalBlobId loss_lbi = GenLogicalBlobId(loss_lbn);
    const auto loss_node_it = std::find_if(
        loss_nodes.cbegin(), loss_nodes.cend(),
        [&](const OpNode* node) { return node->op().op_name() == loss_lbi.op_name(); });
    CHECK(loss_node_it != loss_nodes.cend());
    const OpNode* loss_op_node = *loss_node_it;
    LogicalBlobId* out_diff_lbi = &(*lbi2out_diff_lbi)[loss_lbi];
    std::vector<OperatorConf> ops;
    GenerateOnesAsDiffLbi(loss_lbi, &ops, out_diff_lbi);
    job_builder.AddOps(loss_op_node->parallel_desc().parallel_conf(), ops);
  }

  // generate backward ops
  auto ForEachInNode = [&](OpNode* op_node, const std::function<void(OpNode*)>& Handler) {
    op_node->ForEachNodeOnInEdge([&](OpNode* in_node) {
      if (NeedBackwardOp(in_node)) { Handler(in_node); }
    });
  };
  auto ForEachOutNode = [&](OpNode* op_node, const std::function<void(OpNode*)>& Handler) {
    op_node->ForEachNodeOnOutEdge([&](OpNode* out_node) {
      if (NeedBackwardOp(out_node)) { Handler(out_node); }
    });
  };
  auto HasDiff4LbiOpName = MakePredicatorHasDiff4LbiOpName(op_graph, NeedBackwardOp);
  op_graph.TopoForEachNode(loss_nodes, ForEachOutNode, ForEachInNode, [&](OpNode* op_node) {
    const auto& op_name = op_node->op().op_name();
    auto DiffLbi4BnInOp = [&](const std::string& bn) -> LogicalBlobId* {
      const auto& input_bns = op_node->op().input_bns();
      const auto& output_bns = op_node->op().output_bns();
      const auto& lbi = op_node->op().BnInOp2Lbi(bn);
      if (std::find(input_bns.begin(), input_bns.end(), bn) != input_bns.end()) {
        if (HasDiff4LbiOpName(lbi, op_name) == false) { return nullptr; }
        if (op_node->op().InputBlobModifier4Ibn(bn).requires_grad() == false) { return nullptr; }
        return &(*op_name2ibn2in_diff_lbi)[op_name][bn];
      } else if (std::find(output_bns.begin(), output_bns.end(), bn) != output_bns.end()) {
        if (op_node->op().OutputBlobModifier4Obn(bn).requires_grad() == false) { return nullptr; }
        if (lbi2out_diff_lbi->find(lbi) == lbi2out_diff_lbi->end()) { return nullptr; }
        return &lbi2out_diff_lbi->at(lbi);
      } else {
        UNIMPLEMENTED();
      }
    };
    auto LogicalBlobDesc4BnInOp = [&](const std::string& bn) -> const BlobDesc& {
      return op_graph.GetLogicalBlobDesc(op_node->op().BnInOp2Lbi(bn));
    };
    std::vector<OperatorConf> ops;
    GenerateCloneGradOpIfNeed(*op_node, &ops, *op_name2ibn2in_diff_lbi, lbi2out_diff_lbi);
    GenerateBackwardOpConfIf(op_node->op(), &ops, DiffLbi4BnInOp, LogicalBlobDesc4BnInOp);
    job_builder.AddOps(op_node->parallel_desc().parallel_conf(), ops);
  });
}

void AddTotalLossInstanceNumOpConf(
    const OpGraph& op_graph, Job* job, const HashMap<LogicalBlobId, LogicalBlobId>& lbi2diff_lbi,
    std::function<const LogicalBlobId&(const ParallelDesc&)>* LossInstanceNum4ParallelDesc) {
  JobBuilder job_builder(job);
  std::list<OpNode*> loss_nodes;
  GetLossOpNodes(op_graph, &loss_nodes);
  auto BuildInstanceNumOpConf4LossOpNode = [&](const std::string& loss_lbn, LogicalBlobId* lbi) {
    const LogicalBlobId loss_lbi = GenLogicalBlobId(loss_lbn);
    const auto loss_node_it = std::find_if(
        loss_nodes.cbegin(), loss_nodes.cend(),
        [&](const OpNode* node) { return node->op().op_name() == loss_lbi.op_name(); });
    CHECK(loss_node_it != loss_nodes.cend());
    const OpNode* op_node = *loss_node_it;
    OperatorConf instance_num_op;
    instance_num_op.set_name("System-Autograd-" + loss_lbi.op_name() + "-" + loss_lbi.blob_name()
                             + "-LossInstanceNum");
    auto* instance_num_op_conf = instance_num_op.mutable_shape_elem_cnt_conf();
    instance_num_op_conf->set_x(GenLogicalBlobName(loss_lbi));
    instance_num_op_conf->set_y("y");
    instance_num_op_conf->set_data_type(op_node->LogicalBlobDesc4Lbi(loss_lbi).data_type());
    instance_num_op_conf->mutable_include_axis_conf()->add_axis(0);
    job_builder.AddOps(op_node->parallel_desc().parallel_conf(), {instance_num_op});
    lbi->set_op_name(instance_num_op.name());
    lbi->set_blob_name("y");
  };
  const auto& train_conf = GetTrainConf();
  LogicalBlobId total_loss_instance_num_lbi;
  if (train_conf.loss_lbn().size() == 1) {
    BuildInstanceNumOpConf4LossOpNode(train_conf.loss_lbn().Get(0), &total_loss_instance_num_lbi);
  } else if (train_conf.loss_lbn().size() > 1) {
    OperatorConf op_conf;
    op_conf.set_name("System-Autograd-total_loss_instance_num");
    TotalLossInstanceNumOpConf* total_loss_instance_num_conf =
        op_conf.mutable_total_loss_instance_num_conf();
    for (const std::string& loss_lbn : train_conf.loss_lbn()) {
      LogicalBlobId loss_instance_num_lbi;
      BuildInstanceNumOpConf4LossOpNode(loss_lbn, &loss_instance_num_lbi);
      total_loss_instance_num_conf->add_in(GenLogicalBlobName(loss_instance_num_lbi));
    }
    total_loss_instance_num_conf->set_out("out");

    ParallelConf parallel_conf;
    parallel_conf.set_policy(kDataParallel);
    parallel_conf.add_device_name("0:cpu:0");
    job_builder.AddOps(parallel_conf, {op_conf});

    total_loss_instance_num_lbi.set_op_name(op_conf.name());
    total_loss_instance_num_lbi.set_blob_name("out");
  } else {
    UNIMPLEMENTED();
  }
  BuildTotalLossInstanceNumIdOpConf(op_graph, &job_builder, lbi2diff_lbi,
                                    total_loss_instance_num_lbi, LossInstanceNum4ParallelDesc);
}

}  // namespace oneflow
