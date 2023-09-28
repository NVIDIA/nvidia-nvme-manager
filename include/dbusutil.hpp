#pragma once

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

const std::string resourceErrorDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
/** @brief Create the D-Bus log entry for message registry
 *
 *  @param[in] messageID - Message ID
 *  @param[in] arg0 - argument 0
 *  @param[in] arg1 - argument 1
 *  @param[in] resolution - Resolution field
 *  @param[in] logNamespace - Logging namespace, default is FWUpdate
 */
inline void createLogEntry(std::shared_ptr<sdbusplus::asio::connection>& conn,
                           const std::string& messageID,
                           const Level &level,
                           const std::string& arg0, const std::string& arg1,
                           const std::string& resolution,
                           const std::string& ooc,
                           const std::string logNamespace = "StorageDevice")
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    addData["REDFISH_ORIGIN_OF_CONDITION"] = ooc;

    if (messageID == resourceErrorDetected)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg0 + "," + arg1);
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

    auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            level);
    conn->async_method_call(
        [](boost::system::error_code ec) {
            if (ec)
            {
                lg2::error(
                    "error while logging message registry: {ERROR_MESSAGE}",
                    "ERROR_MESSAGE", ec.message());
                return;
            }
        },
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create", messageID, severity,
        addData);
    return;
}