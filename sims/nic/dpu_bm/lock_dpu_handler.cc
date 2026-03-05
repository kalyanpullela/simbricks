/*
 * Lock DPU Handler — implementation.
 */

#include "lock_dpu_handler.h"

#include <cstring>

#include <hcop/hcop_proto.h>
#include <hcop/lock_proto.h>

namespace lock {

LockDpuHandler::LockDpuHandler(uint32_t max_keys, uint64_t default_lease_ns,
                               uint16_t max_waiters_per_key)
    : mgr_(max_keys, default_lease_ns, max_waiters_per_key) {
}

uint16_t LockDpuHandler::PrimitiveType() const {
  return hcop::kPrimitiveLock;
}

void LockDpuHandler::HandlePacket(dpu::DpuDevice &dev, const void *data,
                                  size_t len, dpu::PacketContext &ctx) {
  std::vector<OutMessage> responses;

  // TODO(Phase 3): Get real timestamp from DPU event loop (main_time_ / 1000).
  // For now, use 0 — lease expiry is checked by a separate periodic timer,
  // not inline during packet processing.
  uint64_t now_ns = 0;

  LockStatus status = mgr_.HandleMessage(data, len, now_ns, responses);

  if (status == LockStatus::kKeyOverflow) {
    // Key table full — escalate to host via PCIe DMA.
    dev.EscalateToHost(ctx.full_frame, ctx.full_frame_len);
    return;
  }

  if (!responses.empty()) {
    SendResponses(dev, responses, ctx);
  }
}

void LockDpuHandler::SendResponses(dpu::DpuDevice &dev,
                                   const std::vector<OutMessage> &msgs,
                                   const dpu::PacketContext &ctx) {
  for (const auto &msg : msgs) {
    auto frame = BuildFrame(msg, ctx);
    dev.SendEth(frame.data(), frame.size());
  }
}

std::vector<uint8_t> LockDpuHandler::BuildFrame(
    const OutMessage &msg, const dpu::PacketContext &ctx) {
  size_t payload_len = msg.data.size();
  size_t frame_len = 14 + sizeof(hcop::HcopHeader) + payload_len;

  std::vector<uint8_t> frame(frame_len, 0);

  // ---- Ethernet header ----
  const uint8_t *in_frame = static_cast<const uint8_t *>(ctx.full_frame);

  // dst MAC: Broadcast to ensure NIC accepts L2-flooded responses
  std::memset(frame.data(), 0xFF, 6);
  std::memcpy(frame.data() + 6, in_frame, 6);     // src = original dst

  // EtherType: HCOP
  frame[12] = (hcop::kHcopEtherType >> 8) & 0xFF;
  frame[13] = hcop::kHcopEtherType & 0xFF;

  // ---- HCOP header ----
  hcop::HcopHeader *hdr =
      reinterpret_cast<hcop::HcopHeader *>(frame.data() + 14);
  hdr->primitive_type = hcop::kPrimitiveLock;
  hdr->exception_type = hcop::kLockNoException;
  hdr->operation_id = ctx.operation_id;
  hdr->source_tier = hcop::kTierDpu;
  auto *in_hdr = reinterpret_cast<const hcop::HcopHeader*>(ctx.full_frame + 14);
  hdr->num_tier_crossings = in_hdr->num_tier_crossings;
  hdr->tier_path = in_hdr->tier_path;
  HCOP_APPEND_PATH(hdr, hcop::kTierDpu);
  hdr->payload_len = static_cast<uint16_t>(payload_len);

  // ---- Lock payload ----
  std::memcpy(frame.data() + 14 + sizeof(hcop::HcopHeader),
              msg.data.data(), payload_len);

  return frame;
}

}  // namespace lock
