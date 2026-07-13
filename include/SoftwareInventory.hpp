#pragma once

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>

#include <algorithm>

using SoftwareVersion =
    sdbusplus::xyz::openbmc_project::Software::server::Version;
using Asset =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset;
using Associations =
    sdbusplus::xyz::openbmc_project::Association::server::Definitions;

using SoftwareInventoryInterfaces =
    sdbusplus::server::object::object<SoftwareVersion, Asset, Associations>;

using AssociationList =
    std::vector<std::tuple<std::string, std::string, std::string>>;

class SoftwareInventory : public SoftwareInventoryInterfaces
{
  public:
    SoftwareInventory(sdbusplus::bus::bus& bus, const std::string& path) :
        SoftwareInventoryInterfaces(
            bus, path.c_str(),
            SoftwareInventoryInterfaces::action::emit_interface_added)
    {
        // Set default values
        SoftwareVersion::purpose(SoftwareVersion::VersionPurpose::Other);
        SoftwareVersion::softwareId("0x0000");
        SoftwareVersion::version("0.0.0");

        // Set default asset information
        Asset::manufacturer("Unknown");
        Asset::model("");
        Asset::serialNumber("");
        Asset::partNumber("");
    }

    void updateVersion(const std::string& version)
    {
        SoftwareVersion::version(version, true);
    }

    void updateSoftwareId(const std::string& softwareId)
    {
        SoftwareVersion::softwareId(softwareId, true);
    }

    void updatePurpose(SoftwareVersion::VersionPurpose purpose)
    {
        SoftwareVersion::purpose(purpose, true);
    }

    void updateManufacturer(const std::string& manufacturer)
    {
        Asset::manufacturer(manufacturer, true);
    }

    void updateModel(const std::string& model)
    {
        Asset::model(model, true);
    }

    void updateSerialNumber(const std::string& serialNumber)
    {
        Asset::serialNumber(serialNumber, true);
    }

    void updatePartNumber(const std::string& partNumber)
    {
        Asset::partNumber(partNumber, true);
    }

    void setAssociations(const AssociationList& associations)
    {
        Associations::associations(associations);
    }

    void addAssociation(const std::string& forwardType,
                        const std::string& reverseType, const std::string& path)
    {
        auto currentAssocs = Associations::associations();
        currentAssocs.emplace_back(forwardType, reverseType, path);
        Associations::associations(currentAssocs, true);
    }

    void removeAssociation(const std::string& forwardType,
                           const std::string& reverseType,
                           const std::string& path)
    {
        auto currentAssocs = Associations::associations();
        currentAssocs.erase(std::remove_if(currentAssocs.begin(),
                                           currentAssocs.end(),
                                           [&](const auto& assoc) {
            return std::get<0>(assoc) == forwardType &&
                   std::get<1>(assoc) == reverseType &&
                   std::get<2>(assoc) == path;
        }),
                            currentAssocs.end());
        Associations::associations(currentAssocs, true);
    }
};
