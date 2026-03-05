/*
 * NetPort — implementation.
 */

#include "net_port.h"

#include <iostream>

namespace hcop_switch {

bool NetPort::Init() {
  struct SimbricksBaseIfParams params = netParams;
  params.sync_mode =
      (sync_ ? kSimbricksBaseIfSyncOptional : kSimbricksBaseIfSyncDisabled);
  params.sock_path = path_.c_str();
  params.blocking_conn = false;

  if (SimbricksBaseIfInit(&netif_.base, &params)) {
    perror("Init: SimbricksBaseIfInit failed");
    return false;
  }
  return true;
}

NetPort::NetPort(const char *path, int sync)
    : rx_(nullptr), sync_(sync), path_(path) {
  memset(&netif_, 0, sizeof(netif_));
}

NetPort::NetPort(const NetPort &other)
    : netif_(other.netif_),
      rx_(other.rx_),
      sync_(other.sync_),
      path_(other.path_) {}

bool NetPort::Prepare() {
  if (!Init()) return false;

  if (SimbricksBaseIfConnect(&netif_.base)) {
    perror("Prepare: SimbricksBaseIfConnect failed");
    return false;
  }
  return true;
}

void NetPort::Prepared() {
  sync_ = SimbricksBaseIfSyncEnabled(&netif_.base);
}

bool NetPort::IsSync() const { 
    return sync_; 
}

void NetPort::Sync(uint64_t cur_ts) {
  while (SimbricksNetIfOutSync(&netif_, cur_ts)) {
  }
}

uint64_t NetPort::NextTimestamp() {
  return SimbricksNetIfInTimestamp(&netif_);
}

NetPort::RxPollState NetPort::RxPacket(const void *&data, size_t &len,
                                       uint64_t cur_ts) {
  // rx_ should be null before polling
  if (rx_ != nullptr) {
      // Logic error in caller? Or maybe just return success if we already have one?
      // net_switch asserts(rx_ == nullptr).
      // We'll mimic.
      // But logging error is better than crash for behavioral model safety.
      std::cerr << "NetPort::RxPacket: rx_ not consumed!\n";
      return kRxPollError;
  }

  rx_ = SimbricksNetIfInPoll(&netif_, cur_ts);
  if (!rx_) return kRxPollFail;

  uint8_t type = SimbricksNetIfInType(&netif_, rx_);
  if (type == SIMBRICKS_PROTO_NET_MSG_PACKET) {
    data = (const void *)rx_->packet.data;
    len = rx_->packet.len;
    return kRxPollSuccess;
  } else if (type == SIMBRICKS_PROTO_MSG_TYPE_SYNC) {
    SimbricksNetIfInDone(&netif_, rx_);
    rx_ = nullptr;
    return kRxPollSync;
  } else if (type == SIMBRICKS_PROTO_MSG_TYPE_TERMINATE) {
    SimbricksNetIfInDone(&netif_, rx_);
    rx_ = nullptr;
    return kRxPollError; // Terminate signal
  } else {
    // Unsupported type
    fprintf(stderr, "switch_pkt: unsupported type=%u\n", type);
    SimbricksNetIfInDone(&netif_, rx_);
    rx_ = nullptr;
    return kRxPollError; 
  }
}

void NetPort::RxDone() {
  if (rx_ != nullptr) {
    SimbricksNetIfInDone(&netif_, rx_);
    rx_ = nullptr;
  }
}

bool NetPort::TxPacket(const void *data, size_t len, uint64_t cur_ts) {
  volatile union SimbricksProtoNetMsg *msg_to =
      SimbricksNetIfOutAlloc(&netif_, cur_ts);
      
  if (!msg_to && !sync_) {
    return false; // Drop if async and buffer full?
  } else if (!msg_to && sync_) {
    // Blocking alloc for sync
    while (!msg_to) msg_to = SimbricksNetIfOutAlloc(&netif_, cur_ts);
  } else if (!msg_to) {
      // Should not happen if non-sync logic is correct
      return false;
  }

  volatile struct SimbricksProtoNetMsgPacket *rx;
  rx = &msg_to->packet;
  rx->len = len;
  rx->port = 0;
  memcpy((void *)rx->data, data, len);

  SimbricksNetIfOutSend(&netif_, msg_to, SIMBRICKS_PROTO_NET_MSG_PACKET);
  return true;
}


// NetListenPort
NetListenPort::NetListenPort(const char *path, int sync)
    : NetPort(path, sync) {
  memset(&pool_, 0, sizeof(pool_));
}

NetListenPort::NetListenPort(const NetListenPort &other)
    : NetPort(other), pool_(other.pool_) {}

bool NetListenPort::Prepare() {
  if (!Init()) return false;

  std::string shm_path = path_;
  shm_path += "-shm";

  if (SimbricksBaseIfSHMPoolCreate(
          &pool_, shm_path.c_str(),
          SimbricksBaseIfSHMSize(&netif_.base.params)) != 0) {
    perror("Prepare: SimbricksBaseIfSHMPoolCreate failed");
    return false;
  }

  if (SimbricksBaseIfListen(&netif_.base, &pool_) != 0) {
    perror("Prepare: SimbricksBaseIfListen failed");
    return false;
  }
  return true;
}

}  // namespace hcop_switch
