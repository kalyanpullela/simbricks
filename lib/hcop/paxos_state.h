/*
 * Paxos State Machine — tier-agnostic Multi-Paxos implementation.
 *
 * This is the shared library that implements the Paxos consensus protocol.
 * It is used by the DPU handler, the host-side program, and (in simplified
 * form) the behavioral switch model. Decision #3: shared library for
 * protocol logic.
 *
 * Protocol: Classic Multi-Paxos with stable leader (Decision #5).
 * - 3 replicas default, configurable to 5.
 * - Steady state: Accept/Accepted (2-message exchange).
 * - Exception: Leader election (Prepare/Promise).
 *
 * This module is pure state machine logic — no networking, no SimBricks
 * dependencies. Input: a message. Output: zero or more response messages
 * plus a status code.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_PAXOS_STATE_H_
#define SIMBRICKS_HCOP_PAXOS_STATE_H_

#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include "paxos_proto.h"

namespace paxos {

// ---- Status returned by state machine operations ----
enum class PaxosStatus : uint8_t {
  kOk = 0,                // Processed normally
  kInstanceOverflow,       // Instance log full (STATE_OVERFLOW exception)
  kAlreadyCommitted,       // Instance already committed; no-op
  kValueTooLarge,          // Value exceeds kMaxValueSize
  kInvalidMessage,         // Message too short or unknown type
};

// ---- Per-instance acceptor state ----
struct InstanceState {
  ProposalNum highest_promised = {0, 0, 0};
  ProposalNum accepted_proposal = {0, 0, 0};
  uint16_t accepted_value_len = 0;
  uint8_t accepted_value[kMaxValueSize] = {};
  bool is_committed = false;
  uint16_t committed_value_len = 0;
  uint8_t committed_value[kMaxValueSize] = {};
};

// ---- Per-instance proposer state (leader tracking) ----
struct ProposerInstanceState {
  ProposalNum proposal = {0, 0, 0};  // proposal being driven
  uint16_t value_len = 0;
  uint8_t value[kMaxValueSize] = {};

  // Phase 1 tracking
  uint16_t promise_count = 0;
  ProposalNum highest_accepted_in_promises = {0, 0, 0};
  uint16_t highest_accepted_value_len = 0;
  uint8_t highest_accepted_value[kMaxValueSize] = {};

  // Phase 2 tracking
  uint16_t accepted_count = 0;
  bool committed = false;
};

// ---- Outbound message ----
// Produced by the state machine; the caller is responsible for routing.
struct OutMessage {
  uint8_t dest_id;  // destination node (255 = broadcast to all replicas)
  std::vector<uint8_t> data;  // serialized message
};

static constexpr uint8_t kBroadcast = 255;

// ---- Exception callback ----
// Called when the state machine detects a condition that should trigger
// an exception in the HCOP placement model.
using ExceptionCallback = std::function<void(uint16_t exception_type,
                                             uint32_t instance_id)>;

// ====================================================================
// PaxosNode — a single Paxos replica
// ====================================================================
class PaxosNode {
 public:
  /**
   * @param node_id        This node's ID (0-based).
   * @param num_replicas   Total number of replicas (3 or 5).
   * @param max_instances  Maximum number of instances to track. When
   *                       instance_log.size() >= max_instances and a new
   *                       instance is needed, returns kInstanceOverflow.
   *                       The switch sets this based on Tofino-2 SRAM budget;
   *                       the DPU sets it very large (bounded by DRAM).
   */
  PaxosNode(uint8_t node_id, uint16_t num_replicas,
            uint32_t max_instances = 10000);

  /**
   * Process an incoming message, producing zero or more outbound messages.
   *
   * @param data  Raw message bytes (starts with PaxosMsgHeader).
   * @param len   Length of message.
   * @param out   Output vector for response messages.
   * @return PaxosStatus indicating the result.
   */
  PaxosStatus HandleMessage(const void *data, size_t len,
                            std::vector<OutMessage> &out);

  // ---- Proposer API ----

  /**
   * Propose a value for a new instance. This node must be the leader.
   * Starts Phase 2 directly (stable leader assumption — skip Phase 1).
   *
   * @param instance_id  The log slot to propose for.
   * @param value        Pointer to value data.
   * @param value_len    Length of value data.
   * @param out          Output vector for Accept messages.
   * @return PaxosStatus::kOk on success,
   *         PaxosStatus::kInstanceOverflow if log full,
   *         PaxosStatus::kAlreadyCommitted if instance already decided.
   */
  PaxosStatus Propose(uint32_t instance_id, const void *value,
                      uint16_t value_len, std::vector<OutMessage> &out);

  /**
   * Initiate leader election (Phase 1) for a specific instance.
   * Used when the stable leader assumption fails (exception path).
   *
   * @param instance_id  The log slot.
   * @param out          Output vector for Prepare messages.
   * @return PaxosStatus::kOk or PaxosStatus::kInstanceOverflow.
   */
  PaxosStatus StartLeaderElection(uint32_t instance_id,
                                  std::vector<OutMessage> &out);

  // ---- Accessors ----
  uint8_t NodeId() const { return node_id_; }
  uint16_t NumReplicas() const { return num_replicas_; }
  uint16_t Quorum() const { return quorum_; }
  uint8_t LeaderId() const { return leader_id_; }
  bool IsLeader() const { return node_id_ == leader_id_; }

  /** Get the state of a specific instance (or nullptr if not tracked). */
  const InstanceState *GetInstance(uint32_t instance_id) const;

  /** Number of instances tracked. */
  size_t InstanceCount() const { return instances_.size(); }

  /** Maximum number of instances to track before STATE_OVERFLOW. */
  uint32_t MaxInstances() const { return max_instances_; }

  /** Set exception callback (for telemetry). */
  void SetExceptionCallback(ExceptionCallback cb) {
    exception_cb_ = std::move(cb);
  }

  /** Set leader ID (for external leader election or test setup). */
  void SetLeader(uint8_t leader_id) { leader_id_ = leader_id; }

  /** Get next proposal round (auto-increments). */
  uint32_t NextRound() { return ++current_round_; }

 private:
  uint8_t node_id_;
  uint16_t num_replicas_;
  uint16_t quorum_;          // majority: (n/2) + 1
  uint8_t leader_id_ = 0;   // initially node 0 is leader
  uint32_t current_round_ = 0;

  // Maximum tracked instances (STATE_OVERFLOW if exceeded)
  uint32_t max_instances_;

  // Per-instance acceptor state
  std::unordered_map<uint32_t, InstanceState> instances_;

  // Per-instance proposer state (only when this node is proposing)
  std::unordered_map<uint32_t, ProposerInstanceState> proposer_state_;

  ExceptionCallback exception_cb_;

  // ---- Internal handlers ----
  PaxosStatus HandlePrepare(const PrepareMsg &msg, std::vector<OutMessage> &out);
  PaxosStatus HandlePromise(const PromiseMsg &msg, std::vector<OutMessage> &out);
  PaxosStatus HandleAccept(const AcceptMsg &msg, std::vector<OutMessage> &out);
  PaxosStatus HandleAccepted(const AcceptedMsg &msg, std::vector<OutMessage> &out);
  PaxosStatus HandleLearn(const LearnMsg &msg, std::vector<OutMessage> &out);
  PaxosStatus HandleNack(const NackMsg &msg, std::vector<OutMessage> &out);

  // Get or create instance state. Returns nullptr on overflow.
  InstanceState *GetOrCreateInstance(uint32_t instance_id);

  // Build outbound messages
  OutMessage MakePromise(uint8_t dest, uint32_t instance_id,
                         const InstanceState &inst);
  OutMessage MakeAccepted(uint8_t dest, uint32_t instance_id,
                          const ProposalNum &proposal,
                          const void *value, uint16_t value_len);
  OutMessage MakeLearn(uint8_t dest, uint32_t instance_id,
                       const void *value, uint16_t value_len);
  OutMessage MakeNack(uint8_t dest, uint32_t instance_id,
                      const ProposalNum &highest);
  OutMessage MakeAcceptBroadcast(uint32_t instance_id,
                                 const ProposalNum &proposal,
                                 const void *value, uint16_t value_len);
  OutMessage MakePrepareBroadcast(uint32_t instance_id,
                                  const ProposalNum &proposal);
};

}  // namespace paxos

#endif  // SIMBRICKS_HCOP_PAXOS_STATE_H_
