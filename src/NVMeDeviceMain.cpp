#include <NVMeDevice.hpp>
#include <MCTPDiscovery.hpp>
#include <boost/asio/steady_timer.hpp>

#include <iostream>
#include <optional>
#include <regex>
#include <vector>

static constexpr bool debug = true;

const constexpr char* mctpEpsPath = "/xyz/openbmc_project/mctp";

//static NVMEMap nvmeDeviceMap;

static void handleMCTPEndpoints(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& mctpEndpoints)
{
    for (const auto& [path, epData] : mctpEndpoints)
    {
        int nvmeCap= 0;
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
                nvmeCap = 1;
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

        addr.push_back(0);
        std::shared_ptr<NVMeDevice> DrivePtr = std::make_shared<NVMeDevice>(
             io, objectServer, dbusConnection, eid, std::move(addr), std::string("/xyz/openbmc_project/inventory/drive/1"));

        DrivePtr->initialize();
        DrivePtr->pollDrive();

    }
    /*
    for (const auto& [_, context] : nvmeDeviceMap)
    {
        context->pollNVMeDevices();
    }*/
}

void createDrives(boost::asio::io_service& io,
                   sdbusplus::asio::object_server& objectServer,
                   std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{

    auto getter = std::make_shared<getMctpEpInfo>(
        dbusConnection,
        std::move([&io, &objectServer, &dbusConnection](
                      const ManagedObjectType& mctpEndpoints) {
            handleMCTPEndpoints(io, objectServer, dbusConnection,
                                       mctpEndpoints);
        }));
    getter->getConfiguration(std::vector<std::string>{
        "xyz.openbmc_project.MCTP.Endpoint",
        "xyz.openbmc_project.Common.UnixSocket"
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
        // remove the drive by Eid
    }
}

int main()
{
    boost::asio::io_service io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    bus->request_name("xyz.openbmc_project.NVMeDevice");
    sdbusplus::asio::object_server objectServer(bus, true);
    objectServer.add_manager("/xyz/openbmc_project/drive");

    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

    io.post([&]() { createDrives(io, objectServer, bus);});

    // need to change
    boost::asio::steady_timer filterTimer(io);
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
                    std::cerr << "Error: " << ec.message() << "\n";
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
