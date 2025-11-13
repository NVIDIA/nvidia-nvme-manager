#include <nvme-mi_config.h>

#include <MCTPDiscovery.hpp>
#include <NVMeDevice.hpp>
#include <boost/asio/steady_timer.hpp>

#include <iostream>
#include <optional>
#include <regex>
#include <vector>

#ifdef INKERNEL_MCTP
using eid_t = uint8_t;
#else
using eid_t = size_t;
#endif

#ifdef INKERNEL_MCTP
const constexpr char* mctpEpsPath = "/au/com/codeconstruct/mctp1";
#else
const constexpr char* mctpEpsPath = "/xyz/openbmc_project/mctp";
#endif

std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>& getDriveMap()
{
    static std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>> driveMap{};
    return driveMap;
}

static void handleEmEndpoints(const ManagedObjectType& objData)
{
    std::string form;
    std::string driveAssoc;
    uint64_t eid = 0;
    uint64_t bus = -1;

    for (const auto& [path, data] : objData)
    {
        auto ep = data.find("xyz.openbmc_project.Inventory.Item.NVMe");
        if (ep == data.end())
        {
            continue;
        }
        (void)bus; // avoid unused variable warning
        ep = data.find("xyz.openbmc_project.Inventory.Decorator.I2CDevice");
        if (ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("Bus");
            if (findProp == prop.end())
            {
                continue;
            }
            bus = std::get<uint64_t>(findProp->second);
        }

        ep = data.find("xyz.openbmc_project.MCTP.Endpoint");
        if (ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("EID");
            if (findProp == prop.end())
            {
                continue;
            }
            eid = std::get<uint64_t>(findProp->second);
        }
        ep = data.find("xyz.openbmc_project.Inventory.Item.Drive");
        if (ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("FormFactor");
            if (findProp == prop.end())
            {
                continue;
            }
            form = std::get<std::string>(findProp->second);
        }
        // To support a design that NVMe drives are on a backplane rather than
        // baseboard/DC-SCM. Get the associations from Dbus object, and assign
        // associations for NVMe drive.
        ep = data.find("xyz.openbmc_project.Association.Definitions");
        if (ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("Associations");
            if (findProp != prop.end())
            {
                const auto assocs = std::get<std::vector<
                    std::tuple<std::string, std::string, std::string>>>(
                    findProp->second);
                for (const auto& assoc : assocs)
                {
                    if (std::get<1>(assoc) == "containing")
                    {
                        driveAssoc = std::get<2>(assoc);
                    }
                }
            }
        }
        auto& driveMap = getDriveMap();
        for (const auto& [index, context] : driveMap)
        {
            // update location and formfactor by comparing EID or bus number
            bool shouldUpdate = false;
#ifdef INKERNEL_MCTP
            shouldUpdate = (index == eid);
#else
            (void)eid; // avoid unused variable warning
            shouldUpdate = (context->getI2CBus() == bus);
#endif
            if (!shouldUpdate)
            {
                continue;
            }
            context->updateFormFactor(form);
            if (!driveAssoc.empty())
            {
                context->driveAssociation = driveAssoc;
                context->updateDriveAssociations();
            }
        }
    }

    // wait for worker ready to handle NVMe-MI commands.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto& driveMap = getDriveMap();
    for (const auto& [_, context] : driveMap)
    {
        context->initialize();
    }
}

void collectInventory(
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    auto getter = std::make_shared<GetObjects>(
        dbusConnection, [](const ManagedObjectType& endpoints) {
        handleEmEndpoints(endpoints);
    });
    getter->getConfiguration(std::vector<std::string>{
        "xyz.openbmc_project.Inventory.Item.Drive",
        "xyz.openbmc_project.Inventory.Item.NVMe",
#ifdef INKERNEL_MCTP
        "xyz.openbmc_project.MCTP.Endpoint",
#else
        "xyz.openbmc_project.Inventory.Decorator.I2CDevice",
#endif
        "xyz.openbmc_project.Inventory.Decorator.LocationCode",
        "xyz.openbmc_project.Inventory.Decorator.Location",
        "xyz.openbmc_project.Association.Definitions",
    });
}

static void handleMCTPEndpoints(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& mctpEndpoints)
{
    for (const auto& [path, epData] : mctpEndpoints)
    {
        bool nvmeCap = false;
        std::vector<uint8_t> addr;
        eid_t eid = 0;
        uint32_t net = 0;
        auto ep = epData.find(NVMeDevice::mctpEpInterface);
        if (ep != epData.end())
        {
            const Properties& prop = ep->second;
            auto findEid = prop.find("EID");
            if (findEid == prop.end())
            {
                continue;
            }
            eid = std::get<eid_t>(findEid->second);

            auto findNetworkId = prop.find("NetworkId");
            if (findNetworkId != prop.end())
            {
                net = std::get<uint32_t>(findNetworkId->second);
            }

            auto findTypes = prop.find("SupportedMessageTypes");
            if (findTypes == prop.end())
            {
                continue;
            }
            auto msgTypes = std::get<std::vector<uint8_t>>(findTypes->second);
            std::vector<uint8_t>::iterator it = std::find(
                msgTypes.begin(), msgTypes.end(), NVME_MI_MSGTYPE_NVME & 0x7F);
            if (it != msgTypes.end())
            {
                nvmeCap = true;
            }
        }
        auto sockInfo = epData.find("xyz.openbmc_project.Common.UnixSocket");
        if (sockInfo != epData.end())
        {
            const Properties& prop = sockInfo->second;
            auto findAddr = prop.find("Address");
            if (findAddr == prop.end())
            {
                continue;
            }
            addr = std::get<std::vector<uint8_t>>(findAddr->second);
        }
        if (!nvmeCap)
        {
            lg2::info("No supported NVMe-MI message type on EID: {EID}", "EID",
                      eid);
            continue;
        }
        uint32_t bus = -1;
        auto findBus =
            epData.find("xyz.openbmc_project.Inventory.Decorator.I2CDevice");
        if (findBus != epData.end())
        {
            const Properties& prop = findBus->second;
            auto find = prop.find("Bus");
            if (find == prop.end())
            {
                continue;
            }
            bus = std::get<uint32_t>(find->second);
        }

        auto& driveMap = getDriveMap();
        addr.push_back(0);
        if (!driveMap.contains(eid))
        {
            lg2::info("Drive is added on EID: {EID}", "EID", eid);

            std::string p("/xyz/openbmc_project/inventory/system/nvme/");
            p += std::string(drivePrefix);
            p += std::to_string(eid);
            auto drivePtr = std::make_shared<NVMeDevice>(
                io, objectServer, dbusConnection, eid, bus,
                static_cast<int>(net), std::move(addr), p);

            // put drive object to map in order to implement drive removal.
            driveMap.emplace(eid, drivePtr);
        }
        else
        {
            lg2::info("Drive has been added on EID: {EID}", "EID", eid);
        }
    }
    // collect inventory data from EM
    collectInventory(dbusConnection);
}

void createDrives(boost::asio::io_context& io,
                  sdbusplus::asio::object_server& objectServer,
                  std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    auto getter = std::make_shared<GetObjects>(
        dbusConnection, [&io, &objectServer, &dbusConnection](
                            const ManagedObjectType& mctpEndpoints) {
        handleMCTPEndpoints(io, objectServer, dbusConnection, mctpEndpoints);
    });
#ifdef INKERNEL_MCTP
    getter->getConfiguration(
        std::vector<std::string>{"xyz.openbmc_project.MCTP.Endpoint",
                                 "au.com.codeconstruct.MCTP.Endpoint1"});
#else
    getter->getConfiguration(std::vector<std::string>{
        "xyz.openbmc_project.MCTP.Endpoint",
        "xyz.openbmc_project.Common.UnixSocket",
        "xyz.openbmc_project.Inventory.Decorator.I2CDevice"});
#endif
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

    try
    {
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
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error("SdBusError: {ERRMSG}", "ERRMSG", e.what());
    }
}

int main()
{
    try
    {
        boost::asio::io_context io;
        auto bus = std::make_shared<sdbusplus::asio::connection>(io);
        sdbusplus::asio::object_server objectServer(bus, true);
        objectServer.add_manager("/xyz/openbmc_project/inventory/system/nvme");
#ifdef FIRMWARE_INVENTORY
        objectServer.add_manager("/xyz/openbmc_project/software");
#endif

        std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

        boost::asio::post(io, [&]() {
            createDrives(io, objectServer, bus);
            bus->request_name("xyz.openbmc_project.NVMeDevice");
        });

        boost::asio::steady_timer filterTimer(io);
        std::function<void(sdbusplus::message::message&)> emHandler =
            [&filterTimer, &bus](sdbusplus::message::message&) {
            filterTimer.expires_after(std::chrono::seconds(1));

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

        // Add interface for storage inventory
        std::string storagePath =
            "/xyz/openbmc_project/inventory/item/storage/1";
        std::unique_ptr<Storage> storageIface = std::make_unique<Storage>(
            static_cast<sdbusplus::bus::bus&>(*bus), storagePath.c_str());
        storageIface->emit_added();

        auto emIfaceAddedMatch = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*bus),
            "type='signal',member='InterfacesAdded',arg0path='" +
                std::string("/xyz/openbmc_project/inventory/system/nvme") +
                "/'",
            emHandler);

        matches.emplace_back(std::move(emIfaceAddedMatch));

        boost::asio::steady_timer debounceTimer(io);
        std::function<void(sdbusplus::message::message&)> eventHandler =
            [&debounceTimer, &io, &objectServer,
             &bus](sdbusplus::message::message&) {
            // this implicitly cancels the timer
            debounceTimer.expires_after(std::chrono::seconds(1));

            debounceTimer.async_wait([&](const boost::system::error_code& ec) {
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
        return 0;
    }
    catch (const std::exception& e)
    {
        lg2::error("Fatal error in main: {ERRMSG}", "ERRMSG", e.what());
        return 1;
    }
}
