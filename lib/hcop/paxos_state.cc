/*
 * Paxos State Machine — implementation.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#include "paxos_state.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "hcop_proto.h"

namespace paxos {

PaxosNode::PaxosNode(uint8_t node_id, uint16_t num_replicas,
                     uint32_t max_instances)
    : node_id_(node_id),
      num_replicas_(num_replicas),
      quorum_(num_replicas / 2 + 1),
      max_instances_(max_instances) {
  assert(num_replicas >= 3 && num_replicas <= 5);
  assert(node_id < num_replicas);
}

// ====================================================================
// Message Dispatch
// ====================================================================

PaxosStatus PaxosNode::HandleMessage(const void *data, size_t len,
                                     std::vector<OutMessage> &out) {
  if (len < sizeof(PaxosMsgHeader)) return PaxosStatus::kInvalidMessage;

  const auto *hdr = static_cast<const PaxosMsgHeader *>(data);

  switch (hdr->msg_type) {
    case kPrepare:
      if (len >= sizeof(PrepareMsg))
        return HandlePrepare(*static_cast<const PrepareMsg *>(data), out);
      return PaxosStatus::kInvalidMessage;
    case kPromise:
      if (len >= sizeof(PromiseMsg) - kMaxValueSize)
        return HandlePromise(*static_cast<const PromiseMsg *>(data), out);
      return PaxosStatus::kInvalidMessage;
    case kAccept:
      if (len >= sizeof(AcceptMsg) - kMaxValueSize)
        return HandleAccept(*static_cast<const AcceptMsg *>(data), out);
      return PaxosStatus::kInvalidMessage;
    case kAccepted:
      if (len >= sizeof(AcceptedMsg) - kMaxValueSize)
        return HandleAccepted(*static_cast<const AcceptedMsg *>(data), out);
      return PaxosStatus::kInvalidMessage;
    case kLearn:
      if (len >= sizeof(LearnMsg) - kMaxValueSize)
        return HandleLearn(*static_cast<const LearnMsg *>(data), out);
      return PaxosStatus::kInvalidMessage;
    case kNack:
      if (len >= sizeof(NackMsg))
        return HandleNack(*static_cast<const NackMsg *>(data), out);
      return PaxosStatus::kInvalidMessage;
    default:
      return PaxosStatus::kInvalidMessage;
  }
}

// ====================================================================
// Proposer API
// ====================================================================

PaxosStatus PaxosNode::Propose(uint32_t instance_id, const void *value,
                               uint16_t value_len,
                               std::vector<OutMessage> &out) {
  if (value_len > kMaxValueSize) return PaxosStatus::kValueTooLarge;

  // Check if already committed.
  auto it = instances_.find(instance_id);
  if (it != instances_.end() && it->second.is_committed) {
    return PaxosStatus::kAlreadyCommitted;
  }

  // Ensure instance is tracked (checks overflow).
  InstanceState *inst = GetOrCreateInstance(instance_id);
  if (!inst) return PaxosStatus::kInstanceOverflow;

  // Stable leader: skip Phase 1, go directly to Phase 2a (Accept).
  ProposalNum proposal = {NextRound(), node_id_, 0};

  // Track proposer state.
  auto &ps = proposer_state_[instance_id];
  ps.proposal = proposal;
  ps.value_len = value_len;
  std::memcpy(ps.value, value, value_len);
  ps.accepted_count = 0;
  ps.committed = false;

  // Broadcast Accept to all replicas (including self).
  out.push_back(MakeAcceptBroadcast(instance_id, proposal, value, value_len));
  return PaxosStatus::kOk;
}

PaxosStatus PaxosNode::StartLeaderElection(uint32_t instance_id,
                                           std::vector<OutMessage> &out) {
  // Ensure instance is tracked (checks overflow).
  InstanceState *inst = GetOrCreateInstance(instance_id);
  if (!inst) return PaxosStatus::kInstanceOverflow;

  // Signal exception: leader election is the non-steady-state path.
  if (exception_cb_) {
    exception_cb_(hcop::kPaxosLeaderElection, instance_id);
  }

  ProposalNum proposal = {NextRound(), node_id_, 0};

  // Initialize proposer state for Phase 1.
  auto &ps = proposer_state_[instance_id];
  ps.proposal = proposal;
  ps.promise_count = 0;
  ps.highest_accepted_in_promises = {0, 0, 0};
  ps.highest_accepted_value_len = 0;
  ps.accepted_count = 0;
  ps.committed = false;

  // Broadcast Prepare to all replicas.
  out.push_back(MakePrepareBroadcast(instance_id, proposal));
  return PaxosStatus::kOk;
}

// ====================================================================
// Phase 1a: PREPARE handler (Acceptor role)
// ====================================================================

PaxosStatus PaxosNode::HandlePrepare(const PrepareMsg &msg,
                                     std::vector<OutMessage> &out) {
  InstanceState *inst = GetOrCreateInstance(msg.hdr.instance_id);
  if (!inst) return PaxosStatus::kInstanceOverflow;

  if (msg.proposal > inst->highest_promised) {
    // Promise: we won't accept proposals lower than this.
    inst->highest_promised = msg.proposal;
    out.push_back(MakePromise(msg.hdr.sender_id, msg.hdr.instance_id, *inst));
  } else {
    // Nack: we've already promised higher.
    out.push_back(MakeNack(msg.hdr.sender_id, msg.hdr.instance_id,
                           inst->highest_promised));
  }
  return PaxosStatus::kOk;
}

// ====================================================================
// Phase 1b: PROMISE handler (Proposer role)
// ====================================================================

PaxosStatus PaxosNode::HandlePromise(const PromiseMsg &msg,
                                     std::vector<OutMessage> &out) {
  auto it = proposer_state_.find(msg.hdr.instance_id);
  if (it == proposer_state_.end()) return PaxosStatus::kOk;

  auto &ps = it->second;
  if (msg.promised != ps.proposal) return PaxosStatus::kOk;  // stale promise

  ps.promise_count++;

  // Track highest accepted value from promises.
  if (!msg.accepted_proposal.IsNull() &&
      msg.accepted_proposal > ps.highest_accepted_in_promises) {
    ps.highest_accepted_in_promises = msg.accepted_proposal;
    ps.highest_accepted_value_len = msg.accepted_value_len;
    std::memcpy(ps.highest_accepted_value, msg.accepted_value,
                std::min(msg.accepted_value_len,
                         static_cast<uint16_t>(kMaxValueSize)));
  }

  // Quorum reached — proceed to Phase 2a.
  if (ps.promise_count >= quorum_) {
    // If any acceptor had an accepted value, we must use the highest one.
    const void *use_value = ps.value;
    uint16_t use_len = ps.value_len;
    if (!ps.highest_accepted_in_promises.IsNull()) {
      use_value = ps.highest_accepted_value;
      use_len = ps.highest_accepted_value_len;
    }

    ps.accepted_count = 0;
    out.push_back(MakeAcceptBroadcast(msg.hdr.instance_id, ps.proposal,
                                      use_value, use_len));
  }
  return PaxosStatus::kOk;
}

// ====================================================================
// Phase 2a: ACCEPT handler (Acceptor role)
// ====================================================================

PaxosStatus PaxosNode::HandleAccept(const AcceptMsg &msg,
                                    std::vector<OutMessage> &out) {
  InstanceState *inst = GetOrCreateInstance(msg.hdr.instance_id);
  if (!inst) return PaxosStatus::kInstanceOverflow;

  // Already committed — send learn instead.
  if (inst->is_committed) {
    out.push_back(MakeLearn(msg.hdr.sender_id, msg.hdr.instance_id,
                            inst->committed_value, inst->committed_value_len));
    return PaxosStatus::kAlreadyCommitted;
  }

  if (msg.proposal >= inst->highest_promised) {
    // Accept the proposal.
    inst->highest_promised = msg.proposal;
    inst->accepted_proposal = msg.proposal;
    inst->accepted_value_len = std::min(msg.value_len,
                                        static_cast<uint16_t>(kMaxValueSize));
    std::memcpy(inst->accepted_value, msg.value, inst->accepted_value_len);

    // Send Accepted to the proposer.
    out.push_back(MakeAccepted(msg.hdr.sender_id, msg.hdr.instance_id,
                               msg.proposal, msg.value, msg.value_len));
  } else {
    // Nack: we've promised higher.
    out.push_back(MakeNack(msg.hdr.sender_id, msg.hdr.instance_id,
                           inst->highest_promised));
    // This is a conflict — signal exception.
    if (exception_cb_) {
      exception_cb_(hcop::kPaxosConflict, msg.hdr.instance_id);
    }
  }
  return PaxosStatus::kOk;
}

// ====================================================================
// Phase 2b: ACCEPTED handler (Proposer/Learner role)
// ====================================================================

PaxosStatus PaxosNode::HandleAccepted(const AcceptedMsg &msg,
                                      std::vector<OutMessage> &out) {
  auto it = proposer_state_.find(msg.hdr.instance_id);
  if (it == proposer_state_.end()) return PaxosStatus::kOk;

  auto &ps = it->second;
  if (msg.proposal != ps.proposal) return PaxosStatus::kOk;  // stale
  if (ps.committed) return PaxosStatus::kAlreadyCommitted;

  ps.accepted_count++;

  // Quorum reached — value is committed.
  if (ps.accepted_count >= quorum_) {
    ps.committed = true;

    // Record in instance log.
    InstanceState *inst = GetOrCreateInstance(msg.hdr.instance_id);
    if (inst) {
      inst->is_committed = true;
      inst->committed_value_len = msg.value_len;
      std::memcpy(inst->committed_value, msg.value,
                  std::min(msg.value_len,
                           static_cast<uint16_t>(kMaxValueSize)));
    }

    // Broadcast Learn to all replicas.
    for (uint16_t i = 0; i < num_replicas_; ++i) {
      if (i != node_id_) {
        out.push_back(MakeLearn(i, msg.hdr.instance_id,
                                msg.value, msg.value_len));
      }
    }
  }
  return PaxosStatus::kOk;
}

// ====================================================================
// LEARN handler
// ====================================================================

PaxosStatus PaxosNode::HandleLearn(const LearnMsg &msg,
                                   std::vector<OutMessage> &out) {
  (void)out;
  InstanceState *inst = GetOrCreateInstance(msg.hdr.instance_id);
  if (!inst) return PaxosStatus::kInstanceOverflow;

  if (!inst->is_committed) {
    inst->is_committed = true;
    inst->committed_value_len = msg.value_len;
    std::memcpy(inst->committed_value, msg.value,
                std::min(msg.value_len,
                         static_cast<uint16_t>(kMaxValueSize)));
  }
  return PaxosStatus::kOk;
}

// ====================================================================
// NACK handler
// ====================================================================

PaxosStatus PaxosNode::HandleNack(const NackMsg &msg,
                                  std::vector<OutMessage> &out) {
  // A NACK means our proposal was rejected because a higher proposal exists.
  // In stable-leader mode, this indicates a conflict.
  if (exception_cb_) {
    exception_cb_(hcop::kPaxosConflict, msg.hdr.instance_id);
  }

  // Bump our round past the highest seen and retry via leader election.
  if (msg.highest_seen.round >= current_round_) {
    current_round_ = msg.highest_seen.round;
  }

  // Could auto-retry with StartLeaderElection, but let the caller decide.
  (void)out;
  return PaxosStatus::kOk;
}

// ====================================================================
// Accessors
// ====================================================================

const InstanceState *PaxosNode::GetInstance(uint32_t instance_id) const {
  auto it = instances_.find(instance_id);
  if (it == instances_.end()) return nullptr;
  return &it->second;
}

// ====================================================================
// Internal Helpers
// ====================================================================

InstanceState *PaxosNode::GetOrCreateInstance(uint32_t instance_id) {
  // Already exists — return it.
  auto it = instances_.find(instance_id);
  if (it != instances_.end()) return &it->second;

  // Check capacity before creating.
  if (instances_.size() >= max_instances_) {
    if (exception_cb_) {
      exception_cb_(hcop::kPaxosStateOverflow, instance_id);
    }
    return nullptr;  // OVERFLOW
  }

  return &instances_[instance_id];
}

// ====================================================================
// Message Builders
// ====================================================================

OutMessage PaxosNode::MakePromise(uint8_t dest, uint32_t instance_id,
                                  const InstanceState &inst) {
  PromiseMsg msg = {};
  msg.hdr.msg_type = kPromise;
  msg.hdr.sender_id = node_id_;
  msg.hdr.num_replicas = num_replicas_;
  msg.hdr.instance_id = instance_id;
  msg.promised = inst.highest_promised;
  msg.accepted_proposal = inst.accepted_proposal;
  msg.accepted_value_len = inst.accepted_value_len;
  std::memcpy(msg.accepted_value, inst.accepted_value, inst.accepted_value_len);

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage PaxosNode::MakeAccepted(uint8_t dest, uint32_t instance_id,
                                    const ProposalNum &proposal,
                                    const void *value, uint16_t value_len) {
  AcceptedMsg msg = {};
  msg.hdr.msg_type = kAccepted;
  msg.hdr.sender_id = node_id_;
  msg.hdr.num_replicas = num_replicas_;
  msg.hdr.instance_id = instance_id;
  msg.proposal = proposal;
  msg.value_len = std::min(value_len, static_cast<uint16_t>(kMaxValueSize));
  std::memcpy(msg.value, value, msg.value_len);

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage PaxosNode::MakeLearn(uint8_t dest, uint32_t instance_id,
                                const void *value, uint16_t value_len) {
  LearnMsg msg = {};
  msg.hdr.msg_type = kLearn;
  msg.hdr.sender_id = node_id_;
  msg.hdr.num_replicas = num_replicas_;
  msg.hdr.instance_id = instance_id;
  msg.value_len = std::min(value_len, static_cast<uint16_t>(kMaxValueSize));
  std::memcpy(msg.value, value, msg.value_len);

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage PaxosNode::MakeNack(uint8_t dest, uint32_t instance_id,
                                const ProposalNum &highest) {
  NackMsg msg = {};
  msg.hdr.msg_type = kNack;
  msg.hdr.sender_id = node_id_;
  msg.hdr.num_replicas = num_replicas_;
  msg.hdr.instance_id = instance_id;
  msg.highest_seen = highest;

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage PaxosNode::MakeAcceptBroadcast(uint32_t instance_id,
                                           const ProposalNum &proposal,
                                           const void *value,
                                           uint16_t value_len) {
  AcceptMsg msg = {};
  msg.hdr.msg_type = kAccept;
  msg.hdr.sender_id = node_id_;
  msg.hdr.num_replicas = num_replicas_;
  msg.hdr.instance_id = instance_id;
  msg.proposal = proposal;
  msg.value_len = std::min(value_len, static_cast<uint16_t>(kMaxValueSize));
  std::memcpy(msg.value, value, msg.value_len);

  OutMessage out;
  out.dest_id = kBroadcast;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage PaxosNode::MakePrepareBroadcast(uint32_t instance_id,
                                            const ProposalNum &proposal) {
  PrepareMsg msg = {};
  msg.hdr.msg_type = kPrepare;
  msg.hdr.sender_id = node_id_;
  msg.hdr.num_replicas = num_replicas_;
  msg.hdr.instance_id = instance_id;
  msg.proposal = proposal;

  OutMessage out;
  out.dest_id = kBroadcast;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

}  // namespace paxos
