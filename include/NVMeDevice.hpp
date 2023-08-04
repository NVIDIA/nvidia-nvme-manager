#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <NVMeMI.hpp>

using StatusInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus,
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset,
    sdbusplus::xyz::openbmc_project::Software::server::Version,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using DriveInterface =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;

using AssociationList =
    std::vector<std::tuple<std::string, std::string, std::string>>;

namespace fs = std::filesystem;


// using NVMEMap = boost::container::flat_map<unit8_t,
// std::shared_ptr<NVMeDevice>>;
class NVMeDevice :
    public StatusInterface,
    public std::enable_shared_from_this<NVMeDevice>
{
  public:
    static constexpr const char* mctpEpInterface =
        "xyz.openbmc_project.MCTP.Endpoint";

    NVMeDevice(boost::asio::io_service& io,
               sdbusplus::asio::object_server& objectServer,
               std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
               uint8_t, std::vector<uint8_t>);
    ~NVMeDevice();

    NVMeDevice& operator=(const NVMeDevice& other) = delete;

    void pollDevices(void);

    std::string stripString(char *src, size_t len);
    std::shared_ptr<NVMeMI> getNVMeCtx()
    {
        return nvmeCtx;
    }
  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> driveInterface;
    sdbusplus::asio::object_server& objServer;
    boost::asio::deadline_timer scanTimer;

    std::shared_ptr<NVMeMI> nvmeCtx;

};
