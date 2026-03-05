/*
 * Paxos DPU Handler — implementation.
 */

#include "paxos_dpu_handler.h"

#include <cstring>

#include <hcop/hcop_proto.h>
#include <hcop/paxos_proto.h>

namespace paxos {

PaxosDpuHandler::PaxosDpuHandler(uint8_t node_id, uint16_t num_replicas,
                                 uint32_t max_instances)
    : node_(node_id, num_replicas, max_instances) {
}

uint16_t PaxosDpuHandler::PrimitiveType() const {
  return hcop::kPrimitivePaxos;
}

void PaxosDpuHandler::HandlePacket(dpu::DpuDevice &dev, const void *data,
                                    size_t len, dpu::PacketContext &ctx) {
  std::vector<OutMessage> responses;
  PaxosStatus status = node_.HandleMessage(data, len, responses);

  if (status == PaxosStatus::kInstanceOverflow) {
    // Instance log full — escalate to host via PCIe DMA.
    dev.EscalateToHost(ctx.full_frame, ctx.full_frame_len);
    return;
  }

  if (!responses.empty()) {
    SendResponses(dev, responses, ctx);
  }
}

void PaxosDpuHandler::SendResponses(dpu::DpuDevice &dev,
                                     const std::vector<OutMessage> &msgs,
                                     const dpu::PacketContext &ctx) {
  for (const auto &msg : msgs) {
    auto frame = BuildFrame(msg, ctx);
    dev.SendEth(frame.data(), frame.size());
  }
}

std::vector<uint8_t> PaxosDpuHandler::BuildFrame(
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
  hdr->primitive_type = hcop::kPrimitivePaxos;
  hdr->exception_type = hcop::kPaxosNoException;
  hdr->operation_id = ctx.operation_id;
  hdr->source_tier = hcop::kTierDpu;
  auto *in_hdr = reinterpret_cast<const hcop::HcopHeader*>(ctx.full_frame + 14);
  hdr->num_tier_crossings = in_hdr->num_tier_crossings;
  hdr->tier_path = in_hdr->tier_path;
  HCOP_APPEND_PATH(hdr, hcop::kTierDpu);
  hdr->payload_len = static_cast<uint16_t>(payload_len);

  // ---- Paxos payload ----
  std::memcpy(frame.data() + 14 + sizeof(hcop::HcopHeader),
              msg.data.data(), payload_len);

  return frame;
}

}  // namespace paxos
