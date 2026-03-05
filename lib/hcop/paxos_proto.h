/*
 * Paxos Wire Protocol — message formats for Multi-Paxos coordination.
 *
 * These structures define the payload that rides inside an HCOP packet
 * (after the HcopHeader). They are shared across all tiers (switch, DPU, host).
 *
 * Protocol: Classic Multi-Paxos with stable leader (Decision #5).
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_PAXOS_PROTO_H_
#define SIMBRICKS_HCOP_PAXOS_PROTO_H_

#include <cstdint>

namespace paxos {

// Maximum value size that can be carried in a Paxos message.
// Kept small — these are coordination messages, not bulk data.
static constexpr size_t kMaxValueSize = 128;

// ---- Paxos message types ----
enum MessageType : uint8_t {
  kPrepare  = 1,   // Phase 1a: proposer → acceptors
  kPromise  = 2,   // Phase 1b: acceptor → proposer
  kAccept   = 3,   // Phase 2a: proposer → acceptors
  kAccepted = 4,   // Phase 2b: acceptor → proposer/learners
  kLearn    = 5,   // Learner notification (committed value)
  kNack     = 6,   // Reject (acceptor has higher promise)
};

// ---- Proposal number ----
// Globally unique: (round, node_id). Compared lexicographically.
struct ProposalNum {
  uint32_t round;
  uint16_t node_id;
  uint16_t _pad;    // alignment padding

  bool operator<(const ProposalNum &o) const {
    return round < o.round || (round == o.round && node_id < o.node_id);
  }
  bool operator<=(const ProposalNum &o) const {
    return !(o < *this);
  }
  bool operator>(const ProposalNum &o) const {
    return o < *this;
  }
  bool operator>=(const ProposalNum &o) const {
    return !(*this < o);
  }
  bool operator==(const ProposalNum &o) const {
    return round == o.round && node_id == o.node_id;
  }
  bool operator!=(const ProposalNum &o) const {
    return !(*this == o);
  }
  bool IsNull() const { return round == 0 && node_id == 0; }
} __attribute__((packed));

static_assert(sizeof(ProposalNum) == 8, "ProposalNum must be 8 bytes");

// ---- Wire messages ----
// All messages start with a common header, followed by type-specific fields.

struct PaxosMsgHeader {
  uint8_t  msg_type;      // MessageType enum
  uint8_t  sender_id;     // node ID of sender (0-based)
  uint16_t num_replicas;  // total replica count (for validation)
  uint32_t instance_id;   // Paxos instance (log slot)
} __attribute__((packed));

static_assert(sizeof(PaxosMsgHeader) == 8, "PaxosMsgHeader must be 8 bytes");

// Phase 1a: PREPARE
struct PrepareMsg {
  PaxosMsgHeader hdr;
  ProposalNum    proposal;
} __attribute__((packed));

// Phase 1b: PROMISE
struct PromiseMsg {
  PaxosMsgHeader hdr;
  ProposalNum    promised;           // the proposal being promised
  ProposalNum    accepted_proposal;  // highest accepted proposal (null if none)
  uint16_t       accepted_value_len; // 0 if no accepted value
  uint8_t        accepted_value[kMaxValueSize];
} __attribute__((packed));

// Phase 2a: ACCEPT
struct AcceptMsg {
  PaxosMsgHeader hdr;
  ProposalNum    proposal;
  uint16_t       value_len;
  uint8_t        value[kMaxValueSize];
} __attribute__((packed));

// Phase 2b: ACCEPTED
struct AcceptedMsg {
  PaxosMsgHeader hdr;
  ProposalNum    proposal;
  uint16_t       value_len;
  uint8_t        value[kMaxValueSize];
} __attribute__((packed));

// LEARN (committed value notification)
struct LearnMsg {
  PaxosMsgHeader hdr;
  uint16_t       value_len;
  uint8_t        value[kMaxValueSize];
} __attribute__((packed));

// NACK (reject — acceptor has a higher promise)
struct NackMsg {
  PaxosMsgHeader hdr;
  ProposalNum    highest_seen;  // the higher proposal the acceptor has seen
} __attribute__((packed));

}  // namespace paxos

#endif  // SIMBRICKS_HCOP_PAXOS_PROTO_H_
