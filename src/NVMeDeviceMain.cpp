#include <NVMeDevice.hpp>
#include <MCTPDiscovery.hpp>

#include <boost/asio/steady_timer.hpp>

#include <iostream>
#include <optional>
#include <regex>
#include <vector>

const constexpr char* mctpEpsPath = "/xyz/openbmc_project/mctp";

std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>> driveMap;

static void handleEmEndpoints(const ManagedObjectType& objData)
{
    std::string loc;
    std::string form;
    std::string parentChassis;
    uint64_t bus = -1;

    for (const auto& [path, data] : objData)
    {
        auto ep = data.find("xyz.openbmc_project.Inventory.Item.NVMe");
        if (ep == data.end())
        {
            continue;
        }

        ep = data.find("xyz.openbmc_project.Inventory.Decorator.Location");
        if ( ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("LocationCode");
            if (findProp == prop.end())
            {
                continue;
            }
            loc = std::get<std::string>(findProp->second);
        }
        ep = data.find("xyz.openbmc_project.Inventory.Decorator.I2CDevice");
        if ( ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("Bus");
            if (findProp == prop.end())
                continue;
            bus = std::get<uint64_t>(findProp->second);
        }
        ep = data.find("xyz.openbmc_project.Inventory.Item.Drive");
        if ( ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("FormFactor");
            if (findProp == prop.end())
            {
                continue;
            }
            form = std::get<std::string>(findProp->second);
        }

        for (const auto& [_, context] : driveMap)
        {
            // update location and formfactor by comparing bus number
            if (context->getI2CBus() != bus)
            {
                continue;
            }
            context->updateLocation(loc);
            context->updateFormFactor(form);
        }
    }

    // wait for worker ready to handle NVMe-MI commands.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    for (const auto& [_, context] : driveMap)
    {
        context->initialize();
    }
}

void collectInventory(
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{

    auto getter = std::make_shared<getObjects>(
        dbusConnection,
        std::move([&dbusConnection](const ManagedObjectType& endpoints) {
            handleEmEndpoints(endpoints);
        }));
    getter->getConfiguration(std::vector<std::string>{
        "xyz.openbmc_project.Inventory.Item.Drive",
        "xyz.openbmc_project.Inventory.Item.NVMe",
        "xyz.openbmc_project.Inventory.Decorator.I2CDevice",
        "xyz.openbmc_project.Inventory.Decorator.Location",
        "xyz.openbmc_project.Association.Definitions",
        });
}

static void handleMCTPEndpoints(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& mctpEndpoints)
{
    for (const auto& [path, epData] : mctpEndpoints)
    {
        bool nvmeCap= false;
        size_t eid = 0;
        std::vector<uint8_t> addr;
        auto ep = epData.find(NVMeDevice::mctpEpInterface);
        if ( ep != epData.end())
        {
            const Properties& prop = ep->second;
            auto findEid = prop.find("EID");
            if (findEid == prop.end())
                continue;
            eid = std::get<size_t>(findEid->second);

            auto findTypes = prop.find("SupportedMessageTypes");
            if (findTypes == prop.end())
                continue;
            auto msgTypes = std::get<std::vector<uint8_t>>(findTypes->second);
            std::vector<uint8_t>::iterator it = std::find(msgTypes.begin(), msgTypes.end(), NVME_MI_MSGTYPE_NVME & 0x7F ); 
            if (it != msgTypes.end()) {
                nvmeCap = true;
            }
        }
        auto sockInfo = epData.find("xyz.openbmc_project.Common.UnixSocket");
        if (sockInfo != epData.end())
        {
            const Properties& prop = sockInfo->second;
            auto findAddr = prop.find("Address");
            if (findAddr == prop.end())
                continue;
            addr = std::get<std::vector<uint8_t>>(findAddr->second);
        }
        if (!nvmeCap) {
            continue;
        }
        uint32_t bus = -1;
        auto findBus = epData.find("xyz.openbmc_project.Inventory.Decorator.I2CDevice");
        if (findBus != epData.end())
        {
            const Properties& prop = findBus->second;
            auto find = prop.find("Bus");
            if (find == prop.end())
                continue;
            bus = std::get<uint32_t>(find->second);
        }

        addr.push_back(0);
        if (driveMap.find(eid) == driveMap.end())
        {
            lg2::info("Drive is added on EID: {EID}", "EID", eid);

            std::string p("/xyz/openbmc_project/inventory/drive/");
            p += std::to_string(eid);
            auto DrivePtr = std::make_shared<NVMeDevice>(
                io, objectServer, dbusConnection, eid, bus, std::move(addr), p);

            // put drive object to map in order to implement drive removal.
            driveMap.emplace(eid, DrivePtr);
        }
        else
        {
            lg2::info("Drive has been added on EID: {EID}", "EID", eid);
        }

    }
    // collect inventory data from EM
    collectInventory(dbusConnection);
}

void createDrives(boost::asio::io_service& io,
                   sdbusplus::asio::object_server& objectServer,
                   std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{

    auto getter = std::make_shared<getObjects>(
        dbusConnection,
        std::move([&io, &objectServer, &dbusConnection](
                      const ManagedObjectType& mctpEndpoints) {
            handleMCTPEndpoints(io, objectServer, dbusConnection,
                                       mctpEndpoints);
        }));
    getter->getConfiguration(std::vector<std::string>{
        "xyz.openbmc_project.MCTP.Endpoint",
        "xyz.openbmc_project.Common.UnixSocket",
        "xyz.openbmc_project.Inventory.Decorator.I2CDevice"
        });
}

static void interfaceRemoved(sdbusplus::message::message& message)
{
    if (message.is_method_error())
    {
        lg2::error("interfacesRemoved callback method error");
        return;
    }

    std::string objectName;
    boost::container::flat_map<std::string, std::variant<size_t>> values;
    message.read(objectName, values);

    auto findEid = values.find("EID");
    if (findEid != values.end())
    {
        auto obj = findEid->second;
        auto eid = std::get<size_t>(obj);
        lg2::info("Remove Drive:{EID}.", "EID", eid);
        // Todo: implement it for drive hotplug.
    }
}

int main()
{
    boost::asio::io_service io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(bus, true);
    objectServer.add_manager("/xyz/openbmc_project/inventory/drive");

    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

    io.post([&]() { 
        createDrives(io, objectServer, bus);
        bus->request_name("xyz.openbmc_project.NVMeDevice");
        });

    boost::asio::steady_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> emHandler =
        [&filterTimer, &bus](sdbusplus::message::message&) {
            filterTimer.expires_from_now(std::chrono::seconds(1));

            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }

                if (ec)
                {
                    lg2::error("Error: {MSG}", "MSG", ec.message());
                    return;
                }

                // collect inventory data from EM
                collectInventory(bus);
            });
        };

    auto emIfaceAddedMatch = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*bus),
        "type='signal',member='InterfacesAdded',arg0path='" +
            std::string("/xyz/openbmc_project/inventory/system/nvme") + "/'",
        emHandler);

    matches.emplace_back(std::move(emIfaceAddedMatch));

    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&filterTimer, &io, &objectServer,
         &bus](sdbusplus::message::message&) {
            // this implicitly cancels the timer
            filterTimer.expires_from_now(std::chrono::seconds(1));

            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }

                if (ec)
                {
                    lg2::error("Error: {MSG}", "MSG", ec.message());
                    return;
                }

                createDrives(io, objectServer, bus);
            });
        };

    auto ifaceAddedMatch = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*bus),
        "type='signal',member='InterfacesAdded',arg0path='" +
            std::string(mctpEpsPath) + "/'",
        eventHandler);
    matches.emplace_back(std::move(ifaceAddedMatch));

    // Watch for mctp service to remove configuration interfaces
    // so the corresponding Drives can be removed.
    auto ifaceRemovedMatch = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*bus),
        "type='signal',member='InterfacesRemoved',arg0path='" +
            std::string(mctpEpsPath) + "/'",
        [&filterTimer](sdbusplus::message::message& msg) {
            filterTimer.cancel();
            interfaceRemoved(msg);
        });
    matches.emplace_back(std::move(ifaceRemovedMatch));

    io.run();
}
