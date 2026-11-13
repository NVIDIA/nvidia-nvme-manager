/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "VariantVisitors.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cerrno>
#include <filesystem>
#include <functional>
#include <regex>
#include <tuple>
#include <variant>
#include <vector>

namespace nvme
{
namespace sensors
{

constexpr const char* unitDegreesC =
    "xyz.openbmc_project.Sensor.Value.Unit.DegreesC";

namespace association
{
constexpr const char* interface = "xyz.openbmc_project.Association.Definitions";
}

namespace mapper
{
constexpr const char* busName = "xyz.openbmc_project.ObjectMapper";
constexpr const char* path = "/xyz/openbmc_project/object_mapper";
constexpr const char* interface = "xyz.openbmc_project.ObjectMapper";
constexpr const char* subtree = "GetSubTree";
} // namespace mapper

using Association = std::tuple<std::string, std::string, std::string>;
using AssociationList = std::vector<Association>;

inline std::string escapePathForDbus(const std::string& name)
{
    return std::regex_replace(name, std::regex("[^a-zA-Z0-9_/]+"), "_");
}

/** @brief Look up the chassis an NVMe sensor should associate to.
 *
 * The sensor's EM config lives under its inventory parent (e.g.
 * .../inventory/system/nvme/NVMe_E1S_IO0), which is the sensor's default
 * chassis. When the parent carries an EM
 * `Associations: {"chassis": "board/IO_Board_n"}` entry (published by the
 * mapper as a `<parent>/chassis` association endpoint), the sensor belongs
 * under that board instead, so it shows up in the board's
 * EnvironmentMetrics.
 *
 * Asynchronous: calls onFound with the board path once the mapper answers.
 * onFound is not called while the association is not on D-Bus (yet);
 * entity-manager may publish the board after the sensor is created.
 */
inline void
    resolveChassisPath(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                       const std::string& parentPath,
                       std::function<void(const std::string&)> onFound)
{
    conn->async_method_call(
        [parentPath, onFound = std::move(onFound)](
            const boost::system::error_code& ec,
            const std::variant<std::vector<std::string>>& endpoints) {
        if (ec)
        {
            // EBADR: the mapper has no <parent>/chassis object yet
            if (ec.value() != EBADR)
            {
                lg2::error("Chassis association lookup for {PATH} failed: "
                           "{ERROR}",
                           "PATH", parentPath, "ERROR", ec.message());
            }
            return;
        }
        const auto* eps = std::get_if<std::vector<std::string>>(&endpoints);
        if (eps != nullptr && !eps->empty())
        {
            onFound(eps->front());
        }
    },
        mapper::busName, parentPath + "/chassis",
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.Association", "endpoints");
}

inline void createAssociationTo(
    std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& chassisPath)
{
    if (association)
    {
        std::vector<Association> associations;
        associations.emplace_back("chassis", "all_sensors", chassisPath);
        association->register_property("Associations", associations);
        association->initialize();
    }
}

constexpr const char* configInterfacePrefix =
    "xyz.openbmc_project.Configuration.";

using BasicVariantType =
    std::variant<std::vector<std::string>, std::vector<uint8_t>,
                 std::vector<std::uint64_t>, std::string, int64_t, uint64_t,
                 double, int32_t, uint32_t, int16_t, uint16_t, uint8_t, bool>;
using SensorBaseConfigMap =
    boost::container::flat_map<std::string, BasicVariantType>;
using SensorData = boost::container::flat_map<std::string, SensorBaseConfigMap>;
using ManagedObjectType =
    boost::container::flat_map<sdbusplus::object_path, SensorData>;

using GetSubTreeType = std::vector<
    std::pair<std::string,
              std::vector<std::pair<std::string, std::vector<std::string>>>>>;

inline std::string escapeName(const std::string& sensorName)
{
    return boost::replace_all_copy(sensorName, " ", "_");
}

inline std::string configInterfaceName(const std::string& type)
{
    return std::string(configInterfacePrefix) + type;
}

inline float getPollRate(const SensorBaseConfigMap& cfg, float dflt)
{
    float pollRate = dflt;
    auto findPollRate = cfg.find("PollRate");
    if (findPollRate != cfg.end())
    {
        pollRate = std::visit(VariantToFloatVisitor(), findPollRate->second);
        if (!std::isfinite(pollRate) || pollRate <= 0.0F)
        {
            pollRate = dflt;
        }
    }
    return pollRate;
}

} // namespace sensors
} // namespace nvme
