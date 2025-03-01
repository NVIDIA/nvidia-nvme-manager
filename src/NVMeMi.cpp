
#include "NVMeMi.hpp"

#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/endian.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cerrno>
#include <iostream>

std::map<int, std::weak_ptr<NVMeMi::Worker>> NVMeMi::workerMap{};

// libnvme-mi root service
nvme_root_t NVMeMi::nvmeRoot = nvme_mi_create_root(stderr, DEFAULT_LOGLEVEL);

constexpr size_t maxNVMeMILength = 4096;

NVMeMi::NVMeMi(boost::asio::io_context& io,
               std::shared_ptr<sdbusplus::asio::connection> conn,
               std::vector<uint8_t> sockName, uint8_t eid) :
    io(io),
    conn(conn), dbus(*conn.get()), eid(eid)
{
    // reset to unassigned nid/eid and endpoint
    nid = -1;
    mctpPath.erase();
    nvmeEP = nullptr;

    // set update the worker thread
    if (!nvmeRoot)
    {
        throw std::runtime_error("invalid NVMe root");
    }

    addr.assign(sockName.begin() + 1, sockName.end());

    // only create one share worker for all drives
    auto res = workerMap.find(0);
    if (res == workerMap.end() || res->second.expired())
    {
        worker = std::make_shared<Worker>();
        workerMap[0] = worker;
    }
    else
    {
        worker = res->second.lock();
    }

    nvmeEP = nvme_mi_open_libmctp(nvmeRoot, 0, (char*)sockName.data(), eid);
    if (nvmeEP == nullptr)
    {
        nid = -1;
        eid = 0;
        // MCTPd won't expect to delete the ep object, just to erase the record
        // here.
        nvmeEP = nullptr;
        auto str = std::to_string(nid) + ":" + std::to_string(eid);
        lg2::error("[addr:{ADDR}] can't open MCTP endpoint {MSG}", "ADDR", addr,
                   "MSG", str);
    }
}

NVMeMi::Worker::Worker()
{ // start worker thread
    workerStop = false;
    thread = std::thread([&io = workerIO, &stop = workerStop, &mtx = workerMtx,
                          &cv = workerCv]() {
        // With BOOST_ASIO_DISABLE_THREADS, boost::asio::executor_work_guard
        // issues null_event across the thread, which caused invalid invokation.
        // We implement a simple invoke machenism based std::condition_variable.
        while (1)
        {
            io.run();
            io.restart();
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock);
                if (stop)
                {
                    // exhaust all tasks and exit
                    io.run();
                    break;
                }
            }
        }
    });
}

NVMeMi::Worker::~Worker()
{
    // close worker
    workerStop = true;
    {
        std::unique_lock<std::mutex> lock(workerMtx);
        workerCv.notify_all();
    }
    thread.join();
}
NVMeMi::~NVMeMi()
{
    // closeMCTP();
}

void NVMeMi::Worker::post(std::function<void(void)>&& func)
{
    if (!workerStop)
    {
        std::unique_lock<std::mutex> lock(workerMtx);
        if (!workerStop)
        {
            workerIO.post(std::move(func));
            workerCv.notify_all();
            return;
        }
    }
    throw std::runtime_error("NVMeMi has been stopped");
}

void NVMeMi::post(std::function<void(void)>&& func)
{
    worker->post(
        [self{std::move(shared_from_this())}, func{std::move(func)}]() {
        std::unique_lock<std::mutex> lock(self->mctpMtx);
        func();
    });
}

// Calls .post(), catching runtime_error and returning an error code on failure.
std::error_code NVMeMi::try_post(std::function<void(void)>&& func)
{
    try
    {
        post([self{shared_from_this()}, func{std::move(func)}]() { func(); });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        return std::make_error_code(std::errc::no_such_device);
    }
    return std::error_code();
}

void NVMeMi::miPCIePortInformation(
    std::function<void(const std::error_code&, nvme_mi_read_port_info*)>&& cb)
{
    if (!nvmeEP)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] vme endpoint is invalid", "ADDR",
                   addr, "EID", static_cast<int>(eid));

        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), nullptr);
        });
        return;
    }

    try
    {
        post([self{shared_from_this()}, cb{std::move(cb)}]() {
            nvme_mi_read_nvm_ss_info ss_info;
            auto rc = nvme_mi_mi_read_mi_data_subsys(self->nvmeEP, &ss_info);
            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] mi_read_mi_data_subsys: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));

                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       nullptr);
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] mi_read_mi_data_subsys: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", errMsg);

                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), nullptr);
                });
                return;
            }
            struct nvme_mi_read_port_info port;
            memset(&port, 0, sizeof(port));
            for (auto i = 0; i <= ss_info.nump; i++)
            {
                auto rc = nvme_mi_mi_read_mi_data_port(self->nvmeEP, i, &port);
                if (rc != 0)
                {
                    std::string_view errMsg =
                        statusToString(static_cast<nvme_mi_resp_status>(rc));
                    lg2::error(
                        "[addr:{ADDR}, eid:{EID}] mi_read_mi_data_subsys: {ERR}",
                        "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                        "ERR", errMsg);
                    self->io.post([cb{std::move(cb)}]() {
                        cb(std::make_error_code(std::errc::bad_message),
                           nullptr);
                    });
                    return;
                }
                // only select PCIe port
                if (port.portt == 0x1)
                {
                    break;
                }
            }

            self->io.post([cb{std::move(cb)}, port{std::move(port)}]() mutable {
                cb({}, &port);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}]  {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
}

void NVMeMi::miSubsystemHealthStatusPoll(
    std::function<void(const std::error_code&, nvme_mi_nvm_ss_health_status*)>&&
        cb)
{
    if (!nvmeEP)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] nvme endpoint is invalid ", "ADDR",
                   addr, "EID", static_cast<int>(eid));
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), nullptr);
        });
        return;
    }

    try
    {
        post([self{shared_from_this()}, cb{std::move(cb)}]() {
            nvme_mi_nvm_ss_health_status ss_health;
            auto rc = nvme_mi_mi_subsystem_health_status_poll(self->nvmeEP,
                                                              true, &ss_health);
            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] subsystem_health_status_poll: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));

                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       nullptr);
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));

                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] subsystem_health_status_poll:{MSG}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "MSG", errMsg);
                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), nullptr);
                });
                return;
            }

            self->io.post(
                [cb{std::move(cb)}, ss_health{std::move(ss_health)}]() mutable {
                cb({}, &ss_health);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
}

void NVMeMi::miScanCtrl(std::function<void(const std::error_code&,
                                           const std::vector<nvme_mi_ctrl_t>&)>
                            cb)
{
    if (!nvmeEP)
    {
        lg2::error("nvme endpoint is invalid");

        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }

    try
    {
        post([self{shared_from_this()}, cb{std::move(cb)}]() {
            int rc = nvme_mi_scan_ep(self->nvmeEP, true);
            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to scan controllers:{ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       {});
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to scan controllers: {MSG}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "MSG", errMsg);
                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), {});
                });
                return;
            }

            std::vector<nvme_mi_ctrl_t> list;
            nvme_mi_ctrl_t c;
            nvme_mi_for_each_ctrl(self->nvmeEP, c)
            {
                list.push_back(c);
            }
            self->io.post(
                [cb{std::move(cb)}, list{std::move(list)}]() { cb({}, list); });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
}

void NVMeMi::adminIdentify(
    nvme_mi_ctrl_t ctrl, nvme_identify_cns cns, uint32_t nsid, uint16_t cntid,
    uint16_t read_length,
    std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb)
{
    if (!nvmeEP)
    {
        lg2::error("nvme endpoint is invalid");
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }

    lg2::debug("[eid:{EID}] identify cmd resp length: {RSPLEN}", "EID",
               static_cast<int>(eid), "RSPLEN",
               static_cast<unsigned int>(read_length));

    if ((read_length > 0) && (read_length < NVME_IDENTIFY_DATA_SIZE))
        NVMeMi::adminIdentifyPartial(ctrl, cns, nsid, cntid, read_length,
                                     std::move(cb));
    else
        NVMeMi::adminIdentifyFull(ctrl, cns, nsid, cntid, std::move(cb));
}

void NVMeMi::adminIdentifyFull(
    nvme_mi_ctrl_t ctrl, nvme_identify_cns cns, uint32_t nsid, uint16_t cntid,
    std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb)
{
    try
    {
        post([ctrl, cns, nsid, cntid, self{shared_from_this()},
              cb{std::move(cb)}]() {
            int rc = 0;
            std::vector<uint8_t> data;

            data.resize(NVME_IDENTIFY_DATA_SIZE);
            nvme_identify_args args{};
            memset(&args, 0, sizeof(args));
            args.result = nullptr;
            args.data = data.data();
            args.args_size = sizeof(args);
            args.cns = cns;
            args.csi = NVME_CSI_NVM;
            args.nsid = nsid;
            args.cntid = cntid;
            args.cns_specific_id = NVME_CNSSPECID_NONE;
            args.uuidx = NVME_UUID_NONE,

            rc = nvme_mi_admin_identify(ctrl, &args);

            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to do nvme identify: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       {});
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to do nvme identify: {MSG}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "MSG", errMsg);
                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), {});
                });
                return;
            }

            self->io.post([cb{std::move(cb)}, data{std::move(data)}]() mutable {
                std::span<uint8_t> span{data.data(), data.size()};
                cb({}, span);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
}

void NVMeMi::adminIdentifyPartial(
    nvme_mi_ctrl_t ctrl, nvme_identify_cns cns, uint32_t nsid, uint16_t cntid,
    uint16_t read_length,
    std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb)
{
    try
    {
        post([ctrl, cns, nsid, cntid, read_length, self{shared_from_this()},
              cb{std::move(cb)}]() {
            int rc = 0;
            std::vector<uint8_t> data;
            switch (cns)
            {
                case NVME_IDENTIFY_CNS_SECONDARY_CTRL_LIST:
                    data.resize(sizeof(nvme_secondary_ctrl_list));
                    break;

                default:
                    data.resize(read_length);
            }

            nvme_identify_args args{};
            memset(&args, 0, sizeof(args));
            args.result = nullptr;
            args.data = data.data();
            args.args_size = sizeof(args);
            args.cns = cns;
            args.csi = NVME_CSI_NVM;
            args.nsid = nsid;
            args.cntid = cntid;
            args.cns_specific_id = NVME_CNSSPECID_NONE;
            args.uuidx = NVME_UUID_NONE,

            rc = nvme_mi_admin_identify_partial(ctrl, &args, 0, data.size());

            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to do nvme identify partial: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       {});
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to do nvme identify partial: {MSG}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "MSG", errMsg);
                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), {});
                });
                return;
            }

            self->io.post([cb{std::move(cb)}, data{std::move(data)}]() mutable {
                std::span<uint8_t> span{data.data(), data.size()};
                cb({}, span);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
}

static int nvme_mi_admin_get_log_telemetry_host_rae(nvme_mi_ctrl_t ctrl,
                                                    bool /*rae*/, __u64 offset,
                                                    __u32 len, void* log)
{
    return nvme_mi_admin_get_log_telemetry_host(ctrl, offset, len, log);
}

// Get Temetery Log header and return the size for hdr + data area (Area 1, 2,
// 3, or maybe 4)
int getTelemetryLog(nvme_mi_ctrl_t ctrl, bool host, bool create,
                    std::vector<uint8_t>& data)
{
    int rc = 0;
    data.resize(sizeof(nvme_telemetry_log));
    nvme_telemetry_log& log =
        *reinterpret_cast<nvme_telemetry_log*>(data.data());
    auto func = host ? nvme_mi_admin_get_log_telemetry_host_rae
                     : nvme_mi_admin_get_log_telemetry_ctrl;

    // Only host telemetry log requires create.
    if (host && create)
    {
        rc = nvme_mi_admin_get_log_create_telemetry_host(ctrl, &log);
        if (rc)
        {
            lg2::error("failed to create telemetry host log");
            return rc;
        }
        return 0;
    }

    rc = func(ctrl, false, 0, sizeof(log), &log);

    if (rc)
    {
        auto str = (host ? "host" : "ctrl");
        lg2::error("failed to retain telemetry log for {MSG}", "MSG", str);
        return rc;
    }

    long size =
        static_cast<long>((boost::endian::little_to_native(log.dalb3) + 1)) *
        NVME_LOG_TELEM_BLOCK_SIZE;

    data.resize(size);
    rc = func(ctrl, false, 0, data.size(), data.data());
    if (rc)
    {
        auto str = (host ? "host" : "ctrl");
        lg2::error("failed to get full telemetry log for {MSG}", "MSG", str);
        return rc;
    }
    return 0;
}

void NVMeMi::adminSanitize(
    nvme_mi_ctrl_t ctrl, nvme_sanitize_sanact sanact, uint8_t owpass,
    uint32_t owpattern,
    std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb)
{
    if (!nvmeEP)
    {
        lg2::error("nvme endpoint is invalid");
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
    try
    {
        post([ctrl, sanact, owpass, owpattern, self{shared_from_this()},
              cb{std::move(cb)}]() {
            int rc = 0;
            std::vector<uint8_t> data(8);
            struct nvme_sanitize_nvm_args args;
            memset(&args, 0, sizeof(args));

            args.args_size = sizeof(args);
            args.sanact = sanact;
            args.owpass = owpass;
            args.nodas = 0x1;
            args.ovrpat = owpattern;
            args.result = (uint32_t*)data.data();

            rc = nvme_mi_admin_sanitize_nvm(ctrl, &args);
            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to do nvme sanitize: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       {});
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to do nvme sanitize: {MSG} rc: {RC}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "MSG", errMsg, "RC", std::to_string(rc));
                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), {});
                });
                return;
            }

            self->io.post([cb{std::move(cb)}, data{std::move(data)}]() mutable {
                std::span<uint8_t> span{data.data(), data.size()};
                cb({}, span);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
    return;
}

void NVMeMi::adminGetLogPage(
    nvme_mi_ctrl_t ctrl, nvme_cmd_get_log_lid lid, uint32_t nsid, uint8_t lsp,
    uint16_t lsi,
    std::function<void(const std::error_code&, std::span<uint8_t>)>&& cb)
{
    if (!nvmeEP)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] nvme endpoint is invalid", "ADDR",
                   addr, "EID", static_cast<int>(eid));
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }

    try
    {
        post([ctrl, nsid, lid, lsp, lsi, self{shared_from_this()},
              cb{std::move(cb)}]() {
            std::vector<uint8_t> data;

            int rc = 0;
            switch (lid)
            {
                case NVME_LOG_LID_ERROR:
                {
                    data.resize(nvme_mi_xfer_size);
                    // The number of entries for most recent error logs.
                    // Currently we only do one nvme mi transfer for the
                    // error log to avoid blocking other tasks
                    static constexpr int num = nvme_mi_xfer_size /
                                               sizeof(nvme_error_log_page);
                    nvme_error_log_page* log =
                        reinterpret_cast<nvme_error_log_page*>(data.data());

                    rc = nvme_mi_admin_get_log_error(ctrl, num, false, log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get error log",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_SMART:
                {
                    data.resize(sizeof(nvme_smart_log));
                    nvme_smart_log* log =
                        reinterpret_cast<nvme_smart_log*>(data.data());

                    constexpr int read_len = sizeof(nvme_smart_log) -
                                             sizeof(log->rsvd232);
                    rc = nvme_mi_admin_get_nsid_log(ctrl, false, lid, nsid,
                                                    read_len, log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get smart log",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_FW_SLOT:
                {
                    data.resize(sizeof(nvme_firmware_slot));
                    nvme_firmware_slot* log =
                        reinterpret_cast<nvme_firmware_slot*>(data.data());
                    rc = nvme_mi_admin_get_log_fw_slot(ctrl, false, log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get firmware slot",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_CMD_EFFECTS:
                {
                    data.resize(sizeof(nvme_cmd_effects_log));
                    nvme_cmd_effects_log* log =
                        reinterpret_cast<nvme_cmd_effects_log*>(data.data());

                    // nvme rev 1.3 doesn't support csi,
                    // set to default csi = NVME_CSI_NVM
                    rc = nvme_mi_admin_get_log_cmd_effects(ctrl, NVME_CSI_NVM,
                                                           log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get cmd supported and effects log",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_DEVICE_SELF_TEST:
                {
                    data.resize(sizeof(nvme_self_test_log));
                    nvme_self_test_log* log =
                        reinterpret_cast<nvme_self_test_log*>(data.data());
                    rc = nvme_mi_admin_get_log_device_self_test(ctrl, log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get device self test log",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_CHANGED_NS:
                {
                    data.resize(sizeof(nvme_ns_list));
                    nvme_ns_list* log =
                        reinterpret_cast<nvme_ns_list*>(data.data());
                    rc = nvme_mi_admin_get_log_changed_ns_list(ctrl, false,
                                                               log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get changed namespace list",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_TELEMETRY_HOST:
                // fall through to NVME_LOG_LID_TELEMETRY_CTRL
                case NVME_LOG_LID_TELEMETRY_CTRL:
                {
                    bool host = false;
                    bool create = false;
                    if (lid == NVME_LOG_LID_TELEMETRY_HOST)
                    {
                        host = true;
                        if (lsp == NVME_LOG_TELEM_HOST_LSP_CREATE)
                        {
                            create = true;
                        }
                        else if (lsp == NVME_LOG_TELEM_HOST_LSP_RETAIN)
                        {
                            create = false;
                        }
                        else
                        {
                            lg2::error(
                                "[addr:{ADDR}, eid:{EID}] invalid lsp for telemetry host log",
                                "ADDR", self->addr, "EID",
                                static_cast<int>(self->eid));
                            rc = -1;
                            errno = EINVAL;
                            break;
                        }
                    }
                    else
                    {
                        host = false;
                    }

                    rc = getTelemetryLog(ctrl, host, create, data);
                }
                break;
                case NVME_LOG_LID_RESERVATION:
                {
                    data.resize(sizeof(nvme_resv_notification_log));
                    nvme_resv_notification_log* log =
                        reinterpret_cast<nvme_resv_notification_log*>(
                            data.data());

                    int rc = nvme_mi_admin_get_log_reservation(ctrl, false,
                                                               log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get reservation notification log",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                case NVME_LOG_LID_SANITIZE:
                {
                    data.resize(sizeof(nvme_sanitize_log_page));
                    nvme_sanitize_log_page* log =
                        reinterpret_cast<nvme_sanitize_log_page*>(data.data());

                    int rc = nvme_mi_admin_get_log_sanitize(ctrl, false, log);
                    if (rc)
                    {
                        lg2::error(
                            "[addr:{ADDR}, eid:{EID}] fail to get sanitize status log",
                            "ADDR", self->addr, "EID",
                            static_cast<int>(self->eid));
                        break;
                    }
                }
                break;
                default:
                {
                    lg2::error(
                        "[addr:{ADDR}, eid:{EID}] unknown lid for GetLogPage",
                        "ADDR", self->addr, "EID", static_cast<int>(self->eid));
                    rc = -1;
                    errno = EINVAL;
                }
            }

            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to get log page {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       {});
                });
                return;
            }
            else if (rc > 0)
            {
                std::string_view errMsg =
                    statusToString(static_cast<nvme_mi_resp_status>(rc));

                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to get log page: {MSG}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "MSG", errMsg);
                self->io.post([cb{std::move(cb)}]() {
                    cb(std::make_error_code(std::errc::bad_message), {});
                    return;
                });
            }

            self->io.post([cb{std::move(cb)}, data{std::move(data)}]() mutable {
                std::span<uint8_t> span{data.data(), data.size()};
                cb({}, span);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error(
            "[addr:{ADDR}, eid:{EID}] NVMeMi adminGetLogPage throws: {MSG}",
            "ADDR", addr, "EID", static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {});
        });
        return;
    }
}

void NVMeMi::adminXfer(
    nvme_mi_ctrl_t ctrl, const nvme_mi_admin_req_hdr& admin_req,
    std::span<uint8_t> data, unsigned int timeout_ms,
    std::function<void(const std::error_code&, const nvme_mi_admin_resp_hdr&,
                       std::span<uint8_t>)>&& cb)
{
    if (!nvmeEP)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] nvme endpoint is invalid", "ADDR",
                   addr, "EID", static_cast<int>(eid));
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {}, {});
        });
        return;
    }
    try
    {
        std::vector<uint8_t> req(sizeof(nvme_mi_admin_req_hdr) + data.size());
        memcpy(req.data(), &admin_req, sizeof(nvme_mi_admin_req_hdr));
        memcpy(req.data() + sizeof(nvme_mi_admin_req_hdr), data.data(),
               data.size());
        post([ctrl, req{std::move(req)}, self{shared_from_this()}, timeout_ms,
              cb{std::move(cb)}]() mutable {
            int rc = 0;

            nvme_mi_admin_req_hdr* reqHeader =
                reinterpret_cast<nvme_mi_admin_req_hdr*>(req.data());

            size_t respDataSize =
                boost::endian::little_to_native<size_t>(reqHeader->dlen);
            off_t respDataOffset =
                boost::endian::little_to_native<off_t>(reqHeader->doff);
            size_t bufSize = sizeof(nvme_mi_admin_resp_hdr) + respDataSize;
            std::vector<uint8_t> buf(bufSize);
            nvme_mi_admin_resp_hdr* respHeader =
                reinterpret_cast<nvme_mi_admin_resp_hdr*>(buf.data());

            // set timeout
            unsigned timeout = nvme_mi_ep_get_timeout(self->nvmeEP);
            nvme_mi_ep_set_timeout(self->nvmeEP, timeout_ms);

            rc = nvme_mi_admin_xfer(ctrl, reqHeader,
                                    req.size() - sizeof(nvme_mi_admin_req_hdr),
                                    respHeader, respDataOffset, &respDataSize);
            // revert to previous timeout
            nvme_mi_ep_set_timeout(self->nvmeEP, timeout);

            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] failed to nvme_mi_admin_xfer",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       {}, {});
                });
                return;
            }
            // the MI interface will only consume protocol/io errors
            // The client will take the reponsibility to deal with nvme-mi
            // status flag and nvme status field(cwd3). cmd specific return
            // value (cdw0) is also client's job.

            buf.resize(sizeof(nvme_mi_admin_resp_hdr) + respDataSize);
            self->io.post([cb{std::move(cb)}, data{std::move(buf)}]() mutable {
                std::span<uint8_t> span(
                    data.begin() + sizeof(nvme_mi_admin_resp_hdr), data.end());
                cb({}, *reinterpret_cast<nvme_mi_admin_resp_hdr*>(data.data()),
                   span);
            });
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device), {}, {});
        });
        return;
    }
}

void NVMeMi::adminFwCommit(
    nvme_mi_ctrl_t ctrl, nvme_fw_commit_ca action, uint8_t slot, bool bpid,
    std::function<void(const std::error_code&, nvme_status_field)>&& cb)
{
    if (!nvmeEP)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] nvme endpoint is invalid", "ADDR",
                   addr, "EID", static_cast<int>(eid));
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device),
               nvme_status_field::NVME_SC_MASK);
        });
        return;
    }
    try
    {
        nvme_fw_commit_args args;
        memset(&args, 0, sizeof(args));
        args.args_size = sizeof(args);
        args.action = action;
        args.slot = slot;
        args.bpid = bpid;
        io.post([ctrl, args, cb{std::move(cb)},
                 self{shared_from_this()}]() mutable {
            int rc = nvme_mi_admin_fw_commit(ctrl, &args);
            if (rc < 0)
            {
                lg2::error(
                    "[addr:{ADDR}, eid:{EID}] fail to nvme_mi_admin_fw_commit: {ERR}",
                    "ADDR", self->addr, "EID", static_cast<int>(self->eid),
                    "ERR", std::strerror(errno));
                self->io.post([cb{std::move(cb)}, last_errno{errno}]() {
                    cb(std::make_error_code(static_cast<std::errc>(last_errno)),
                       nvme_status_field::NVME_SC_MASK);
                });
                return;
            }
            else if (rc >= 0)
            {
                switch (rc & 0x7ff)
                {
                    case NVME_SC_SUCCESS:
                    case NVME_SC_FW_NEEDS_CONV_RESET:
                    case NVME_SC_FW_NEEDS_SUBSYS_RESET:
                    case NVME_SC_FW_NEEDS_RESET:
                        self->io.post([rc, cb{std::move(cb)}]() {
                            cb({}, static_cast<nvme_status_field>(rc));
                        });
                        break;
                    default:
                        std::string_view errMsg = statusToString(
                            static_cast<nvme_mi_resp_status>(rc));
                        lg2::error("fail to nvme_mi_admin_fw_commit: {MSG} ",
                                   "MSG", errMsg);
                        self->io.post([rc, cb{std::move(cb)}]() {
                            cb(std::make_error_code(std::errc::bad_message),
                               static_cast<nvme_status_field>(rc));
                        });
                }
                return;
            }
        });
    }
    catch (const std::runtime_error& e)
    {
        lg2::error("[addr:{ADDR}, eid:{EID}] {MSG}", "ADDR", addr, "EID",
                   static_cast<int>(eid), "MSG", e.what());
        io.post([cb{std::move(cb)}]() {
            cb(std::make_error_code(std::errc::no_such_device),
               nvme_status_field::NVME_SC_MASK);
        });
        return;
    }
}

void NVMeMi::adminSecuritySend(
    nvme_mi_ctrl_t ctrl, uint8_t proto, uint16_t proto_specific,
    std::span<uint8_t> data,
    std::function<void(const std::error_code&, int nvme_status)>&& cb)
{
    std::error_code post_err =
        try_post([self{shared_from_this()}, ctrl, proto, proto_specific, data,
                  cb{std::move(cb)}]() {
        struct nvme_security_send_args args;
        memset(&args, 0x0, sizeof(args));
        args.secp = proto;
        args.spsp0 = proto_specific & 0xff;
        args.spsp1 = proto_specific >> 8;
        args.nssf = 0;
        args.data = data.data();
        args.data_len = data.size_bytes();
        args.args_size = sizeof(struct nvme_security_send_args);

        int status = nvme_mi_admin_security_send(ctrl, &args);
        self->io.post([cb{std::move(cb)}, nvme_errno{errno}, status]() {
            auto err = std::make_error_code(static_cast<std::errc>(nvme_errno));
            cb(err, status);
        });
    });
    if (post_err)
    {
        lg2::error(
            "[addr:{ADDR}, eid:{EID}] adminSecuritySend post failed: {MSG}",
            "ADDR", addr, "EID", static_cast<int>(eid), "MSG",
            post_err.message());
        io.post([cb{std::move(cb)}, post_err]() { cb(post_err, -1); });
    }
}

void NVMeMi::adminSecurityReceive(
    nvme_mi_ctrl_t ctrl, uint8_t proto, uint16_t proto_specific,
    uint32_t transfer_length,
    std::function<void(const std::error_code&, int nvme_status,
                       std::span<uint8_t> data)>&& cb)
{
    if (transfer_length > maxNVMeMILength)
    {
        cb(std::make_error_code(std::errc::invalid_argument), -1, {});
        return;
    }

    std::error_code post_err =
        try_post([self{shared_from_this()}, ctrl, proto, proto_specific,
                  transfer_length, cb{std::move(cb)}]() {
        std::vector<uint8_t> data(transfer_length);

        struct nvme_security_receive_args args;
        memset(&args, 0x0, sizeof(args));
        args.secp = proto;
        args.spsp0 = proto_specific & 0xff;
        args.spsp1 = proto_specific >> 8;
        args.nssf = 0;
        args.data = data.data();
        args.data_len = data.size();
        args.args_size = sizeof(struct nvme_security_receive_args);

        int status = nvme_mi_admin_security_recv(ctrl, &args);
        if (args.data_len > maxNVMeMILength)
        {
            lg2::error(
                "[addr:{ADDR}, eid:{EID}] nvme_mi_admin_security_send returned excess data, {LEN}",
                "ADDR", self->addr, "EID", static_cast<int>(self->eid), "LEN",
                args.data_len);
            self->io.post([cb]() {
                cb(std::make_error_code(std::errc::protocol_error), -1, {});
            });
            return;
        }

        data.resize(args.data_len);
        self->io.post(
            [cb{std::move(cb)}, nvme_errno{errno}, status, data]() mutable {
            std::span<uint8_t> span{data.data(), data.size()};
            auto err = std::make_error_code(static_cast<std::errc>(nvme_errno));
            cb(err, status, span);
        });
    });
    if (post_err)
    {
        lg2::error(
            "[addr:{ADDR}, eid:{EID}] adminSecuritySend post failed: {MSG}",
            "ADDR", addr, "EID", static_cast<int>(eid), "MSG",
            post_err.message());
        io.post([cb{std::move(cb)}, post_err]() { cb(post_err, -1, {}); });
    }
}
