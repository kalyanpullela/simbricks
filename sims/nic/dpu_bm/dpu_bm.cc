/*
 * DPU Behavioral Model — device implementation.
 *
 * Implements a BlueField-3 DPU as a NIC-like SimBricks device:
 *   - PCIe to host (register access + DMA for exception escalation)
 *   - Single Ethernet to switch (coordination protocol traffic)
 *
 * See docs/hcop_1a_decisions.md for design rationale.
 */

#include "dpu_bm.h"

#include <cstdio>
#include <cstring>
#include <iostream>

#include <simbricks/nicbm/multinic.h>

#include "paxos_dpu_handler.h"
#include "lock_dpu_handler.h"
#include "barrier_dpu_handler.h"
#include <hcop/hcop_telemetry.h>

namespace dpu {

// ======================================================================
// DpuDevice construction
// ======================================================================

DpuDevice::DpuDevice()
    : DpuDevice(DefaultConfig()) {
}

DpuDevice::DpuDevice(const DpuConfig &cfg)
    : cfg_(cfg),
      core_pool_(cfg.arm_cores),
      dram_(cfg.dram_capacity_mb * 1024ULL * 1024ULL) {
  
  // Initialize DMA state
  dma_op_.write_ = false;
  dma_op_.dma_addr_ = 0;
  dma_op_.len_ = 0;
  dma_op_.data_ = nullptr;
  std::memset(dma_buf_, 0, sizeof(dma_buf_));

  // Initialize sampling event
  sampling_event_ = new SamplingEvent();
  sampling_event_->interval_ps = cfg.telemetry_interval_ms * 1000ULL * 1000000ULL;
  sampling_event_->time_ = sampling_event_->interval_ps;
  // It will be scheduled in EthRx when runner gets initialized, or SetupIntro
  // Actually, we must schedule it after runner_ is set. SetupIntro is a good place.

  // Register default primitive handlers
  // TODO(Phase 2): Make these configurable/selectable via config?
  // For now, enable all supported primitives.
  
  // Paxos (Experiment 1A Phase 2.2)
  RegisterHandler(std::make_unique<paxos::PaxosDpuHandler>(
      cfg.node_id, cfg.num_replicas));

  // Locking (Experiment 1A Phase 2.3)
  RegisterHandler(std::make_unique<lock::LockDpuHandler>());

  // Barrier (Experiment 1A Phase 2.4)
  RegisterHandler(std::make_unique<barrier::BarrierDpuHandler>());
}

// ======================================================================
// PCIe Setup  (Task 1.2)
// ======================================================================

void DpuDevice::SetupIntro(struct SimbricksProtoPcieDevIntro &di) {
  // BAR 0: 64 KB register space (status + control)
  di.bars[kBarRegs].len = 64 * 1024;
  di.bars[kBarRegs].flags = SIMBRICKS_PROTO_PCIE_BAR_64;

  // BAR 3: MSI-X table (8 vectors sufficient for DPU)
  di.bars[kBarMsix].len = 4 * 1024;
  di.bars[kBarMsix].flags =
      SIMBRICKS_PROTO_PCIE_BAR_64 | SIMBRICKS_PROTO_PCIE_BAR_DUMMY;

  // NVIDIA BlueField-3 DPU PCI IDs
  di.pci_vendor_id = 0x15B3;   // Mellanox/NVIDIA
  di.pci_device_id = 0xA2DC;   // BlueField-3 integrated ConnectX-7
  di.pci_class = 0x02;         // Network controller
  di.pci_subclass = 0x00;      // Ethernet controller
  di.pci_revision = 0x01;

  di.pci_msi_nvecs = 1;
  di.pci_msix_nvecs = 8;
  di.pci_msix_table_bar = kBarMsix;
  di.pci_msix_pba_bar = kBarMsix;
  di.pci_msix_table_offset = 0x0;
  di.pci_msix_pba_offset = 0x800;
  di.psi_msix_cap_offset = 0x70;

  sampling_event_->time_ = runner_->TimePs() + sampling_event_->interval_ps;
  runner_->EventSchedule(*sampling_event_);
}

// ======================================================================
// Register I/O  (Task 1.2)
// ======================================================================

void DpuDevice::RegRead(uint8_t bar, uint64_t addr, void *dest, size_t len) {
  if (bar != kBarRegs || len != 4) {
    std::memset(dest, 0, len);
    return;
  }

  uint32_t val = 0;
  switch (addr) {
    case kRegStatus:
      val = 1;  // device ready
      break;
    case kRegCoreActive:
      val = core_pool_.ActiveCount();
      break;
    case kRegCoreCapacity:
      val = core_pool_.Capacity();
      break;
    case kRegDramUsedLo:
      val = static_cast<uint32_t>(dram_.UsedBytes());
      break;
    case kRegDramUsedHi:
      val = static_cast<uint32_t>(dram_.UsedBytes() >> 32);
      break;
    case kRegDramCapLo:
      val = static_cast<uint32_t>(dram_.CapacityBytes());
      break;
    case kRegDramCapHi:
      val = static_cast<uint32_t>(dram_.CapacityBytes() >> 32);
      break;
    case kRegPktProcessed:
      val = static_cast<uint32_t>(pkts_processed_);
      break;
    case kRegPktDropped:
      val = static_cast<uint32_t>(pkts_dropped_);
      break;
    case kRegPktEscalated:
      val = static_cast<uint32_t>(pkts_escalated_);
      break;
    default:
      break;
  }
  std::memcpy(dest, &val, sizeof(val));
}

void DpuDevice::RegWrite(uint8_t bar, uint64_t addr, const void *src,
                         size_t len) {
  // All registers are read-only in Phase 1.
  // Future: control registers for configuration, reset, etc.
  (void)bar;
  (void)addr;
  (void)src;
  (void)len;
}

// ======================================================================
// DMA Completion
// ======================================================================

void DpuDevice::DmaComplete(nicbm::DMAOp &op) {
  // DMA write to host completed — nothing to do for fire-and-forget escalation.
  (void)op;
}

// ======================================================================
// Ethernet RX — Packet Ingress  (Task 1.5)
// ======================================================================

void DpuDevice::EthRx(uint8_t port, const void *data, size_t len) {
  // Minimum frame: Ethernet header (14) + HcopHeader (12)
  if (len < hcop::kMinHcopFrameLen) {
    ++pkts_dropped_;
    return;
  }

  // Parse Ethernet header — check EtherType.
  const uint8_t *frame = static_cast<const uint8_t *>(data);
  uint16_t ethertype = (static_cast<uint16_t>(frame[12]) << 8) | frame[13];
  if (ethertype != hcop::kHcopEtherType) {
    // Not an HCOP packet — drop silently.
    ++pkts_dropped_;
    return;
  }

  // Parse HCOP header (starts at offset 14).
  const hcop::HcopHeader *hdr =
      reinterpret_cast<const hcop::HcopHeader *>(frame + 14);

  // Look up handler for this primitive type.
  auto it = handlers_.find(hdr->primitive_type);
  if (it == handlers_.end()) {
    // No handler registered — drop with warning.
    std::fprintf(stderr,
                 "dpu_bm: no handler for primitive_type=%u, dropping\n",
                 hdr->primitive_type);
    ++pkts_dropped_;
    return;
  }

  // Try to acquire an ARM core for processing.
  auto core = core_pool_.TryAcquire();
  if (!core.has_value()) {
    // All cores busy — set exception type and escalate to host via PCIe.
    std::vector<uint8_t> frame(static_cast<const uint8_t *>(data),
                               static_cast<const uint8_t *>(data) + len);
    auto *ehdr = reinterpret_cast<hcop::HcopHeader *>(frame.data() + 14);
    ehdr->exception_type = hcop::kDpuCoreExhausted;
    HCOP_APPEND_PATH(ehdr, hcop::kTierDpu);
    EscalateToHost(frame.data(), frame.size());
    return;
  }
  
  pkts_queued_++;

  // Schedule processing event with simulated latency.
  auto *evt = new ProcessingEvent();
  evt->core_id = core.value();
  evt->primitive_type = hdr->primitive_type;
  evt->ctx.port = port;
  evt->ctx.operation_id = hdr->operation_id;
  evt->ctx.source_tier = hdr->source_tier;
  evt->ctx.full_frame = nullptr;  // will be set from payload copy
  evt->ctx.full_frame_len = len;

  // Copy the full frame into the event (data pointer won't survive).
  evt->payload.assign(frame, frame + len);
  evt->ctx.full_frame = evt->payload.data();
  
  auto *mut_hdr = reinterpret_cast<hcop::HcopHeader *>(evt->payload.data() + 14);
  HCOP_APPEND_PATH(mut_hdr, hcop::kTierDpu);

  // Schedule: latency in picoseconds (SimBricks time unit).
  uint64_t latency_ps = cfg_.per_packet_base_latency_ns * 1000ULL;
  evt->time_ = runner_->TimePs() + latency_ps;
  evt->priority_ = 0;
  runner_->EventSchedule(*evt);
}

// ======================================================================
// Timed Event — Processing Completion  (Task 1.5)
// ======================================================================

void DpuDevice::Timed(nicbm::TimedEvent &te) {
  if (auto *se = dynamic_cast<SamplingEvent*>(&te)) {
      float core_util = 0;
      if (core_pool_.Capacity() > 0) {
          core_util = (float)core_pool_.ActiveCount() / core_pool_.Capacity() * 100.0f;
      }
      
      size_t handlers_mem = 0;
      for (const auto& pair : handlers_) {
          handlers_mem += pair.second->MemoryUsedBytes();
      }
      
      hcop::TelemetryLogger::Get().LogUtilization(
          runner_->TimePs() / 1000ULL, hcop::kTierDpu, core_util,
          dram_.UsedBytes() + handlers_mem, dram_.CapacityBytes(),
          pkts_processed_, pkts_queued_, pkts_dropped_
      );
      
      se->time_ = runner_->TimePs() + se->interval_ps;
      runner_->EventSchedule(*se);
      return;
  }
  
  ProcessingEvent &evt = static_cast<ProcessingEvent &>(te);

  // Look up handler (should still be registered).
  auto it = handlers_.find(evt.primitive_type);
  if (it != handlers_.end()) {
    // Payload starts after Ethernet header (14) + HcopHeader (12).
    const uint8_t *payload_start = evt.payload.data() + hcop::kMinHcopFrameLen;
    size_t payload_len = (evt.payload.size() > hcop::kMinHcopFrameLen)
                             ? evt.payload.size() - hcop::kMinHcopFrameLen
                             : 0;
    it->second->HandlePacket(*this, payload_start, payload_len, evt.ctx);
    ++pkts_processed_;
  } else {
    ++pkts_dropped_;
  }
  
  if (pkts_queued_ > 0) pkts_queued_--;

  // Release the ARM core.
  core_pool_.Release(evt.core_id);

  // Clean up the heap-allocated event.
  delete &evt;
}

// ======================================================================
// Handler Registration  (Task 1.6)
// ======================================================================

void DpuDevice::RegisterHandler(std::unique_ptr<PrimitiveHandler> handler) {
  uint16_t type = handler->PrimitiveType();
  handlers_[type] = std::move(handler);
}

// ======================================================================
// Ethernet TX
// ======================================================================

void DpuDevice::SendEth(const void *data, size_t len) {
  if (eth_send_cb_) {
    eth_send_cb_(data, len);
    return;
  }
  runner_->EthSend(data, len);
}

// ======================================================================
// PCIe Host Exception Path  (Task 1.8)
// ======================================================================

void DpuDevice::EscalateToHost(const void *data, size_t len) {
  // Note: Host guest missing PCIe DMA driver. Fallback server listens on eth0.
  // Escalate over Ethernet instead so switch forwards to fallback_port.
  SendEth(data, len);
  ++pkts_escalated_;
}

}  // namespace dpu

// ======================================================================
// Entry Point  (follows i40e_bm pattern with MultiNicRunner)
// Guard: tests define DPU_BM_NO_MAIN to avoid duplicate main().
// ======================================================================

#ifndef DPU_BM_NO_MAIN

class DpuFactory : public nicbm::MultiNicRunner::DeviceFactory {
 public:
  nicbm::Runner::Device &create() override {
    return *new dpu::DpuDevice;
  }
};

int main(int argc, char *argv[]) {
  DpuFactory fact;
  nicbm::MultiNicRunner mr(fact);
  return mr.RunMain(argc, argv);
}

#endif  // DPU_BM_NO_MAIN
/* Telemetry fixes complete */
