#include <NVMeDevice.hpp>
#include <nvme-mi_config.h>

#include <boost/multiprecision/cpp_int.hpp>

#include <iostream>
#include <filesystem>

template <typename T>
T *toResponse(std::span<uint8_t> &data) 
{
    std::vector<uint8_t> tmp;

    for (auto d : data) {
        tmp.push_back(d);
    }
    T *resp = reinterpret_cast<T *>(tmp.data());
    return resp;
}

NVMeDevice::NVMeDevice(
    boost::asio::io_service& io,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    uint8_t eid, std::vector<uint8_t> addr, std::string path) :
    NvmeInterfaces(
        static_cast<sdbusplus::bus::bus&>(*conn),
        path.c_str(),
        NvmeInterfaces::action::defer_emit),std::enable_shared_from_this<NVMeDevice>(),
    objServer(objectServer), scanTimer(io)
{
    std::filesystem::path p(path);
    

    AssociationList assocs = {};

    assocs.emplace_back("chassis", "drive", driveLocation);
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);

    nvmeIntf = NVMeIntf::create<NVMeMi>(io, conn, addr, eid);
    intf = std::get<std::shared_ptr<NVMeMiIntf>>(nvmeIntf.getInferface());

}
    
std::string NVMeDevice::stripString(char *src, size_t len)
{
    std::string s;

    s.assign(src, src+len);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

std::string NVMeDevice::getManufacture(uint16_t vid)
{

    if (vid == 0x144d)
    {
        return std::string("Samsung");
    }
    else if (vid == 0x1344)
    {
        return std::string("Mircon");
    }
    else if (vid == 0x1e0f)
    {
        return std::string("Kioxia");
    }

    return "Unkown";
}

void NVMeDevice::initialize()
{
    presence = 0;

    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive::type(
        sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive::
            DriveType::SSD);
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive::protocol(
        sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive::
            DriveProtocol::NVMe);

    intf->miScanCtrl([self{shared_from_this()}](
                         const std::error_code& ec,
                         const std::vector<nvme_mi_ctrl_t>& ctrlList) mutable {
        if (ec || ctrlList.size() == 0)
        {
            lg2::error("fail to scan controllers for the nvme subsystem {ERR}: {MSG}", "ERR", ec.value(), "MSG", ec.message());
            self->presence = false;
            self->sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(false);
            return;
        }
        self->presence = true;
        self->sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(true);

        self->ctrl = ctrlList.back();
        self->getIntf()->adminIdentify(
            self->ctrl, nvme_identify_cns::NVME_IDENTIFY_CNS_CTRL,
            NVME_NSID_NONE, 0,
            [self{self->shared_from_this()}](const std::error_code &ec,
                                             std::span<uint8_t> data) {
              if (ec) {
                lg2::error("fail to do Identify command");
                return;
              }

              auto id = toResponse<nvme_id_ctrl>(data);

              self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                  server::Asset::manufacturer(self->getManufacture(id->vid));
              self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                  server::Asset::serialNumber(
                      self->stripString(id->sn, sizeof(id->sn)));

              self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                  server::Asset::model(
                      self->stripString(id->mn, sizeof(id->mn)));

              uint64_t drive_capacity[2];
              memcpy(&drive_capacity, id->tnvmcap, 16);

              /* 8 bytes presenting the drive capacity is enough to support all
               * drives outside market.
               */
              self->sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                  Drive::capacity(drive_capacity[0]);
            });
    });
}

void NVMeDevice::markFunctional(bool functional)
{
    driveFunctional = functional;
    // mark device state
    if (!functional)
    {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::OperationalStatus::functional(false);
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::Fault);
    }
    else {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::OperationalStatus::functional(true);
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::None);
    }
}

void NVMeDevice::pollDrive()
{

    scanTimer.expires_from_now(std::chrono::seconds(5));
    scanTimer.async_wait([self{shared_from_this()}](
                             const boost::system::error_code errorCode) {
        if (errorCode == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        else if (errorCode)
        {
            lg2::error("Error: {MSG}\n", "MSG", errorCode.message());
            return;
        }
        // try to re-initialize the drive
        if (self->presence == false) {
            self->initialize();
            self->pollDrive();
            return;
        }

        auto miIntf = self->getIntf();
        miIntf->miSubsystemHealthStatusPoll(
            [self](__attribute__((unused)) const std::error_code &err,
                   nvme_mi_nvm_ss_health_status *ss) {
              if (err) {
                lg2::error("fail to query SubSystemHealthPoll for the nvme "
                           "subsystem {ERR}:{MSG}",
                           "ERR", err.value(), "MSG", err.message());
                return;
              }
              self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                  driveLifeUsed(std::to_string(ss->pdlu));
              self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                  smartWarnings(std::to_string(ss->sw));

              // the percentage is allowed to exceed 100 based on the spec.
              auto percentage = (ss->pdlu > 100) ? 100 : ss->pdlu;
              self->sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                  Drive::predictedMediaLifeLeftPercent(100 - percentage);

              // drive failure
              if (ss->nss & 0x20) {
                self->markFunctional(false);
              }

              lg2::error(" NVM subsystem status : {VAL}", "VAL", ss->nss);
              lg2::error(" NVM composite temp. : {VAL}", "VAL", ss->ctemp);
            });

        miIntf->adminGetLogPage(
            self->ctrl, NVME_LOG_LID_SMART, 0xFFFFFFFF, 0, 0,
            [self](const std::error_code &ec, std::span<uint8_t> smart) {
              if (ec) {

                lg2::error("fail to query SMART for the nvme subsystem {ERR}:{MSG}", "ERR", ec.value(), "MSG", ec.message());
                return;
              }

              auto log = toResponse<nvme_smart_log>(smart);

              // the error indicator is from smart warning
              if (log->critical_warning & 0x10) {
                self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                    backupDeviceFault(true);
              }
              if (log->critical_warning & 0x01) {
                self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                    capacityFault(true);
              }
              if (log->critical_warning & 0x02) {
                self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                    temperatureFault(true);
              }
              if (log->critical_warning & 0x04) {
                self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                    degradesFault(true);
              }
              if (log->critical_warning & 0x08) {
                self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::
                    mediaFault(true);
              }
              boost::multiprecision::uint128_t powerOnHours;
              memcpy((void *)&powerOnHours,log->power_on_hours,16);
            });

        self->pollDrive();
    });
}

NVMeDevice::~NVMeDevice()
{
    std::cout <<"~NVMEDevice"<<std::endl;
}
