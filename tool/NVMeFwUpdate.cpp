/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 *
 * Thin client: monitor NVMe firmware update progress and status from
 * nvme-manager (which performs the update when nvme-update@.service starts).
 * Parses the same CLI args, subscribes to xyz.openbmc_project.Common.Progress
 * on each drive path, and exits 0 if all complete successfully else 1.
 */

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <vector>

PHOSPHOR_LOG2_USING;

constexpr std::chrono::minutes progressTimeout{10};
constexpr std::chrono::seconds progressChangeTimeout{30};
constexpr const char* progressInterface = "xyz.openbmc_project.Common.Progress";
constexpr const char* progressStatusProp = "Status";
constexpr const char* statusCompleted =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";
constexpr const char* statusFailed =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Failed";

static std::string dashToSlash(std::string s)
{
    std::replace(s.begin(), s.end(), '-', '/');
    return s;
}

static void printUsage(const char* programName)
{
    std::cout << "Usage: " << programName
              << " <firmware_path> <version> <object_path_prefix> <eid1> "
                 "[eid2] ... [-v]\n"
              << "\n"
              << "Arguments are the same as before; the actual update is\n"
              << "performed by nvme-manager when nvme-update@.service starts.\n"
              << "This tool only monitors Progress on each drive and exits\n"
              << "0 (all success) or 1 (any failed/timeout).\n";
}

int main(int argc, char* argv[])
{
    try
    {
        std::string imagePath;
        std::string version;
        std::string objectPathPrefix;
        std::vector<uint8_t> eids;
        bool verbose = false;
        bool help = false;

        std::span<char*> args(argv, argc);
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
            else if (imagePath.empty())
            {
                imagePath = args[i];
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
                try
                {
                    int eid = std::stoi(args[i]);
                    if (eid >= 0 && eid <= 255)
                    {
                        eids.push_back(static_cast<uint8_t>(eid));
                    }
                }
                catch (...)
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

        if (eids.empty() || imagePath.empty() || version.empty() ||
            objectPathPrefix.empty())
        {
            lg2::error("Required: firmware_path, version, object_path_prefix, "
                       "and at least one EID");
            printUsage(args[0]);
            return 1;
        }

        // object_path_prefix is e.g. /xyz/.../nvme/NVMe_E1S_ (dashes in
        // instance)
        std::string prefix = dashToSlash(objectPathPrefix);

        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);

        std::set<uint8_t> pending(eids.begin(), eids.end());
        std::set<uint8_t> failed;
        std::set<uint8_t> eidsWithProgress;
        bool done = false;
        int exitCode = 0;
        boost::asio::steady_timer timeoutTimer(io);
        boost::asio::steady_timer progressChangeTimer(io);
        std::vector<std::shared_ptr<sdbusplus::bus::match::match>> matches;

        auto checkDone = [&]() {
            if (done)
            {
                return;
            }
            if (pending.empty())
            {
                done = true;
                timeoutTimer.cancel();
                progressChangeTimer.cancel();
                exitCode = failed.empty() ? 0 : 1;
                io.stop();
                return;
            }
            if (!failed.empty())
            {
                done = true;
                timeoutTimer.cancel();
                progressChangeTimer.cancel();
                exitCode = 1;
                io.stop();
            }
        };

        for (uint8_t eid : eids)
        {
            std::string path = prefix + std::to_string(eid);
            matches.push_back(std::make_shared<sdbusplus::bus::match::match>(
                static_cast<sdbusplus::bus::bus&>(*conn),
                "type='signal',member='PropertiesChanged',path='" + path +
                    "',arg0='" + progressInterface + "'",
                [&, eid, path](sdbusplus::message::message& msg) {
                eidsWithProgress.insert(eid);
                std::string iface;
                std::map<std::string, std::variant<std::string>> props;
                try
                {
                    msg.read(iface, props);
                }
                catch (...)
                {
                    return;
                }
                auto it = props.find(progressStatusProp);
                if (it == props.end())
                {
                    return;
                }
                std::string status = std::get<std::string>(it->second);
                if (status == statusCompleted)
                {
                    pending.erase(eid);
                    if (verbose)
                    {
                        lg2::info("EID {EID} Progress Status Completed", "EID",
                                  static_cast<int>(eid));
                    }
                }
                else if (status == statusFailed)
                {
                    pending.erase(eid);
                    failed.insert(eid);
                    lg2::error("EID {EID} Progress Status Failed", "EID",
                               static_cast<int>(eid));
                }
                checkDone();
            }));

            // Initial read of Status so we don't miss if already completed
            conn->async_method_call(
                [&, eid](boost::system::error_code ec,
                         std::variant<std::string> statusVar) {
                if (ec)
                {
                    return;
                }
                std::string status = std::get<std::string>(statusVar);
                if (status == statusCompleted)
                {
                    pending.erase(eid);
                    if (verbose)
                    {
                        lg2::info("EID {EID} already Completed", "EID",
                                  static_cast<int>(eid));
                    }
                }
                else if (status == statusFailed)
                {
                    pending.erase(eid);
                    failed.insert(eid);
                }
                checkDone();
            },
                "xyz.openbmc_project.NVMeDevice", path,
                "org.freedesktop.DBus.Properties", "Get", progressInterface,
                progressStatusProp);
        }

        progressChangeTimer.expires_after(progressChangeTimeout);
        progressChangeTimer.async_wait([&](boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted || done)
            {
                return;
            }
            for (auto it = pending.begin(); it != pending.end();)
            {
                uint8_t eid = *it;
                if (!eidsWithProgress.contains(eid))
                {
                    lg2::error("EID {EID} no progress change within 30 seconds",
                               "EID", static_cast<int>(eid));
                    failed.insert(eid);
                    it = pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            checkDone();
        });

        timeoutTimer.expires_after(progressTimeout);
        timeoutTimer.async_wait([&](boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (!done)
            {
                done = true;
                progressChangeTimer.cancel();
                lg2::error("Timeout waiting for Progress");
                for (uint8_t eid : pending)
                {
                    failed.insert(eid);
                }
                exitCode = 1;
                io.stop();
            }
        });

        io.run();

        if (exitCode == 0)
        {
            lg2::info("All firmware updates completed successfully");
        }
        else
        {
            lg2::error("One or more firmware updates failed or timed out");
        }
        return exitCode;
    }
    catch (const std::exception& e)
    {
        lg2::error("Error: {ERROR}", "ERROR", e.what());
        return 1;
    }
}
