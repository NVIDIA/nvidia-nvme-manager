/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "SensorUtil.hpp"

#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/message.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GetSensorConfiguration :
    std::enable_shared_from_this<GetSensorConfiguration>
{
    GetSensorConfiguration(
        std::shared_ptr<sdbusplus::asio::connection> connection,
        std::function<void(nvme::sensors::ManagedObjectType& resp)>&&
            callbackFunc) :
        dbusConnection(std::move(connection)), callback(std::move(callbackFunc))
    {}
    GetSensorConfiguration(const GetSensorConfiguration&) = delete;
    GetSensorConfiguration& operator=(const GetSensorConfiguration&) = delete;
    GetSensorConfiguration(GetSensorConfiguration&&) = delete;
    GetSensorConfiguration& operator=(GetSensorConfiguration&&) = delete;

    void getConfiguration(const std::vector<std::string>& types,
                          size_t retries = 0);

    ~GetSensorConfiguration()
    {
        callback(respData);
    }

  private:
    void getPath(const std::string& path, const std::string& interface,
                 const std::string& owner, size_t retries = 5);

    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::function<void(nvme::sensors::ManagedObjectType& resp)> callback;
    nvme::sensors::ManagedObjectType respData;
};
