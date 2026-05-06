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

#include <algorithm>
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

static bool hasPendingEid(const std::vector<uint8_t>& pendingEids, uint8_t eid)
{
    return std::find(pendingEids.begin(), pendingEids.end(), eid) !=
           pendingEids.end();
}

static bool hasNvmeDriveInstance(uint8_t eid)
{
    auto& driveMap = getDriveMap();
    return driveMap.contains(eid);
}

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

    lg2::info("handleSensorConfigurations: {COUNT} EM config objects, "
              "{NEIDS} EIDs pending",
              "COUNT", sensorConfigurations.size(), "NEIDS",
              eidsToCreate.size());

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
                lg2::warning("Skipping NVME1000 config at {PATH}: "
                             "name={NAME} address/EID={EID} (missing or zero)",
                             "PATH", interfacePath.str, "NAME",
                             sensorName.value_or("(missing)"), "EID", eid);
                continue;
            }

            lg2::info("Found NVME1000 config: name={NAME} EID={EID}", "NAME",
                      *sensorName, "EID", eid);
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
                lg2::warning("Skipping Nvmem2 config at {PATH}: "
                             "name={NAME} address/EID={EID} (missing or zero)",
                             "PATH", interfacePath.str, "NAME",
                             sensorName.value_or("(missing)"), "EID", eid);
                continue;
            }

            lg2::info("Found Nvmem2 config: name={NAME} EID={EID}", "NAME",
                      *sensorName, "EID", eid);
            float pollRate = getPollRate(sensorConfig, 1.0F);
            pendingSensors.push_back(
                {interfacePath.str, *sensorName, eid, {}, pollRate, false});
        }
    }

    // Group config by EID
    std::map<uint8_t, std::vector<SensorConfig>> configsByEid;
    for (const auto& cfg : pendingSensors)
    {
        configsByEid[cfg.eid].push_back(cfg);
    }

    // Only update cache when EM returns actual configs; preserve existing
    // cache if EM is temporarily unavailable (e.g., timing race on BMC boot)
    if (!configsByEid.empty())
    {
        cachedConfigByEid = configsByEid;
    }
    configLoadInProgress = false;

    // Create sensors only for requested EIDs that still have a live NVMe
    // instance.  The async EM config callback can run after drive removal.
    for (uint8_t eid : eidsToCreate)
    {
        if (!hasNvmeDriveInstance(eid))
        {
            continue;
        }

        createSensorsWithConfig(eid, configsByEid);
    }

    // If EM now has configs, also create sensors for drives already in
    // driveMap that have no sensor context yet — handles the timing race
    // where drives were discovered before EM published its configs
    if (!configsByEid.empty())
    {
        auto& driveMap = getDriveMap();
        for (const auto& [eid, _] : driveMap)
        {
            if (!sensorContexts.contains(eid))
            {
                lg2::info(
                    "EM config now available, creating sensors for drive EID {EID} already in driveMap",
                    "EID", eid);
                createSensorsWithConfig(eid, configsByEid);
            }
        }
    }
}

void NVMeMiSensorManager::createSensorsWithConfig(
    uint8_t eid,
    const std::map<uint8_t, std::vector<SensorConfig>>& configsByEid)
{
    if (!hasNvmeDriveInstance(eid))
    {
        return;
    }

    if (sensorContexts.contains(eid))
    {
        return;
    }

    auto cfgIt = configsByEid.find(eid);
    if (cfgIt == configsByEid.end())
    {
        lg2::warning("No EM sensor config found for drive EID {EID} — "
                     "check that NVME1000/Nvmem2 config has Address={EID}",
                     "EID", eid);
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

void NVMeMiSensorManager::refreshSensors()
{
    // Call createSensors() for every drive that has no sensor context yet.
    // createSensors() uses the cache when available (instant), or batches
    // EIDs into a single async GetSensorConfiguration call.  It is a no-op
    // for drives that already have sensors, so spurious calls are harmless.
    auto& driveMap = getDriveMap();
    for (const auto& [eid, _] : driveMap)
    {
        if (!sensorContexts.contains(eid))
        {
            lg2::info(
                "refreshSensors: no sensor context for EID {EID}, triggering creation",
                "EID", eid);
            createSensors(eid);
        }
    }
}

void NVMeMiSensorManager::createSensors(uint8_t eid)
{
    if (!hasNvmeDriveInstance(eid))
    {
        return;
    }

    if (sensorContexts.contains(eid))
    {
        return;
    }

    auto cfgIt = cachedConfigByEid.find(eid);
    if (hasPendingEid(pendingEids, eid))
    {
        return;
    }

    if (cfgIt != cachedConfigByEid.end())
    {
        lg2::info("Creating sensors for drive EID {EID} from cached EM config",
                  "EID", eid);
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
