#include <libnvme-mi.h>
#include <syslog.h>

#include <boost/asio/io_context.hpp>
#include <dbusutil.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <thread>
#include <vector>

// Undefine problematic macros that conflict with C++ standard library
#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

PHOSPHOR_LOG2_USING;

constexpr size_t firmwareDownloadSize = 4096; // 4KB chunks
constexpr uint32_t firmwareSlot = 0;          // Use slot 0 for firmware update

// Retry configuration for endpoint scanning
constexpr int maxEndpointScanRetries = 3;
constexpr int retryDelayMs = 100; // 100ms delay between retries

// Retry configuration for firmware download
constexpr int maxFirmwareDownloadRetries = 3;
constexpr int retryDelayMsFirmware = 100; // 100ms delay between retries

// Log completion timeout
constexpr int logCompletionTimeoutMs =
    200; // Time to wait for async D-Bus log calls to complete

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

// Additional firmware update event logging functions
void logTransferFailed(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                       const std::string& deviceInfo,
                       const std::string& version,
                       const std::string& objectPath)
{
    std::string resolution =
        "Check network connectivity and device availability. "
        "Ensure the firmware file is accessible and not corrupted.";
    createLogEntry(conn, transferFailed, Level::Error, deviceInfo, version,
                   resolution, objectPath, "FWUpdate");
}

void logTransferringToComponent(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& deviceInfo, const std::string& version,
    const std::string& objectPath)
{
    createLogEntry(conn, transferringToComponent, Level::Informational,
                   deviceInfo, version, "", objectPath, "FWUpdate");
}

void logVerificationFailed(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& deviceInfo, const std::string& version,
    const std::string& objectPath)
{
    std::string resolution =
        "Verify the firmware file integrity and compatibility. "
        "Ensure the firmware is signed and compatible with the device.";
    createLogEntry(conn, verificationFailed, Level::Error, deviceInfo, version,
                   resolution, objectPath, "FWUpdate");
}

void logUpdateSuccessful(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& deviceInfo, const std::string& version,
    const std::string& objectPath)
{
    createLogEntry(conn, updateSuccessful, Level::Informational, deviceInfo,
                   version, "", objectPath, "FWUpdate");
}

void logAwaitToActivate(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& deviceInfo, const std::string& version,
    const std::string& objectPath)
{
    createLogEntry(conn, awaitToActivate, Level::Informational, deviceInfo,
                   version, "", objectPath, "FWUpdate");
}

void logApplyFailed(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                    const std::string& deviceInfo, const std::string& version,
                    const std::string& objectPath)
{
    std::string resolution =
        "Check device status and available space. "
        "Ensure the device is not in use and has sufficient storage.";
    createLogEntry(conn, applyFailed, Level::Error, deviceInfo, version,
                   resolution, objectPath, "FWUpdate");
}

void logActivateFailed(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                       const std::string& deviceInfo,
                       const std::string& version,
                       const std::string& objectPath)
{
    std::string resolution =
        "Check device compatibility and firmware validation. "
        "Ensure the firmware is compatible and properly signed.";
    createLogEntry(conn, activateFailed, Level::Error, deviceInfo, version,
                   resolution, objectPath, "FWUpdate");
}

void logTargetDetermined(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& deviceInfo, const std::string& version,
    const std::string& objectPath)
{
    createLogEntry(conn, targetDetermined, Level::Informational, deviceInfo,
                   version, "", objectPath, "FWUpdate");
}

void logNoMatchingDevices(
    const std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    std::string serviceName = "Firmware Update Service";
    std::string errorDescription = "No Matching Devices";
    std::string resolution =
        "Verify the FW package has devices that are listed in the"
        " Redfish FW Inventory";
    createLogEntry(conn, resourceErrorDetected, Level::Error, serviceName,
                   errorDescription, resolution, "", "FWUpdate");
}

// Helper function to ensure async log entries complete before exit
// Processes the I/O context for a reasonable duration to allow async
// D-Bus calls to complete
void ensureLogCompletion(const std::shared_ptr<boost::asio::io_context>& io)
{
    // run_for() processes handlers for the specified duration
    // This gives async D-Bus calls time to complete
    io->run_for(std::chrono::milliseconds(logCompletionTimeoutMs));
}

void printUsage(const char* programName)
{
    std::cout
        << "Usage: " << programName
        << " <firmware_path> <version> <object_path_prefix> <eid1> [eid2] [eid3] ... [-v]\n"
        << "\n"
        << "Arguments:\n"
        << "  firmware_path      Path to the firmware binary file\n"
        << "  version           Version string (e.g., GB232)\n"
        << "  object_path_prefix Inventory object path prefix\n"
        << "  eid1, eid2...     MCTP Endpoint IDs of NVMe devices to update\n"
        << "  -v                Enable verbose output\n"
        << "\n"
        << "Examples:\n"
        << "  " << programName
        << " /path/to/firmware.bin GB232 /xyz/openbmc_project/inventory/system/chassis/motherboard/drive 200 201 202\n"
        << "  " << programName
        << " /path/to/firmware.bin GB232 /xyz/openbmc_project/inventory/system/chassis/motherboard/drive 200 201 202 -v\n"
        << "\n"
        << "This tool will:\n"
        << "  1. Open MCTP connections to all specified NVMe devices\n"
        << "  2. Download the firmware file to all devices in parallel\n"
        << "  3. Commit the firmware to activate it on all devices\n"
        << "  4. Report success or failure for each device\n";
}

// Function to update firmware for a single device
bool updateFirmwareForDevice(
    const std::string& filename, const std::string& version, uint8_t eid,
    int net, bool verbose,
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& objectPathPrefix)
{
    std::string deviceInfo = PLATFORM_DRIVE_PREFIX + std::to_string(eid);
    std::string objectPath = objectPathPrefix + std::to_string(eid);
    nvme_mi_ep_t ep = nullptr;

    lg2::info(
        "Starting firmware update for EID: {EID}, Version: {VERSION}, Net: {NET}, Verbose: {VERBOSE}",
        "EID", eid, "VERSION", version, "NET", net, "VERBOSE", verbose);

    // Log target determined event
    if (conn)
    {
        logTargetDetermined(conn, deviceInfo, version, objectPath);
    }

    // Create NVMe-MI root for this device
    nvme_root_t root = nvme_mi_create_root(stderr, LOG_DEBUG);
    if (root == nullptr)
    {
        lg2::error("Failed to create NVMe-MI root for EID {EID}", "EID", eid);
        return false;
    }

    // Open MCTP connection
#ifdef INKERNEL_MCTP
    ep = nvme_mi_open_mctp(root, net, eid);
#endif

    if (ep == nullptr)
    {
        lg2::error("Failed to open MCTP connection for EID {EID}", "EID", eid);
        nvme_mi_free_root(root);
        return false;
    }

    if (verbose)
    {
        lg2::info("Successfully opened MCTP connection for EID {EID}", "EID",
                  eid);
    }

    // Scan for endpoints with retry logic
    int err = 0;
    for (int retry = 0; retry <= maxEndpointScanRetries; ++retry)
    {
        err = nvme_mi_scan_ep(ep, false);
        if (err == 0)
        {
            // Success, break out of retry loop
            break;
        }

        if (retry < maxEndpointScanRetries)
        {
            lg2::info("errno:{ERRNO} {ERR}", "ERRNO", errno, "ERR", err);
            lg2::info(
                "Endpoint scan attempt {RETRY} failed for EID {EID}, retrying in {DELAY}ms",
                "RETRY", retry + 1, "EID", eid, "DELAY", retryDelayMs);

            std::this_thread::sleep_for(
                std::chrono::milliseconds(retryDelayMs));
        }
    }

    if (err != 0)
    {
        lg2::error(
            "Failed to scan MCTP endpoints for EID {EID} after {RETRIES} attempts: {ERROR}",
            "EID", eid, "RETRIES", maxEndpointScanRetries + 1, "ERROR", err);
        nvme_mi_close(ep);
        nvme_mi_free_root(root);
        return false;
    }

    if (verbose)
    {
        lg2::info("Successfully scanned MCTP endpoints for EID {EID}", "EID",
                  eid);
    }

    // Get the first controller from the endpoint (discovered by scan)
    nvme_mi_ctrl_t ctrl = nvme_mi_first_ctrl(ep);
    if (ctrl == nullptr)
    {
        lg2::error("No controllers found for EID {EID}", "EID", eid);
        nvme_mi_close(ep);
        nvme_mi_free_root(root);
        return false;
    }

    // Print controller ID (should be the actual NVMe controller ID, not EID)
    __u16 ctrlId = nvme_mi_ctrl_id(ctrl);
    lg2::info("EID {EID} has Controller ID: {CTRLID}", "EID", eid, "CTRLID",
              ctrlId);

    // Open firmware file
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        lg2::error("Failed to open firmware file: {FILENAME}", "FILENAME",
                   filename);
        nvme_mi_close_ctrl(ctrl);
        nvme_mi_close(ep);
        nvme_mi_free_root(root);
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (verbose)
    {
        lg2::info("Firmware file size for EID {EID}: {SIZE} bytes", "EID", eid,
                  "SIZE", fileSize);
    }

    // Log transfer start
    if (conn)
    {
        logTransferringToComponent(conn, deviceInfo, version, objectPath);
    }

    // Download firmware in chunks
    std::vector<uint8_t> buffer(firmwareDownloadSize);
    uint32_t offset = 0;
    bool downloadSuccess = true;

    while (file.good() && offset < fileSize)
    {
        // Read chunk from file
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        file.read(reinterpret_cast<char*>(buffer.data()), firmwareDownloadSize);
        size_t bytesRead = file.gcount();

        if (bytesRead == 0)
        {
            break;
        }

        if (verbose)
        {
            lg2::info(
                "Downloading firmware chunk for EID {EID} at offset {OFFSET}, size: {SIZE}",
                "EID", eid, "OFFSET", offset, "SIZE", bytesRead);
        }

        struct nvme_fw_download_args args = {};
        args.args_size = sizeof(args);
        args.result = nullptr;
        args.offset = offset;
        args.data_len = static_cast<__u32>(bytesRead);
        args.data = buffer.data();

        // Firmware download with retry logic
        err = 0;
        for (int retry = 0; retry <= maxFirmwareDownloadRetries; ++retry)
        {
            err = nvme_mi_admin_fw_download(ctrl, &args);
            if (err == 0)
            {
                // Success, break out of retry loop
                break;
            }

            if (retry < maxFirmwareDownloadRetries)
            {
                lg2::info(
                    "Firmware download attempt {RETRY} failed for EID {EID} at offset {OFFSET}, retrying in {DELAY}ms",
                    "RETRY", retry + 1, "EID", eid, "OFFSET", offset, "DELAY",
                    retryDelayMsFirmware);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(retryDelayMsFirmware));
            }
        }

        if (err != 0)
        {
            lg2::error(
                "Firmware download failed for EID {EID} at offset {OFFSET} after {RETRIES} attempts: {ERROR} {ERRNO}",
                "EID", eid, "OFFSET", offset, "RETRIES",
                maxFirmwareDownloadRetries + 1, "ERROR", err, "ERRNO", errno);
            downloadSuccess = false;
            break;
        }

        offset += bytesRead;
    }

    file.close();

    if (!downloadSuccess)
    {
        lg2::error("Firmware download failed for EID {EID}", "EID", eid);

        // Log transfer failure event
        if (conn)
        {
            logTransferFailed(conn, deviceInfo, version, objectPath);
        }

        nvme_mi_close_ctrl(ctrl);
        nvme_mi_close(ep);
        nvme_mi_free_root(root);
        return false;
    }

    lg2::info("Firmware download completed successfully for EID {EID}", "EID",
              eid);

    // Log await to activate
    if (conn)
    {
        logAwaitToActivate(conn, deviceInfo, version, objectPath);
    }

    // Commit firmware
    lg2::info("Committing firmware to slot {SLOT} for EID {EID}", "SLOT",
              firmwareSlot, "EID", eid);

    struct nvme_fw_commit_args commitArgs = {};
    commitArgs.args_size = sizeof(commitArgs);
    commitArgs.fd = -1;
    commitArgs.timeout = 0;
    commitArgs.result = nullptr;
    commitArgs.slot = firmwareSlot;
    commitArgs.action = NVME_FW_COMMIT_CA_REPLACE_AND_ACTIVATE;
    commitArgs.bpid = false;
    err = nvme_mi_admin_fw_commit(ctrl, &commitArgs);
    if (err != 0)
    {
        lg2::error("Firmware commit failed for EID {EID}: {ERROR}", "EID", eid,
                   "ERROR", err);

        // Log apply/activate failure event
        if (conn)
        {
            logApplyFailed(conn, deviceInfo, version, objectPath);
        }

        nvme_mi_close_ctrl(ctrl);
        nvme_mi_close(ep);
        nvme_mi_free_root(root);
        return false;
    }

    lg2::info("Firmware commit completed successfully for EID {EID}", "EID",
              eid);

    // Log firmware update completion event
    if (conn)
    {
        logUpdateSuccessful(conn, deviceInfo, version, objectPath);
    }

    // Cleanup
    nvme_mi_close_ctrl(ctrl);
    nvme_mi_close(ep);
    nvme_mi_free_root(root);

    return true;
}

int main(int argc, char* argv[])
{
    try
    {
        std::string filename;
        std::string version;
        std::string objectPathPrefix;
        std::vector<uint8_t> eids;
        bool verbose = false;
        bool help = false;

        // Create D-Bus connection for event logging
        auto io = std::make_shared<boost::asio::io_context>();
        auto conn = std::make_shared<sdbusplus::asio::connection>(*io);
        conn->request_name("xyz.openbmc_project.NVMeFwUpdate");

        // Convert argv to span to avoid pointer arithmetic warnings
        std::span<char*> args(argv, argc);

        // Parse command line arguments
        for (size_t i = 1; i < args.size(); i++)
        {
            if (strcmp(args[i], "-h") == 0 || strcmp(args[i], "--help") == 0)
            {
                help = true;
            }
            else if (strcmp(args[i], "-v") == 0 ||
                     strcmp(args[i], "--verbose") == 0)
            {
                verbose = true;
            }
            else if (filename.empty())
            {
                filename = args[i];
            }
            else if (version.empty())
            {
                version = args[i];
            }
            else if (objectPathPrefix.empty())
            {
                objectPathPrefix = args[i];
            }
            else
            {
                // Parse EID
                try
                {
                    uint8_t eid = static_cast<uint8_t>(std::stoi(args[i]));
                    eids.push_back(eid);
                }
                catch (const std::exception& e)
                {
                    lg2::error("Invalid EID: {EID}", "EID", args[i]);
                    printUsage(args[0]);
                    return 1;
                }
            }
        }

        if (help)
        {
            printUsage(args[0]);
            return 0;
        }

        if (eids.empty())
        {
            lg2::error(
                "Error: At least one EID is required - no matching devices found");

            // Create Redfish event log for no matching devices
            logNoMatchingDevices(conn);
            ensureLogCompletion(io);

            return 1;
        }

        if (filename.empty() || version.empty() || objectPathPrefix.empty())
        {
            lg2::error(
                "Error: firmware_path, version, and object_path_prefix are required");
            printUsage(args[0]);
            return 1;
        }

        // Check if firmware file exists
        std::ifstream testFile(filename);
        if (!testFile.good())
        {
            lg2::error(
                "Error: Firmware file '{FILENAME}' does not exist or is not readable",
                "FILENAME", filename);
            return 1;
        }
        testFile.close();

        // Set up logging
        if (verbose)
        {
            lg2::info("Verbose logging enabled");
        }

        lg2::info("Starting parallel NVMe firmware update tool");
        lg2::info("Firmware file: {FILENAME}", "FILENAME", filename);
        lg2::info("Version: {VERSION}", "VERSION", version);
        lg2::info("Object path prefix: {PREFIX}", "PREFIX", objectPathPrefix);

        // Print EIDs
        std::ostringstream eidStream;
        for (size_t i = 0; i < eids.size(); ++i)
        {
            if (i > 0)
            {
                eidStream << " ";
            }
            eidStream << static_cast<int>(eids[i]);
        }
        lg2::info("Target EIDs: {EIDS}", "EIDS", eidStream.str());

        // Launch parallel firmware updates using std::async
        std::vector<std::future<bool>> futures;

        futures.reserve(eids.size());
        for (uint8_t eid : eids)
        {
            // Launch task in a separate thread
            futures.push_back(std::async(std::launch::async,
                                         [filename, version, eid, verbose, conn,
                                          objectPathPrefix]() mutable {
                return updateFirmwareForDevice(filename, version, eid, 1,
                                               verbose, conn, objectPathPrefix);
            }));
        }

        // Wait for all tasks to complete and collect results
        std::vector<bool> results;
        results.reserve(futures.size());
        for (auto& future : futures)
        {
            results.push_back(future.get());
        }

        // Report results
        bool overallSuccess = true;
        for (size_t i = 0; i < eids.size(); ++i)
        {
            if (results[i])
            {
                lg2::info(
                    "Firmware update completed successfully for EID {EID}",
                    "EID", eids[i]);
                lg2::info("EID {EID}: SUCCESS", "EID",
                          static_cast<int>(eids[i]));
            }
            else
            {
                lg2::error("Firmware update failed for EID {EID}", "EID",
                           eids[i]);
                lg2::error("EID {EID}: FAILED", "EID",
                           static_cast<int>(eids[i]));
                overallSuccess = false;
            }
        }

        // Ensure all async log entries complete before exit
        ensureLogCompletion(io);

        if (overallSuccess)
        {
            lg2::info("All firmware updates completed successfully");
            return 0;
        }

        lg2::error("Some firmware updates failed");
        return 1;
    }
    catch (const std::exception& e)
    {
        lg2::error("Error: {ERROR}", "ERROR", e.what());
        return 1;
    }
}
