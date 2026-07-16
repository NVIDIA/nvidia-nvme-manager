/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 *
 * NVMe FW update handler: monitor nvme-update@.service via systemd JobNew,
 * read request env files (image path, version, object path prefix, drive
 * targets), run download+commit per drive, update Progress and emit FW update
 * events.
 */

#include <libnvme.h>
#include <nvme-mi_config.h>

#include <NVMeDevice.hpp>
#include <NVMeFwUpdateHandler.hpp>
#include <dbusutil.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

/** Defined in NVMeDeviceMain.cpp; used to resolve EID -> drive for FW update.
 */
extern std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>& getDriveMap();

namespace
{
constexpr size_t fwUpdateChunkSize = 4096;
constexpr uint32_t fwUpdateSlot = 0;
constexpr size_t fwDownloadMaxRetries = 5;
constexpr size_t fwCommitResetRestartMaxRetries = 1;
constexpr int progressLogIntervalSec = 20;
constexpr uint32_t nvmeStatusFieldMask = (NVME_SCT_MASK << NVME_SCT_SHIFT) |
                                         (NVME_SC_MASK << NVME_SC_SHIFT);
constexpr const char* systemdService = "org.freedesktop.systemd1";
constexpr const char* dbusPropertiesInterface =
    "org.freedesktop.DBus.Properties";
constexpr const char* systemdJobInterface = "org.freedesktop.systemd1.Job";
constexpr const char* systemdJobTypeProperty = "JobType";

struct ParsedNvmeUpdateArgs
{
    std::string imagePath;
    std::string version;
    std::string objectPathPrefix;
    std::vector<std::string> targets;
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

std::string trim(const std::string& s)
{
    const char* whitespace = " \t\r\n";
    const auto first = s.find_first_not_of(whitespace);
    if (first == std::string::npos)
    {
        return "";
    }
    const auto last = s.find_last_not_of(whitespace);
    return s.substr(first, last - first + 1);
}

std::string unquoteEnvValue(std::string value)
{
    value = trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    {
        return value;
    }

    std::string out;
    bool escaped = false;
    for (size_t i = 1; i + 1 < value.size(); ++i)
    {
        const char c = value[i];
        if (escaped)
        {
            out += c;
            escaped = false;
        }
        else if (c == '\\')
        {
            escaped = true;
        }
        else
        {
            out += c;
        }
    }
    if (escaped)
    {
        out += '\\';
    }
    return out;
}

std::vector<std::string> splitTargets(const std::string& targetList)
{
    std::istringstream iss(targetList);
    std::vector<std::string> targets;
    std::string target;
    while (iss >> target)
    {
        targets.push_back(std::move(target));
    }
    return targets;
}

std::filesystem::path requestFilePath(const std::string& requestId)
{
    return std::filesystem::path(nvmeUpdateRequestDir) / (requestId + ".env");
}

std::optional<ParsedNvmeUpdateArgs>
    parseNvmeUpdateRequest(const std::string& requestId)
{
    std::ifstream envFile(requestFilePath(requestId));
    if (!envFile)
    {
        lg2::warning("Failed to open nvme-update request file for {REQUEST}",
                     "REQUEST", requestId);
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> env;
    std::string line;
    while (std::getline(envFile, line))
    {
        line = trim(line);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        env.emplace(trim(line.substr(0, eq)),
                    unquoteEnvValue(line.substr(eq + 1)));
    }

    ParsedNvmeUpdateArgs out;
    if (auto it = env.find("FW_IMAGE"); it != env.end())
    {
        out.imagePath = it->second;
    }
    if (auto it = env.find("FW_VERSION"); it != env.end())
    {
        out.version = it->second;
    }
    if (auto it = env.find("FW_PREFIX"); it != env.end())
    {
        out.objectPathPrefix = it->second;
    }
    if (auto it = env.find("FW_TARGETS"); it != env.end())
    {
        out.targets = splitTargets(it->second);
    }

    if (out.imagePath.empty() || out.version.empty() ||
        out.objectPathPrefix.empty() || out.targets.empty())
    {
        return std::nullopt;
    }
    return out;
}

std::string driveObjPath(const std::string& driveName)
{
    return std::string("/xyz/openbmc_project/inventory/system/nvme/") +
           driveName;
}

std::string driveNameFromTarget(std::string target)
{
    size_t lastSlash = target.find_last_of('/');
    if (lastSlash != std::string::npos)
    {
        target = target.substr(lastSlash + 1);
    }
    return target;
}

std::shared_ptr<NVMeDevice> resolveDriveTarget(
    const std::string& target,
    std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>& driveMap,
    uint8_t& eid)
{
    std::string driveName = driveNameFromTarget(target);
    for (const auto& [driveEid, drive] : driveMap)
    {
        if (drive && drive->getDriveName() == driveName)
        {
            eid = driveEid;
            return drive;
        }
    }
    return nullptr;
}

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
using OperationStatus =
    sdbusplus::xyz::openbmc_project::Common::server::Progress::OperationStatus;

uint32_t nvmeStatusField(nvme_status_field status)
{
    return static_cast<uint32_t>(status) & nvmeStatusFieldMask;
}

constexpr uint32_t commandSpecificStatus(uint32_t statusCode)
{
    return (NVME_SCT_CMD_SPECIFIC << NVME_SCT_SHIFT) | statusCode;
}

bool isFwCommitNeedsResetStatus(nvme_status_field status)
{
    uint32_t sc = nvmeStatusField(status);
    return sc == commandSpecificStatus(NVME_SC_FW_NEEDS_CONV_RESET) ||
           sc == commandSpecificStatus(NVME_SC_FW_NEEDS_SUBSYS_RESET) ||
           sc == commandSpecificStatus(NVME_SC_FW_NEEDS_RESET);
}

bool isFwCommitImageLostAfterReset(nvme_status_field status)
{
    // Host reset can discard the downloaded image before commit observes it.
    return nvmeStatusField(status) ==
           commandSpecificStatus(NVME_SC_FIRMWARE_IMAGE);
}

bool shouldRestartFwUpdateAfterCommitError(const std::error_code& ec,
                                           nvme_status_field status)
{
    return ec && isFwCommitImageLostAfterReset(status);
}

/** Run full FW update for one drive on a dedicated thread: download chunk by
 *  chunk in a loop
 */
void runFwUpdate(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                 const ParsedNvmeUpdateArgs& args, uint8_t eid,
                 const std::string& driveName,
                 std::weak_ptr<NVMeDevice> weakDrive)
{
    std::string objPath = driveObjPath(driveName);
    std::string deviceInfo = std::string(redfishDrivePathPrefix) + driveName;

    // Update progress only if drive is still alive (not power-offed).
    // Returns false if the drive was removed — caller should abort.
    auto setProgress = [&weakDrive](uint32_t pct, OperationStatus st) -> bool {
        auto d = weakDrive.lock();
        if (!d)
        {
            return false;
        }
        d->setFwUpdateProgress(pct, st);
        return true;
    };

    if (!setProgress(0, OperationStatus::InProgress))
    {
        return;
    }
    logTargetDetermined(conn, deviceInfo, args.version, objPath);

    std::ifstream file(args.imagePath, std::ios::binary);
    if (!file)
    {
        lg2::error("FW update: cannot open image {PATH}", "PATH",
                   args.imagePath);
        setProgress(0, OperationStatus::Failed);
        return;
    }
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    logTransferringToComponent(conn, deviceInfo, args.version, objPath);

    std::shared_ptr<NVMeMiIntf> intf;
    {
        auto d = weakDrive.lock();
        if (!d)
        {
            return;
        }
        intf = d->getIntf();
        if (!intf)
        {
            lg2::error("FW update: no interface for EID {EID}", "EID",
                       static_cast<int>(eid));
            d->setFwUpdateProgress(0, OperationStatus::Failed);
            return;
        }
    }

    const auto updateStartTime = std::chrono::steady_clock::now();

    for (size_t updateAttempt = 0;
         updateAttempt <= fwCommitResetRestartMaxRetries; updateAttempt++)
    {
        if (updateAttempt > 0)
        {
            file.clear();
            file.seekg(0, std::ios::beg);
            if (!setProgress(0, OperationStatus::InProgress))
            {
                return;
            }
        }

        auto lastProgressLogTime =
            updateStartTime - std::chrono::seconds(progressLogIntervalSec + 1);
        uint32_t offset = 0;
        size_t downloadRetryCount = 0;

        while (offset < fileSize)
        {
            if (shouldLogEverySeconds(lastProgressLogTime,
                                      progressLogIntervalSec))
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
                lg2::error(
                    "FW update EID {EID} failed after {MS} ms: read error",
                    "EID", static_cast<int>(eid), "MS", msRead);
                setProgress(0, OperationStatus::Failed);
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
                auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - updateStartTime)
                        .count();
                lg2::error(
                    "FW update EID {EID} failed after {MS} ms (download at offset {OFFSET} after {MAX} retries): {ERR}",
                    "EID", static_cast<int>(eid), "MS", ms, "OFFSET", offset,
                    "MAX", fwDownloadMaxRetries, "ERR", ec.message());
                logTransferFailed(conn, deviceInfo, args.version, objPath);
                setProgress(0, OperationStatus::Failed);
                return;
            }
            downloadRetryCount = 0;
            offset += static_cast<uint32_t>(n);
            uint32_t pct =
                (fileSize != 0U)
                    ? static_cast<uint32_t>((static_cast<size_t>(offset) * 90) /
                                            fileSize)
                    : 0;
            if (!setProgress(pct, OperationStatus::InProgress))
            {
                lg2::info(
                    "FW update EID {EID} aborted: drive removed during download",
                    "EID", static_cast<int>(eid));
                return;
            }
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
        bool needsReset = isFwCommitNeedsResetStatus(statusCommit);
        if (ecCommit)
        {
            if (shouldRestartFwUpdateAfterCommitError(ecCommit, statusCommit) &&
                updateAttempt < fwCommitResetRestartMaxRetries)
            {
                lg2::warning(
                    "FW update EID {EID} commit lost downloaded image after reset, restarting firmware update attempt {ATTEMPT}/{MAX}: error {EC} status {STATUS}",
                    "EID", static_cast<int>(eid), "ATTEMPT", updateAttempt + 2,
                    "MAX", fwCommitResetRestartMaxRetries + 1, "EC",
                    ecCommit.value(), "STATUS", nvmeStatusField(statusCommit));
                continue;
            }
            lg2::error(
                "FW update EID {EID} failed after {MS} ms: commit error {EC} status {STATUS}",
                "EID", static_cast<int>(eid), "MS", ms, "EC", ecCommit.value(),
                "STATUS", nvmeStatusField(statusCommit));
            logActivateFailed(conn, deviceInfo, args.version, objPath);
            setProgress(0, OperationStatus::Failed);
            return;
        }
        if (needsReset)
        {
            lg2::info(
                "FW update EID {EID} committed in {MS} ms, activation pending reset (status {STATUS})",
                "EID", static_cast<int>(eid), "MS", ms, "STATUS",
                nvmeStatusField(statusCommit));
        }

        lg2::info("FW update EID {EID} completed in {MS} ms", "EID",
                  static_cast<int>(eid), "MS", ms);
        logUpdateSuccessful(conn, deviceInfo, args.version, objPath);
        setProgress(100, OperationStatus::Completed);
        return;
    }
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

void handleNvmeUpdateStartJob(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& unitId)
{
    const std::string prefix(nvmeUpdateServicePrefix);
    const std::string suffix(".service");

    if (shouldSkipDuplicateJob(unitId))
    {
        return;
    }
    std::string requestId = unitId.substr(
        prefix.size(), unitId.size() - prefix.size() - suffix.size());
    auto args = parseNvmeUpdateRequest(requestId);
    if (!args)
    {
        lg2::warning("Failed to parse nvme-update request: {REQUEST}",
                     "REQUEST", requestId);
        logNoMatchingDevices(conn);
        return;
    }
    if (args->targets.empty())
    {
        lg2::warning(
            "No matching devices found for nvme-update request: {REQUEST}",
            "REQUEST", requestId);
        logNoMatchingDevices(conn);
        return;
    }
    const ParsedNvmeUpdateArgs& argsCopy = *args;
    auto& driveMap = getDriveMap();
    for (const std::string& target : argsCopy.targets)
    {
        uint8_t eid = 0;
        std::shared_ptr<NVMeDevice> drive = resolveDriveTarget(target, driveMap,
                                                               eid);
        if (!drive)
        {
            lg2::warning("FW update: no drive for target {TARGET}, skipping",
                         "TARGET", target);
            continue;
        }
        std::string driveName = drive->getDriveName();
        std::weak_ptr<NVMeDevice> weakDrive = drive;
        std::thread([conn, argsCopy, eid, driveName, weakDrive]() {
            runFwUpdate(conn, argsCopy, eid, driveName, weakDrive);
        }).detach();
    }
}

void onNvmeUpdateJobNew(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    sdbusplus::message::message& msg)
{
    uint32_t jobId = 0;
    sdbusplus::object_path jobPath;
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

    const std::string jobPathStr = jobPath.str;
    conn->async_method_call(
        [conn, jobId, jobPathStr,
         unitId](boost::system::error_code ec,
                 std::variant<std::string> jobTypeVar) {
        if (ec)
        {
            lg2::warning(
                "Failed to read systemd JobType for nvme-update JobNew: job {JOB} path {PATH} unit {UNIT}: {ERROR}",
                "JOB", jobId, "PATH", jobPathStr, "UNIT", unitId, "ERROR",
                ec.message());
            return;
        }

        const std::string* jobType = std::get_if<std::string>(&jobTypeVar);
        if (jobType == nullptr)
        {
            lg2::warning(
                "Unexpected systemd JobType variant for nvme-update JobNew: job {JOB} path {PATH} unit {UNIT}",
                "JOB", jobId, "PATH", jobPathStr, "UNIT", unitId);
            return;
        }

        if (*jobType != "start")
        {
            return;
        }

        handleNvmeUpdateStartJob(conn, unitId);
    },
        systemdService, jobPathStr, dbusPropertiesInterface, "Get",
        systemdJobInterface, systemdJobTypeProperty);
}

} // anonymous namespace

void startNvmeFwUpdateMonitor(
    const std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    static std::unique_ptr<sdbusplus::bus::match_t> match;
    match = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',sender='org.freedesktop.systemd1',"
        "path='/org/freedesktop/systemd1',"
        "interface='org.freedesktop.systemd1.Manager',member='JobNew'",
        [conn](sdbusplus::message::message& msg) {
        onNvmeUpdateJobNew(conn, msg);
    });
    lg2::info("NVMe FW update monitor started");
}
