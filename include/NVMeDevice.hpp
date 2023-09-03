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
using NvMeStatus = sdbusplus::xyz::openbmc_project::Nvme::server::Status;

using NvmeInterfaces = sdbusplus::server::object::object<
    Item,
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::StorageController,
    Port, Drive, Health, OperationalStatus, Asset, Version, NvMeStatus,
    Associations>;

using AssociationList =
    std::vector<std::tuple<std::string, std::string, std::string>>;

namespace fs = std::filesystem;


// using NVMEMap = boost::container::flat_map<unit8_t,
// std::shared_ptr<NVMeDevice>>;
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
};
