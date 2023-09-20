#pragma once
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/Nvme/Status/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Health/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/StorageController/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/LocationCode/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Common/Progress/server.hpp>
#include <xyz/openbmc_project/Nvme/SecureErase/server.hpp>
#include <NVMeMi.hpp>

using Item = sdbusplus::xyz::openbmc_project::Inventory::server::Item;
using Drive = sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;
using Asset =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset;
using Port = sdbusplus::xyz::openbmc_project::Inventory::Item::server::Port;
using Version = sdbusplus::xyz::openbmc_project::Software::server::Version;
using Health =
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health;
using Associations =
    sdbusplus::xyz::openbmc_project::Association::server::Definitions;
using OperationalStatus = sdbusplus::xyz::openbmc_project::State::Decorator::
    server::OperationalStatus;
using NVMeStatus = sdbusplus::xyz::openbmc_project::Nvme::server::Status;
using Location = sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::LocationCode;
using StorageController = sdbusplus::xyz::openbmc_project::Inventory::Item::server::StorageController;
using Progress = sdbusplus::xyz::openbmc_project::Common::server::Progress;
using SecureErase = sdbusplus::xyz::openbmc_project::Nvme::server::SecureErase;

using NvmeInterfaces = sdbusplus::server::object::object<
    Item, StorageController, Port, Drive, Health, OperationalStatus, Asset,
    Version, NVMeStatus, Location, Associations, Progress, SecureErase>;
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

    NVMeDevice(boost::asio::io_service& io,
               sdbusplus::asio::object_server& objectServer,
               std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
               uint8_t, std::vector<uint8_t>, std::string path);
    ~NVMeDevice();

    NVMeDevice& operator=(const NVMeDevice& other) = delete;

    void initialize();
    void pollDrive(void);
    void markFunctional(bool functional);
    void markStatus(std::string status);
    void generateRedfishEventbySmart(uint8_t sw);
    void updateSanitizeStatus(EraseMethod type);

    std::string stripString(char *src, size_t len);
    std::string getManufacture(uint16_t vid);

    std::shared_ptr<NVMeMiIntf> getIntf()
    {
        return intf;
    }

    uint8_t getSmartWarning()
    {
        return smartWarning;
    }

    void updateSmartWarning(uint8_t newValue)
    {
        smartWarning = newValue;
    }
    
    bool getDriveFunctional()
    {
        return driveFunctional;
    }

    bool getNodmmas()
    {
        return nodmmas;
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

    uint16_t getEstimateTime()
    {
        return estimatedTime;
    }

    void setEstimateTime(uint16_t time)
    {
        estimatedTime = time;
    }

    void updatePercent(uint16_t endTime);
    void erase(uint16_t overwritePasses, EraseMethod eraseType);

  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> driveInterface;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer scanTimer;

    bool driveFunctional;
    uint8_t smartWarning;
    NVMeIntf nvmeIntf;
    std::shared_ptr<NVMeMiIntf> intf;
    std::string driveIndex;

    AssociationList assocs;
    nvme_mi_ctrl_t ctrl;
    bool presence;
    std::string objPath;
    uint8_t eid;
    // flag of no-deallocate modifies meida after sanitize(NODMMAS)
    uint32_t nodmmas;
    EraseMethod eraseType;
    uint16_t estimatedTime;

};
