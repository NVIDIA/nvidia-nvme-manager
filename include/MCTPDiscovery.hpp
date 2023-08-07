#pragma once
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/message/types.hpp>
#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

const constexpr char* inventoryPath = "/xyz/openbmc_project/inventory";

using BasicVariantType =
    std::variant<std::vector<std::string>, std::vector<uint8_t>, std::string,
                 int64_t, uint64_t, int32_t, uint32_t, int16_t,
                 uint16_t, uint8_t>;
using Properties =
    boost::container::flat_map<std::string, BasicVariantType>;
using DbusObject = boost::container::flat_map<std::string, Properties>;
using ManagedObjectType =
    boost::container::flat_map<sdbusplus::message::object_path, DbusObject>;

using GetSubTreeType = std::vector<
    std::pair<std::string,
              std::vector<std::pair<std::string, std::vector<std::string>>>>>;
using Association = std::tuple<std::string, std::string, std::string>;
using AssociationList =
    std::vector<std::tuple<std::string, std::string, std::string>>;
/*
bool getMctpEpInfo(
    const std::string& type,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    ManagedObjectType& resp);
*/

void createAssociation(
    std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path);

namespace mapper
{
constexpr const char* busName = "xyz.openbmc_project.ObjectMapper";
constexpr const char* path = "/xyz/openbmc_project/object_mapper";
constexpr const char* interface = "xyz.openbmc_project.ObjectMapper";
constexpr const char* subtree = "GetSubTree";
} // namespace mapper

namespace properties
{
constexpr const char* interface = "org.freedesktop.DBus.Properties";
constexpr const char* get = "Get";
constexpr const char* set = "Set";
} // namespace properties

namespace association
{
const static constexpr char* interface =
    "xyz.openbmc_project.Association.Definitions";
} // namespace association

void createInventoryAssoc(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path);

struct getMctpEpInfo :
    std::enable_shared_from_this<getMctpEpInfo>
{
    getMctpEpInfo(
        std::shared_ptr<sdbusplus::asio::connection> connection,
        std::function<void(ManagedObjectType& resp)>&& callbackFunc) :
        dbusConnection(std::move(connection)),
        callback(std::move(callbackFunc))
    {}

    void getPath(const std::string& path, const std::string& interface,
                 const std::string& owner, size_t retries = 5)
    {
        if (retries > 5)
        {
            retries = 5;
        }
        std::shared_ptr<getMctpEpInfo> self = shared_from_this();

        self->dbusConnection->async_method_call(
            [self, path, interface, owner,
             retries](const boost::system::error_code ec,
                      boost::container::flat_map<std::string, BasicVariantType>&
                          data) {
                if (ec)
                {
                  lg2::error("Error getting {PATH} : retries left {RETRY}",
                             "PATH", path, "RETRY", retries - 1);
                  if (!retries) {
                    return;
                    }
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        self->dbusConnection->get_io_context());
                    timer->expires_after(std::chrono::seconds(10));
                    timer->async_wait([self, timer, path, interface, owner,
                                       retries](boost::system::error_code ec) {
                        if (ec)
                        {
                            lg2::error("Timer error");
                            return;
                        }
                        self->getPath(path, interface, owner, retries - 1);
                    });
                    return;
                }

                self->respData[path][interface] = std::move(data);
            },
            owner, path, "org.freedesktop.DBus.Properties", "GetAll",
            interface);
    }

    void getConfiguration(const std::vector<std::string>& interfaces,
                          size_t retries = 0)
    {
        if (retries > 5)
        {
            retries = 5;
        }

        std::shared_ptr<getMctpEpInfo> self = shared_from_this();
        dbusConnection->async_method_call(
            [self, interfaces, retries](const boost::system::error_code ec,
                                        const GetSubTreeType& ret) {
                if (ec)
                {
                    lg2::error("Error calling mapper");
                    if (!retries)
                    {
                        return;
                    }
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        self->dbusConnection->get_io_context());
                    timer->expires_after(std::chrono::seconds(10));
                    timer->async_wait([self, timer, interfaces,
                                       retries](boost::system::error_code ec) {
                        if (ec)
                        {
                            lg2::error("Timer error");
                            return;
                        }
                        self->getConfiguration(interfaces, retries - 1);
                    });

                    return;
                }
                for (const auto& [path, objDict] : ret)
                {
                    if (objDict.empty())
                    {
                        return;
                    }
                    const std::string& owner = objDict.begin()->first;

                    for (const std::string& interface : objDict.begin()->second)
                    {
                        // anything that starts with a requested configuration
                        // is good
                        if (std::find_if(
                                interfaces.begin(), interfaces.end(),
                                [interface](const std::string& possible) {
                                    return boost::starts_with(interface,
                                                              possible);
                                }) == interfaces.end())
                        {
                            continue;
                        }
                        self->getPath(path, interface, owner);
                    }
                }
            },
            mapper::busName, mapper::path, mapper::interface, mapper::subtree,
            "/", 0, interfaces);
    }

    ~getMctpEpInfo()
    {
        callback(respData);
    }

    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::function<void(ManagedObjectType& resp)> callback;
    ManagedObjectType respData;
};