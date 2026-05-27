/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "SensorUtil.hpp"
#include "Thresholds.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

constexpr size_t sensorErrorThreshold = 5;

class NVMeMiSensor
{
  public:
    static constexpr const char* sensorType = "NVME1000";

    NVMeMiSensor(sdbusplus::asio::object_server& objectServer,
                 std::shared_ptr<sdbusplus::asio::connection>& conn,
                 const std::string& sensorName,
                 std::vector<thresholds::Threshold>&& thresholds,
                 const std::string& sensorConfiguration, uint8_t eid);
    ~NVMeMiSensor();
    NVMeMiSensor(const NVMeMiSensor&) = delete;
    NVMeMiSensor& operator=(const NVMeMiSensor&) = delete;
    NVMeMiSensor(NVMeMiSensor&&) = delete;
    NVMeMiSensor& operator=(NVMeMiSensor&&) = delete;

    double getValue() const
    {
        return value;
    }
    const std::string& getName() const
    {
        return name;
    }
    std::shared_ptr<sdbusplus::asio::dbus_interface>
        getThresholdInterface(thresholds::Level lev);

    static std::string propertyLevel(thresholds::Level lev,
                                     thresholds::Direction dir);
    static std::string propertyAlarm(thresholds::Level lev,
                                     thresholds::Direction dir);

    void updateValue(double newValue);
    void incrementError();
    bool inError() const;
    static bool readingStateGood();

    std::vector<thresholds::Threshold> thresholds;
    const uint8_t eid;

  private:
    void checkThresholds();
    void markFunctional(bool isFunctional);
    void markAvailable(bool isAvailable);
    bool requiresUpdate(double lVal, double rVal) const;

    std::string name;
    std::string configurationPath;
    std::string configInterface;
    double value = std::numeric_limits<double>::quiet_NaN();
    static constexpr double maxReading = 127;
    static constexpr double minReading = 0;
    double maxValue{maxReading};
    double minValue{minReading};
    double hysteresisTrigger{(maxReading - minReading) * 0.01};
    double hysteresisPublish{(maxReading - minReading) * 0.0001};

    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> association;
    std::shared_ptr<sdbusplus::asio::dbus_interface> availableInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> operationalInterface;

    std::array<std::shared_ptr<sdbusplus::asio::dbus_interface>,
               thresholds::thresProp.size()>
        thresholdInterfaces;

    size_t errCount{0};
};
