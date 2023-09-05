#include <NVMeDevice.hpp>
#include <nvme-mi_config.h>
#include <dbusutil.hpp>

#include <nlohmann/json.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>

const std::string driveFailureResolution{
    "Ensure all cables are properly and securely connected. Ensure all drives are fully seated. Replace the defective cables, drive, or both."};
const std::string drivePfaResolution{
    "If this drive is not part of a fault-tolerant volume, first back up all data, then replace the drive and restore all data afterward. If this drive is part of a fault-tolerant volume, replace this drive as soon as possible as long as the health is OK"};

const std::string redfishDrivePathPrefix{"/redfish/v1/Systems/System_0/Storage/1/Drives/"};
const std::string redfishDriveName{"NVMe Drive"};

const std::string driveConfig{"/usr/share/nvidia-nvme-manager/drive.json"};

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
    smartWarning(0xff), objPath(path), eid(eid)
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
                Location::locationCode(loc);
                auto formFactor = getDriveFormFactor(d["form_factor"].get<std::string>());
                Drive::formFactor(formFactor);
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
              self->NVMeStatus::driveLifeUsed(std::to_string(ss->pdlu));

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
                  self->NVMeStatus::backupDeviceFault(
                      log->critical_warning & NVME_SMART_CRIT_VOLATILE_MEMORY);
                  self->NVMeStatus::capacityFault(log->critical_warning &
                                                  NVME_SMART_CRIT_SPARE);
                  self->NVMeStatus::temperatureFault(
                      log->critical_warning & NVME_SMART_CRIT_TEMPERATURE);
                  self->NVMeStatus::degradesFault(log->critical_warning &
                                                  NVME_SMART_CRIT_DEGRADED);
                  self->NVMeStatus::mediaFault(log->critical_warning &
                                               NVME_SMART_CRIT_MEDIA);
                  self->NVMeStatus::smartWarnings(
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
