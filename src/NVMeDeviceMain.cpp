#include <nvme-mi_config.h>

#include <MCTPDiscovery.hpp>
#include <NVMeDevice.hpp>
#include <NVMeFwUpdateHandler.hpp>
#ifdef NVME_MI_SENSORS
#include <sensors/NVMeMiSensorManager.hpp>
#endif
#include <libnvme.h>

#include <boost/asio/steady_timer.hpp>
#include <dbusutil.hpp>
#include <nlohmann/json.hpp>
#ifdef NVME_MI_SENSORS
#include <tal.hpp>
#endif

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

using Json = nlohmann::json;

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

#ifdef NVME_MI_SENSORS
static std::unique_ptr<NVMeMiSensorManager>& getSensorManager()
{
    static std::unique_ptr<NVMeMiSensorManager> sensorManager;
    return sensorManager;
}
#endif

std::set<uint8_t>& getDiscoveredDriveEids()
{
    static std::set<uint8_t> discoveredDriveEids;
    return discoveredDriveEids;
}

bool& getColdRemovalCheckComplete()
{
    static bool coldRemovalCheckComplete = false;
    return coldRemovalCheckComplete;
}

// Forward declarations
static bool isHostOff(const std::shared_ptr<sdbusplus::asio::connection>& conn);
static void markDriveAsRemoved(uint8_t eid);
static std::optional<eid_t> parseEidFromObjectPath(const std::string& path);
static void checkForColdRemovedDrives(
    const std::shared_ptr<sdbusplus::asio::connection>& conn);

static void removeDuplicateDriveStates(Json& driveStates, uint8_t eid)
{
    bool found = false;
    for (auto drive = driveStates.begin(); drive != driveStates.end();)
    {
        if (drive->contains("eid") && (*drive)["eid"] == eid)
        {
            if (found)
            {
                drive = driveStates.erase(drive);
                continue;
            }
            found = true;
        }
        ++drive;
    }
}

struct PendingMCTPEndpoint
{
    eid_t eid = 0;
    uint32_t bus = -1;
    uint32_t net = 0;
    std::vector<uint8_t> addr;
};

static std::string getRedfishDrivePath(const NVMeDevice& drive)
{
    return std::string(redfishDrivePathPrefix) + drive.getDriveName();
}

static void handleEmEndpoints(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& objData,
    const std::vector<PendingMCTPEndpoint>& pendingEndpoints)
{
    uint64_t eid = 0;
    uint64_t bus = -1;

    for (const auto& [path, data] : objData)
    {
        auto ep = data.find("xyz.openbmc_project.Inventory.Item.NVMe");
        if (ep == data.end())
        {
            continue;
        }
        const Properties& nvmeProps = ep->second;
        std::string form;
        std::string driveAssoc;
        std::string locCode;
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
        // Get LocationCode from EntityManager
        ep = data.find("xyz.openbmc_project.Inventory.Decorator.LocationCode");
        if (ep != data.end())
        {
            const Properties& prop = ep->second;
            auto findProp = prop.find("LocationCode");
            if (findProp != prop.end())
            {
                locCode = std::get<std::string>(findProp->second);
            }
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
        auto findName = nvmeProps.find("Name");
        if (findName == nvmeProps.end())
        {
            lg2::warning("Skipping EM NVMe config at {PATH}: missing Name",
                         "PATH", path.str);
            continue;
        }

        const std::string& driveName = std::get<std::string>(findName->second);
        if (driveName.empty())
        {
            lg2::warning("Skipping EM NVMe config at {PATH}: empty Name",
                         "PATH", path.str);
            continue;
        }

        const PendingMCTPEndpoint* endpoint = nullptr;
        for (const PendingMCTPEndpoint& pendingEndpoint : pendingEndpoints)
        {
#ifdef INKERNEL_MCTP
            if (pendingEndpoint.eid == eid)
#else
            if (pendingEndpoint.bus == bus)
#endif
            {
                endpoint = &pendingEndpoint;
                break;
            }
        }
        if (endpoint == nullptr)
        {
            lg2::warning(
                "No NVMe-capable MCTP endpoint found for EM drive {NAME} EID {EID}",
                "NAME", driveName, "EID", eid);
            continue;
        }
#ifndef INKERNEL_MCTP
        eid = endpoint->eid;
#endif

        auto& driveMap = getDriveMap();
        bool newDrive = !driveMap.contains(eid);

        // Track discovered drive EID for cold-removal detection
        getDiscoveredDriveEids().insert(eid);

        if (newDrive)
        {
            lg2::info("Drive is added on EID: {EID}", "EID", eid);
            lg2::info("Found EM NVMe config name: EID {EID} -> {NAME}", "EID",
                      eid, "NAME", driveName);

            std::string p("/xyz/openbmc_project/inventory/system/nvme/");
            p += driveName;
            auto drivePtr = std::make_shared<NVMeDevice>(
                io, objectServer, dbusConnection, eid, endpoint->bus,
                static_cast<int>(endpoint->net), endpoint->addr, p, form,
                driveAssoc, locCode);

            // put drive object to map in order to implement drive removal.
            driveMap.emplace(eid, drivePtr);
        }
        else
        {
            lg2::info("Drive has been added on EID: {EID}", "EID", eid);
#ifdef NVME_MI_SENSORS
            lg2::info(
                "Drive EID {EID} already in driveMap, sensors not re-created",
                "EID", eid);
#endif

            // Clear connectivity degraded flag since InterfacesAdded means
            // endpoint is available (mctpd may not always emit connectivity
            // change signal)
            auto driveIt = driveMap.find(eid);
            if (driveIt != driveMap.end())
            {
                bool wasDegraded = driveIt->second->isConnectivityDegraded();
                driveIt->second->setConnectivityDegraded(false);

                if (wasDegraded)
                {
                    lg2::info(
                        "Drive EID {EID} MCTP endpoint re-discovered, clearing degraded state and resuming polling",
                        "EID", eid);
                }
            }
        }
#ifdef NVME_MI_SENSORS
        if (newDrive)
        {
            // Create sensor manager on first drive add; create sensors for
            // every newly added drive. createSensors() uses cached config when
            // available, or loads EM config async and creates sensors for all
            // pending EIDs.
            if (!getSensorManager())
            {
                getSensorManager() = std::make_unique<NVMeMiSensorManager>(
                    objectServer, dbusConnection);
            }
            getSensorManager()->createSensors(eid);
        }
#endif
    }

    // wait for worker ready to handle NVMe-MI commands.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Continue with drive initialization
    auto& driveMap = getDriveMap();
    for (const auto& [_, context] : driveMap)
    {
        context->initialize();
    }
}

static void collectInventory(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::vector<PendingMCTPEndpoint>& pendingEndpoints)
{
    auto inventoryGetter = std::make_shared<GetObjects>(
        dbusConnection, [&io, &objectServer, &dbusConnection, pendingEndpoints](
                            const ManagedObjectType& nvmeInventory) {
        handleEmEndpoints(io, objectServer, dbusConnection, nvmeInventory,
                          pendingEndpoints);
    });
    inventoryGetter->getConfiguration(std::vector<std::string>{
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
    std::vector<PendingMCTPEndpoint> pendingEndpoints;

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

        addr.push_back(0);
        pendingEndpoints.push_back({eid, bus, net, std::move(addr)});
    }

    collectInventory(io, objectServer, dbusConnection, pendingEndpoints);
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

/**
 * @brief Update or add a single drive's state in the state file
 * Does not affect other drives in the file
 * @param eid Drive EID to update
 */
void updateSingleDriveState(uint8_t eid)
{
    try
    {
        std::string filePath = driveStateFile;
        Json driveStates = Json::array();

        // Read existing state file if it exists
        if (std::filesystem::exists(filePath))
        {
            std::ifstream inputFile(filePath);
            if (inputFile.is_open())
            {
                try
                {
                    inputFile >> driveStates;
                }
                catch (const std::exception& e)
                {
                    lg2::warning("Failed to parse state file: {ERR}", "ERR",
                                 e.what());
                    driveStates = Json::array();
                }
            }
        }

        // Find the drive in driveMap
        auto& driveMap = getDriveMap();
        auto driveIt = driveMap.find(eid);
        if (driveIt == driveMap.end())
        {
            lg2::warning("Drive EID {EID} not found in driveMap", "EID", eid);
            return;
        }

        removeDuplicateDriveStates(driveStates, eid);

        // Build the new drive state
        Json newDriveState;
        newDriveState["eid"] = eid;
        newDriveState["driveName"] = driveIt->second->getDriveName();

        const auto& locCode = driveIt->second->getLocationCode();
        if (!locCode.empty())
        {
            newDriveState["locationCode"] = locCode;
        }

        const auto& serialNum = driveIt->second->serialNumber();
        if (!serialNum.empty())
        {
            newDriveState["serialNumber"] = serialNum;
        }

        newDriveState["connectivity"] = "Available";

        // Find and update existing entry or add new one
        bool found = false;
        for (auto& drive : driveStates)
        {
            if (drive.contains("eid") && drive["eid"] == eid)
            {
                drive = newDriveState;
                found = true;
                lg2::info("Updated state for drive EID {EID}", "EID", eid);
                break;
            }
        }

        if (!found)
        {
            driveStates.push_back(newDriveState);
            lg2::info("Added new state for drive EID {EID}", "EID", eid);
        }

        // Ensure directory exists
        std::filesystem::path dirPath =
            std::filesystem::path(filePath).parent_path();
        if (!dirPath.empty() && !std::filesystem::exists(dirPath))
        {
            std::filesystem::create_directories(dirPath);
        }

        // Write back to file
        std::ofstream outputFile(filePath);
        if (outputFile.is_open())
        {
            outputFile << driveStates.dump(4) << '\n';
        }
        else
        {
            lg2::error("Failed to open state file: {ERR}", "ERR",
                       std::strerror(errno));
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to update single drive state: {ERR}", "ERR",
                   e.what());
    }
}

/**
 * @brief Mark drive as removed in state file
 * Removes serial number to help detect drive swaps properly
 * @param eid Drive EID
 */
static void markDriveAsRemoved(uint8_t eid)
{
    try
    {
        std::string filePath = driveStateFile;
        Json driveStates = Json::array();

        // Read existing state file if it exists
        if (std::filesystem::exists(filePath))
        {
            std::ifstream inputFile(filePath);
            if (inputFile.is_open())
            {
                try
                {
                    inputFile >> driveStates;
                }
                catch (const std::exception& e)
                {
                    lg2::warning("Failed to parse state file: {ERR}", "ERR",
                                 e.what());
                    driveStates = Json::array();
                }
            }
        }

        removeDuplicateDriveStates(driveStates, eid);

        // Find and update the drive entry
        bool found = false;
        for (auto& drive : driveStates)
        {
            if (drive.contains("eid") && drive["eid"] == eid)
            {
                drive["connectivity"] = "Removed";

                // Remove serial number to help detect drive swaps properly
                if (drive.contains("serialNumber"))
                {
                    drive.erase("serialNumber");
                    lg2::info(
                        "Removed serial number for drive EID {EID} marked as Removed",
                        "EID", eid);
                }

                found = true;
                lg2::info("Marked drive EID {EID} as Removed in state file",
                          "EID", eid);
                break;
            }
        }

        // If not found, create new entry
        if (!found)
        {
            Json newDrive;
            newDrive["eid"] = eid;
            newDrive["connectivity"] = "Removed";
            driveStates.push_back(newDrive);
            lg2::info("Created new Removed entry for drive EID {EID}", "EID",
                      eid);
        }

        // Write back to file
        std::filesystem::path dirPath =
            std::filesystem::path(filePath).parent_path();
        if (!dirPath.empty() && !std::filesystem::exists(dirPath))
        {
            std::filesystem::create_directories(dirPath);
        }

        std::ofstream outputFile(filePath);
        if (outputFile.is_open())
        {
            outputFile << driveStates.dump(4) << '\n';
        }
        else
        {
            lg2::error("Failed to open state file: {ERR}", "ERR",
                       std::strerror(errno));
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to mark drive as removed: {ERR}", "ERR", e.what());
    }
}

/**
 * @brief Parse EID from MCTP object path
 * @param path MCTP object path (e.g., "/au/com/codeconstruct/mctp1/200")
 * @return EID value if successful, std::nullopt otherwise
 */
static std::optional<eid_t> parseEidFromObjectPath(const std::string& path)
{
    size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos || lastSlash + 1 == path.size())
    {
        lg2::debug("MCTP path does not end with an EID: {PATH}", "PATH", path);
        return std::nullopt;
    }

    std::string eidStr = path.substr(lastSlash + 1);
    if (eidStr.find_first_not_of("0123456789") != std::string::npos)
    {
        lg2::debug("MCTP path does not end with an EID: {PATH}", "PATH", path);
        return std::nullopt;
    }

    try
    {
        int eidValue = std::stoi(eidStr);
        if (eidValue < 0 || eidValue > 255)
        {
            lg2::warning("EID out of range in path {PATH}: {EID}", "PATH", path,
                         "EID", eidValue);
            return std::nullopt;
        }
        return static_cast<eid_t>(eidValue);
    }
    catch (const std::exception& e)
    {
        lg2::warning("Failed to parse EID from path {PATH}: {ERR}", "PATH",
                     path, "ERR", e.what());
        return std::nullopt;
    }
}

/**
 * @brief Check if host is powered off or transitioning to off
 * @return true if host is off/transitioning to off, false if running
 */
static bool isHostOff(const std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    try
    {
        auto method = conn->new_method_call("xyz.openbmc_project.State.Host",
                                            "/xyz/openbmc_project/state/host0",
                                            "org.freedesktop.DBus.Properties",
                                            "Get");
        method.append("xyz.openbmc_project.State.Host", "CurrentHostState");

        auto reply = conn->call(method);
        std::variant<std::string> hostState;
        reply.read(hostState);

        std::string state = std::get<std::string>(hostState);

        // Return true if host is Off or transitioning to Off
        // xyz.openbmc_project.State.Host.HostState.Off
        // xyz.openbmc_project.State.Host.HostState.TransitioningToOff
        return (state.find("Off") != std::string::npos);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        // Service not available yet during early boot - this is normal
        // ServiceUnknown and NameHasNoOwner both map to ENXIO
        int err = e.get_errno();
        if (err == ENXIO)
        {
            lg2::debug(
                "Host state service not available yet ({ERROR}), assuming host is running",
                "ERROR", e.name());
        }
        else
        {
            lg2::warning("Failed to get host power state: {ERR}, "
                         "assuming host is running",
                         "ERR", e.what());
        }
        // Default to "not off" to be conservative (generate event on
        // uncertainty)
        return false;
    }
    catch (const std::exception& e)
    {
        lg2::warning(
            "Failed to get host power state: {ERR}, assuming host is running",
            "ERR", e.what());
        return false;
    }
}

static void connectivityChanged(
    sdbusplus::message::message& message,
    const std::shared_ptr<sdbusplus::asio::connection>& /* conn */,
    const std::string& mctpPath)
{
    if (message.is_method_error())
    {
        lg2::error("connectivityChanged callback method error");
        return;
    }

    try
    {
        std::string interfaceName;
        std::map<std::string, std::variant<std::string>> changedProperties;
        message.read(interfaceName, changedProperties);

        auto connectivityIt = changedProperties.find("Connectivity");
        if (connectivityIt == changedProperties.end())
        {
            return;
        }

        std::string connectivity =
            std::get<std::string>(connectivityIt->second);

        // Extract EID from MCTP path
        // Path format: /au/com/codeconstruct/mctp1/networks/1/endpoints/211
        auto eidOpt = parseEidFromObjectPath(mctpPath);
        if (!eidOpt.has_value())
        {
            return;
        }
        eid_t eid8 = eidOpt.value();

        // Update drive connectivity state
        auto& driveMap = getDriveMap();
        auto driveIt = driveMap.find(static_cast<uint8_t>(eid8));

        if (driveIt != driveMap.end())
        {
            bool isDegraded = (connectivity != "Available");
            driveIt->second->setConnectivityDegraded(isDegraded);

            if (isDegraded)
            {
                lg2::warning(
                    "Drive EID {EID} MCTP connectivity degraded, stopping sensor polling",
                    "EID", static_cast<int>(eid8));
            }
            else
            {
                lg2::info(
                    "Drive EID {EID} MCTP connectivity restored, resuming sensor polling",
                    "EID", static_cast<int>(eid8));
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception in connectivityChanged: {ERRMSG}", "ERRMSG",
                   e.what());
    }
}

/**
 * @brief Check for cold-removed drives after boot
 * Compares discovered drive EIDs with state file to detect drives that were
 * removed while system was powered off
 */
static void checkForColdRemovedDrives(
    const std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    try
    {
        std::string filePath = driveStateFile;

        // If no state file exists, nothing to check
        if (!std::filesystem::exists(filePath))
        {
            lg2::info(
                "No previous drive state file, no cold-removal check needed");
            getColdRemovalCheckComplete() = true;
            return;
        }

        // Read state file
        std::ifstream inputFile(filePath);
        if (!inputFile.is_open())
        {
            lg2::warning("Cannot open state file for cold-removal check");
            getColdRemovalCheckComplete() = true;
            return;
        }

        Json driveStates;
        try
        {
            inputFile >> driveStates;
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to parse drive state file: {ERR}", "ERR",
                       e.what());
            getColdRemovalCheckComplete() = true;
            return;
        }

        std::set<uint8_t> processedEids;

        // Check each drive in state file
        for (const auto& drive : driveStates)
        {
            if (!drive.contains("eid"))
            {
                continue;
            }

            uint8_t eid = drive["eid"];
            std::string connectivity = drive.contains("connectivity")
                                           ? drive["connectivity"]
                                           : "Available";

            // Only check drives that were Available (not already Removed)
            if (connectivity != "Available")
            {
                continue;
            }

            // If drive was Available but not discovered after boot =
            // cold-removed
            if (!getDiscoveredDriveEids().contains(eid))
            {
                if (!processedEids.insert(eid).second)
                {
                    continue;
                }

                std::string location = drive.contains("locationCode")
                                           ? drive["locationCode"]
                                           : "Unknown Location";
                std::string serialNumber = drive.contains("serialNumber")
                                               ? drive["serialNumber"]
                                               : "Unknown";
                std::string driveName =
                    drive.contains("driveName") ? drive["driveName"] : "";
                std::string redfishPath =
                    driveName.empty()
                        ? ""
                        : std::string(redfishDrivePathPrefix) + driveName;

                lg2::info(
                    "Cold-removal detected: Drive EID {EID} (SN:{SN}, Loc:{LOC}) not present after boot",
                    "EID", static_cast<int>(eid), "SN", serialNumber, "LOC",
                    location);

                createLogEntry(conn, driveRemoved, Level::Critical, location,
                               "", driveRemovedResolution, redfishPath);
                lg2::info(
                    "Generated DriveRemoved event for cold-removed drive EID {EID} at {LOC}",
                    "EID", static_cast<int>(eid), "LOC", location);

                // Mark drive as Removed in state file
                markDriveAsRemoved(eid);
            }
        }

        getColdRemovalCheckComplete() = true;
        lg2::info("Cold-removal detection completed");
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception in checkForColdRemovedDrives: {ERR}", "ERR",
                   e.what());
        getColdRemovalCheckComplete() = true;
    }
}

/**
 * Defer destructor and io.poll to a posted task so D-Bus unregister runs
 * outside any D-Bus callback. Cancel and erase from map (non-post), then
 * post destroy.
 */
static void deferredDestroyDrive(
    boost::asio::io_context& io,
    std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>& driveMap,
    std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>::iterator driveIt)
{
    uint8_t eidVal = driveIt->first;
    driveIt->second->cancelPendingOperations();
    driveIt->second->clearSensorsUpdater();
#ifdef NVME_MI_SENSORS
    if (getSensorManager())
    {
        getSensorManager()->removeSensors(eidVal);
    }
#endif
    auto keepAlive = driveIt->second;
    driveMap.erase(driveIt);
    lg2::info("Drive EID {EID} removed from driveMap, posting destroy", "EID",
              static_cast<int>(eidVal));
    boost::asio::post(io, [keepAlive, &io]() mutable {
        keepAlive.reset();
        io.poll();
    });
}

static void
    interfaceRemoved(sdbusplus::message::message& message,
                     const std::shared_ptr<sdbusplus::asio::connection>& conn,
                     boost::asio::io_context& io)
{
    if (message.is_method_error())
    {
        lg2::error("interfacesRemoved callback method error");
        return;
    }

    sdbusplus::message::object_path objectPath;
    std::vector<std::string> interfacesRemoved;

    try
    {
        message.read(objectPath, interfacesRemoved);

        // Check if MCTP.Endpoint interface is being removed
        bool hasMctpEndpoint = false;
        for (const auto& iface : interfacesRemoved)
        {
            if (iface == "xyz.openbmc_project.MCTP.Endpoint" ||
                iface == "au.com.codeconstruct.MCTP.Endpoint1")
            {
                hasMctpEndpoint = true;
                break;
            }
        }

        if (!hasMctpEndpoint)
        {
            return;
        }

        auto eidOpt = parseEidFromObjectPath(objectPath.str);
        if (!eidOpt.has_value())
        {
            return;
        }
        uint8_t eid = static_cast<uint8_t>(eidOpt.value());
        lg2::info("InterfacesRemoved: path {PATH} EID {EID}", "PATH",
                  objectPath.str, "EID", static_cast<int>(eid));

        // If power off, remove from driveMap but don't generate events
        if (isHostOff(conn))
        {
            auto& driveMap = getDriveMap();
            auto driveIt = driveMap.find(eid);
            if (driveIt != driveMap.end())
            {
                lg2::info(
                    "Drive EID {EID} endpoint removed during host power-off, removing from driveMap",
                    "EID", static_cast<int>(eid));
                deferredDestroyDrive(io, driveMap, driveIt);
            }
            return;
        }

        // Check if EID in drive map
        auto& driveMap = getDriveMap();
        auto driveIt = driveMap.find(eid);
        if (driveIt == driveMap.end())
        {
            lg2::debug("Drive EID {EID} not in driveMap, ignoring removal",
                       "EID", static_cast<int>(eid));
            return;
        }

        // Host is running and drive exists - this is TRUE hot-removal
        lg2::info("Drive EID {EID} physically removed while host running",
                  "EID", static_cast<int>(eid));

        // Get location code before removing the drive
        const auto& locationCode = driveIt->second->getLocationCode();
        auto location = locationCode.empty() ? "Unknown Location"
                                             : std::string(locationCode);

        // Generate DriveRemoved Redfish event (Severity: Critical)
        std::string redfishPath = getRedfishDrivePath(*driveIt->second);
        createLogEntry(conn, driveRemoved, Level::Critical,
                       location, // arg0: location of the drive
                       "",       // arg1 not used for drive events
                       driveRemovedResolution, redfishPath);
        lg2::info(
            "Generated DriveRemoved event for EID {EID} at location {LOC}",
            "EID", static_cast<int>(eid), "LOC", location);

        // Mark drive as Removed in state file (keep the entry for future
        // comparison)
        markDriveAsRemoved(eid);

        deferredDestroyDrive(io, driveMap, driveIt);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error("SdBusError: {ERRMSG}", "ERRMSG", e.what());
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception in interfaceRemoved: {ERRMSG}", "ERRMSG",
                   e.what());
    }
}

int main(int argc, char* argv[])
{
    bool debug = false;
    auto args = std::span<char*>(argv, static_cast<size_t>(argc));
    for (size_t i = 1; i < args.size(); ++i)
    {
        if (std::string_view(args[i]) == "--debug")
        {
            debug = true;
        }
    }

    if (debug)
    {
        NVMeMi::initLogging();
    }

    try
    {
        boost::asio::io_context io;
        auto bus = std::make_shared<sdbusplus::asio::connection>(io);
        sdbusplus::asio::object_server objectServer(bus, true);
        objectServer.add_manager("/xyz/openbmc_project/inventory/system/nvme");
        objectServer.add_manager("/xyz/openbmc_project/sensors");
#ifdef FIRMWARE_INVENTORY
        objectServer.add_manager("/xyz/openbmc_project/software");
#endif
#ifdef NVME_MI_SENSORS
        if (tal::TelemetryAggregator::namespaceInit(tal::ProcessType::Producer,
                                                    "nvmesensor"))
        {
            lg2::info("Successfully registered TAL namespace for NVMe sensors");
        }
        else
        {
            lg2::error("Failed to register TAL namespace for NVMe sensors");
        }
#endif

        std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches;

        boost::asio::post(io, [&]() {
            createDrives(io, objectServer, bus);
            bus->request_name("xyz.openbmc_project.NVMeDevice");
        });

        boost::asio::steady_timer filterTimer(io);
        std::function<void(sdbusplus::message::message&)> emHandler =
            [&filterTimer, &io, &objectServer,
             &bus](sdbusplus::message::message&) {
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

                createDrives(io, objectServer, bus);
            });
        };

        // Add interface for storage inventory
        std::string storagePath =
            "/xyz/openbmc_project/inventory/item/storage/1";
        std::unique_ptr<Storage> storageIface = std::make_unique<Storage>(
            static_cast<sdbusplus::bus_t&>(*bus), storagePath.c_str());
        storageIface->emit_added();

        auto emIfaceAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',member='InterfacesAdded',arg0path='" +
                std::string("/xyz/openbmc_project/inventory/system/nvme") +
                "/'",
            emHandler);

        matches.emplace_back(std::move(emIfaceAddedMatch));

#ifdef NVME_MI_SENSORS
        // Watch for entity-manager sensor config interfaces appearing under
        // the inventory tree.  This handles the race where EM starts late and
        // publishes NVME1000/Nvmem2 configs after drives were already
        // discovered and the initial GetSensorConfiguration call returned
        // empty.  Debounced 1 s to batch rapid EM startup activity.
        // refreshSensors() is a no-op once all drives have sensor contexts, so
        // spurious signals (e.g. fan/temp configs) add no real overhead.
        boost::asio::steady_timer emSensorConfigTimer(io);
        auto emSensorConfigMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',member='InterfacesAdded',"
            "arg0path='/xyz/openbmc_project/inventory/'",
            [&emSensorConfigTimer](sdbusplus::message::message&) {
            emSensorConfigTimer.expires_after(std::chrono::seconds(1));
            emSensorConfigTimer.async_wait(
                [](const boost::system::error_code& ec) {
                if (ec)
                {
                    return;
                }
                if (getSensorManager())
                {
                    lg2::info(
                        "EM inventory InterfacesAdded, refreshing sensor inventory");
                    getSensorManager()->refreshSensors();
                }
            });
        });
        matches.emplace_back(std::move(emSensorConfigMatch));
#endif

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

        auto ifaceAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',member='InterfacesAdded',arg0path='" +
                std::string(mctpEpsPath) + "/'",
            eventHandler);
        matches.emplace_back(std::move(ifaceAddedMatch));

        // Watch for mctp service to remove configuration interfaces
        // so the corresponding Drives can be removed.
        auto ifaceRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',member='InterfacesRemoved',arg0path='" +
                std::string(mctpEpsPath) + "/'",
            [&filterTimer, bus, &io](sdbusplus::message::message& msg) {
            filterTimer.cancel();
            interfaceRemoved(msg, bus, io);
        });
        matches.emplace_back(std::move(ifaceRemovedMatch));

        // Watch for MCTP Connectivity property changes on all endpoints
        // Monitor au.com.codeconstruct.MCTP.Endpoint1 interface
        auto connectivityMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(mctpEpsPath) +
                "',arg0='au.com.codeconstruct.MCTP.Endpoint1'",
            [bus](sdbusplus::message::message& msg) {
            std::string path(msg.get_path());
            connectivityChanged(msg, bus, path);
        });
        matches.emplace_back(std::move(connectivityMatch));

        // Monitor boot progress for cold-removal detection
        // Wait for OSRunning state or configured timeout
        auto bootProgressTimer =
            std::make_shared<boost::asio::steady_timer>(io);
        bootProgressTimer->expires_after(
            std::chrono::seconds(bootProgressTimeout));
        bootProgressTimer->async_wait(
            [bus, bootProgressTimer](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return; // Timer was cancelled (boot reached OSRunning)
            }

            if (!getColdRemovalCheckComplete())
            {
                lg2::info(
                    "Boot progress timeout ({TIMEOUT} seconds), checking for cold-removed drives",
                    "TIMEOUT", bootProgressTimeout);
                checkForColdRemovedDrives(bus);
            }
        });

        auto bootProgressMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',"
            "path='/xyz/openbmc_project/state/host0',"
            "arg0='xyz.openbmc_project.State.Boot.Progress'",
            [bus, bootProgressTimer](sdbusplus::message::message& msg) {
            if (getColdRemovalCheckComplete())
            {
                return; // Already checked
            }

            std::string interfaceName;
            std::map<std::string, std::variant<std::string>> changedProperties;

            try
            {
                msg.read(interfaceName, changedProperties);

                auto it = changedProperties.find("BootProgress");
                if (it != changedProperties.end())
                {
                    std::string bootProgress =
                        std::get<std::string>(it->second);
                    lg2::info("Boot progress changed to: {PROGRESS}",
                              "PROGRESS", bootProgress);

                    // Check if OSRunning state reached
                    // xyz.openbmc_project.State.Boot.Progress.ProgressStages.OSRunning
                    if (bootProgress.find("OSRunning") != std::string::npos)
                    {
                        lg2::info(
                            "OS running state reached, checking for cold-removed drives");
                        bootProgressTimer->cancel();
                        checkForColdRemovedDrives(bus);
                    }
                }
            }
            catch (const std::exception& e)
            {
                lg2::error("Error processing boot progress change: {ERR}",
                           "ERR", e.what());
            }
        });
        matches.emplace_back(std::move(bootProgressMatch));

        // Monitor host power state to clean up drives on power-off
        // NVMe drives are power-on devices and must be reinitialized after
        // power cycle
        auto hostStateMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*bus),
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',"
            "path='/xyz/openbmc_project/state/host0',"
            "arg0='xyz.openbmc_project.State.Host'",
            [bootProgressTimer, &io, &objectServer,
             &bus](sdbusplus::message::message& msg) {
            std::string interfaceName;
            std::map<std::string, std::variant<std::string>> changedProperties;

            try
            {
                msg.read(interfaceName, changedProperties);

                auto it = changedProperties.find("CurrentHostState");
                if (it != changedProperties.end())
                {
                    std::string hostState = std::get<std::string>(it->second);
                    lg2::info("Host state changed to: {STATE}", "STATE",
                              hostState);

                    // Clean up all drives when host powers off. Erase from map
                    // in callback; destroy each drive in a posted task so
                    // D-Bus unregister runs outside this callback.
                    if (hostState.find("Off") != std::string::npos)
                    {
                        lg2::info("Host powered off, cleaning up NVMe drives");

                        auto& driveMap = getDriveMap();
                        if (!driveMap.empty())
                        {
                            // Cancel every endpoint before closing any one of
                            // them so all old work in the shared worker queue
                            // can drain without issuing more transport calls.
                            for (auto& [_, drive] : driveMap)
                            {
                                drive->cancelPendingOperations();
                            }

                            for (auto it = driveMap.begin();
                                 it != driveMap.end();)
                            {
                                if (auto intf = it->second->getIntf(); intf)
                                {
                                    intf->closeEndpoint();
                                }
                                deferredDestroyDrive(io, driveMap, it++);
                            }
                        }

                        // Reset cold-removal check state
                        getColdRemovalCheckComplete() = false;
                        getDiscoveredDriveEids().clear();

                        // Cancel boot progress timer to prevent unnecessary
                        // cold-removal check
                        bootProgressTimer->cancel();
                    }
                    // Re-scan for drives on host running. Handles warm reboot
                    // (ForceWarmReboot/watchdog) where DC power is never cut
                    // and mctpd never removes/re-adds endpoints, so no
                    // InterfacesAdded signal fires to trigger createDrives().
                    else if (hostState.find("HostState.Running") !=
                             std::string::npos)
                    {
                        lg2::info("Host running, re-scanning for NVMe drives");
                        createDrives(io, objectServer, bus);
                    }
                }
            }
            catch (const std::exception& e)
            {
                lg2::error("Error processing host state change: {ERR}", "ERR",
                           e.what());
            }
        });
        matches.emplace_back(std::move(hostStateMatch));

        startNvmeFwUpdateMonitor(bus);

        io.run();
        return 0;
    }
    catch (const std::exception& e)
    {
        lg2::error("Fatal error in main: {ERRMSG}", "ERRMSG", e.what());
        return 1;
    }
}
