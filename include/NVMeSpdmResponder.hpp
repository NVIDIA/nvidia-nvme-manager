/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <spdmcpp/context.hpp>
#include <spdmcpp/dbus_impl_responder.hpp>

#include <bitset>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

class NVMeDevice;
class MctpEndpointCoordinator;

namespace nvme::spdm
{

struct Endpoint
{
    uint8_t eid{};
    sdbusplus::object_path mctpPath;
    sdbusplus::object_path inventoryPath;
    std::weak_ptr<NVMeDevice> drive;
    std::shared_ptr<MctpEndpointCoordinator> coordinator;
};

class Manager;
class Responder;

class Responder : public spdmcpp::Responder
{
  public:
    Responder() = delete;
    Responder(const Responder&) = delete;
    Responder& operator=(const Responder&) = delete;
    Responder(Responder&&) = delete;
    Responder& operator=(Responder&&) = delete;

    Responder(Manager& manager, const std::string& path,
              const Endpoint& endpoint);
    ~Responder() override;

    void refresh(uint8_t slot, std::vector<uint8_t> nonce,
                 std::vector<uint8_t> measurementIndices,
                 uint32_t sessionId) override;
    void discover();

  private:
    Manager& manager;
    std::weak_ptr<NVMeDevice> drive;
    std::shared_ptr<MctpEndpointCoordinator> coordinator;
    bool spdmRequestPending{};
    bool spdmCoordinatorActive{};

    void handleError(spdmcpp::RetStat rs) override;
    spdmcpp::RetStat handleEventForRefresh(spdmcpp::EventClass& event) override;
    void releaseCoordinator();
};

class Manager
{
  public:
    Manager(boost::asio::io_context& io,
            std::shared_ptr<sdbusplus::asio::connection> conn,
            std::ostream& logStream, bool debug);
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager() = default;

    void addOrUpdateResponder(const Endpoint& endpoint);
    void removeResponder(uint8_t eid);
    void removeAllResponders();

    boost::asio::io_context& getIo()
    {
        return io;
    }

    sdbusplus::asio::connection& getBus()
    {
        return *conn;
    }

    spdmcpp::ContextClass& getContext()
    {
        return context;
    }

    spdmcpp::LogClass& getLog()
    {
        return log;
    }

    void reportError(const std::string& message);

  private:
    bool ensureTransport();
    void waitForMctpPacket(std::shared_ptr<spdmcpp::MctpIoClass> mctpIo);
    void mctpCallback(uint32_t revents, spdmcpp::MctpIoClass& mctpIo);
    static std::string
        makeResponderPath(const sdbusplus::object_path& inventoryPath,
                          uint8_t eid);

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    spdmcpp::LogClass log;
    spdmcpp::ContextClass context;
    std::unordered_map<uint8_t, std::shared_ptr<Responder>> responders;
    std::vector<uint8_t> packetBuffer;

    std::unique_ptr<boost::asio::posix::stream_descriptor> afMctpEvent;
};

} // namespace nvme::spdm
