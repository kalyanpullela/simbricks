/*
 * NetPort — SimBricks network interface wrapper.
 * Based on sims/net/net_switch/switch.cc pattern.
 */

#ifndef SIMBRICKS_HCOP_SWITCH_NET_PORT_H_
#define SIMBRICKS_HCOP_SWITCH_NET_PORT_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <simbricks/base/cxxatomicfix.h>
extern "C" {
#include <simbricks/network/if.h>
#include <simbricks/nicif/nicif.h>
}

// Global params needed by Init/Prepare (copied from net_switch)
extern struct SimbricksBaseIfParams netParams;

namespace hcop_switch {

class NetPort {
 public:
  enum RxPollState {
    kRxPollSuccess = 0,
    kRxPollFail = 1,
    kRxPollSync = 2,
    kRxPollError = 3,
  };

  struct SimbricksNetIf netif_;

 protected:
  volatile union SimbricksProtoNetMsg *rx_;
  int sync_;
  std::string path_;

  bool Init();

 public:
  NetPort(const char *path, int sync);
  NetPort(const NetPort &other);
  virtual ~NetPort() = default;

  virtual bool Prepare();
  virtual void Prepared();

  bool IsSync() const;
  void Sync(uint64_t cur_ts);
  uint64_t NextTimestamp();

  enum RxPollState RxPacket(const void *&data, size_t &len, uint64_t cur_ts);
  void RxDone();

  bool TxPacket(const void *data, size_t len, uint64_t cur_ts);
};

/* Listening switch port (connected to by another network) */
class NetListenPort : public NetPort {
 protected:
  struct SimbricksBaseIfSHMPool pool_;

 public:
  NetListenPort(const char *path, int sync);
  NetListenPort(const NetListenPort &other);
  
  bool Prepare() override;
};

}  // namespace hcop_switch

#endif  // SIMBRICKS_HCOP_SWITCH_NET_PORT_H_
