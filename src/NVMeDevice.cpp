#include <NVMeDevice.hpp>
#include <nvme-mi_config.h>
#include <dbusutil.hpp>

#include <boost/multiprecision/cpp_int.hpp>

#include <iostream>
#include <filesystem>

const std::string driveFailureResolution{
    "Ensure all cables are properly and securely connected. Ensure all drives are fully seated. Replace the defective cables, drive, or both."};
const std::string drivePfaResolution{
    "If this drive is not part of a fault-tolerant volume, first back up all data, then replace the drive and restore all data afterward. If this drive is part of a fault-tolerant volume, replace this drive as soon as possible as long as the health is OK"};

const std::string redfishDrivePathPrefix{"/redfish/v1/Systems/System_0/Storage/1/Drives/"};
const std::string redfishDriveName{"NVMe Drive"};

using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

NVMeDevice::NVMeDevice(boost::asio::io_service& io,
                       sdbusplus::asio::object_server& objectServer,
                       std::shared_ptr<sdbusplus::asio::connection>& conn,
                       uint8_t eid, std::vector<uint8_t> addr,
                       std::string path) :
    NvmeInterfaces(static_cast<sdbusplus::bus::bus&>(*conn), path.c_str(),
                   NvmeInterfaces::action::defer_emit),
    std::enable_shared_from_this<NVMeDevice>(), conn(conn),
    objServer(objectServer), scanTimer(io), driveFunctional(false),
    smartWarning(0xff), objPath(path)
{
    std::filesystem::path p(path);

    driveIndex = p.filename();

    //assume the drive is good and update Dbus properties at the first place.
    markFunctional(true);

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

inline uint32_t getMaxLinkSpeed(uint8_t speed_vec, uint8_t lanes)
{
    // starting from 32 Gbs
    int base = 32;

    for (auto i = 4 ; i >= 0; i--)
    {
        if (speed_vec & ( 1 << i))
        {
            break;
        }
        base = base / 2;
    }

    return base * lanes;
}

inline uint32_t getCurrLinkSpeed(uint8_t speed, uint8_t lanes)
{
    uint32_t base = 32;
    if (speed == 0)
    {
        // link not active
        return 0;
    }

    for (auto i = 4 ; i >= 0; i--)
    {
        if ((speed - 1) == i)
        {
            break;
        }
        base = base / 2;
    }
    return base * lanes;
}

void NVMeDevice::initialize()
{
    presence = 0;

    Drive::type(Drive::DriveType::SSD);
    Drive::protocol(Drive::DriveProtocol::NVMe);

    intf->miScanCtrl([self{shared_from_this()}](
                         const std::error_code& ec,
                         const std::vector<nvme_mi_ctrl_t>& ctrlList) mutable {
        if (ec || ctrlList.size() == 0)
        {
            lg2::error("fail to scan controllers for the nvme subsystem {ERR}: {MSG}", "ERR", ec.value(), "MSG", ec.message());
            self->presence = false;
            self->Item::present(false);
            return;
        }
        self->presence = true;
        self->Item::present(true);

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

              struct nvme_id_ctrl* id = (struct nvme_id_ctrl*)data.data();

              self->Asset::manufacturer(self->getManufacture(id->vid));
              self->Asset::serialNumber(
                  self->stripString(id->sn, sizeof(id->sn)));
              self->Asset::model(self->stripString(id->mn, sizeof(id->mn)));

              std::string fr;
              fr.assign(id->fr, id->fr + 8);
              self->Version::version(fr);

              uint64_t drive_capacity[2];
              memcpy(&drive_capacity, id->tnvmcap, 16);

              /* 8 bytes presenting the drive capacity is enough to support all
               * drives outside market.
               */
              self->Drive::capacity(drive_capacity[0]);
            });
    });
    intf->miPCIePortInformation(
        [self{shared_from_this()}](__attribute__((unused))
                                   const std::error_code& err,
                                   nvme_mi_read_port_info* port) {
            if (err)
            {
                lg2::error("fail to get PCIePortInformation");
                return;
            }
            self->Port::maxSpeed(
                getMaxLinkSpeed(port->pcie.sls, port->pcie.mlw));
            self->Port::currentSpeed(
                getCurrLinkSpeed(port->pcie.cls, port->pcie.nlw));
        });
}

void NVMeDevice::markStatus(std::string status)
{
    assocs = {};

    if (status == "critical")
    {
        assocs.emplace_back("health", status, objPath.c_str());
        Health::health(Health::HealthType::Critical);
    }
    else if (status == "warning")
    {
        assocs.emplace_back("health", status, objPath.c_str());
        Health::health(Health::HealthType::Warning);
    }
    else
    {
        Health::health(Health::HealthType::OK);
    }
    assocs.emplace_back("chassis", "drive", driveLocation);
    Associations::associations(assocs);
}

void NVMeDevice::markFunctional(bool functional)
{
    if (driveFunctional != functional)
    {
        // mark device state
        if (functional == false)
        {
            OperationalStatus::functional(false);
            OperationalStatus::state(OperationalStatus::StateType::Fault);
            markStatus("critical");

            createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                           Level::Critical, redfishDriveName + driveIndex,
                           "Drive Failure", driveFailureResolution,
                           redfishDrivePathPrefix + driveIndex);
        }
        else
        {
            OperationalStatus::functional(true);
            OperationalStatus::state(OperationalStatus::StateType::None);
            markStatus("ok");
        }
    }
    driveFunctional = functional;
}

void NVMeDevice::generateRedfishEventbySmart(uint8_t sw)
{
    if (sw & NVME_SMART_CRIT_PMR_RO)
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            redfishDriveName + driveIndex,
            "Persistent Memory Region has become read-only or unreliable",
            drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & NVME_SMART_CRIT_VOLATILE_MEMORY)
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, redfishDriveName + driveIndex,
                       "volatile memory backup device has failed",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & NVME_SMART_CRIT_SPARE)
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            redfishDriveName + driveIndex,
            "available spare capacity has fallen below the threshold",
            drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & NVME_SMART_CRIT_DEGRADED)
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, redfishDriveName + driveIndex,
                       "NVM subsystem reliability has been degraded",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & NVME_SMART_CRIT_MEDIA)
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, redfishDriveName + driveIndex,
                       "all of the media has been placed in read only mode",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & NVME_SMART_CRIT_TEMPERATURE)
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            redfishDriveName + driveIndex,
            "temperature is over or under the threshold",
            "Check the condition of the resource listed in OriginOfCondition",
            redfishDrivePathPrefix + driveIndex);
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
              self->NvMeStatus::driveLifeUsed(std::to_string(ss->pdlu));

              // the percentage is allowed to exceed 100 based on the spec.
              auto percentage = (ss->pdlu > 100) ? 100 : ss->pdlu;
              self->sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                  Drive::predictedMediaLifeLeftPercent(100 - percentage);

              self->markFunctional(ss->nss & 0x20);

              lg2::error(" NVM composite temp. : {VAL}", "VAL", ss->ctemp);
            });

        miIntf->adminGetLogPage(
            self->ctrl, NVME_LOG_LID_SMART, 0xFFFFFFFF, 0, 0,
            [self](const std::error_code &ec, std::span<uint8_t> smart) {
              if (ec) {

                lg2::error("fail to query SMART for the nvme subsystem {ERR}:{MSG}", "ERR", ec.value(), "MSG", ec.message());
                return;
              }

              struct nvme_smart_log *log;
              log = (struct nvme_smart_log *) smart.data();

              auto sw = self->getSmartWarning();
              if (log->critical_warning != sw)
              {
                  // the error indicator is from smart warning
                  self->NvMeStatus::backupDeviceFault(
                      log->critical_warning & NVME_SMART_CRIT_VOLATILE_MEMORY);
                  self->NvMeStatus::capacityFault(log->critical_warning &
                                                  NVME_SMART_CRIT_SPARE);
                  self->NvMeStatus::temperatureFault(
                      log->critical_warning & NVME_SMART_CRIT_TEMPERATURE);
                  self->NvMeStatus::degradesFault(log->critical_warning &
                                                  NVME_SMART_CRIT_DEGRADED);
                  self->NvMeStatus::mediaFault(log->critical_warning &
                                               NVME_SMART_CRIT_MEDIA);
                  self->NvMeStatus::smartWarnings(
                      std::to_string(log->critical_warning));

                  self->markStatus("warning");
                  self->generateRedfishEventbySmart(log->critical_warning);
              }
              self->updateSmartWarning(log->critical_warning);
              boost::multiprecision::uint128_t powerOnHours;
              memcpy((void *)&powerOnHours, log->power_on_hours, 16);
            });

        self->pollDrive();
    });
}

NVMeDevice::~NVMeDevice()
{
    std::cout <<"~NVMEDevice"<<std::endl;
}
