/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#include "NVMeMiSensorManager.hpp"

#include "GetSensorConfiguration.hpp"
#include "NVMeMiSensor.hpp"
#include "NVMeMiStatusSensor.hpp"
#include "SensorUtil.hpp"
#include "Thresholds.hpp"
#include "VariantVisitors.hpp"

#include <NVMeDevice.hpp>
#include <NVMeIntf.hpp>
#include <phosphor-logging/lg2.hpp>

using nvme::sensors::configInterfaceName;
using nvme::sensors::getPollRate;
using nvme::sensors::SensorBaseConfigMap;

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// NVMe-MI NSS and CTEMP constants (from NVMe-MI spec)
constexpr uint8_t nvmeMiNssDriveFault = (1 << 5);
constexpr uint8_t nvmeMiCtempNoData = 0x80;
constexpr uint8_t nvmeMiCtempSensorFail = 0x81;
constexpr uint8_t nvmeMiCtempMaxTemp = 0x7F;
constexpr uint8_t nvmeMiCtempMinTemp = 0xC4;
constexpr uint8_t nvmeMiCtempTwosCompStart = 0xC5;

// Scan delay ticks for sensors in error (5 min)
constexpr unsigned int scanDelayTicks = 5 * 60;

SensorContext::SensorContext(uint8_t eid) : eid(eid) {}

static uint8_t extractAddress(const SensorBaseConfigMap& properties)
{
    auto findAddr = properties.find("Address");
    if (findAddr == properties.end())
    {
        return 0;
    }
    return static_cast<uint8_t>(
        std::visit(VariantToUnsignedIntVisitor(), findAddr->second));
}

static std::optional<std::string>
    extractSensorName(const std::string& path,
                      const SensorBaseConfigMap& properties)
{
    auto findName = properties.find("Name");
    if (findName == properties.end())
    {
        lg2::error("could not determine configuration name for {PATH}", "PATH",
                   path);
        return std::nullopt;
    }
    return std::get<std::string>(findName->second);
}

double NVMeMiSensorManager::getTemperatureReading(
    nvme_mi_nvm_ss_health_status* healthLog)
{
    uint8_t ctemp = healthLog->ctemp;

    if (ctemp == nvmeMiCtempNoData)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (ctemp == nvmeMiCtempSensorFail)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (ctemp == nvmeMiCtempMaxTemp)
    {
        return 127.0;
    }
    if (ctemp == nvmeMiCtempMinTemp)
    {
        return -60.0;
    }
    if (ctemp <= 0x7E)
    {
        return static_cast<double>(ctemp);
    }
    if (ctemp >= nvmeMiCtempTwosCompStart)
    {
        int8_t tempSigned = static_cast<int8_t>(ctemp);
        return static_cast<double>(tempSigned);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

NVMeMiSensorManager::NVMeMiSensorManager(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn) :
    objectServer(objectServer), conn(conn)
{}

void NVMeMiSensorManager::handleSensorConfigurations(
    const nvme::sensors::ManagedObjectType& sensorConfigurations,
    const std::vector<uint8_t>& eidsToCreate)
{
    std::vector<SensorConfig> pendingSensors;

    for (const auto& [interfacePath, sensorData] : sensorConfigurations)
    {
        auto tempSensorBase =
            sensorData.find(configInterfaceName(NVMeMiSensor::sensorType));
        if (tempSensorBase != sensorData.end())
        {
            const SensorBaseConfigMap& sensorConfig = tempSensorBase->second;
            auto sensorName = extractSensorName(interfacePath.str,
                                                sensorConfig);
            uint8_t eid = extractAddress(sensorConfig);

            if (!sensorName || eid == 0)
            {
                continue;
            }

            std::vector<thresholds::Threshold> sensorThresholds;
            thresholds::parseThresholdsFromConfig(sensorData, sensorThresholds);

            float pollRate = getPollRate(sensorConfig, 1.0F);
            pendingSensors.push_back({interfacePath.str, *sensorName, eid,
                                      std::move(sensorThresholds), pollRate,
                                      true});
        }

        auto statusSensorBase = sensorData.find(
            configInterfaceName(NVMeMiStatusSensor::sensorType));
        if (statusSensorBase != sensorData.end())
        {
            const SensorBaseConfigMap& sensorConfig = statusSensorBase->second;
            auto sensorName = extractSensorName(interfacePath.str,
                                                sensorConfig);
            uint8_t eid = extractAddress(sensorConfig);

            if (!sensorName || eid == 0)
            {
                continue;
            }

            float pollRate = getPollRate(sensorConfig, 1.0F);
            pendingSensors.push_back(
                {interfacePath.str, *sensorName, eid, {}, pollRate, false});
        }
    }

    // Group config by EID and cache for future createSensorsForEid calls
    std::map<uint8_t, std::vector<SensorConfig>> configsByEid;
    for (const auto& cfg : pendingSensors)
    {
        configsByEid[cfg.eid].push_back(cfg);
    }
    cachedConfigByEid = configsByEid;
    configLoadInProgress = false;

    // Create sensors for each requested EID (match by config, no driveMap
    // lookup)
    for (uint8_t eid : eidsToCreate)
    {
        createSensorsWithConfig(eid, configsByEid);
        // if drive disappeared during async config load, don't leave zombie
        auto& driveMap = getDriveMap();
        if (!driveMap.contains(eid))
        {
            sensorContexts.erase(eid);
        }
    }
}

void NVMeMiSensorManager::createSensorsWithConfig(
    uint8_t eid,
    const std::map<uint8_t, std::vector<SensorConfig>>& configsByEid)
{
    auto cfgIt = configsByEid.find(eid);
    if (cfgIt == configsByEid.end())
    {
        return;
    }
    const auto& configs = cfgIt->second;

    float minPollRate = 1.0F;
    for (const auto& cfg : configs)
    {
        if (cfg.pollRate < minPollRate)
        {
            minPollRate = cfg.pollRate;
        }
    }

    auto ctx = std::make_unique<SensorContext>(eid);
    ctx->pollRateSec = minPollRate;

    for (const auto& cfg : configs)
    {
        if (cfg.isTempSensor)
        {
            ctx->tempSensor = std::make_shared<NVMeMiSensor>(
                objectServer, conn, cfg.sensorName,
                std::vector<thresholds::Threshold>(cfg.thresholds),
                cfg.interfacePath, eid);
        }
        else
        {
            ctx->statusSensor = std::make_shared<NVMeMiStatusSensor>(
                objectServer, conn, cfg.sensorName, cfg.interfacePath, eid);
        }
    }

    if (ctx->tempSensor || ctx->statusSensor)
    {
        sensorContexts[eid] = std::move(ctx);
        auto& driveMap = getDriveMap();
        auto driveIt = driveMap.find(eid);
        if (driveIt != driveMap.end())
        {
            auto drive = driveIt->second;
            drive->setSensorsUpdater(
                [this, eid](nvme_mi_nvm_ss_health_status* ss) {
                updateSensors(eid, ss);
            }, minPollRate);
        }
    }
}

void NVMeMiSensorManager::updateSensors(uint8_t eid,
                                        nvme_mi_nvm_ss_health_status* ss)
{
    auto it = sensorContexts.find(eid);
    if (it == sensorContexts.end())
    {
        return;
    }
    auto& ctx = it->second;

    if (ss == nullptr)
    {
        if (ctx->tempSensor)
        {
            ctx->tempSensor->incrementError();
        }
        return;
    }

    if (ctx->tempSensor)
    {
        if (ctx->tempSensor->inError())
        {
            if (ctx->tempScanDelay == 0)
            {
                ctx->tempScanDelay = scanDelayTicks;
            }
            ctx->tempScanDelay--;
        }

        if (ctx->tempScanDelay == 0)
        {
            double temp = getTemperatureReading(ss);
            if (std::isnan(temp))
            {
                ctx->tempSensor->incrementError();
            }
            else
            {
                ctx->tempSensor->updateValue(temp);
            }
        }
    }

    if (ctx->statusSensor)
    {
        bool present = true;
        bool functional = true;
        bool fault = false;
        if ((ss->nss & nvmeMiNssDriveFault) != 0)
        {
            fault = true;
            functional = false;
        }
        if (ss->sw != 0)
        {
            fault = true;
            functional = false;
        }
        ctx->statusSensor->updateStatus(present, functional, fault);
    }
}

void NVMeMiSensorManager::removeSensors(uint8_t eid)
{
    auto it = sensorContexts.find(eid);
    if (it != sensorContexts.end())
    {
        sensorContexts.erase(it);
    }
}

void NVMeMiSensorManager::createSensors(uint8_t eid)
{
    auto cfgIt = cachedConfigByEid.find(eid);
    if (cfgIt != cachedConfigByEid.end())
    {
        createSensorsWithConfig(eid, cachedConfigByEid);
        return;
    }

    pendingEids.push_back(eid);
    if (configLoadInProgress)
    {
        return;
    }
    configLoadInProgress = true;

    auto getter = std::make_shared<GetSensorConfiguration>(
        conn, [this](nvme::sensors::ManagedObjectType& configs) {
        std::vector<uint8_t> eids = std::move(pendingEids);
        pendingEids.clear();
        handleSensorConfigurations(configs, eids);
    });
    getter->getConfiguration(std::vector<std::string>{
        NVMeMiSensor::sensorType, NVMeMiStatusSensor::sensorType});
}
