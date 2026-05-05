#pragma once
#include <nvme/types.h>

#include <NVMeMi.hpp>
#include <SoftwareInventoryManager.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Common/Progress/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Location/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/LocationCode/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Storage/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/StorageController/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/Nvme/Operation/server.hpp>
#include <xyz/openbmc_project/Nvme/SecureErase/server.hpp>
#include <xyz/openbmc_project/Nvme/Status/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Health/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <functional>

using Item = sdbusplus::xyz::openbmc_project::Inventory::server::Item;
using Drive = sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;
using Asset =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset;
using PortInfo =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo;
using Version = sdbusplus::xyz::openbmc_project::Software::server::Version;
using Health =
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health;
using Associations =
    sdbusplus::xyz::openbmc_project::Association::server::Definitions;
using OperationalStatus = sdbusplus::xyz::openbmc_project::State::Decorator::
    server::OperationalStatus;
using NVMeStatus = sdbusplus::xyz::openbmc_project::Nvme::server::Status;
using LocationCode =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::LocationCode;
using Location =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Location;
using StorageController =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::StorageController;
using StorageController =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::StorageController;
using Progress = sdbusplus::xyz::openbmc_project::Common::server::Progress;
using SecureErase = sdbusplus::xyz::openbmc_project::Nvme::server::SecureErase;
using Operation = sdbusplus::xyz::openbmc_project::Nvme::server::Operation;
using Storage =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Storage;

using NvmeInterfaces = sdbusplus::server::object::object<
    Item, StorageController, PortInfo, Drive, Health, OperationalStatus, Asset,
    Version, NVMeStatus, Associations, Progress, SecureErase, Operation>;
using AssociationList =
    std::vector<std::tuple<std::string, std::string, std::string>>;

class NVMeDevice :
    public NvmeInterfaces,
    public std::enable_shared_from_this<NVMeDevice>
{
  public:
    static constexpr const char* mctpEpInterface =
        "xyz.openbmc_project.MCTP.Endpoint";

    NVMeDevice(boost::asio::io_context& io,
               sdbusplus::asio::object_server& objectServer,
               std::shared_ptr<sdbusplus::asio::connection>& conn, uint8_t eid,
               uint32_t bus, int net, const std::vector<uint8_t>& addr,
               const std::string& path, const std::string& formFactor,
               const std::string& driveAssoc, const std::string& locCode);
    NVMeDevice(const NVMeDevice& other) = delete;

    NVMeDevice(NVMeDevice&& other) = delete;
    NVMeDevice& operator=(NVMeDevice&& other) = delete;
    ~NVMeDevice() override = default;

    NVMeDevice& operator=(const NVMeDevice& other) = delete;

    void initialize();
    void queryController();
    void getDriveInfo();
    void getDriveLink();
    void pollDrive();
    void markFunctional(bool functional);
    void markStatus(const std::string& status);
    void generateRedfishEventbySmart(uint8_t sw);
    void updateSanitizeStatus(EraseMethod type);

    static std::string stripString(std::span<const char> src);
    static std::string getManufacture(uint16_t vid);
    std::string driveAssociation;

    std::shared_ptr<NVMeMiIntf> getIntf()
    {
        return intf;
    }

    bool getDriveFunctional() const
    {
        return driveFunctional;
    }

    bool getNodmmas() const
    {
        return nodmmas != 0U;
    }

    void setNodmmas(uint8_t value)
    {
        // SANICAP 31:30
        // Following is the value 0x10 of NODMMAS.
        // Media is additionally modified by the controller
        // after sanitize operation completes successfully.
        nodmmas = value & (0x80000000);
    }

    uint32_t getSanicap() const
    {
        return sanicap;
    }

    void setSanicap(uint32_t value)
    {
        sanicap = value;
        // Also set nodmmas for backward compatibility
        nodmmas = value & (0x80000000);
    }

    EraseMethod getEraseType()
    {
        return eraseType;
    }

    void setEraseType(EraseMethod type)
    {
        eraseType = type;
    }

    uint32_t getI2CBus() const
    {
        return bus;
    }

    void erase(uint16_t overwritePasses, EraseMethod eraseType) override;

    // Software inventory methods
    void createSoftwareInventory();
    void updateSoftwareInventory();
    std::shared_ptr<SoftwareInventory> getSoftwareInventory() const;
    std::string getFirmwareVersion();

    // Drive state management
    void checkAndGenerateDriveEvent();
    uint8_t getEid() const
    {
        return eid;
    }
    const std::string& getObjPath() const
    {
        return objPath;
    }
    const std::string& getLocationCode() const
    {
        return locationCode;
    }

    void setConnectivityDegraded(bool degraded)
    {
        connectivityDegraded = degraded;
    }

    bool isConnectivityDegraded() const
    {
        return connectivityDegraded;
    }

    // Cleanup method to cancel all pending async operations and prevent
    // in-flight callbacks (e.g. miScanCtrl) from scheduling new timers.
    void cancelPendingOperations()
    {
        operationsCancelled = true;
        scanTimer.cancel();
        initRetryTimer.cancel();
    }

    bool backupDeviceFault(bool value) override
    {
        backupDeviceErr = value;
        return value;
    }
    bool temperatureFault(bool value) override
    {
        temperatureErr = value;
        return value;
    }
    bool degradesFault(bool value) override
    {
        degradesErr = value;
        return value;
    }
    bool mediaFault(bool value) override
    {
        mediaErr = value;
        return value;
    }
    bool capacityFault(bool value) override
    {
        capacityErr = value;
        return value;
    }

    /** Register updater for temp/status sensors.
     *  When set, poll interval uses pollIntervalSec instead of default 5s. */
    void setSensorsUpdater(
        std::function<void(nvme_mi_nvm_ss_health_status*)> updater,
        float pollIntervalSec);

    /** Clear sensors updater when sensors are removed. */
    void clearSensorsUpdater();

    /** Set Progress interface for firmware update (Status and Progress 0-100).
     *  Used by nvme-manager FW update handler to report update state. */
    void setFwUpdateProgress(uint32_t percent, OperationStatus status);

  private:
    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer scanTimer;
    boost::asio::steady_timer initRetryTimer;

    bool driveFunctional{false};
    uint8_t smartWarning{0xff};
    NVMeIntf nvmeIntf;
    std::shared_ptr<NVMeMiIntf> intf;
    std::string driveIndex;
    std::shared_ptr<SoftwareInventory> softwareInventory;
    std::unique_ptr<SoftwareInventoryManager> softwareInventoryManager;

    AssociationList assocs;
    nvme_mi_ctrl_t ctrl{};
    bool initialized{false};
    bool presence{false};
    bool inProgress{false};
    std::string objPath;
    uint8_t eid;
    uint32_t bus;
    int net;
    uint8_t retry{1};
    int initRetryCount{0};
    bool operationsCancelled{false};

    // flag of no-deallocate modifies meida after sanitize(NODMMAS)
    uint32_t nodmmas{0};
    uint32_t sanicap{0}; // Store full sanitize capabilities
    EraseMethod eraseType = EraseMethod::BlockErase;

    // triggered the smart error from Dbus.
    bool backupDeviceErr{false};
    bool temperatureErr{false};
    bool degradesErr{false};
    bool mediaErr{false};
    bool capacityErr{false};

    // Drive state information
    std::string locationCode;

    // MCTP connectivity state
    bool connectivityDegraded{false};

    // Sensor integration: when set, poll at sensorPollIntervalSec and
    // notify updater with health data for temp/status sensors (nullptr on
    // error)
    std::function<void(nvme_mi_nvm_ss_health_status*)> sensorsUpdater;
    float sensorPollIntervalSec{0};
};

// Drive state management function
void updateSingleDriveState(uint8_t eid);
