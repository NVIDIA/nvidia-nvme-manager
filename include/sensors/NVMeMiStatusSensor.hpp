/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "SensorUtil.hpp"

#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <cstdint>
#include <memory>
#include <string>

using StatusInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using DriveInterface =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;

class NVMeMiStatusSensor :
    public StatusInterface,
    public std::enable_shared_from_this<NVMeMiStatusSensor>
{
  public:
    static constexpr const char* sensorType = "Nvmem2";

    NVMeMiStatusSensor(sdbusplus::asio::object_server& objectServer,
                       std::shared_ptr<sdbusplus::asio::connection>& conn,
                       const std::string& sensorName,
                       const std::string& sensorConfiguration, uint8_t eid);
    ~NVMeMiStatusSensor() override;
    NVMeMiStatusSensor(const NVMeMiStatusSensor&) = delete;
    NVMeMiStatusSensor& operator=(const NVMeMiStatusSensor&) = delete;
    NVMeMiStatusSensor(NVMeMiStatusSensor&&) = delete;
    NVMeMiStatusSensor& operator=(NVMeMiStatusSensor&&) = delete;

    void updateStatus(bool present, bool functional, bool fault);

    const uint8_t eid;
    std::string name;
    std::string configurationPath;

  private:
    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> driveInterface;
};
