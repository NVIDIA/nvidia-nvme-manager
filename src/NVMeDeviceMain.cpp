#include <NVMeDevice.hpp>
#include <MCTPDiscovery.hpp>
#include <boost/asio/deadline_timer.hpp>

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
        DrivePtr->pollDevices();

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
        std::cerr << "interfacesRemoved callback method error\n";
        return;
    }

    sdbusplus::message::object_path path;
    std::vector<std::string> interfaces;

    message.read(path, interfaces);
    //free NVMe drive instances

 
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.NVMeDevice");
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/drive");

    io.post([&]() { createDrives(io, objectServer, systemBus) ;std::cout <<"io post\n";});

    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&filterTimer, &io, &objectServer,
         &systemBus](sdbusplus::message::message&) {
            // this implicitly cancels the timer
            filterTimer.expires_from_now(boost::posix_time::seconds(1));

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

                std::cout <<"filter timeer\n";
                createDrives(io, objectServer, systemBus);
            });
        };

    sdbusplus::bus::match::match configMatch(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='PropertiesChanged',path_namespace='" +
            std::string(mctpEpsPath) + "',arg0namespace='" +
            std::string(NVMeDevice::mctpEpInterface) + "'",
        eventHandler);

    // Watch for mctp service to remove configuration interfaces
    // so the corresponding Drives can be removed.
    auto ifaceRemovedMatch = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='InterfacesRemoved',arg0path='" +
            std::string(mctpEpsPath) + "/'",
        [](sdbusplus::message::message& msg) {
            interfaceRemoved(msg);
        });

    io.run();
}
