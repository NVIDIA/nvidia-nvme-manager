/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <sdbusplus/asio/connection.hpp>

#include <memory>
#include <string>
#include <vector>

/** Start monitoring systemd for nvme-update@.service and run FW updates.
 *  Registers a match for JobNew; when the unit is
 * nvme-update@<instance>.service, parses instance (image path with -→/,
 * version, object path prefix, EIDs), runs download+commit per EID on the
 * nvme-manager Worker, updates Progress on each drive and emits FW update
 * events.
 */
void startNvmeFwUpdateMonitor(
    const std::shared_ptr<sdbusplus::asio::connection>& conn);
