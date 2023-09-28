#include <NVMeDevice.hpp>
#include <nvme-mi_config.h>
#include <dbusutil.hpp>

#include <nlohmann/json.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>

const std::string driveFailureResolution{
    "Ensure all cables are properly and securely connected. Ensure all drives \
     are fully seated. Replace the defective cables, drive, or both."};
const std::string drivePfaResolution{
    "If this drive is not part of a fault-tolerant volume, first back up all \
     data, then replace the drive and restore all data afterward. If this \
     drive is part of a fault-tolerant volume, replace this drive as soon as \
     possible as long as the health is OK"};

const std::string redfishDrivePathPrefix{
    "/redfish/v1/Systems/System_0/Storage/1/Drives/"};
const std::string redfishDriveName{"NVMe Drive"};

const std::string driveConfig{"/usr/share/nvidia-nvme-manager/drive.json"};

const std::uint8_t pollInterval = 5;
using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

using Json = nlohmann::json;

NVMeDevice::NVMeDevice(boost::asio::io_service& io,
                       sdbusplus::asio::object_server& objectServer,
                       std::shared_ptr<sdbusplus::asio::connection>& conn,
                       uint8_t eid, std::vector<uint8_t> addr,
                       std::string path) :
    NvmeInterfaces(static_cast<sdbusplus::bus::bus&>(*conn), path.c_str(),
                   NvmeInterfaces::action::defer_emit),
    std::enable_shared_from_this<NVMeDevice>(), conn(conn),
    objServer(objectServer), scanTimer(io), driveFunctional(false),
    smartWarning(0xff), inProgress(false), objPath(path), eid(eid)
{
    std::filesystem::path p(path);

    driveIndex = p.filename();

    //assume the drive is good and update Dbus properties at the first place.
    markFunctional(true);

    nvmeIntf = NVMeIntf::create<NVMeMi>(io, conn, addr, eid);
    intf = std::get<std::shared_ptr<NVMeMiIntf>>(nvmeIntf.getInferface());

}

inline Drive::DriveFormFactor getDriveFormFactor(std::string form)
{
    if (form == "Drive3_5")
    {
        return Drive::DriveFormFactor::Drive3_5;
    }
    else if (form == "Drive2_5")
    {
        return Drive::DriveFormFactor::Drive2_5;
    }
    else if (form == "EDSFF_1U_Long")
    {
        return Drive::DriveFormFactor::EDSFF_1U_Long;
    }
    else if (form == "EDSFF_1U_Short")
    {
        return Drive::DriveFormFactor::EDSFF_1U_Short;
    }
    else if (form == "EDSFF_E3_Short")
    {
        return Drive::DriveFormFactor::EDSFF_E3_Short;
    }
    else if (form == "EDSFF_E3_Long")
    {
        return Drive::DriveFormFactor::EDSFF_E3_Long;
    }
    else if (form == "M2_2230")
    {
        return Drive::DriveFormFactor::M2_2230;
    }
    else if (form == "M2_2242")
    {
        return Drive::DriveFormFactor::M2_2242;
    }
    else if (form == "M2_2260")
    {
        return Drive::DriveFormFactor::M2_2260;
    }
    else if (form == "M2_2280")
    {
        return Drive::DriveFormFactor::M2_2280;
    }
    else if (form == "M2_22110")
    {
        return Drive::DriveFormFactor::M2_22110;
    }
    else if (form == "U2")
    {
        return Drive::DriveFormFactor::U2;
    }
    else if (form == "PCIeSlotFullLength")
    {
        return Drive::DriveFormFactor::PCIeSlotFullLength;
    }
    else if (form == "PCIeSlotLowProfile")
    {
        return Drive::DriveFormFactor::PCIeSlotLowProfile;
    }
    else if (form == "PCIeHalfLength")
    {
        return Drive::DriveFormFactor::PCIeHalfLength;
    }
    else if (form == "OEM")
    {
        return Drive::DriveFormFactor::OEM;
    }
    return Drive::DriveFormFactor::U2;
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

    Drive::type(DriveType::SSD);
    Drive::protocol(DriveProtocol::NVMe);

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

              self->Asset::manufacturer(self->getManufacture(id->vid), false);
              self->Asset::serialNumber(
                  self->stripString(id->sn, sizeof(id->sn)), false);
              self->Asset::model(self->stripString(id->mn, sizeof(id->mn)),
                                 false);

              std::string fr;
              fr.assign(id->fr, id->fr + 8);
              self->Version::version(fr, false);

              uint64_t drive_capacity[2];
              memcpy(&drive_capacity, id->tnvmcap, 16);

              /* 8 bytes presenting the drive capacity is enough to support all
               * drives outside market.
               */
              self->Drive::capacity(drive_capacity[0], false);

              // check the drive sanitize capability
              std::vector<EraseMethod> saniCap;
              if (id->sanicap & (NVME_CTRL_SANICAP_OWS))
              {
                  saniCap.push_back(EraseMethod::Overwrite);
              }
              if (id->sanicap & (NVME_CTRL_SANICAP_BES))
              {
                  saniCap.push_back(EraseMethod::BlockErase);
              }
              if (id->sanicap & (NVME_CTRL_SANICAP_CES))
              {
                  saniCap.push_back(EraseMethod::CryptoErase);
              }
              self->SecureErase::sanitizeCapability(saniCap, false);
              self->setNodmmas(id->sanicap);
            });
            // nvme_mi_ctrl is needed inside pollDrive
            self->pollDrive();
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
                getMaxLinkSpeed(port->pcie.sls, port->pcie.mlw), false);
            self->Port::currentSpeed(
                getCurrLinkSpeed(port->pcie.cls, port->pcie.nlw), false);
        });

    // get drive's location and form factor from Json file.
    std::ifstream jsonFile(driveConfig);
    auto data = Json::parse(jsonFile, nullptr, false);
    try
    {
        auto drives = data["drive"];
        for(auto &d : drives) {
            auto driveEid = d["eid"].get<std::uint8_t>();
            if (eid == driveEid)
            {
                auto loc = d["location"].get<std::string>();
                Location::locationCode(loc, false);
                auto formFactor = getDriveFormFactor(d["form_factor"].get<std::string>());
                Drive::formFactor(formFactor, false);
            }
        }
    }
    catch (const std::exception& e)
    {
        // drive json only populate location and form factor
        // continue with other properties if json parsing failed
        lg2::error("failed to parse drive json file.");
    }
}

void NVMeDevice::markStatus(std::string status)
{
    assocs = {};

    if (status == "critical")
    {
        assocs.emplace_back("health", status, objPath.c_str());
        Health::health(HealthType::Critical, false);
    }
    else if (status == "warning")
    {
        assocs.emplace_back("health", status, objPath.c_str());
        Health::health(HealthType::Warning, false);
    }
    else
    {
        Health::health(HealthType::OK);
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
            OperationalStatus::functional(false, false);
            OperationalStatus::state(OperationalStatus::StateType::Fault, false);
            markStatus("critical");

            createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                           Level::Critical, redfishDriveName + driveIndex,
                           "Drive Failure", driveFailureResolution,
                           redfishDrivePathPrefix + driveIndex);
        }
        else
        {
            OperationalStatus::functional(true, false);
            OperationalStatus::state(OperationalStatus::StateType::None, false);
            markStatus("ok");
        }
    }
    driveFunctional = functional;
}

void NVMeDevice::generateRedfishEventbySmart(uint8_t sw)
{
    if (sw & (NVME_SMART_CRIT_PMR_RO))
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            redfishDriveName + driveIndex,
            "Persistent Memory Region has become read-only or unreliable",
            drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & (NVME_SMART_CRIT_VOLATILE_MEMORY))
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, redfishDriveName + driveIndex,
                       "volatile memory backup device has failed",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & (NVME_SMART_CRIT_SPARE))
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            redfishDriveName + driveIndex,
            "available spare capacity has fallen below the threshold",
            drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & (NVME_SMART_CRIT_DEGRADED))
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, redfishDriveName + driveIndex,
                       "NVM subsystem reliability has been degraded",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & (NVME_SMART_CRIT_MEDIA))
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, redfishDriveName + driveIndex,
                       "all of the media has been placed in read only mode",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if (sw & (NVME_SMART_CRIT_TEMPERATURE))
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            redfishDriveName + driveIndex,
            "temperature is over or under the threshold",
            "Check the condition of the resource listed in OriginOfCondition",
            redfishDrivePathPrefix + driveIndex);
    }
}

void NVMeDevice::updatePercent(uint16_t endTime)
{
    auto time = getEstimateTime() + pollInterval;
    auto percent = (time * 100) / endTime;

    lg2::info("percent: {NUM} - {ECLTIME} / {MAXTIME}\n", "NUM", percent,
              "ECLTIME", time, "MAXTIME", endTime);
    // the actual time greater than the estimated time so
    // fine tune percent
    if (time >= endTime)
    {
        percent = 99;
    }
    Progress::progress(percent);
    setEstimateTime(time);
}

void NVMeDevice::pollDrive()
{
    scanTimer.expires_from_now(std::chrono::seconds(pollInterval));
    scanTimer.async_wait([self{shared_from_this()}](
                             const boost::system::error_code errorCode) {
        if (errorCode == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        else if (errorCode)
        {
            lg2::error("Error: {MSG}", "MSG", errorCode.message());
            return;
        }
        // try to re-initialize the drive
        if (self->presence == false) {
            self->initialize();
            return;
        }

        auto miIntf = self->getIntf();
        if (self->Operation::operation() == OperationType::Sanitize)
        {
            miIntf->adminGetLogPage(
                self->ctrl, NVME_LOG_LID_SANITIZE, 0, 0, 0,
                [self](const std::error_code& ec, std::span<uint8_t> status) {
                    if (ec)
                    {
                        lg2::error(
                            "fail to query satinize status for the nvme subsystem {ERR}:{MSG}",
                            "ERR", ec.value(), "MSG", ec.message());
                        return;
                    }

                    struct nvme_sanitize_log_page* log =
                        (struct nvme_sanitize_log_page*)status.data();

                    uint8_t res = log->sstat & NVME_SANITIZE_SSTAT_STATUS_MASK;
                    if (res == NVME_SANITIZE_SSTAT_STATUS_COMPLETE_SUCCESS ||
                        res == NVME_SANITIZE_SSTAT_STATUS_ND_COMPLETE_SUCCESS)
                    {
                        self->Progress::status(OperationStatus::Completed);
                        self->Progress::progress(100);
                        self->inProgress = false;
                    }
                    else if (res == NVME_SANITIZE_SSTAT_STATUS_COMPLETED_FAILED)
                    {
                        self->Progress::status(OperationStatus::Failed);
                        self->Progress::progress(0);
                        self->inProgress = false;
                    }
                    if (res != NVME_SANITIZE_SSTAT_STATUS_IN_PROGESS)
                    {
                        // sanitize is done no matter that the result it success
                        // or fail
                        self->pollDrive();
                        return;
                    }

                    auto type = self->getEraseType();
                    auto noDeAlloc = self->getNodmmas();
                    uint16_t time = 0;
                    if (type == EraseMethod::CryptoErase)
                    {
                        if (noDeAlloc)
                        {
                            time = log->etcend;
                        }
                        else
                        {
                            time = log->etce;
                        }
                    }
                    else if (type == EraseMethod::BlockErase)
                    {
                        if (noDeAlloc)
                        {
                            time = log->etbend;
                        }
                        else
                        {
                            time = log->etbe;
                        }
                    }
                    else if (type == EraseMethod::Overwrite)
                    {
                        if (noDeAlloc)
                        {
                            time = log->etond;
                        }
                        else
                        {
                            time = log->eto;
                        }
                    }
                    self->updatePercent(time);
                });
            // not do health polling during the sanitize process.
            self->pollDrive();
            return;
        }

        miIntf->miSubsystemHealthStatusPoll(
            [self](__attribute__((unused)) const std::error_code &err,
                   nvme_mi_nvm_ss_health_status *ss) {
              if (err) {
                lg2::error("fail to query SubSystemHealthPoll for the nvme "
                           "subsystem {ERR}:{MSG}",
                           "ERR", err.value(), "MSG", err.message());
                return;
              }
              self->NVMeStatus::driveLifeUsed(std::to_string(ss->pdlu), false);

              // the percentage is allowed to exceed 100 based on the spec.
              auto percentage = (ss->pdlu > 100) ? 100 : ss->pdlu;
              self->sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                  Drive::predictedMediaLifeLeftPercent(100 - percentage, false);

              self->markFunctional(ss->nss & 0x20);

            });

        miIntf->adminGetLogPage(
            self->ctrl, NVME_LOG_LID_SMART, 0xFFFFFFFF, 0, 0,
            [self](const std::error_code &ec, std::span<uint8_t> smart) {
              if (ec) {
                  lg2::error(
                      "fail to query SMART for the nvme subsystem {ERR}:{MSG}",
                      "ERR", ec.value(), "MSG", ec.message());
                  return;
              }

              struct nvme_smart_log *log;
              log = (struct nvme_smart_log *) smart.data();

              auto sw = self->getSmartWarning();
              if (log->critical_warning != sw)
              {
                  // the error indicator is from smart warning
                  self->NVMeStatus::backupDeviceFault(
                      log->critical_warning & (NVME_SMART_CRIT_VOLATILE_MEMORY),
                      false);
                  self->NVMeStatus::capacityFault(
                      log->critical_warning & (NVME_SMART_CRIT_SPARE), false);
                  self->NVMeStatus::temperatureFault(
                      log->critical_warning & (NVME_SMART_CRIT_TEMPERATURE),
                      false);
                  self->NVMeStatus::degradesFault(
                      log->critical_warning & (NVME_SMART_CRIT_DEGRADED),
                      false);
                  self->NVMeStatus::mediaFault(
                      log->critical_warning & (NVME_SMART_CRIT_MEDIA), false);
                  self->NVMeStatus::smartWarnings(
                      std::to_string(log->critical_warning), false);

                  if (log->critical_warning != 0)
                  {
                      self->markStatus("warning");
                  }
                  self->generateRedfishEventbySmart(log->critical_warning);
              }
              self->updateSmartWarning(log->critical_warning);
              boost::multiprecision::uint128_t powerOnHours;
              memcpy((void*)&powerOnHours, log->power_on_hours,
                     sizeof(powerOnHours));
            });
        self->pollDrive();
    });
}

void NVMeDevice::updateSanitizeStatus(EraseMethod type)
{
    setEstimateTime(0);
    Progress::status(OperationStatus::InProgress);
    inProgress = true;
    setEraseType(type);
    Operation::operation(OperationType::Sanitize, false);
}

void NVMeDevice::erase(uint16_t overwritePasses, EraseMethod type)
{
    if (inProgress)
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed();
    }

    auto cap = SecureErase::sanitizeCapability();
    if (std::find(cap.begin(), cap.end(), type) == cap.end())
    {
        lg2::error("sanitize method is not supported");
        return;
    }

    if (type == EraseMethod::Overwrite)
    {
        uint32_t pattern =  ~0x04030201;
        intf->adminSanitize(
            ctrl, NVME_SANITIZE_SANACT_START_OVERWRITE, overwritePasses,
            pattern,
            [self{shared_from_this()}, type](
                const std::error_code& ec,
                __attribute__((unused)) std::span<uint8_t> status) {
                if (ec)
                {
                    self->Progress::status(OperationStatus::Failed);
                    self->inProgress = false;
                    lg2::error("fail to do sanitize(Overwite)");
                    return;
                }
                self->updateSanitizeStatus(type);
            });
    }
    if (type == EraseMethod::CryptoErase)
    {
        intf->adminSanitize(
            ctrl, NVME_SANITIZE_SANACT_START_CRYPTO_ERASE, 0, 0,
            [self{shared_from_this()}, type](
                const std::error_code& ec,
                __attribute__((unused)) std::span<uint8_t> status) {
                if (ec)
                {
                    self->Progress::status(OperationStatus::Failed);
                    self->inProgress = false;
                    lg2::error("fail to do sanitize(CryptoErase)");
                    return;
                }
                self->updateSanitizeStatus(type);
            });
    }
    if (type == EraseMethod::BlockErase)
    {
        intf->adminSanitize(
            ctrl, NVME_SANITIZE_SANACT_START_BLOCK_ERASE, 0, 0,
            [self{shared_from_this()}, type](
                const std::error_code& ec,
                __attribute__((unused)) std::span<uint8_t> status) {
                if (ec)
                {
                    self->Progress::status(OperationStatus::Failed);
                    self->inProgress = false;
                    lg2::error("fail to do sanitize(BlockErase)");
                    return;
                }
                self->updateSanitizeStatus(type);
            });
    }
}

NVMeDevice::~NVMeDevice()
{
}