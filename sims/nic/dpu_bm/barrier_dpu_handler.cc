/*
 * Barrier DPU Handler — implementation.
 */

#include "barrier_dpu_handler.h"

#include <cstring>
#include <vector>

#include <hcop/hcop_proto.h>
#include <hcop/barrier_proto.h>

namespace barrier {

BarrierDpuHandler::BarrierDpuHandler(uint32_t max_barriers)
    : mgr_(max_barriers, 2) {  // 2 participants for 2-host topology
}

uint16_t BarrierDpuHandler::PrimitiveType() const {
  return hcop::kPrimitiveBarrier;
}

void BarrierDpuHandler::HandlePacket(dpu::DpuDevice &dev, const void *data,
                                     size_t len, dpu::PacketContext &ctx) {
  std::vector<OutMessage> responses;

  BarrierStatus status = mgr_.HandleMessage(data, len, responses);

  if (status == BarrierStatus::kLayoutOverflow) {
    // Escalate overflow to host
    dev.EscalateToHost(ctx.full_frame, ctx.full_frame_len);
    return;
  }

  if (!responses.empty()) {
    SendResponses(dev, responses, ctx);
  }
}

void BarrierDpuHandler::SendResponses(dpu::DpuDevice &dev,
                                      const std::vector<OutMessage> &msgs,
                                      const dpu::PacketContext &ctx) {
  for (const auto &msg : msgs) {
    // If broadcast, need to send to all participants?
    // In Phase 2, we simulate broadcast by sending to kBroadcast multicast group
    // or relying on switch to replicate if dest_id=255.
    // For now, adhere to HCOP spec: destination in HCOP header or Ethernet multicast.
    
    // msg.dest_id == 255 (kBroadcast).
    // The BuildFrame will handle MAC address for broadcast.
    
    auto frame = BuildFrame(msg, ctx);
    dev.SendEth(frame.data(), frame.size());
  }
}

std::vector<uint8_t> BarrierDpuHandler::BuildFrame(
    const OutMessage &msg, const dpu::PacketContext &ctx) {
  size_t payload_len = msg.data.size();
  size_t frame_len = 14 + sizeof(hcop::HcopHeader) + payload_len;

  std::vector<uint8_t> frame(frame_len, 0);

  // ---- Ethernet header ----
  const uint8_t *in_frame = static_cast<const uint8_t *>(ctx.full_frame);

  // dst MAC: Broadcast to ensure NIC accepts L2-flooded responses
  std::memset(frame.data(), 0xFF, 6);
  
  // Src MAC = original dst (DPU)
  std::memcpy(frame.data() + 6, in_frame, 6);

  // EtherType: HCOP
  frame[12] = (hcop::kHcopEtherType >> 8) & 0xFF;
  frame[13] = hcop::kHcopEtherType & 0xFF;

  // ---- HCOP header ----
  hcop::HcopHeader *hdr =
      reinterpret_cast<hcop::HcopHeader *>(frame.data() + 14);
  hdr->primitive_type = hcop::kPrimitiveBarrier;
  hdr->exception_type = hcop::kBarrierNoException;
  hdr->operation_id = ctx.operation_id;
  hdr->source_tier = hcop::kTierDpu;
  auto *in_hdr = reinterpret_cast<const hcop::HcopHeader*>(ctx.full_frame + 14);
  hdr->num_tier_crossings = in_hdr->num_tier_crossings;  // Preserve count (already incremented by DpuDevice)
  hdr->tier_path = in_hdr->tier_path;                    // Preserve path
  HCOP_APPEND_PATH(hdr, hcop::kTierDpu);
  hdr->payload_len = static_cast<uint16_t>(payload_len);

  // ---- Payload ----
  std::memcpy(frame.data() + 14 + sizeof(hcop::HcopHeader),
              msg.data.data(), payload_len);

  return frame;
}

}  // namespace barrier
