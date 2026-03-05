/*
 * HCOP Switch — Main Device Class.
 *
 * Implements a behavioral model of a Tofino-2 programmable switch running HCOP logic.
 * Manages ports, pipeline latency simulation via event queue, and dispatch to PrimitiveEngine.
 */

#ifndef SIMBRICKS_HCOP_SWITCH_DEVICE_H_
#define SIMBRICKS_HCOP_SWITCH_DEVICE_H_

#include <queue>
#include <vector>
#include <cstdint>

#include "net_port.h"
#include "primitive_engine.h"
#include "switch_config.h"

namespace hcop_switch {

struct PacketEvent {
  uint64_t timestamp;
  size_t ingress_port;
  
  // We copy the packet data because SimBricks NetIf buffers are transient/circular.
  // Behavioral model uses std::vector for simplicity/safety over raw pointers.
  std::vector<uint8_t> data;

  // Priority queue orders by smallest timestamp first (min-heap behavior)
  // std::priority_queue uses operator< for max-heap, so default is largest first.
  // We want smallest first. So operator< should return "true if A has LOWER priority than B".
  // i.e. A < B means A comes AFTER B.
  // Wait, priority_queue pops the "largest" element.
  // If we want smallest timestamp (earliest) popped first, "largest" needs to mean "smallest timestamp".
  // So operator<: return timestamp > other.timestamp.
  bool operator<(const PacketEvent &other) const {
    return timestamp > other.timestamp;
  }
};

class HcopSwitch {
 public:
  HcopSwitch(const SwitchConfig &cfg);
  
  void AddPort(NetPort *port);
  void Run(); // Main loop
  void Stop() { exiting_ = true; }

 private:
  SwitchConfig config_;
  std::vector<NetPort *> ports_;
  std::priority_queue<PacketEvent> event_queue_;
  PrimitiveEngine primitive_engine_;
  
  uint64_t cur_ts_ = 0;
  bool exiting_ = false;

  uint64_t pkts_processed_ = 0;
  uint64_t pkts_dropped_ = 0;
  uint64_t pkts_queued_ = 0;

  void ProcessEvents();
  void PollPorts();
  void SchedulePacket(size_t ingress_port, const void *data, size_t len);
  void ForwardPacket(const void *data, size_t len, int dst_port);
};

}  // namespace hcop_switch

#endif  // SIMBRICKS_HCOP_SWITCH_DEVICE_H_
