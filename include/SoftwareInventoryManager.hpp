#pragma once

#include "SoftwareInventory.hpp"

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>

#include <memory>
#include <unordered_map>

using SoftwareVersion =
    sdbusplus::xyz::openbmc_project::Software::server::Version;

class SoftwareInventoryManager
{
  public:
    explicit SoftwareInventoryManager(sdbusplus::bus::bus& bus) : bus(bus) {}

    // Create software inventory object for an NVMe device
    std::shared_ptr<SoftwareInventory> createNVMeSoftwareInventory(
        const std::string& devicePath, const std::string& manufacturer,
        const std::string& model, const std::string& serialNumber,
        const std::string& partNumber, const std::string& version,
        const std::string& softwareId = "0x0000")
    {
        std::string softwarePath = "/xyz/openbmc_project/software/" +
                                   getSoftwareName(devicePath);

        auto software = std::make_shared<SoftwareInventory>(bus, softwarePath);

        // Set software version information
        software->updatePurpose(SoftwareVersion::VersionPurpose::Other);
        software->updateSoftwareId(softwareId);
        software->updateVersion(version);

        // Set asset information
        software->updateManufacturer(manufacturer);
        software->updateModel(model);
        software->updateSerialNumber(serialNumber);
        software->updatePartNumber(partNumber);

        // Create associations
        AssociationList associations;
        associations.emplace_back("inventory", "activation", devicePath);
        associations.emplace_back("software_version", "updateable",
                                  "/xyz/openbmc_project/software");
        software->setAssociations(associations);

        // Store the software inventory object
        softwareInventoryMap[devicePath] = software;

        return software;
    }

    // Update firmware version for an NVMe device
    void updateNVMeFirmwareVersion(const std::string& devicePath,
                                   const std::string& version)
    {
        auto it = softwareInventoryMap.find(devicePath);
        if (it != softwareInventoryMap.end())
        {
            it->second->updateVersion(version);
        }
    }

    // Get software inventory object for a device
    std::shared_ptr<SoftwareInventory>
        getSoftwareInventory(const std::string& devicePath)
    {
        auto it = softwareInventoryMap.find(devicePath);
        if (it != softwareInventoryMap.end())
        {
            return it->second;
        }
        return nullptr;
    }

    // Remove software inventory object for a device
    void removeSoftwareInventory(const std::string& devicePath)
    {
        auto it = softwareInventoryMap.find(devicePath);
        if (it != softwareInventoryMap.end())
        {
            softwareInventoryMap.erase(it);
        }
    }

    // Check if software inventory exists for a device
    bool hasSoftwareInventory(const std::string& devicePath)
    {
        return softwareInventoryMap.contains(devicePath);
    }

  private:
    sdbusplus::bus::bus& bus;
    std::unordered_map<std::string, std::shared_ptr<SoftwareInventory>>
        softwareInventoryMap;

    // Generate a unique software name from device path
    static std::string getSoftwareName(const std::string& devicePath)
    {
        // Extract device identifier from path
        // Example: /xyz/openbmc_project/inventory/system/nvme/NVMe_SSD_1
        // becomes: NVMe_SSD_1_FW
        size_t lastSlash = devicePath.find_last_of('/');
        if (lastSlash != std::string::npos)
        {
            std::string deviceName = devicePath.substr(lastSlash + 1);
            return "FW_" + deviceName;
        }
        return "Unknown_FW";
    }
};
