/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#include "NVMeMiStatusSensor.hpp"

#include "SensorUtil.hpp"

#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <tuple>

using nvme::sensors::escapeName;

namespace fs = std::filesystem;

NVMeMiStatusSensor::NVMeMiStatusSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::string& sensorConfiguration,
    const uint8_t eid) :
    StatusInterface(
        static_cast<sdbusplus::bus_t&>(*conn),
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName))
            .c_str(),
        StatusInterface::action::defer_emit),
    eid(eid), name(sensorName), configurationPath(sensorConfiguration),
    dbusConnection(conn), objServer(objectServer)
{
    std::string path = "/xyz/openbmc_project/sensors/drive/" +
                       escapeName(sensorName);

    driveInterface = objectServer.add_interface(path,
                                                DriveInterface::interface);

    fs::path p(sensorConfiguration);
    nvme::sensors::AssociationList assocs = {};
    std::string chassisPath = p.parent_path().string();
    if (auto resolved = nvme::sensors::tryResolveChassisPath(conn, chassisPath))
    {
        chassisPath = *resolved;
        chassisResolved = true;
    }
    assocs.emplace_back(std::make_tuple("chassis", "all_sensors", chassisPath));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);

    if (!driveInterface->initialize())
    {
        lg2::error("Failed to initialize drive interface");
    }

    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(true);
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus::functional(false);
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus::state(
            sdbusplus::xyz::openbmc_project::State::Decorator::server::
                OperationalStatus::StateType::None);

    emit_object_added();
}

void NVMeMiStatusSensor::refreshAssociation()
{
    if (chassisResolved)
    {
        return;
    }
    auto resolved = nvme::sensors::tryResolveChassisPath(
        dbusConnection, fs::path(configurationPath).parent_path().string());
    if (!resolved)
    {
        return;
    }
    chassisResolved = true;
    nvme::sensors::AssociationList assocs;
    assocs.emplace_back(std::make_tuple("chassis", "all_sensors", *resolved));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);
    lg2::info("Sensor {NAME} now associated with chassis {CHASSIS}", "NAME",
              name, "CHASSIS", *resolved);
}

NVMeMiStatusSensor::~NVMeMiStatusSensor()
{
    objServer.remove_interface(driveInterface);
}

void NVMeMiStatusSensor::updateStatus(bool present, bool functional, bool fault)
{
    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(present);
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus::functional(functional);

    if (fault)
    {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::Fault);
    }
    else
    {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::None);
    }
}
