/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 *
 * NVMe FW update handler: monitor nvme-update@.service via systemd JobNew,
 * parse instance (image path, version, object path prefix, EIDs), run
 * download+commit per EID, update Progress and emit FW update events.
 */

#include <libnvme.h>
#include <nvme-mi_config.h>

#include <NVMeDevice.hpp>
#include <NVMeFwUpdateHandler.hpp>
#include <dbusutil.hpp>
#include <phosphor-logging/lg2.hpp>

#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/** Defined in NVMeDeviceMain.cpp; used to resolve EID -> drive for FW update.
 */
extern std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>& getDriveMap();

namespace
{
constexpr size_t fwUpdateChunkSize = 4096;
constexpr uint32_t fwUpdateSlot = 0;
constexpr size_t fwDownloadMaxRetries = 5;
constexpr int progressLogIntervalSec = 20;

struct ParsedNvmeUpdateArgs
{
    std::string imagePath;
    std::string version;
    std::string objectPathPrefix;
    std::vector<uint8_t> eids;
};

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

/** Throttle progress log to once per interval per EID.
 */
bool shouldLogEverySeconds(std::chrono::steady_clock::time_point& lastLogTime,
                           int intervalSec)
{
    auto now = std::chrono::steady_clock::now();
    if (now - lastLogTime >= std::chrono::seconds(intervalSec))
    {
        lastLogTime = now;
        return true;
    }
    return false;
}

/** Replace literal 4-char sequence backslash-x-2-0 with a single space (0x20).
 */
void normalizeInstanceSpaces(std::string& s)
{
    const char space = '\x20';
    const std::string escaped("\\x20"); // backslash, 'x', '2', '0'
    for (std::string::size_type pos = 0;
         (pos = s.find(escaped, pos)) != std::string::npos;)
    {
        s.replace(pos, escaped.size(), 1, space);
        pos += 1;
    }
}

/** Parse instance string from nvme-update@<instance>.service.
 *  Args from code-manager: " <path_with_dashes> <version> <prefix_with_dashes>
 * <eid1> [eid2] ..." Replace '-' with '/' for path and prefix to recover actual
 * paths.
 */
std::optional<ParsedNvmeUpdateArgs>
    parseNvmeUpdateInstance(std::string instance)
{
    normalizeInstanceSpaces(instance);
    std::istringstream iss(instance);
    std::vector<std::string> tokens;
    std::string t;
    while (iss >> t)
    {
        tokens.push_back(std::move(t));
    }
    // Need at least: path, version, prefix, one eid (code-manager may add
    // leading space -> empty token)
    if (tokens.size() < 4)
    {
        return std::nullopt;
    }
    ParsedNvmeUpdateArgs out;
    auto dashToSlash = [](std::string s) {
        std::replace(s.begin(), s.end(), '-', '/');
        return s;
    };
    size_t iPath = 0;
    while (iPath < tokens.size() && tokens[iPath].empty())
    {
        ++iPath;
    }
    if (iPath + 3 > tokens.size())
    {
        return std::nullopt;
    }
    out.imagePath = dashToSlash(tokens[iPath]);
    out.version = tokens[iPath + 1];
    out.objectPathPrefix = dashToSlash(tokens[iPath + 2]);
    for (size_t i = iPath + 3; i < tokens.size(); ++i)
    {
        if (tokens[i].empty())
        {
            continue;
        }
        try
        {
            int eid = std::stoi(tokens[i]);
            if (eid >= 0 && eid <= 255)
            {
                out.eids.push_back(static_cast<uint8_t>(eid));
            }
        }
        catch (...)
        {
            break;
        }
    }
    if (out.eids.empty())
    {
        return std::nullopt;
    }
    return out;
}

std::string driveObjPath(uint8_t eid)
{
    return std::string("/xyz/openbmc_project/inventory/system/nvme/") +
           std::string(drivePrefix) + std::to_string(eid);
}

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
using OperationStatus =
    sdbusplus::xyz::openbmc_project::Common::server::Progress::OperationStatus;

/** Run full FW update for one EID on a dedicated thread: download chunk by
 *  chunk in a loop
 */
void runFwUpdate(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                 const ParsedNvmeUpdateArgs& args, size_t eidIndex)
{
    if (eidIndex >= args.eids.size())
    {
        return;
    }
    uint8_t eid = args.eids[eidIndex];
    auto& driveMap = getDriveMap();
    auto it = driveMap.find(eid);
    if (it == driveMap.end())
    {
        lg2::warning("FW update: no drive for EID {EID}, skipping", "EID",
                     static_cast<int>(eid));
        return;
    }
    std::shared_ptr<NVMeDevice> drive = it->second;
    std::string objPath = driveObjPath(eid);
    std::string deviceInfo = std::string(redfishDrivePathPrefix) +
                             std::string(drivePrefix) + std::to_string(eid);

    drive->setFwUpdateProgress(0, OperationStatus::InProgress);
    logTargetDetermined(conn, deviceInfo, args.version, objPath);

    std::ifstream file(args.imagePath, std::ios::binary);
    if (!file)
    {
        lg2::error("FW update: cannot open image {PATH}", "PATH",
                   args.imagePath);
        drive->setFwUpdateProgress(0, OperationStatus::Failed);
        return;
    }
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    logTransferringToComponent(conn, deviceInfo, args.version, objPath);

    auto intf = drive->getIntf();
    if (!intf)
    {
        lg2::error("FW update: no interface for EID {EID}", "EID",
                   static_cast<int>(eid));
        drive->setFwUpdateProgress(0, OperationStatus::Failed);
        return;
    }

    const auto updateStartTime = std::chrono::steady_clock::now();
    auto lastProgressLogTime = updateStartTime -
                               std::chrono::seconds(progressLogIntervalSec + 1);
    uint32_t offset = 0;
    size_t downloadRetryCount = 0;

    while (offset < fileSize)
    {
        if (shouldLogEverySeconds(lastProgressLogTime, progressLogIntervalSec))
        {
            lg2::info(
                "FW update EID {EID} downloading chunk at offset {OFFSET}",
                "EID", static_cast<int>(eid), "OFFSET", offset);
        }
        size_t toRead = std::min(static_cast<size_t>(fwUpdateChunkSize),
                                 static_cast<size_t>(fileSize - offset));
        std::vector<char> buf(toRead);
        file.seekg(offset, std::ios::beg);
        file.read(buf.data(), static_cast<std::streamsize>(toRead));
        size_t n = file.gcount();
        if (n == 0)
        {
            auto msRead =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - updateStartTime)
                    .count();
            lg2::error("FW update EID {EID} failed after {MS} ms: read error",
                       "EID", static_cast<int>(eid), "MS", msRead);
            drive->setFwUpdateProgress(0, OperationStatus::Failed);
            return;
        }
        buf.resize(n);

        auto p = std::make_shared<
            std::promise<std::pair<std::error_code, nvme_status_field>>>();
        auto fut = p->get_future();
        intf->adminFwDownload(
            eid, offset, static_cast<uint32_t>(n), std::move(buf),
            [p](const std::error_code& ec, nvme_status_field status) {
            p->set_value({ec, status});
        });
        auto [ec, status] = fut.get();

        if (ec)
        {
            if (downloadRetryCount < fwDownloadMaxRetries)
            {
                downloadRetryCount++;
                lg2::warning(
                    "FW download failed at offset {OFFSET} for EID {EID}, retry {RETRY}/{MAX}: {ERR}",
                    "OFFSET", offset, "EID", static_cast<int>(eid), "RETRY",
                    downloadRetryCount, "MAX", fwDownloadMaxRetries, "ERR",
                    ec.message());
                continue;
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - updateStartTime)
                          .count();
            lg2::error(
                "FW update EID {EID} failed after {MS} ms (download at offset {OFFSET} after {MAX} retries): {ERR}",
                "EID", static_cast<int>(eid), "MS", ms, "OFFSET", offset, "MAX",
                fwDownloadMaxRetries, "ERR", ec.message());
            logTransferFailed(conn, deviceInfo, args.version, objPath);
            drive->setFwUpdateProgress(0, OperationStatus::Failed);
            return;
        }
        downloadRetryCount = 0;
        offset += static_cast<uint32_t>(n);
        uint32_t pct = (fileSize != 0U)
                           ? static_cast<uint32_t>(
                                 (static_cast<size_t>(offset) * 90) / fileSize)
                           : 0;
        drive->setFwUpdateProgress(pct, OperationStatus::InProgress);
    }

    logAwaitToActivate(conn, deviceInfo, args.version, objPath);

    auto pCommit = std::make_shared<
        std::promise<std::pair<std::error_code, nvme_status_field>>>();
    auto futCommit = pCommit->get_future();
    intf->adminFwCommit(
        eid, NVME_FW_COMMIT_CA_REPLACE_AND_ACTIVATE, fwUpdateSlot, false,
        [pCommit](const std::error_code& ec, nvme_status_field status) {
        pCommit->set_value({ec, status});
    });
    auto [ecCommit, statusCommit] = futCommit.get();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - updateStartTime)
                  .count();
    if (ecCommit)
    {
        /* "Needs reset" statuses mean the commit succeeded but activation
         * requires a reset — treat them as success, not failure. */
        auto sc = static_cast<uint32_t>(statusCommit);
        bool needsReset = sc == ((NVME_SCT_CMD_SPECIFIC << NVME_SCT_SHIFT) |
                                 NVME_SC_FW_NEEDS_CONV_RESET) ||
                          sc == ((NVME_SCT_CMD_SPECIFIC << NVME_SCT_SHIFT) |
                                 NVME_SC_FW_NEEDS_SUBSYS_RESET) ||
                          sc == ((NVME_SCT_CMD_SPECIFIC << NVME_SCT_SHIFT) |
                                 NVME_SC_FW_NEEDS_RESET);
        if (!needsReset)
        {
            lg2::error(
                "FW update EID {EID} failed after {MS} ms: commit error {EC} status {STATUS}",
                "EID", static_cast<int>(eid), "MS", ms, "EC", ecCommit.value(),
                "STATUS", static_cast<uint32_t>(statusCommit));
            logActivateFailed(conn, deviceInfo, args.version, objPath);
            drive->setFwUpdateProgress(0, OperationStatus::Failed);
            return;
        }
        lg2::info(
            "FW update EID {EID} committed in {MS} ms, activation pending reset (status {STATUS})",
            "EID", static_cast<int>(eid), "MS", ms, "STATUS",
            static_cast<uint32_t>(statusCommit));
    }
    lg2::info("FW update EID {EID} completed in {MS} ms", "EID",
              static_cast<int>(eid), "MS", ms);
    logUpdateSuccessful(conn, deviceInfo, args.version, objPath);
    drive->setFwUpdateProgress(100, OperationStatus::Completed);
}

/** Deduplicate JobNew: avoid running the same nvme-update@instance twice
 *  (e.g. duplicate systemd signals), which would double update time under
 *  the same per-net mutex.
 */
bool shouldSkipDuplicateJob(const std::string& unitId)
{
    using namespace std::chrono;
    static std::mutex dedupMtx;
    static std::string lastUnitId;
    static steady_clock::time_point lastStart = steady_clock::time_point::min();
    constexpr auto dedupWindow = 30s;
    std::lock_guard<std::mutex> lock(dedupMtx);
    auto now = steady_clock::now();
    if (unitId == lastUnitId && (now - lastStart) < dedupWindow)
    {
        lg2::info(
            "Skip duplicate nvme-update JobNew for same unit within window",
            "UNIT", unitId);
        return true;
    }
    lastUnitId = unitId;
    lastStart = now;
    return false;
}

void onNvmeUpdateJobNew(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    sdbusplus::message::message& msg)
{
    uint32_t jobId = 0;
    sdbusplus::message::object_path jobPath;
    std::string unitId;
    try
    {
        msg.read(jobId, jobPath, unitId);
    }
    catch (const std::exception& e)
    {
        lg2::error("JobNew read failed: {ERR}", "ERR", e.what());
        return;
    }
    const std::string prefix(nvmeUpdateServicePrefix);
    const std::string suffix(".service");
    if (unitId.size() < prefix.size() + suffix.size() ||
        !unitId.starts_with(prefix) || !unitId.ends_with(suffix))
    {
        return;
    }
    if (shouldSkipDuplicateJob(unitId))
    {
        return;
    }
    std::string instance = unitId.substr(
        prefix.size(), unitId.size() - prefix.size() - suffix.size());
    auto args = parseNvmeUpdateInstance(instance);
    if (!args)
    {
        lg2::warning("Failed to parse nvme-update instance: {INST}", "INST",
                     instance);
        logNoMatchingDevices(conn);
        return;
    }
    if (args->eids.empty())
    {
        lg2::warning(
            "No matching devices found for nvme-update instance: {INST}",
            "INST", instance);
        logNoMatchingDevices(conn);
        return;
    }
    const ParsedNvmeUpdateArgs& argsCopy = *args;
    for (size_t i = 0; i < argsCopy.eids.size(); ++i)
    {
        std::thread([conn, argsCopy, i]() {
            runFwUpdate(conn, argsCopy, i);
        }).detach();
    }
}

} // anonymous namespace

void startNvmeFwUpdateMonitor(
    const std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    static std::unique_ptr<sdbusplus::bus::match::match> match;
    match = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "type='signal',sender='org.freedesktop.systemd1',"
        "path='/org/freedesktop/systemd1',"
        "interface='org.freedesktop.systemd1.Manager',member='JobNew'",
        [conn](sdbusplus::message::message& msg) {
        onNvmeUpdateJobNew(conn, msg);
    });
    lg2::info("NVMe FW update monitor started");
}
