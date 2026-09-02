/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "SensorUtil.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class NVMeMiSensor; // Forward declaration
namespace thresholds
{
enum class Level
{
    WARNING,
    CRITICAL,
    PERFORMANCELOSS,
    SOFTSHUTDOWN,
    HARDSHUTDOWN,
    ERROR
};
enum class Direction
{
    HIGH,
    LOW,
    ERROR
};
struct Threshold
{
    Threshold(
        const Level& lev, const Direction& dir, const double& val,
        const double hysteresis = std::numeric_limits<double>::quiet_NaN(),
        bool write = true) :
        level(lev), direction(dir), value(val), hysteresis(hysteresis),
        writeable(write)
    {}
    Level level;
    Direction direction;
    double value;
    double hysteresis;
    bool writeable;
    // Threshold violations must be observed repeatedly before asserting an
    // alarm.  These fields are runtime state and are reset on recovery.
    size_t hitCount{0};
    bool asserted{false};
};

void assertThresholds(NVMeMiSensor* sensor, double assertValue,
                      thresholds::Level level, thresholds::Direction direction,
                      bool assert);

struct ThresholdDefinition
{
    Level level;
    uint8_t sevOrder;
    const char* levelName;
};

constexpr static std::array<thresholds::ThresholdDefinition, 5> thresProp = {
    {{Level::WARNING, 0, "Warning"},
     {Level::CRITICAL, 1, "Critical"},
     {Level::PERFORMANCELOSS, 2, "PerformanceLoss"},
     {Level::SOFTSHUTDOWN, 3, "SoftShutdown"},
     {Level::HARDSHUTDOWN, 4, "HardShutdown"}}};

std::string getInterface(Level level);

bool parseThresholdsFromConfig(
    const nvme::sensors::SensorData& sensorData,
    std::vector<thresholds::Threshold>& thresholdVector,
    const std::string* matchLabel = nullptr, const int* sensorIndex = nullptr);

bool checkThresholds(NVMeMiSensor* sensor);
} // namespace thresholds
