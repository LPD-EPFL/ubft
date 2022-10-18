#pragma once

#include <iostream>
#include <sstream>
#include <string>

#include <dory/ctrl/block.hpp>
#include <dory/shared/logger.hpp>

namespace dory::conn {
struct RemoteConnection {
  struct __attribute__((packed)) RemoteConnectionInfo {
    uint16_t lid;
    uint32_t qpn;

    uintptr_t buf_addr;
    uint64_t buf_size;
    uint32_t rkey;
  };

  RemoteConnection() {
    rci.lid = 0;
    rci.qpn = 0;
    rci.buf_addr = 0;
    rci.buf_size = 0;
    rci.rkey = 0;
  }

  RemoteConnection(uint16_t lid, uint32_t qpn, uintptr_t buf_addr,
                   uint64_t buf_size, uint32_t rkey) {
    rci.lid = lid;
    rci.qpn = qpn;
    rci.buf_addr = buf_addr;
    rci.buf_size = buf_size;
    rci.rkey = rkey;
  }

  RemoteConnection(RemoteConnectionInfo rci) : rci{rci} {}

  std::string serialize() const {
    std::ostringstream os;

    os << std::hex << rci.lid << ":" << rci.qpn << ":" << rci.buf_addr << ":"
       << rci.buf_size << ":" << rci.rkey;
    return os.str();
  }

  static RemoteConnection fromStr(std::string const &str) {
    RemoteConnectionInfo rci;

    std::string res(str);

    std::replace(res.begin(), res.end(), ':', ' ');  // replace ':' by ' '

    std::stringstream ss(res);

    uint16_t lid;
    uint32_t qpn;

    uintptr_t buf_addr;
    uint64_t buf_size;
    uint32_t rkey;

    ss >> std::hex >> lid;
    ss >> std::hex >> qpn;
    ss >> std::hex >> buf_addr;
    ss >> std::hex >> buf_size;
    ss >> std::hex >> rkey;

    rci.lid = lid;
    rci.qpn = qpn;
    rci.buf_addr = buf_addr;
    rci.buf_size = buf_size;
    rci.rkey = rkey;

    return RemoteConnection(rci);
  }

  // private:
  RemoteConnectionInfo rci;
};

class ReliableConnection {
 public:
  enum Cq { SendCq, RecvCq };

  enum RdmaReq { RdmaRead = IBV_WR_RDMA_READ, RdmaWrite = IBV_WR_RDMA_WRITE };

  static int constexpr WrDepth = 128;
  static int constexpr SgeDepth = 16;
  static int constexpr MaxInlining = 256;
  static uint32_t constexpr DefaultPsn = 3185;
  static int constexpr CasLength = sizeof(uint64_t);

  ReliableConnection(ctrl::ControlBlock &cb);

  void bindToPd(std::string const &pd_name);

  void bindToMr(std::string const &mr_name);

  void associateWithCq(std::string const &send_cp_name,
                       std::string const &recv_cp_name);

  void reset();

  void init(ctrl::ControlBlock::MemoryRights rights);
  void reinit();

  void connect(RemoteConnection const &rci, int proc_id);
  void reconnect();

  int procId() const { return proc_id; }

  bool needsReset();
  bool changeRights(ctrl::ControlBlock::MemoryRights rights);
  bool changeRightsIfNeeded(ctrl::ControlBlock::MemoryRights rights);

  bool postSendSingle(RdmaReq req, uint64_t req_id, void *buf, uint32_t len,
                      uintptr_t remote_addr, bool signaled = true);

  // Only re-use this method when the previous WR posted by this method is
  // completed and a corresponding WC was consumed, otherwise unexpected
  // behaviour might occur. In case the WR is posted with `IBV_SEND_INLINE`
  // (which is the case when the length of the payload is smaller or equal to
  // `MaxInlining`) one can reuse this method right after it returns.
  bool postSendSingleCached(RdmaReq req, uint64_t req_id, void *buf,
                            uint32_t len, uintptr_t remote_addr);

  bool postSendSingle(RdmaReq req, uint64_t req_id, void *buf, uint32_t len,
                      uint32_t lkey, uintptr_t remote_addr,
                      bool signaled = true);

  bool postSendSingleCas(uint64_t req_id, void *buf, uintptr_t remote_addr,
                         uint64_t expected, uint64_t swap,
                         bool signaled = true);

  /**
   * @brief Posts a send request.
   *
   * The buffer must lie within the MR given at construction time.
   */
  bool postSendSingleSend(uint64_t req_id, void *buf, uint32_t len,
                          std::optional<uint32_t> immediate = std::nullopt,
                          bool signaled = true);

  /**
   * @brief Posts `number` recv requests.
   *
   * NOT THREAD-SAFE AS IT REUSES PRE-ALLOCATED BUFFERS.
   *
   * The buffers must lie within the MR given at construction time.
   * All the RECV will have the same `len`.
   * The req ids will span [base_req_id, base_req_id+number).
   */
  bool postRecvMany(uint64_t base_req_id, void **bufs, size_t number,
                    uint32_t len);

  bool pollCqIsOk(Cq cq, std::vector<struct ibv_wc> &entries) const;

  RemoteConnection remoteInfo() const;

  uintptr_t remoteBuf() const { return rconn.rci.buf_addr; }

  uint64_t remoteSize() const { return rconn.rci.buf_size; }

  ctrl::ControlBlock::MemoryRegion const &getMr() const { return mr; }

  void queryQp(ibv_qp_attr &qp_attr, ibv_qp_init_attr &init_attr,
               int attr_mask) const;

 private:
  bool postSend(ibv_send_wr &wr);

  static void wrDeleter(struct ibv_send_wr *wr) { free(wr); }

  static size_t roundUp(size_t numToRound, size_t multiple) {
    if (multiple == 0) {
      {
        return numToRound;
      }
    }

    size_t remainder = numToRound % multiple;
    if (remainder == 0) {
      {
        return numToRound;
      }
    }

    return numToRound + multiple - remainder;
  }

  ctrl::ControlBlock &cb;
  struct ibv_pd *pd;
  struct ibv_qp_init_attr create_attr;
  struct ibv_qp_attr conn_attr;
  int proc_id;
  deleted_unique_ptr<struct ibv_qp> uniq_qp;
  ctrl::ControlBlock::MemoryRegion mr;
  RemoteConnection rconn;
  ctrl::ControlBlock::MemoryRights init_rights;
  deleted_unique_ptr<struct ibv_send_wr> wr_cached;

  std::vector<struct ibv_recv_wr> recv_wr_cached;
  std::vector<struct ibv_sge> recv_sg_cached;

  LOGGER_DECL(logger);
};
}  // namespace dory::conn
