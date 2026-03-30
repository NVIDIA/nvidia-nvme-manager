/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "GetSensorConfiguration.hpp"
#include "NVMeMiSensor.hpp"
#include "NVMeMiStatusSensor.hpp"
#include "SensorUtil.hpp"
#include "Thresholds.hpp"

#include <nvme/types.h>

#include <NVMeDevice.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

std::unordered_map<uint8_t, std::shared_ptr<NVMeDevice>>& getDriveMap();

struct SensorContext
{
    std::shared_ptr<NVMeMiSensor> tempSensor;
    std::shared_ptr<NVMeMiStatusSensor> statusSensor;
    float pollRateSec{1.0F};
    uint8_t eid;
    unsigned int tempScanDelay{0};

    explicit SensorContext(uint8_t eid);
};

class NVMeMiSensorManager
{
  public:
    NVMeMiSensorManager(sdbusplus::asio::object_server& objectServer,
                        std::shared_ptr<sdbusplus::asio::connection>& conn);
    ~NVMeMiSensorManager() = default;
    NVMeMiSensorManager(const NVMeMiSensorManager&) = delete;
    NVMeMiSensorManager& operator=(const NVMeMiSensorManager&) = delete;
    NVMeMiSensorManager(NVMeMiSensorManager&&) = delete;
    NVMeMiSensorManager& operator=(NVMeMiSensorManager&&) = delete;

    /** Create temp + status sensors for this EID. Uses eid to match EM config.
     */
    void createSensors(uint8_t eid);

    /** Remove sensors for this EID when drive is removed. */
    void removeSensors(uint8_t eid);

    /** Update temp and status sensors from health poll result.
     *  Called by NVMeDevice via sensorsUpdater when miSubsystemHealthStatusPoll
     * completes. ss=nullptr indicates poll error. */
    void updateSensors(uint8_t eid, nvme_mi_nvm_ss_health_status* ss);

  private:
    struct SensorConfig
    {
        std::string interfacePath;
        std::string sensorName;
        uint8_t eid;
        std::vector<thresholds::Threshold> thresholds;
        float pollRate;
        bool isTempSensor;
    };

    void handleSensorConfigurations(
        const nvme::sensors::ManagedObjectType& sensorConfigurations,
        const std::vector<uint8_t>& eidsToCreate);
    void createSensorsWithConfig(
        uint8_t eid,
        const std::map<uint8_t, std::vector<SensorConfig>>& configsByEid);
    static double
        getTemperatureReading(nvme_mi_nvm_ss_health_status* healthLog);

    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    std::map<uint8_t, std::unique_ptr<SensorContext>> sensorContexts;
    std::map<uint8_t, std::vector<SensorConfig>> cachedConfigByEid;
    std::vector<uint8_t> pendingEids;
    bool configLoadInProgress{false};
};
