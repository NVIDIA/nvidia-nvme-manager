#pragma once
#include <NVMeMi.hpp>
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

namespace fs = std::filesystem;

class NVMeDevice :
    public NvmeInterfaces,
    public std::enable_shared_from_this<NVMeDevice>
{
  public:
    static constexpr const char* mctpEpInterface =
        "xyz.openbmc_project.MCTP.Endpoint";

    NVMeDevice(boost::asio::io_context& io,
               sdbusplus::asio::object_server& objectServer,
               std::shared_ptr<sdbusplus::asio::connection>& conn,
               uint8_t /*eid*/, uint32_t /*bus*/,
               const std::vector<uint8_t> /*addr*/&, const std::string& path);
    NVMeDevice(const NVMeDevice& other) = delete;

    NVMeDevice(NVMeDevice&& other) = delete;
    NVMeDevice& operator=(NVMeDevice&& other) = delete;
    ~NVMeDevice() override = default;

    NVMeDevice& operator=(const NVMeDevice& other) = delete;

    void initialize();
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

    EraseMethod getEraseType()
    {
        return eraseType;
    }

    void setEraseType(EraseMethod type)
    {
        eraseType = type;
    }

    uint32_t getEstimateTime() const
    {
        return estimatedTime;
    }

    void setEstimateTime(uint32_t time)
    {
        estimatedTime = time;
    }
    uint32_t getI2CBus() const
    {
        return bus;
    }

    void updatePercent(uint32_t endTime);
    void updateFormFactor(const std::string& form);
    void updateDriveAssociations();
    void erase(uint16_t overwritePasses, EraseMethod eraseType) override;

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

  private:
    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer scanTimer;

    bool driveFunctional{false};
    uint8_t smartWarning{0xff};
    NVMeIntf nvmeIntf;
    std::shared_ptr<NVMeMiIntf> intf;
    std::string driveIndex;

    AssociationList assocs;
    nvme_mi_ctrl_t ctrl{};
    bool initialized{false};
    bool presence{false};
    bool inProgress{false};
    std::string objPath;
    uint8_t eid;
    uint32_t bus;
    uint8_t retry{1};

    // flag of no-deallocate modifies meida after sanitize(NODMMAS)
    uint32_t nodmmas{0};
    EraseMethod eraseType = EraseMethod::BlockErase;
    uint32_t estimatedTime{0};

    // triggered the smart error from Dbus.
    bool backupDeviceErr{false};
    bool temperatureErr{false};
    bool degradesErr{false};
    bool mediaErr{false};
    bool capacityErr{false};
};
