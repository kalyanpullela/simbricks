/*
 * DPU Behavioral Model — main header.
 *
 * DpuDevice subclasses nicbm::Runner::Device to model a BlueField-3 DPU
 * with a single PCIe interface (host-facing) and single Ethernet port
 * (switch-facing). See docs/hcop_1a_decisions.md #1.
 */

#ifndef SIMBRICKS_DPU_BM_DPU_BM_H_
#define SIMBRICKS_DPU_BM_DPU_BM_H_

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <simbricks/nicbm/nicbm.h>

#include "dpu_config.h"
#include <hcop/hcop_proto.h>

namespace dpu {

class DpuDevice;

// --------------------------------------------------------------------------
// ARM Core Pool  (Task 1.3)
// --------------------------------------------------------------------------
// Models N concurrent processing slots as a simple available-count model.
// No real threading — timing is simulated via TimedEvents.
class ArmCorePool {
 public:
  explicit ArmCorePool(uint32_t capacity);

  /** Try to acquire a core. Returns core_id on success, nullopt if exhausted. */
  std::optional<uint32_t> TryAcquire();

  /** Release a previously acquired core. */
  void Release(uint32_t core_id);

  /** Number of cores currently in use. */
  uint32_t ActiveCount() const;

  /** Total number of cores. */
  uint32_t Capacity() const;

 private:
  uint32_t capacity_;
  uint32_t active_;
  // Bit set tracking which cores are in use (up to 64 cores).
  uint64_t in_use_;
};

// --------------------------------------------------------------------------
// Simulated DRAM  (Task 1.4)
// --------------------------------------------------------------------------
// Key-value store with capacity enforcement.
class DramStore {
 public:
  explicit DramStore(uint64_t capacity_bytes);

  /**
   * Allocate storage for a key.
   * Returns false if capacity would be exceeded or key already exists.
   */
  bool Allocate(uint64_t key, size_t size);

  /** Read data for a key. Returns nullptr/0 if key not found. */
  const uint8_t *Read(uint64_t key, size_t *out_len) const;

  /** Write data to an existing allocation. Returns false if key not found. */
  bool Write(uint64_t key, const uint8_t *data, size_t len);

  /** Free a key's allocation. Safe to call on nonexistent keys (no-op). */
  void Free(uint64_t key);

  uint64_t UsedBytes() const;
  uint64_t CapacityBytes() const;

 private:
  uint64_t capacity_bytes_;
  uint64_t used_bytes_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> store_;
};

// --------------------------------------------------------------------------
// Primitive Handler Interface  (Task 1.6)
// --------------------------------------------------------------------------
// Forward declaration of context passed to handlers.
struct PacketContext {
  uint8_t port;             // ingress port
  uint32_t operation_id;    // from HCOP header
  uint8_t source_tier;      // from HCOP header
  const void *full_frame;   // pointer to full Ethernet frame
  size_t full_frame_len;    // length of full Ethernet frame
};

class PrimitiveHandler {
 public:
  virtual ~PrimitiveHandler() = default;

  /** Return the primitive type this handler processes. */
  virtual uint16_t PrimitiveType() const = 0;

  /**
   * Handle a packet for this primitive.
   * Called after simulated processing delay on an ARM core.
   *
   * @param dev   The DPU device (for sending responses).
   * @param data  Pointer to the HCOP payload (after HcopHeader).
   * @param len   Length of the payload.
   * @param ctx   Context about the packet and frame.
   */
  virtual void HandlePacket(DpuDevice &dev, const void *data, size_t len,
                            PacketContext &ctx) = 0;

  /** Return the dynamic memory size in bytes used by this handler's state. */
  virtual size_t MemoryUsedBytes() const { return 0; }
};

// --------------------------------------------------------------------------
// Processing Event  (for Timed callback)
// --------------------------------------------------------------------------
class ProcessingEvent : public nicbm::TimedEvent {
 public:
  uint32_t core_id;          // ARM core allocated for this operation
  uint16_t primitive_type;   // for handler lookup on completion
  PacketContext ctx;

  // Copy of payload data (owned by this event).
  std::vector<uint8_t> payload;
};

// --------------------------------------------------------------------------
// Sampling Event  (for utilization sampling)
// --------------------------------------------------------------------------
class SamplingEvent : public nicbm::TimedEvent {
 public:
  uint64_t interval_ps;
};

// --------------------------------------------------------------------------
// DPU Device  (Tasks 1.2, 1.5, 1.8)
// --------------------------------------------------------------------------
class DpuDevice : public nicbm::Runner::Device {
 public:
  DpuDevice();
  explicit DpuDevice(const DpuConfig &cfg);
  ~DpuDevice() = default;

  // ----- Runner::Device overrides -----
  void SetupIntro(struct SimbricksProtoPcieDevIntro &di) override;
  void RegRead(uint8_t bar, uint64_t addr, void *dest, size_t len) override;
  void RegWrite(uint8_t bar, uint64_t addr, const void *src,
                size_t len) override;
  void DmaComplete(nicbm::DMAOp &op) override;
  void EthRx(uint8_t port, const void *data, size_t len) override;
  void Timed(nicbm::TimedEvent &te) override;

  // ----- Public API for handlers -----
  /** Register a primitive handler. Ownership transferred. */
  void RegisterHandler(std::unique_ptr<PrimitiveHandler> handler);

  /** Send an Ethernet frame out the DPU's network port. */
  void SendEth(const void *data, size_t len);

  /**
   * Set a callback for intercepting EthSend (for testing).
   * When set, SendEth() calls the callback instead of runner_->EthSend().
   */
  void SetEthSendCallback(
      std::function<void(const void *, size_t)> cb) {
    eth_send_cb_ = std::move(cb);
  }

  /** Escalate a packet to the host via PCIe DMA. */
  void EscalateToHost(const void *data, size_t len);

  // ----- Accessors for telemetry / testing -----
  const DpuConfig &Config() const { return cfg_; }
  const ArmCorePool &CorePool() const { return core_pool_; }
  const DramStore &Dram() const { return dram_; }

 private:
  // BAR indices
  static constexpr unsigned kBarRegs = 0;
  static constexpr unsigned kBarMsix = 3;

  // Register offsets (BAR 0)
  static constexpr uint64_t kRegStatus         = 0x00;  // R: device status
  static constexpr uint64_t kRegCoreActive     = 0x04;  // R: active core count
  static constexpr uint64_t kRegCoreCapacity   = 0x08;  // R: total cores
  static constexpr uint64_t kRegDramUsedLo     = 0x0C;  // R: DRAM used (low 32)
  static constexpr uint64_t kRegDramUsedHi     = 0x10;  // R: DRAM used (high 32)
  static constexpr uint64_t kRegDramCapLo      = 0x14;  // R: DRAM cap (low 32)
  static constexpr uint64_t kRegDramCapHi      = 0x18;  // R: DRAM cap (high 32)
  static constexpr uint64_t kRegPktProcessed   = 0x1C;  // R: packets processed
  static constexpr uint64_t kRegPktDropped     = 0x20;  // R: packets dropped
  static constexpr uint64_t kRegPktEscalated   = 0x24;  // R: packets escalated

  // Host DMA ring buffer for exception escalation
  static constexpr uint64_t kHostDmaBase = 0x100000;   // host physical addr
  static constexpr size_t kHostDmaSlotSize = 2048;
  static constexpr size_t kHostDmaSlots = 64;

  DpuConfig cfg_;
  ArmCorePool core_pool_;
  DramStore dram_;

  // Handler registry: primitive_type → handler
  std::unordered_map<uint16_t, std::unique_ptr<PrimitiveHandler>> handlers_;

  // Telemetry counters
  uint64_t pkts_processed_ = 0;
  uint64_t pkts_dropped_ = 0;
  uint64_t pkts_escalated_ = 0;
  uint64_t pkts_queued_ = 0;

  SamplingEvent *sampling_event_ = nullptr;

  // DMA escalation ring write pointer
  uint32_t dma_ring_head_ = 0;

  // Optional callback for testing (intercepts SendEth)
  std::function<void(const void *, size_t)> eth_send_cb_;

  // Scratch buffer for DMA operations
  nicbm::DMAOp dma_op_;
  uint8_t dma_buf_[kHostDmaSlotSize];
};

}  // namespace dpu

#endif  // SIMBRICKS_DPU_BM_DPU_BM_H_
