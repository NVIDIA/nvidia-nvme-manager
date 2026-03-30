/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#include "Thresholds.hpp"

#include "NVMeMiSensor.hpp"
#include "SensorUtil.hpp"
#include "VariantVisitors.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

using nvme::sensors::SensorData;

#include <limits>
#include <string>
#include <vector>

namespace thresholds
{
Level findThresholdLevel(uint8_t sev)
{
    for (const ThresholdDefinition& prop : thresProp)
    {
        if (prop.sevOrder == sev)
        {
            return prop.level;
        }
    }
    return Level::ERROR;
}

Direction findThresholdDirection(const std::string& direct)
{
    if (direct == "greater than")
    {
        return Direction::HIGH;
    }
    if (direct == "less than")
    {
        return Direction::LOW;
    }
    return Direction::ERROR;
}

bool parseThresholdsFromConfig(
    const SensorData& sensorData,
    std::vector<thresholds::Threshold>& thresholdVector,
    const std::string* matchLabel, const int* sensorIndex)
{
    for (const auto& [intf, cfg] : sensorData)
    {
        if (intf.find("Thresholds") == std::string::npos)
        {
            continue;
        }
        if (matchLabel != nullptr)
        {
            auto labelFind = cfg.find("Label");
            if (labelFind == cfg.end())
            {
                continue;
            }
            if (std::visit(VariantToStringVisitor(), labelFind->second) !=
                *matchLabel)
            {
                continue;
            }
        }

        if (sensorIndex != nullptr)
        {
            auto indexFind = cfg.find("Index");
            if ((indexFind == cfg.end()) && (*sensorIndex != 1))
            {
                continue;
            }
            if ((indexFind != cfg.end()) &&
                (std::visit(VariantToIntVisitor(), indexFind->second) !=
                 *sensorIndex))
            {
                continue;
            }
        }

        double hysteresis = std::numeric_limits<double>::quiet_NaN();
        auto hysteresisFind = cfg.find("Hysteresis");
        if (hysteresisFind != cfg.end())
        {
            hysteresis = std::visit(VariantToDoubleVisitor(),
                                    hysteresisFind->second);
        }

        auto directionFind = cfg.find("Direction");
        auto severityFind = cfg.find("Severity");
        auto valueFind = cfg.find("Value");
        if (valueFind == cfg.end() || severityFind == cfg.end() ||
            directionFind == cfg.end())
        {
            lg2::error(
                "Malformed threshold on configuration interface: '{INTERFACE}'",
                "INTERFACE", intf);
            return false;
        }
        unsigned int severity = std::visit(VariantToUnsignedIntVisitor(),
                                           severityFind->second);
        std::string directions = std::visit(VariantToStringVisitor(),
                                            directionFind->second);
        Level level = findThresholdLevel(severity);
        Direction direction = findThresholdDirection(directions);

        if ((level == Level::ERROR) || (direction == Direction::ERROR))
        {
            continue;
        }
        double val = std::visit(VariantToDoubleVisitor(), valueFind->second);
        thresholdVector.emplace_back(level, direction, val, hysteresis);
    }
    return true;
}

struct ChangeParam
{
    ChangeParam(Threshold whichThreshold, bool status, double value) :
        threshold(whichThreshold), asserted(status), assertValue(value)
    {}
    Threshold threshold;
    bool asserted;
    double assertValue;
};

static std::vector<ChangeParam> checkThresholds(NVMeMiSensor* sensor,
                                                double value)
{
    std::vector<ChangeParam> thresholdChanges;
    if (sensor->thresholds.empty())
    {
        return thresholdChanges;
    }

    for (auto& threshold : sensor->thresholds)
    {
        if (threshold.direction == thresholds::Direction::HIGH)
        {
            if (value >= threshold.value)
            {
                thresholdChanges.emplace_back(threshold, true, value);
            }
            else if (value < (threshold.value - threshold.hysteresis))
            {
                thresholdChanges.emplace_back(threshold, false, value);
            }
        }
        else if (threshold.direction == thresholds::Direction::LOW)
        {
            if (value <= threshold.value)
            {
                thresholdChanges.emplace_back(threshold, true, value);
            }
            else if (value > (threshold.value + threshold.hysteresis))
            {
                thresholdChanges.emplace_back(threshold, false, value);
            }
        }
    }
    return thresholdChanges;
}

bool checkThresholds(NVMeMiSensor* sensor)
{
    bool status = true;
    double value = sensor->getValue();
    std::vector<ChangeParam> changes = checkThresholds(sensor, value);
    for (const auto& change : changes)
    {
        assertThresholds(sensor, change.assertValue, change.threshold.level,
                         change.threshold.direction, change.asserted);
        if (change.threshold.level == thresholds::Level::CRITICAL &&
            change.asserted)
        {
            status = false;
        }
    }
    return status;
}

void assertThresholds(NVMeMiSensor* sensor, double assertValue,
                      thresholds::Level level, thresholds::Direction direction,
                      bool assert)
{
    auto interface = sensor->getThresholdInterface(level);
    if (!interface)
    {
        return;
    }

    std::string property = NVMeMiSensor::propertyAlarm(level, direction);
    if (property.empty())
    {
        return;
    }
    if (interface->set_property<bool, true>(property, assert))
    {
        try
        {
            sdbusplus::message_t msg =
                interface->new_signal("ThresholdAsserted");
            msg.append(sensor->getName(), interface->get_interface_name(),
                       property, assert, assertValue);
            msg.signal_send();
        }
        catch (const sdbusplus::exception_t& e)
        {
            lg2::error("Failed to send thresholdAsserted signal");
        }
    }
}

std::string getInterface(const Level thresholdLevel)
{
    for (const ThresholdDefinition& thresh : thresProp)
    {
        if (thresh.level == thresholdLevel)
        {
            return std::string("xyz.openbmc_project.Sensor.Threshold.") +
                   thresh.levelName;
        }
    }
    return "";
}
} // namespace thresholds
