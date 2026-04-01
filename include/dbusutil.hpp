#pragma once

#include <boost/system/error_code.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>

const std::string resourceErrorDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

// Drive event resolution strings
const std::string driveInsertedResolution{
    "If the drive is not properly displayed, attempt to refresh the cached data."};
const std::string driveRemovedResolution{
    "If the drive is still displayed, attempt to refresh the cached data."};
const std::string driveFailureResolution{
    "Ensure all cables are properly and securely connected. Ensure all drives "
    "are fully seated. Replace the defective cables, drive, or both."};
const std::string drivePfaResolution{
    "If this drive is not part of a fault-tolerant volume, first back up all "
    "data, then replace the drive and restore all data afterward. If this "
    "drive is part of a fault-tolerant volume, replace this drive as soon as "
    "possible as long as the health is OK"};

// Redfish drive path
const std::string redfishDrivePathPrefix{
    "/redfish/v1/Systems/System_0/Storage/1/Drives/"};

// Firmware update event message IDs
const std::string transferFailed{"Update.1.0.TransferFailed"};
const std::string transferringToComponent{"Update.1.0.TransferringToComponent"};
const std::string verificationFailed{"Update.1.0.VerificationFailed"};
const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};
const std::string awaitToActivate{"Update.1.0.AwaitToActivate"};
const std::string applyFailed{"Update.1.0.ApplyFailed"};
const std::string activateFailed{"Update.1.0.ActivateFailed"};
const std::string targetDetermined{"Update.1.0.TargetDetermined"};
// Drive hot-plug event message IDs
const std::string driveInserted{"StorageDevice.1.0.DriveInserted"};
const std::string driveRemoved{"StorageDevice.1.0.DriveRemoved"};

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

/** @brief Get the D-Bus mutex for protecting D-Bus operations
 *
 * This function returns a reference to a static mutex used to protect D-Bus
 * operations from parallel threads. This prevents race conditions when multiple
 * parallel threads attempt to create log entries simultaneously through a
 * shared D-Bus connection.
 *
 * @return Reference to the static D-Bus mutex
 */
inline std::mutex& getDbusMutex()
{
    static std::mutex dbusMutex;
    return dbusMutex;
}

/** @brief Create the D-Bus log entry for message registry
 *
 *  @param[in] conn - D-Bus connection
 *  @param[in] messageID - Message ID
 *  @param[in] level - Log level
 *  @param[in] arg0 - argument 0
 *  @param[in] arg1 - argument 1
 *  @param[in] resolution - Resolution field
 *  @param[in] ooc - Origin of condition
 *  @param[in] logNamespace - Logging namespace, default is StorageDevice
 */
inline void
    createLogEntry(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                   const std::string& messageID, const Level& level,
                   const std::string& arg0, const std::string& arg1,
                   const std::string& resolution, const std::string& ooc,
                   const std::string& logNamespace = "StorageDevice")
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    addData["REDFISH_ORIGIN_OF_CONDITION"] = ooc;

    if (messageID == resourceErrorDetected || messageID == targetDetermined ||
        messageID == updateSuccessful)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg0 + "," + arg1);
    }
    else if (messageID == transferFailed || messageID == verificationFailed ||
             messageID == applyFailed || messageID == activateFailed ||
             messageID == transferringToComponent ||
             messageID == awaitToActivate)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg1 + "," + arg0);
    }
    else if (messageID == driveInserted || messageID == driveRemoved)
    {
        addData["REDFISH_MESSAGE_ARGS"] = arg0;
    }
    else
    {
        lg2::error("Message Registry messageID is not recognised: {MESSAGEID}",
                   "MESSAGEID", messageID);
        return;
    }

    if (!resolution.empty())
    {
        addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    }

    if (!logNamespace.empty())
    {
        addData["namespace"] = logNamespace;
    }

    auto severity = convertForMessage(level);

    std::lock_guard<std::mutex> lock(getDbusMutex());

    conn->async_method_call(
        [messageID](boost::system::error_code ec) {
        if (ec)
        {
            lg2::warning(
                "Failed to create log entry for message {MESSAGEID}: {ERROR_MESSAGE}",
                "MESSAGEID", messageID, "ERROR_MESSAGE", ec.message());
        }
    }, "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create", messageID, severity,
        addData);
}
