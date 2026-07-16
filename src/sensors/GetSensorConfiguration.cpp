/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#include "GetSensorConfiguration.hpp"

#include "SensorUtil.hpp"

#include <boost/asio/steady_timer.hpp>

using nvme::sensors::configInterfaceName;
using nvme::sensors::configInterfacePrefix;
using nvme::sensors::GetSubTreeType;
using nvme::sensors::SensorBaseConfigMap;
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

void GetSensorConfiguration::getPath(const std::string& path,
                                     const std::string& interface,
                                     const std::string& owner, size_t retries)
{
    if (retries > 5)
    {
        retries = 5;
    }
    std::shared_ptr<GetSensorConfiguration> self = shared_from_this();

    self->dbusConnection->async_method_call(
        [self, path, interface, owner, retries](
            const boost::system::error_code ec, SensorBaseConfigMap& data) {
        if (ec)
        {
            if (retries == 0U)
            {
                lg2::error("Error getting '{PATH}': no retries left", "PATH",
                           path);
                return;
            }
            auto timer = std::make_shared<boost::asio::steady_timer>(
                self->dbusConnection->get_io_context());
            timer->expires_after(std::chrono::seconds(10));
            timer->async_wait([self, timer, path, interface, owner,
                               retries](boost::system::error_code ec) {
                if (ec)
                {
                    return;
                }
                self->getPath(path, interface, owner, retries - 1);
            });
            return;
        }

        sdbusplus::object_path objPath(path);
        self->respData[objPath][interface] = std::move(data);
    },
        owner, path, "org.freedesktop.DBus.Properties", "GetAll", interface);
}

void GetSensorConfiguration::getConfiguration(
    const std::vector<std::string>& types, size_t retries)
{
    if (retries > 5)
    {
        retries = 5;
    }

    std::vector<std::string> interfaces;
    interfaces.reserve(types.size());
    for (const auto& type : types)
    {
        interfaces.push_back(configInterfaceName(type));
    }

    std::shared_ptr<GetSensorConfiguration> self = shared_from_this();
    dbusConnection->async_method_call(
        [self, interfaces, retries](const boost::system::error_code ec,
                                    const GetSubTreeType& ret) {
        if (ec)
        {
            lg2::error("Error calling mapper: '{ERROR_MESSAGE}'",
                       "ERROR_MESSAGE", ec.message());
            if (retries == 0U)
            {
                return;
            }
            auto timer = std::make_shared<boost::asio::steady_timer>(
                self->dbusConnection->get_io_context());
            timer->expires_after(std::chrono::seconds(10));
            timer->async_wait([self, timer, interfaces,
                               retries](boost::system::error_code ec) {
                if (ec)
                {
                    return;
                }
                std::vector<std::string> types;
                for (const auto& i : interfaces)
                {
                    if (i.starts_with(configInterfacePrefix))
                    {
                        types.push_back(
                            i.substr(strlen(configInterfacePrefix)));
                    }
                }
                self->getConfiguration(types, retries - 1);
            });
            return;
        }
        for (const auto& [path, objDict] : ret)
        {
            if (objDict.empty())
            {
                continue;
            }
            const std::string& owner = objDict.begin()->first;

            for (const std::string& interface : objDict.begin()->second)
            {
                if (std::find_if(interfaces.begin(), interfaces.end(),
                                 [&interface](const std::string& possible) {
                    return interface.starts_with(possible);
                }) == interfaces.end())
                {
                    continue;
                }
                self->getPath(path, interface, owner);
            }
        }
    },
        nvme::sensors::mapper::busName, nvme::sensors::mapper::path,
        nvme::sensors::mapper::interface, nvme::sensors::mapper::subtree, "/",
        0, interfaces);
}
