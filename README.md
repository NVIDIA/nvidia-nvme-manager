# NVMe Device Manager for OpenBMC

A D-Bus based NVMe device management daemon for OpenBMC systems. This
service discovers, monitors, and manages NVMe drives through the NVMe
Management Interface (NVMe-MI) over MCTP (Management Component Transport
Protocol).

## Overview

This daemon provides comprehensive NVMe drive management capabilities including:

- **Automatic Discovery**: Discovers NVMe drives via MCTP endpoints
- **Health Monitoring**: Monitors drive health through SMART attributes and
  status polling
- **Drive Information**: Exposes detailed drive information (model, serial,
  firmware version, capacity, etc.)
- **Secure Erase**: Supports various sanitization methods (block erase,
  overwrite, crypto erase)
- **Firmware Inventory**: Optional firmware version tracking and inventory management
- **D-Bus Integration**: Full integration with OpenBMC D-Bus infrastructure

## Features

### Drive Management

- NVMe-MI protocol support via libnvme-mi
- Real-time drive status and health monitoring
- SMART warning detection and Redfish event generation
- Drive association with system topology

### Supported Operations

- Identify drive (model, serial number, manufacturer, capacity)
- Query drive health and operational status
- Secure erase with progress tracking
- Link status detection
- Form factor detection

### D-Bus Interfaces

Exposes the following OpenBMC D-Bus interfaces:

- `xyz.openbmc_project.Inventory.Item`
- `xyz.openbmc_project.Inventory.Item.StorageController`
- `xyz.openbmc_project.Inventory.Item.Drive`
- `xyz.openbmc_project.State.Decorator.Health`
- `xyz.openbmc_project.State.Decorator.OperationalStatus`
- `xyz.openbmc_project.Nvme.Status`
- `xyz.openbmc_project.Nvme.SecureErase`
- `xyz.openbmc_project.Nvme.Operation`
- And more...

## Building

### Prerequisites

- C++23 compatible compiler (GCC 12+ or Clang 15+)
- Meson build system (>= 1.1.1)
- libnvme and libnvme-mi libraries
- sdbusplus (OpenBMC D-Bus library)
- phosphor-logging
- Boost.Asio
- nlohmann-json
- systemd

### Build Instructions

```bash
#Configure the build
meson setup build

#Compile
ninja -C build

#Install
sudo ninja -C build install
```

### Build Options

Configure build options using `-D<option>=<value>`:

| Option                    | Type    | Default           | Description    |
| ------------------------- | ------- | ----------------- | -------------- |
| `platform_drive_location` | string  | `.../Baseboard_0` | Location       |
| `platform_drive_prefix`   | string  | `NVMe_SSD_`       | Drive prefix   |
| `drive_sanitize_time`     | integer | 30                | Sanitize (sec) |
| `identify_rsp_length`     | integer | 384               | Identify len   |
| `inkernel_mctp`           | boolean | false             | In-kernel MCTP |
| `firmware_inventory`      | boolean | false             | FW inventory   |

Example:

```bash
meson setup build -Dinkernel_mctp=true -Dfirmware_inventory=true
```

## Usage

### Service Management

The daemon is managed through systemd:

```bash
#Start the service
sudo systemctl start nvidia-nvme-manager.service

#Enable on boot
sudo systemctl enable nvidia-nvme-manager.service

#Check status
sudo systemctl status nvidia-nvme-manager.service
```

### D-Bus Interface

Query NVMe drives via D-Bus:

```bash
#List all NVMe drives
busctl tree xyz.openbmc_project.NVMeDevice

#Get drive properties
busctl introspect xyz.openbmc_project.NVMeDevice \
    /xyz/openbmc_project/inventory/system/nvme/NVMe_SSD_0

#Read specific property(e.g., model)
busctl get-property xyz.openbmc_project.NVMeDevice \
    /xyz/openbmc_project/inventory/system/nvme/NVMe_SSD_0 \
    xyz.openbmc_project.Inventory.Decorator.Asset Model
```

## Firmware Update Tool

The project includes a standalone firmware update utility (`NVMeFwUpdate`)
for updating NVMe drive firmware over NVMe-MI/MCTP.

### Tool Features

- **Parallel Updates**: Update multiple NVMe drives simultaneously
- **Chunked Transfer**: Downloads firmware in 4KB chunks for reliability
- **Retry Logic**: Automatic retry for transient failures
- **Event Logging**: Generates Redfish-style events for update progress
- **D-Bus Integration**: Logs update events to OpenBMC logging infrastructure

### Firmware Update Process

1. **Download**: Transfers firmware file to drive(s) in 4KB chunks
2. **Commit**: Activates the new firmware on the drive
3. **Verification**: Reports success or failure for each device

### Tool Usage

```bash
#Update single drive
NVMeFwUpdate /path/to/firmware.bin GB232 \
    /xyz/openbmc_project/inventory/system/chassis/motherboard/drive 200

#Update multiple drives in parallel
NVMeFwUpdate /path/to/firmware.bin GB232 \
    /xyz/openbmc_project/inventory/system/chassis/motherboard/drive 200 201 202

#Enable verbose logging
NVMeFwUpdate /path/to/firmware.bin GB232 \
    /xyz/openbmc_project/inventory/system/chassis/motherboard/drive 200 201 -v
```

### Command Arguments

| Argument             | Description                             |
| -------------------- | --------------------------------------- |
| `firmware_path`      | Path to the firmware binary file        |
| `version`            | Version string identifier (e.g., GB232) |
| `object_path_prefix` | D-Bus inventory object path prefix      |
| `eid1 [eid2] ...`    | One or more MCTP Endpoint IDs to update |
| `-v, --verbose`      | Enable detailed logging output          |

### Event Logging

The tool generates D-Bus events at each stage:

- **Target Determined**: Firmware update target identified
- **Transferring to Component**: Download started
- **Transfer Failed**: Download encountered an error
- **Await to Activate**: Download complete, ready for activation
- **Update Successful**: Firmware committed successfully
- **Apply Failed**: Firmware commit failed
- **Activate Failed**: Firmware activation failed

### Exit Codes

- `0`: All firmware updates completed successfully
- `1`: One or more firmware updates failed or invalid arguments

### Notes

- Requires MCTP endpoints to be configured and accessible
- Supports in-kernel MCTP when built with `-Dinkernel_mctp=true`
- Firmware file must be compatible with target NVMe devices
- Updates run in parallel threads for efficiency

## Architecture

### Components

- **NVMeDevice**: Main class managing individual NVMe drive
- **NVMeMi**: NVMe-MI protocol interface wrapper
- **MCTPDiscovery**: MCTP endpoint discovery and monitoring
- **SoftwareInventoryManager**: Firmware version tracking (optional)

### Communication Flow

1. Daemon discovers MCTP endpoints with NVMe-MI capability
2. Creates NVMeDevice instances for each discovered drive
3. Polls drives periodically for status and health
4. Exposes drive information via D-Bus interfaces
5. Listens for MCTP endpoint addition/removal events

### Threading Model

- Boost.Asio io_context for async operations
- Timer-based polling for drive health
- Event-driven MCTP endpoint monitoring

## Configuration

The service automatically discovers NVMe drives through MCTP. Configuration
is primarily done through:

1. **Build-time options** (see Build Options section)
2. **Entity Manager configuration** for drive topology and associations
3. **MCTP daemon** for endpoint discovery

## Development

### Code Style

- C++23 standard
- Follow OpenBMC coding guidelines
- Use clang-tidy for linting (see `run-clang-tidy.sh`)

### Project Structure

```text
.
├── include/          # Header files
│   ├── NVMeDevice.hpp
│   ├── NVMeMi.hpp
│   └── ...
├── src/              # Source files
│   ├── NVMeDevice.cpp
│   ├── NVMeDeviceMain.cpp
│   └── NVMeMi.cpp
├── tool/             # Utility tools
│   └── NVMeFwUpdate.cpp
├── service_files/    # Systemd service files
└── docs/             # Documentation
```

## License

Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.

## Contributing

This is a proprietary NVIDIA project. For issues or contributions, please
contact NVIDIA.

## References

- [NVMe Management Interface Specification](https://nvmexpress.org/specifications/)
- [OpenBMC Project](https://github.com/openbmc/openbmc)
- [libnvme](https://github.com/linux-nvme/libnvme)
- [MCTP Specification](https://www.dmtf.org/standards/pmci)
