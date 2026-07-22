// NVMeMuxManager.cpp
// Watches xyz.openbmc_project.State.Chassis.CurrentPowerState and
// binds/unbinds pca954x I2C MUX devices on the NVMe backplane accordingly.
// Built only when the 'mux_manager' meson option is enabled.

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <variant>
#include <vector>

static constexpr auto chassisStatePath = "/xyz/openbmc_project/state/chassis0";
static constexpr auto chassisStateIface = "xyz.openbmc_project.State.Chassis";
static constexpr auto chassisStateProp = "CurrentPowerState";
static constexpr auto powerStateOn =
    "xyz.openbmc_project.State.Chassis.PowerState.On";

static constexpr auto pca954xBindPath = "/sys/bus/i2c/drivers/pca954x/bind";
static constexpr auto pca954xUnbindPath = "/sys/bus/i2c/drivers/pca954x/unbind";

static constexpr auto muxConfigPath = "/etc/nvme/mux-devices.json";

static std::vector<std::string> loadMuxDevices()
{
    std::vector<std::string> devices;
    std::ifstream f(muxConfigPath);
    if (!f.is_open())
    {
        lg2::error("NVMe MUX config not found: {PATH}", "PATH", muxConfigPath);
        return devices;
    }
    try
    {
        auto j = nlohmann::json::parse(f);
        for (const auto& entry : j)
        {
            devices.push_back(entry.get<std::string>());
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to parse MUX config: {ERR}", "ERR", e.what());
    }
    return devices;
}

static void writeSysfs(const char* path, const std::string& value)
{
    std::ofstream f(path);
    if (!f)
    {
        lg2::error("Cannot write to {PATH}", "PATH", path);
        return;
    }
    f << value << "\n";
}

static void bindMuxes(const std::vector<std::string>& devices)
{
    for (const auto& dev : devices)
    {
        // Extract bus number from "bus-addr" to build the channel-0 path
        auto dash = dev.find('-');
        std::string channelPath = "/sys/bus/i2c/devices/i2c-" +
                                  dev.substr(0, dash) + "/" + dev +
                                  "/channel-0";
        if (std::filesystem::exists(channelPath))
        {
            continue; // already bound
        }
        lg2::info("Binding NVMe I2C MUX {DEV}", "DEV", dev);
        writeSysfs(pca954xBindPath, dev);
    }
}

static void unbindMuxes(const std::vector<std::string>& devices)
{
    for (const auto& dev : devices)
    {
        if (!std::filesystem::exists(
                std::string("/sys/bus/i2c/drivers/pca954x/") + dev))
        {
            continue;
        }
        lg2::info("Unbinding NVMe I2C MUX {DEV}", "DEV", dev);
        writeSysfs(pca954xUnbindPath, dev);
    }
}

static bool isChassisPowerOn(sdbusplus::asio::connection& conn)
{
    try
    {
        auto method = conn.new_method_call(
            "xyz.openbmc_project.State.Chassis", chassisStatePath,
            "org.freedesktop.DBus.Properties", "Get");
        method.append(chassisStateIface, chassisStateProp);
        auto reply = conn.call(method);
        std::variant<std::string> state;
        reply.read(state);
        return std::get<std::string>(state) == powerStateOn;
    }
    catch (const std::exception& e)
    {
        lg2::debug("Chassis state query failed: {ERR}", "ERR", e.what());
        return false;
    }
}

int main()
{
    try
    {
        auto devices = loadMuxDevices();
        if (devices.empty())
        {
            lg2::error("No MUX devices configured, exiting");
            return 1;
        }

        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);

        // Sync to current chassis state at startup
        if (isChassisPowerOn(*conn))
        {
            bindMuxes(devices);
        }
        else
        {
            unbindMuxes(devices);
        }

        // Watch for chassis power state changes
        auto chassisMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*conn),
            sdbusplus::bus::match::rules::propertiesChanged(chassisStatePath,
                                                            chassisStateIface),
            [&devices](sdbusplus::message_t& msg) {
            std::string iface;
            std::map<std::string, std::variant<std::string>> changed;
            try
            {
                msg.read(iface, changed);
            }
            catch (const std::exception& e)
            {
                lg2::error("PropertiesChanged read failed: {ERR}", "ERR",
                           e.what());
                return;
            }

            auto it = changed.find(chassisStateProp);
            if (it == changed.end())
            {
                return;
            }

            const auto& state = std::get<std::string>(it->second);
            lg2::info("Chassis power state: {STATE}", "STATE", state);

            if (state == powerStateOn)
            {
                bindMuxes(devices);
            }
            else
            {
                unbindMuxes(devices);
            }
        });

        io.run();
    }
    catch (const std::exception& e)
    {
        lg2::error("Fatal error: {ERR}", "ERR", e.what());
        return 1;
    }
    return 0;
}
