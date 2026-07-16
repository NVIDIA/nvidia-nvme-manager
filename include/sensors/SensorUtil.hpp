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

#include <filesystem>
#include <regex>
#include <tuple>
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

inline void createAssociation(
    std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path)
{
    if (association)
    {
        std::filesystem::path p(path);
        std::vector<Association> associations;
        associations.emplace_back("chassis", "all_sensors",
                                  p.parent_path().string());
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
