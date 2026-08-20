#include "NVMeIntf.hpp"

#include <boost/asio.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

class NVMeMi : public NVMeMiIntf, public std::enable_shared_from_this<NVMeMi>
{
  public:
    NVMeMi(boost::asio::io_context& io,
           const std::shared_ptr<sdbusplus::asio::connection>& conn,
           const std::vector<uint8_t>& addr, int net, uint8_t eid);
    ~NVMeMi() override;

    static void initLogging();
    void cancelPendingCommands() override;
    void closeEndpoint() override;

    // Delete copy operations
    NVMeMi(const NVMeMi&) = delete;
    NVMeMi& operator=(const NVMeMi&) = delete;

    // Delete move operations since this class likely manages resources
    NVMeMi(NVMeMi&&) = delete;
    NVMeMi& operator=(NVMeMi&&) = delete;

    void miPCIePortInformation(
        std::function<void(const std::error_code&, nvme_mi_read_port_info*)>&&
            cb) override;
    void miSubsystemHealthStatusPoll(
        std::function<void(const std::error_code&,
                           nvme_mi_nvm_ss_health_status*)>&& cb) override;
    void miScanCtrl(std::function<void(const std::error_code&,
                                       const std::vector<nvme_mi_ctrl_t>&)>
                        cb) override;
    void miVpdRead(uint16_t offset, uint16_t length,
                   std::function<void(const std::error_code&,
                                      std::span<uint8_t>)>&& cb) override;
    void adminIdentify(uint8_t eid, nvme_identify_cns cns, uint32_t nsid,
                       uint16_t cntid, uint16_t readLength,
                       std::function<void(const std::error_code&,
                                          std::span<uint8_t>)>&& cb) override;
    void adminGetLogPage(
        uint8_t eid, nvme_cmd_get_log_lid lid, uint32_t nsid, uint8_t lsp,
        std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb)
        override;

    void adminSanitize(uint8_t eid, nvme_sanitize_sanact sanact, uint8_t owpass,
                       uint32_t owpattern, uint32_t sanicap,
                       std::function<void(const std::error_code&,
                                          std::span<uint8_t>)>&& cb) override;

    void adminFwCommit(
        uint8_t eid, nvme_fw_commit_ca action, uint8_t slot, bool bpid,
        std::function<void(const std::error_code&, nvme_status_field)>&& cb)
        override;

    void adminFwDownload(
        uint8_t eid, uint32_t offset, uint32_t dataLen, std::vector<char> data,
        std::function<void(const std::error_code&, nvme_status_field)>&& cb)
        override;

    void adminXfer(uint8_t eid, const nvme_mi_admin_req_hdr& aadminReq,
                   std::span<uint8_t> data, unsigned int timeoutMs,
                   std::function<void(const std::error_code&,
                                      const nvme_mi_admin_resp_hdr&,
                                      std::span<uint8_t>)>&& cb) override;

    void adminSecuritySend(uint8_t eid, uint8_t proto, uint16_t protoSpecific,
                           std::span<uint8_t> data,
                           std::function<void(const std::error_code&,
                                              int nnnvmeStatus)>&& cb) override;

    void adminSecurityReceive(
        uint8_t eid, uint8_t proto, uint16_t protoSpecific,
        uint32_t transferLength,
        std::function<void(const std::error_code&, int nvmeStatus,
                           std::span<uint8_t> data)>&& cb) override;

  private:
    // the transfer size for nvme mi messages.
    // define in github.com/linux-nvme/libnvme/blob/master/src/nvme/mi.c
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int nvme_mi_xfer_size = 4096;

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::bus_t& dbus;

    // mctp connection
    nvme_mi_ep_t nvmeEP;

    int net{0};
    int nid{0};
    uint8_t eid{0};
    std::string addr;
    std::string mctpPath;

    // Map to store controllers by EID
    std::map<uint8_t, nvme_mi_ctrl_t> controllers;
    std::mutex controllersMtx;

    // Per-EID mutex: serializes NVMe commands for this endpoint
    std::shared_ptr<std::mutex> endpointMux;
    std::atomic_bool commandsCancelled{false};

    // A worker thread for calling NVMeMI cmd.
    class Worker
    {
      private:
        bool workerStop{};
        std::mutex workerMtx;
        std::condition_variable workerCv;
        boost::asio::io_context workerIO;
        std::thread thread;

      public:
        Worker();
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        Worker(Worker&& other) noexcept;
        Worker& operator=(Worker&& other) noexcept;
        ~Worker();
        void post(std::function<void(void)>&& func);
    };

    // A map from root bus number to the Worker
    // This map means to reuse the same worker for all NVMe EP under the same
    // I2C root bus. There is no real physical concurrency among the i2c/mctp
    // devices on the same bus. Though mctp kernel drive can schedule and
    // sequencialize the transactions but assigning individual worker thread to
    // each EP makes no sense.
    static std::map<int, std::shared_ptr<Worker>>& getWorkerMap();

    static nvme_root_t& getNVMeRoot();

    std::shared_ptr<Worker> worker;
    void post(std::function<void(void)>&& func);

    std::error_code tryPost(std::function<void(void)>&& func);

    void adminIdentifyFull(
        uint8_t eid, nvme_identify_cns cns, uint32_t nsid, uint16_t cntid,
        std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb);

    void adminIdentifyPartial(
        uint8_t eid, nvme_identify_cns cns, uint32_t nsid, uint16_t cntid,
        uint16_t readLength,
        std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb);

    nvme_mi_ctrl_t getController(uint8_t eid);
};
