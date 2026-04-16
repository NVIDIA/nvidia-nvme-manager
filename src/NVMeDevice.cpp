#include <nvme-mi_config.h>

#include <NVMeDevice.hpp>
#include <SoftwareInventoryManager.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <dbusutil.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>

const std::string driveConfig{"/usr/share/nvidia-nvme-manager/drive.json"};

const std::uint8_t maxIdentifyCmdRetry = 3;
const std::uint8_t pollInterval = 5;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

using Json = nlohmann::json;

NVMeDevice::NVMeDevice(boost::asio::io_context& io,
                       sdbusplus::asio::object_server& objectServer,
                       std::shared_ptr<sdbusplus::asio::connection>& conn,
                       uint8_t eid, uint32_t bus, int net,
                       const std::vector<uint8_t>& addr,
                       const std::string& path) :
    NvmeInterfaces(static_cast<sdbusplus::bus::bus&>(*conn), path.c_str(),
                   NvmeInterfaces::action::defer_emit),
    conn(conn), objServer(objectServer), scanTimer(io), initRetryTimer(io),
    objPath(path), eid(eid), bus(bus), net(net)
{
    std::filesystem::path p(path);

    driveIndex = p.filename();

    // assume the drive is good and update Dbus properties at the first place.
    markFunctional(true);

    nvmeIntf = NVMeIntf::create<NVMeMi>(io, conn, addr, net, eid);
    intf = std::get<std::shared_ptr<NVMeMiIntf>>(nvmeIntf.getInferface());

#ifdef FIRMWARE_INVENTORY
    softwareInventoryManager =
        std::make_unique<SoftwareInventoryManager>(*conn);
#endif
}

inline Drive::DriveFormFactor getDriveFormFactor(const std::string& form)
{
    if (form == "Drive3_5")
    {
        return Drive::DriveFormFactor::Drive3_5;
    }
    if (form == "Drive2_5")
    {
        return Drive::DriveFormFactor::Drive2_5;
    }
    if (form == "EDSFF_1U_Long")
    {
        return Drive::DriveFormFactor::EDSFF_1U_Long;
    }
    if (form == "EDSFF_1U_Short")
    {
        return Drive::DriveFormFactor::EDSFF_1U_Short;
    }
    if (form == "EDSFF_E3_Short")
    {
        return Drive::DriveFormFactor::EDSFF_E3_Short;
    }
    if (form == "EDSFF_E3_Long")
    {
        return Drive::DriveFormFactor::EDSFF_E3_Long;
    }
    if (form == "M2_2230")
    {
        return Drive::DriveFormFactor::M2_2230;
    }
    if (form == "M2_2242")
    {
        return Drive::DriveFormFactor::M2_2242;
    }
    if (form == "M2_2260")
    {
        return Drive::DriveFormFactor::M2_2260;
    }
    if (form == "M2_2280")
    {
        return Drive::DriveFormFactor::M2_2280;
    }
    if (form == "M2_22110")
    {
        return Drive::DriveFormFactor::M2_22110;
    }
    if (form == "U2")
    {
        return Drive::DriveFormFactor::U2;
    }
    if (form == "PCIeSlotFullLength")
    {
        return Drive::DriveFormFactor::PCIeSlotFullLength;
    }
    if (form == "PCIeSlotLowProfile")
    {
        return Drive::DriveFormFactor::PCIeSlotLowProfile;
    }
    if (form == "PCIeHalfLength")
    {
        return Drive::DriveFormFactor::PCIeHalfLength;
    }
    if (form == "OEM")
    {
        return Drive::DriveFormFactor::OEM;
    }
    return Drive::DriveFormFactor::U2;
}

std::string NVMeDevice::stripString(std::span<const char> src)
{
    std::string s(src.data(), src.size());
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

void NVMeDevice::updateFormFactor(const std::string& form)
{
    size_t pos = form.find_last_of('.');
    auto formFactor = getDriveFormFactor(form.substr(pos + 1));
    Drive::formFactor(formFactor, false);
}

void NVMeDevice::updateDriveAssociations()
{
    HealthType healthType = Health::health();
    assocs = {};

    // Read the current Health state and restore for associations
    if (healthType == HealthType::Critical)
    {
        assocs.emplace_back("health", "critical", objPath.c_str());
    }
    else if (healthType == HealthType::Warning)
    {
        assocs.emplace_back("health", "warning", objPath.c_str());
    }

    // Set Drive's association
    assocs.emplace_back("chassis", "drive", driveAssociation.c_str());

    Associations::associations(assocs);
}

std::string NVMeDevice::getManufacture(uint16_t vid)
{
    if (vid == 0x144d)
    {
        return {"Samsung"};
    }
    if (vid == 0x1344)
    {
        return {"Mircon"};
    }
    if (vid == 0x1e0f)
    {
        return {"Kioxia"};
    }
    if (vid == 0x25e)
    {
        return {"Solidigm"};
    }

    return {"Unkown"};
}

inline uint32_t getMaxLinkSpeed(uint8_t speedVec, uint8_t lanes)
{
    // starting from 64 GT/s (PCIe Gen 6)
    int base = 64;

    for (auto i = 5; i >= 0; i--)
    {
        if ((speedVec & (1 << i)) != 0)
        {
            break;
        }
        base = base / 2;
    }

    return base * lanes;
}

inline uint32_t getCurrLinkSpeed(uint8_t speed, uint8_t lanes)
{
    uint32_t base = 64;
    if (speed == 0)
    {
        // link not active
        return 0;
    }

    for (auto i = 5; i >= 0; i--)
    {
        if ((speed - 1) == i)
        {
            break;
        }
        base = base / 2;
    }
    return base * lanes;
}

void NVMeDevice::getDriveInfo()
{
    getIntf()->adminIdentify(
        eid, nvme_identify_cns::NVME_IDENTIFY_CNS_CTRL, NVME_NSID_NONE, 0,
        identifyRspLength,
        [self{shared_from_this()}](const std::error_code& ec,
                                   std::span<uint8_t> data) {
        if (ec)
        {
            if (self->operationsCancelled)
            {
                return;
            }
            // Identify command's length is up to 4K. There's possibility
            // to get I2C transcation timeout during the transmission.
            // Implement retry method.
            lg2::error("eid:{ID} Retry Identify command {COUNT} times", "ID",
                       self->eid, "COUNT", self->retry);
            if (self->retry < maxIdentifyCmdRetry)
            {
                self->retry++;
                self->getDriveInfo();
            }
            else
            {
                // give up and move forward next command.
                self->retry = 0;
                self->getDriveLink();
            }
            return;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* id = reinterpret_cast<struct nvme_id_ctrl*>(data.data());

        self->Asset::manufacturer(NVMeDevice::getManufacture(id->vid), true);
        auto sn = NVMeDevice::stripString(std::span<const char, 20>(id->sn));
        self->Asset::serialNumber(sn, true);
        auto mn = NVMeDevice::stripString(std::span<const char, 40>(id->mn));
        self->Asset::model(mn, true);

        std::string fr;
        fr.assign(static_cast<const char*>(id->fr), 8);
        self->Version::version(fr, true);

        std::array<uint64_t, 2> driveCapacity{};
        memcpy(driveCapacity.data(), static_cast<const void*>(id->tnvmcap),
               sizeof(driveCapacity));

        /* 8 bytes presenting the drive capacity is enough to support all
         * drives outside market.
         */
        self->Drive::capacity(driveCapacity[0], true);

        // check the drive sanitize capability
        std::vector<EraseMethod> saniCap;
        if ((id->sanicap & (NVME_CTRL_SANICAP_OWS)) != 0U)
        {
            saniCap.push_back(EraseMethod::Overwrite);
        }
        if ((id->sanicap & (NVME_CTRL_SANICAP_BES)) != 0U)
        {
            saniCap.push_back(EraseMethod::BlockErase);
        }
        if ((id->sanicap & (NVME_CTRL_SANICAP_CES)) != 0U)
        {
            saniCap.push_back(EraseMethod::CryptoErase);
        }
        self->SecureErase::sanitizeCapability(saniCap, true);
        self->setSanicap(id->sanicap); // Store full sanicap value

#ifdef FIRMWARE_INVENTORY
        self->createSoftwareInventory();
#endif

        // create drive events if there is new drive or swap drive found
        self->checkAndGenerateDriveEvent();

        self->pollDrive();
    });
}

void NVMeDevice::getDriveLink()
{
    intf->miPCIePortInformation(
        [self{shared_from_this()}](const std::error_code& err,
                                   nvme_mi_read_port_info* port) {
        if (err)
        {
            lg2::error("eid:{ID} - fail to get PCIePortInformation", "ID",
                       self->eid);
            self->pollDrive();
            return;
        }
        const auto& info =
            port->pcie; // NOLINT(cppcoreguidelines-pro-type-union-access)
        uint8_t sls = info.sls;
        uint8_t mlw = info.mlw;
        uint8_t cls = info.cls;
        uint8_t nlw = info.nlw;

        self->PortInfo::maxSpeed(getMaxLinkSpeed(sls, mlw), true);
        self->PortInfo::currentSpeed(getCurrLinkSpeed(cls, nlw), true);
    });
}

void NVMeDevice::initialize()
{
    if (initialized)
    {
        return;
    }
    initialized = true;
    presence = false;

    Drive::type(DriveType::SSD, true);
    Drive::protocol(DriveProtocol::NVMe, true);

    NvmeInterfaces::emit_object_added();

    // Start controller query
    initRetryCount = 0;
    queryController();
}

void NVMeDevice::queryController()
{
    constexpr int maxRetries = 5;
    constexpr int initialDelayMs = 1000;

    intf->miScanCtrl([self{shared_from_this()}](
                         const std::error_code& ec,
                         const std::vector<nvme_mi_ctrl_t>& ctrlList) mutable {
        if (ec || ctrlList.empty())
        {
            if (self->operationsCancelled)
            {
                return;
            }
            if (self->initRetryCount >= maxRetries)
            {
                lg2::error(
                    "eid:{ID} - fail to scan controllers after {RETRIES} attempts {ERR}: {MSG}",
                    "ID", self->eid, "RETRIES", self->initRetryCount + 1, "ERR",
                    ec.value(), "MSG", ec.message());
                self->presence = false;
                self->Item::present(false, true);
                return;
            }

            // Exponential backoff: 1s, 2s, 4s, 8s, 16s
            int delayMs = initialDelayMs << self->initRetryCount;
            lg2::info(
                "eid:{ID} - scan attempt {RETRY} failed, retrying in {DELAY}ms: {MSG}",
                "ID", self->eid, "RETRY", self->initRetryCount + 1, "DELAY",
                delayMs, "MSG", ec.message());

            self->initRetryTimer.expires_after(
                std::chrono::milliseconds(delayMs));
            self->initRetryTimer.async_wait(
                [self](const boost::system::error_code& timerEc) {
                if (timerEc == boost::asio::error::operation_aborted)
                {
                    return; // Timer was cancelled
                }
                if (timerEc)
                {
                    lg2::error("Init retry timer error: {MSG}", "MSG",
                               timerEc.message());
                    return;
                }
                if (self->operationsCancelled)
                {
                    return;
                }
                self->initRetryCount++;
                self->queryController();
            });
            return;
        }

        // Success!
        self->presence = true;
        self->Item::present(true, true);

        if (self->initRetryCount > 0)
        {
            lg2::info("eid:{ID} - scan succeeded after {RETRY} retries", "ID",
                      self->eid, "RETRY", self->initRetryCount);
        }

        self->getDriveInfo();
    });
}

void NVMeDevice::markStatus(const std::string& status)
{
    assocs = {};

    if (status == "critical")
    {
        assocs.emplace_back("health", status, objPath.c_str());
        Health::health(HealthType::Critical, true);
    }
    else if (status == "warning")
    {
        assocs.emplace_back("health", status, objPath.c_str());
        Health::health(HealthType::Warning, true);
    }
    else
    {
        Health::health(HealthType::OK, true);
    }

    if (!driveAssociation.empty())
    {
        assocs.emplace_back("chassis", "drive", driveAssociation.c_str());
    }
    else
    {
        assocs.emplace_back("chassis", "drive", driveLocation);
    }
    Associations::associations(assocs);
}

void NVMeDevice::markFunctional(bool functional)
{
    if (driveFunctional != functional)
    {
        // mark device state
        if (!functional)
        {
            OperationalStatus::functional(false, true);
            OperationalStatus::state(OperationalStatus::StateType::Fault, true);
            markStatus("critical");

            createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                           Level::Critical, driveIndex, "Drive Failure",
                           driveFailureResolution,
                           redfishDrivePathPrefix + driveIndex);
        }
        else
        {
            OperationalStatus::functional(true, true);
            OperationalStatus::state(OperationalStatus::StateType::None, true);
            markStatus("ok");
        }
    }
    driveFunctional = functional;
}

void NVMeDevice::generateRedfishEventbySmart(uint8_t sw)
{
    if ((sw & (NVME_SMART_CRIT_PMR_RO)) != 0)
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            driveIndex,
            "Persistent Memory Region has become read-only or unreliable",
            drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if ((sw & (NVME_SMART_CRIT_VOLATILE_MEMORY)) != 0)
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, driveIndex,
                       "volatile memory backup device has failed",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if ((sw & (NVME_SMART_CRIT_SPARE)) != 0)
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            driveIndex,
            "available spare capacity has fallen below the threshold",
            drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if ((sw & (NVME_SMART_CRIT_DEGRADED)) != 0)
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, driveIndex,
                       "NVM subsystem reliability has been degraded",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if ((sw & (NVME_SMART_CRIT_MEDIA)) != 0)
    {
        createLogEntry(conn, "ResourceEvent.1.0.ResourceErrorsDetected",
                       Level::Warning, driveIndex,
                       "all of the media has been placed in read only mode",
                       drivePfaResolution, redfishDrivePathPrefix + driveIndex);
    }
    if ((sw & (NVME_SMART_CRIT_TEMPERATURE)) != 0)
    {
        createLogEntry(
            conn, "ResourceEvent.1.0.ResourceErrorsDetected", Level::Warning,
            driveIndex, "temperature is over or under the threshold",
            "Check the condition of the resource listed in OriginOfCondition",
            redfishDrivePathPrefix + driveIndex);
    }
}

void NVMeDevice::setSensorsUpdater(
    std::function<void(nvme_mi_nvm_ss_health_status*)> updater,
    float pollIntervalSec)
{
    sensorsUpdater = std::move(updater);
    sensorPollIntervalSec = pollIntervalSec;
}

void NVMeDevice::clearSensorsUpdater()
{
    sensorsUpdater = nullptr;
    sensorPollIntervalSec = 0;
}

void NVMeDevice::pollDrive()
{
    if (operationsCancelled)
    {
        return;
    }
    auto intervalSec = (sensorPollIntervalSec > 0)
                           ? sensorPollIntervalSec
                           : static_cast<float>(pollInterval);
    scanTimer.expires_after(
        std::chrono::milliseconds(static_cast<int>(intervalSec * 1000)));
    scanTimer.async_wait(
        [weak{weak_from_this()}](const boost::system::error_code errorCode) {
        // Try to lock weak_ptr to get shared_ptr
        auto self = weak.lock();
        if (!self)
        {
            // Drive instance has been destroyed, exit
            return;
        }

        if (errorCode == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        if (errorCode)
        {
            lg2::error("Error: {MSG}", "MSG", errorCode.message());
            return;
        }
        // try to re-initialize the drive
        if (!self->presence)
        {
            self->initialize();
            return;
        }

        // Check connectivity state before polling
        if (self->isConnectivityDegraded())
        {
            lg2::debug(
                "eid:{ID} - MCTP connectivity degraded, skipping sensor polling",
                "ID", self->eid);
            self->pollDrive();
            return;
        }

        auto miIntf = self->getIntf();
        if (self->Operation::operation() == OperationType::Sanitize &&
            self->inProgress)
        {
            miIntf->adminGetLogPage(
                self->eid, NVME_LOG_LID_SANITIZE, 0, 0,
                [self](const std::error_code& ec, std::span<uint8_t> status) {
                if (ec)
                {
                    lg2::error(
                        "fail to query satinize status for the nvme subsystem {ERR}:{MSG}",
                        "ERR", ec.value(), "MSG", ec.message());
                    return;
                }

                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                auto* log = reinterpret_cast<struct nvme_sanitize_log_page*>(
                    status.data());

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

                uint32_t percent = (static_cast<uint32_t>(log->sprog) * 100U) /
                                   65536U;
                if (percent > 99)
                {
                    percent = 99;
                }
                lg2::info(
                    "EID {EID} - Sanitize in progress: sprog={SPROG}, {PCT}%",
                    "EID", static_cast<int>(self->eid), "SPROG",
                    static_cast<uint32_t>(log->sprog), "PCT", percent);
                self->Progress::status(OperationStatus::InProgress);
                self->Progress::progress(percent);
            });
        }

        self->getDriveLink();
        miIntf->miSubsystemHealthStatusPoll(
            [self](__attribute__((unused)) const std::error_code& err,
                   nvme_mi_nvm_ss_health_status* ss) {
            if (err)
            {
                lg2::error("fail to query SubSystemHealthPoll for the nvme "
                           "subsystem {ERR}:{MSG}",
                           "ERR", err.value(), "MSG", err.message());
                if (self->sensorsUpdater)
                {
                    self->sensorsUpdater(nullptr);
                }
                return;
            }
            self->NVMeStatus::driveLifeUsed(std::to_string(ss->pdlu), true);

            // the percentage is allowed to exceed 100 based on the spec.
            auto percentage = (ss->pdlu > 100) ? 100 : ss->pdlu;
            self->sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                Drive::predictedMediaLifeLeftPercent(100 - percentage, true);

            self->markFunctional((ss->nss & 0x20) != 0);

            if (self->sensorsUpdater)
            {
                self->sensorsUpdater(ss);
            }
        });

        // change the nsid to 0 for new version of libnvme
        miIntf->adminGetLogPage(
            self->eid, NVME_LOG_LID_SMART, 0, 0,
            [self](const std::error_code& ec, std::span<uint8_t> smart) {
            if (ec)
            {
                lg2::error(
                    "fail to query SMART for the nvme subsystem {ERR}:{MSG}",
                    "ERR", ec.value(), "MSG", ec.message());
                self->pollDrive();
                return;
            }

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            auto* log = reinterpret_cast<struct nvme_smart_log*>(smart.data());

            auto cw = log->critical_warning;

            // overwrite the warning triggered from Dbus
            if (self->backupDeviceErr)
            {
                cw |= (NVME_SMART_CRIT_VOLATILE_MEMORY);
            }
            if (self->capacityErr)
            {
                cw |= (NVME_SMART_CRIT_SPARE);
            }
            if (self->temperatureErr)
            {
                cw |= (NVME_SMART_CRIT_TEMPERATURE);
            }
            if (self->degradesErr)
            {
                cw |= (NVME_SMART_CRIT_DEGRADED);
            }
            if (self->mediaErr)
            {
                cw |= (NVME_SMART_CRIT_MEDIA);
            }

            if (cw != self->smartWarning)
            {
                // the error indicator is from smart warning
                self->NVMeStatus::backupDeviceFault(
                    (cw & (NVME_SMART_CRIT_VOLATILE_MEMORY)) != 0, true);

                self->NVMeStatus::capacityFault(
                    (cw & (NVME_SMART_CRIT_SPARE)) != 0, true);

                self->NVMeStatus::temperatureFault(
                    (cw & (NVME_SMART_CRIT_TEMPERATURE)) != 0, true);

                self->NVMeStatus::degradesFault(
                    (cw & (NVME_SMART_CRIT_DEGRADED)) != 0, true);

                self->NVMeStatus::mediaFault(
                    (cw & ((NVME_SMART_CRIT_MEDIA))) != 0, true);

                self->NVMeStatus::smartWarnings(std::to_string(cw), true);

                if (cw != 0)
                {
                    self->markStatus("warning");
                }
                else
                {
                    self->markStatus("ok");
                }
                self->generateRedfishEventbySmart(cw);
            }
            self->smartWarning = cw;
            boost::multiprecision::uint128_t powerOnHours;
            std::span powerOnHoursSpan(log->power_on_hours);
            memcpy((void*)&powerOnHours, powerOnHoursSpan.data(),
                   sizeof(powerOnHours));
            self->pollDrive();
        });
    });
}

void NVMeDevice::setFwUpdateProgress(uint32_t percent, OperationStatus status)
{
    Progress::progress(percent, false);
    Progress::status(status, false);
}

void NVMeDevice::updateSanitizeStatus(EraseMethod type)
{
    Progress::status(OperationStatus::InProgress);
    inProgress = true;
    setEraseType(type);
    Operation::operation(OperationType::Sanitize, true);
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
        uint32_t pattern = ~0x04030201;
        intf->adminSanitize(
            eid, NVME_SANITIZE_SANACT_START_OVERWRITE, overwritePasses, pattern,
            sanicap, // Pass device's sanitize capabilities
            [self{shared_from_this()},
             type](const std::error_code& ec,
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
            eid, NVME_SANITIZE_SANACT_START_CRYPTO_ERASE, 0, 0,
            sanicap, // Pass device's sanitize capabilities
            [self{shared_from_this()},
             type](const std::error_code& ec,
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
            eid, NVME_SANITIZE_SANACT_START_BLOCK_ERASE, 0, 0,
            sanicap, // Pass device's sanitize capabilities
            [self{shared_from_this()},
             type](const std::error_code& ec,
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

#ifdef FIRMWARE_INVENTORY
void NVMeDevice::createSoftwareInventory()
{
    if (softwareInventory != nullptr)
    {
        return;
    }

    std::string manufacturer = Asset::manufacturer();
    std::string model = Asset::model();
    std::string serialNumber = Asset::serialNumber();
    std::string partNumber = Asset::partNumber();
    std::string firmwareVersion = Version::version();

    // Create software inventory object using the member manager
    softwareInventory = softwareInventoryManager->createNVMeSoftwareInventory(
        objPath, manufacturer, model, serialNumber, partNumber,
        firmwareVersion);
}

void NVMeDevice::updateSoftwareInventory()
{
    if (softwareInventory == nullptr)
    {
        createSoftwareInventory();
        return;
    }

    // Update firmware version from the existing Version property
    std::string firmwareVersion = Version::version();
    softwareInventory->updateVersion(firmwareVersion);

    // Update other device information from existing Asset properties
    softwareInventory->updateManufacturer(Asset::manufacturer());
    softwareInventory->updateModel(Asset::model());
    softwareInventory->updateSerialNumber(Asset::serialNumber());
    softwareInventory->updatePartNumber(Asset::partNumber());
}

std::shared_ptr<SoftwareInventory> NVMeDevice::getSoftwareInventory() const
{
    return softwareInventory;
}

std::string NVMeDevice::getFirmwareVersion()
{
    return Version::version();
}
#endif

void NVMeDevice::updateLocationCode(const std::string& locCode)
{
    locationCode = locCode;
}

void NVMeDevice::checkAndGenerateDriveEvent()
{
    try
    {
        std::string filePath = driveStateFile;

        // If no state file exists, this is first discovery - save state without
        // event
        if (!std::filesystem::exists(filePath))
        {
            lg2::info("No state file, first discovery for EID {EID}", "EID",
                      static_cast<int>(eid));
            updateSingleDriveState(eid);
            return;
        }

        // Read state file
        std::ifstream inputFile(filePath);
        if (!inputFile.is_open())
        {
            lg2::warning("Cannot open state file for EID {EID}", "EID",
                         static_cast<int>(eid));
            return;
        }

        nlohmann::json driveStates;
        try
        {
            inputFile >> driveStates;
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to parse state file: {ERR}", "ERR", e.what());
            return;
        }

        std::string currentSN = Asset::serialNumber();
        std::string currentLoc = locationCode;

        // Check if drive entry exists in state file
        bool found = false;
        for (const auto& drive : driveStates)
        {
            if (drive.contains("eid") && drive["eid"] == eid)
            {
                found = true;
                std::string connectivity = drive.contains("connectivity")
                                               ? drive["connectivity"]
                                               : "Available";

                if (connectivity == "Removed")
                {
                    // Drive was removed and now it's back
                    lg2::info(
                        "Drive EID {EID} returning from Removed state, generating DriveInserted event",
                        "EID", static_cast<int>(eid));

                    std::string location =
                        currentLoc.empty() ? "Unknown Location" : currentLoc;
                    std::string redfishPath =
                        std::string(redfishDrivePathPrefix) +
                        std::string(drivePrefix) + std::to_string(eid);

                    createLogEntry(conn, driveInserted, Level::Informational,
                                   location, "", driveInsertedResolution,
                                   redfishPath);
                    lg2::info(
                        "Generated DriveInserted event for returning drive EID {EID} at {LOC}",
                        "EID", static_cast<int>(eid), "LOC", location);

                    // Update state file
                    updateSingleDriveState(eid);
                }
                else
                {
                    // Drive was Available - check for swap (different SN at
                    // same location)
                    std::string prevSN = drive.contains("serialNumber")
                                             ? drive["serialNumber"]
                                             : "";

                    if (!currentSN.empty() && !prevSN.empty() &&
                        currentSN != prevSN)
                    {
                        // Serial number changed - this is a swap
                        lg2::info(
                            "Drive swap detected at EID {EID}: Old SN:{OLDSN} -> New SN:{NEWSN}",
                            "EID", static_cast<int>(eid), "OLDSN", prevSN,
                            "NEWSN", currentSN);

                        std::string location = currentLoc.empty()
                                                   ? "Unknown Location"
                                                   : currentLoc;
                        std::string redfishPath =
                            std::string(redfishDrivePathPrefix) +
                            std::string(drivePrefix) + std::to_string(eid);

                        // Generate DriveRemoved for old drive
                        createLogEntry(conn, driveRemoved, Level::Critical,
                                       location, "", driveRemovedResolution,
                                       redfishPath);

                        // Generate DriveInserted for new drive
                        createLogEntry(conn, driveInserted,
                                       Level::Informational, location, "",
                                       driveInsertedResolution, redfishPath);

                        lg2::info(
                            "Generated swap events for EID {EID}: old SN removed, new SN inserted",
                            "EID", static_cast<int>(eid));

                        // Update state file with new drive info
                        updateSingleDriveState(eid);
                    }
                    else
                    {
                        lg2::info(
                            "Drive EID {EID} already in state file with same SN, no event",
                            "EID", static_cast<int>(eid));
                        // Just update in case location changed
                        updateSingleDriveState(eid);
                    }
                }
                break;
            }
        }

        if (!found)
        {
            // New drive not in state file - don't generate event, just save
            lg2::info("New drive EID {EID} not in state file, saving state",
                      "EID", static_cast<int>(eid));
            updateSingleDriveState(eid);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception in checkAndGenerateDriveEvent: {ERR}", "ERR",
                   e.what());
    }
}
