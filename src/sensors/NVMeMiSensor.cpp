/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#include "NVMeMiSensor.hpp"

#include "SensorUtil.hpp"
#include "Thresholds.hpp"

#include <phosphor-logging/lg2.hpp>
#include <tal.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>

using nvme::sensors::configInterfaceName;
using nvme::sensors::createAssociation;
using nvme::sensors::escapePathForDbus;
using nvme::sensors::unitDegreesC;

NVMeMiSensor::NVMeMiSensor(sdbusplus::asio::object_server& objectServer,
                           std::shared_ptr<sdbusplus::asio::connection>& conn,
                           const std::string& sensorName,
                           std::vector<thresholds::Threshold>&& thresholdsIn,
                           const std::string& sensorConfiguration,
                           const uint8_t eid) :
    thresholds(std::move(thresholdsIn)), eid(eid),
    name(escapePathForDbus(sensorName)), configurationPath(sensorConfiguration),
    configInterface(configInterfaceName(NVMeMiSensor::sensorType)),
    objServer(objectServer), dbusConnection(conn)
{
    for (auto& threshold : thresholds)
    {
        if (std::isnan(threshold.hysteresis))
        {
            threshold.hysteresis = hysteresisTrigger;
        }
    }

    std::string sensorPath = "/xyz/openbmc_project/sensors/temperature/" + name;
    sensorInterface =
        objServer.add_interface(sensorPath, "xyz.openbmc_project.Sensor.Value");

    association = objServer.add_interface(
        sensorPath, nvme::sensors::association::interface);
    createAssociation(association, configurationPath);

    sensorInterface->register_property("Unit", unitDegreesC);
    sensorInterface->register_property("MaxValue", maxValue);
    sensorInterface->register_property("MinValue", minValue);
    sensorInterface->register_property("Value", value);

    for (const auto& threshold : thresholds)
    {
        size_t levelIdx = static_cast<size_t>(threshold.level);
        if (!thresholdInterfaces[levelIdx])
        {
            thresholdInterfaces[levelIdx] = objServer.add_interface(
                sensorPath, thresholds::getInterface(threshold.level));
        }
        auto ifacePtr = thresholdInterfaces[levelIdx];

        std::string levelProp = propertyLevel(threshold.level,
                                              threshold.direction);
        std::string alarmProp = propertyAlarm(threshold.level,
                                              threshold.direction);
        if (!levelProp.empty() && !alarmProp.empty())
        {
            ifacePtr->register_property(levelProp, threshold.value);
            ifacePtr->register_property(alarmProp, false);
        }
    }

    if (!sensorInterface->initialize())
    {
        lg2::error("error initializing value interface");
    }
    for (auto& thresIface : thresholdInterfaces)
    {
        if (thresIface && !thresIface->initialize(true))
        {
            lg2::error("Error initializing threshold interface");
        }
    }

    availableInterface = objServer.add_interface(
        sensorPath, "xyz.openbmc_project.State.Decorator.Availability");
    availableInterface->register_property("Available", false);
    availableInterface->initialize();

    operationalInterface = objServer.add_interface(
        sensorPath, "xyz.openbmc_project.State.Decorator.OperationalStatus");
    operationalInterface->register_property("Functional", true);
    operationalInterface->initialize();
}

NVMeMiSensor::~NVMeMiSensor()
{
    for (const auto& iface : thresholdInterfaces)
    {
        if (iface)
        {
            objServer.remove_interface(iface);
        }
    }
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
    if (availableInterface)
    {
        objServer.remove_interface(availableInterface);
    }
    if (operationalInterface)
    {
        objServer.remove_interface(operationalInterface);
    }
}

std::shared_ptr<sdbusplus::asio::dbus_interface>
    NVMeMiSensor::getThresholdInterface(thresholds::Level lev)
{
    size_t index = static_cast<size_t>(lev);
    if (index >= thresholdInterfaces.size())
    {
        return nullptr;
    }
    return thresholdInterfaces[index];
}

std::string NVMeMiSensor::propertyLevel(thresholds::Level lev,
                                        thresholds::Direction dir)
{
    for (const thresholds::ThresholdDefinition& prop : thresholds::thresProp)
    {
        if (prop.level == lev)
        {
            if (dir == thresholds::Direction::HIGH)
            {
                return std::string(prop.levelName) + "High";
            }
            if (dir == thresholds::Direction::LOW)
            {
                return std::string(prop.levelName) + "Low";
            }
        }
    }
    return "";
}

std::string NVMeMiSensor::propertyAlarm(thresholds::Level lev,
                                        thresholds::Direction dir)
{
    for (const thresholds::ThresholdDefinition& prop : thresholds::thresProp)
    {
        if (prop.level == lev)
        {
            if (dir == thresholds::Direction::HIGH)
            {
                return std::string(prop.levelName) + "AlarmHigh";
            }
            if (dir == thresholds::Direction::LOW)
            {
                return std::string(prop.levelName) + "AlarmLow";
            }
        }
    }
    return "";
}

void NVMeMiSensor::updateValue(double newValue)
{
    if (!readingStateGood())
    {
        markAvailable(false);
        if (sensorInterface)
        {
            sensorInterface->set_property(
                "Value", std::numeric_limits<double>::quiet_NaN());
        }
        return;
    }

    if (requiresUpdate(value, newValue))
    {
        value = newValue;
        if (sensorInterface &&
            !sensorInterface->set_property("Value", newValue))
        {
            lg2::error("error setting property 'Value' to '{VALUE}'", "VALUE",
                       newValue);
        }
    }

    checkThresholds();

    std::string objPath = sensorInterface->get_object_path();
    std::string ifaceName = sensorInterface->get_interface_name();
    std::string parentChassis =
        sdbusplus::message::object_path(configurationPath).parent_path();
    nv::sensor_aggregation::DbusVariantType propValue = newValue;
    std::vector<uint8_t> rawPropValue = {};
    uint64_t timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    tal::TelemetryAggregator::updateTelemetry(objPath, ifaceName, "Value",
                                              rawPropValue, timestamp, 0,
                                              propValue, parentChassis);

    if (!std::isnan(newValue))
    {
        markFunctional(true);
        markAvailable(true);
    }
}

void NVMeMiSensor::incrementError()
{
    if (!readingStateGood())
    {
        markAvailable(false);
        return;
    }
    if (errCount >= sensorErrorThreshold)
    {
        return;
    }
    errCount++;
    if (errCount == sensorErrorThreshold)
    {
        lg2::error("Sensor name: {NAME}, reading error!", "NAME", name);
        markFunctional(false);
    }
}

bool NVMeMiSensor::inError() const
{
    return errCount >= sensorErrorThreshold;
}

bool NVMeMiSensor::readingStateGood()
{
    return true; // Power state always on for NVMe in nvme-manager
}

void NVMeMiSensor::checkThresholds()
{
    thresholds::checkThresholds(this);
}

void NVMeMiSensor::markFunctional(bool isFunctional)
{
    if (operationalInterface)
    {
        operationalInterface->set_property("Functional", isFunctional);
    }
    if (isFunctional)
    {
        errCount = 0;
    }
    else
    {
        value = std::numeric_limits<double>::quiet_NaN();
        if (sensorInterface)
        {
            sensorInterface->set_property("Value", value);
        }
    }
}

void NVMeMiSensor::markAvailable(bool isAvailable)
{
    if (availableInterface)
    {
        availableInterface->set_property("Available", isAvailable);
        errCount = 0;
    }
}

bool NVMeMiSensor::requiresUpdate(double lVal, double rVal) const
{
    const auto lNan = std::isnan(lVal);
    const auto rNan = std::isnan(rVal);
    if (lNan || rNan)
    {
        return (lNan != rNan);
    }
    return std::abs(lVal - rVal) > hysteresisPublish;
}
