/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */

#include <nvme-mi_config.h>
#include <sys/epoll.h>

#include <MctpEndpointCoordinator.hpp>
#include <NVMeDevice.hpp>
#include <NVMeSpdmResponder.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <bitset>
#include <exception>
#include <utility>

namespace nvme::spdm
{
using NotAllowed = sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
using Unavailable = sdbusplus::xyz::openbmc_project::Common::Error::Unavailable;

namespace
{

constexpr auto spdmRootObjectPath = "/xyz/openbmc_project/SPDM";

} // namespace

Responder::Responder(Manager& manager, const std::string& path,
                     const Endpoint& endpoint) :
    spdmcpp::Responder(manager.getBus(), manager.getIo(), manager.getContext(),
                       manager.getLog(),
                       [&manager](const std::string& message) {
                           manager.reportError(message);
                       }, path, endpoint.eid, endpoint.mctpPath,
                       endpoint.inventoryPath, "", "", ""),
    manager(manager), drive(endpoint.drive), coordinator(endpoint.coordinator)
{}

Responder::~Responder()
{
    releaseCoordinator();
}

void Responder::handleError(spdmcpp::RetStat rs)
{
    releaseCoordinator();
    spdmcpp::Responder::handleError(rs);
}

spdmcpp::RetStat Responder::handleEventForRefresh(spdmcpp::EventClass& event)
{
    const auto rs = spdmcpp::Responder::handleEventForRefresh(event);
    if (!spdmcpp::isError(rs) && event.is<spdmcpp::EventReceiveClass>() &&
        !connection.isWaitingForResponse())
    {
        releaseCoordinator();
    }
    return rs;
}

void Responder::refresh(uint8_t slotIndex, std::vector<uint8_t> nonceValue,
                        std::vector<uint8_t> measurementIndices,
                        uint32_t sessionId)
{
    if (drive.expired())
    {
        status(SPDMStatus::Error_Other);
        throw NotAllowed();
    }

    if (sessionId != 0)
    {
        status(SPDMStatus::Error_InvalidArguments);
        return;
    }

    if (connection.isWaitingForResponse() || spdmRequestPending)
    {
        getLog().iprint("SPDM refresh rejected while previous request is "
                        "still processing for EID ");
        getLog().iprintln(eid);
        throw Unavailable();
    }

    if (slotIndex >= spdmcpp::ConnectionClass::slotNum)
    {
        status(SPDMStatus::Error_InvalidArguments);
        return;
    }

    if (!nonceValue.empty() && nonceValue.size() != 32)
    {
        status(SPDMStatus::Error_InvalidArguments);
        return;
    }

    std::bitset<256> meas;
    if (measurementIndices.empty() ||
        (measurementIndices.size() == 1 && measurementIndices[0] == 255))
    {
        meas.set(255);
    }
    else
    {
        for (const auto index : measurementIndices)
        {
            if (index >= 255 || meas[index])
            {
                status(SPDMStatus::Error_InvalidArguments);
                return;
            }
            meas.set(index);
        }
    }

    spdmRequestPending = true;
    auto weak = weak_from_this();
    std::weak_ptr<MctpEndpointCoordinator> coordinatorWeak = coordinator;
    auto start = [weak, coordinatorWeak, slotIndex,
                  nonceValue = std::move(nonceValue),
                  measurementIndices = std::move(measurementIndices),
                  sessionId]() mutable {
        auto self = std::static_pointer_cast<Responder>(weak.lock());
        if (!self)
        {
            if (auto coordinator = coordinatorWeak.lock())
            {
                coordinator->releaseSpdm();
            }
            return;
        }

        boost::asio::post(self->manager.getIo(),
                          [self, slotIndex, nonceValue = std::move(nonceValue),
                           measurementIndices = std::move(measurementIndices),
                           sessionId]() mutable {
            self->spdmRequestPending = false;
            self->spdmCoordinatorActive = self->coordinator != nullptr;
            if (self->drive.expired())
            {
                self->status(SPDMStatus::Error_Other);
                self->releaseCoordinator();
                return;
            }
            self->status(SPDMStatus::GettingVersion);
            try
            {
                self->spdmcpp::Responder::refresh(
                    slotIndex, std::move(nonceValue),
                    std::move(measurementIndices), sessionId);
            }
            catch (const std::exception& e)
            {
                lg2::error("Failed to start SPDM refresh for EID {EID}: {ERR}",
                           "EID", static_cast<int>(self->eid), "ERR", e.what());
                self->handleError(spdmcpp::RetStat::ERROR_UNKNOWN);
            }
            catch (...)
            {
                lg2::error("Failed to start SPDM refresh for EID {EID}", "EID",
                           static_cast<int>(self->eid));
                self->handleError(spdmcpp::RetStat::ERROR_UNKNOWN);
            }
        });
    };

    auto onStartFailure = [weak](std::exception_ptr exception) {
        auto self = std::static_pointer_cast<Responder>(weak.lock());
        if (!self)
        {
            return;
        }
        self->spdmRequestPending = false;

        try
        {
            std::rethrow_exception(exception);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to schedule SPDM refresh for EID {EID}: {ERR}",
                       "EID", static_cast<int>(self->eid), "ERR", e.what());
        }
        catch (...)
        {
            lg2::error("Failed to schedule SPDM refresh for EID {EID}", "EID",
                       static_cast<int>(self->eid));
        }

        self->handleError(spdmcpp::RetStat::ERROR_UNKNOWN);
    };

    if (coordinator &&
        !coordinator->requestSpdm(std::move(start), std::move(onStartFailure)))
    {
        spdmRequestPending = false;
        getLog().iprint("SPDM refresh rejected while another transaction is "
                        "pending for EID ");
        getLog().iprintln(eid);
        throw Unavailable();
    }
    else if (!coordinator)
    {
        start();
    }
}

void Responder::releaseCoordinator()
{
    if (spdmCoordinatorActive && coordinator)
    {
        spdmCoordinatorActive = false;
        coordinator->releaseSpdm();
    }
}

void Responder::discover()
{
    if (drive.expired())
    {
        status(SPDMStatus::Error_Other);
        lg2::info("Skipping automatic SPDM discovery for EID {EID}: drive "
                  "unavailable",
                  "EID", static_cast<int>(eid));
        return;
    }

    try
    {
        refresh(0, {}, {}, 0);
    }
    catch (const std::exception& e)
    {
        status(SPDMStatus::Error_Other);
        lg2::warning("SPDM discovery failed to start for EID {EID}: {ERR}",
                     "EID", static_cast<int>(eid), "ERR", e.what());
    }
    catch (...)
    {
        status(SPDMStatus::Error_Other);
        lg2::warning("SPDM discovery failed to start for EID {EID}", "EID",
                     static_cast<int>(eid));
    }
}

Manager::Manager(boost::asio::io_context& io,
                 std::shared_ptr<sdbusplus::asio::connection> conn,
                 std::ostream& logStream, bool debug) :
    io(io), conn(std::move(conn)), log(logStream)
{
    if (debug)
    {
        log.setLogLevel(spdmcpp::LogClass::Level::Debug);
        lg2::info("SPDM debug logging enabled");
    }
    else
    {
        log.setLogLevel(spdmcpp::LogClass::Level::Error);
    }
}

void Manager::reportError(const std::string& message)
{
    lg2::error("{MSG}", "MSG", message);
}

std::string
    Manager::makeResponderPath(const sdbusplus::object_path& inventoryPath,
                               uint8_t eid)
{
    std::string path(spdmRootObjectPath);
    path += '/';
    auto name = inventoryPath.filename();
    if (!name.empty())
    {
        path += spdmResourcePrefix;
        path += name;
    }
    else
    {
        path += spdmResourcePrefix;
        path += "NVMe_";
        path += std::to_string(eid);
    }
    return path;
}

bool Manager::ensureTransport()
{
    if (context.getIO())
    {
        return true;
    }

    auto mctpIo = std::make_shared<spdmcpp::MctpIoClass>(log);
    if (!mctpIo->createSocket())
    {
        lg2::error("Failed to create SPDM AF_MCTP socket");
        return false;
    }

    afMctpEvent = std::make_unique<boost::asio::posix::stream_descriptor>(
        io, mctpIo->getSocket());
    context.registerIo(mctpIo);
    waitForMctpPacket(std::move(mctpIo));
    return true;
}

void Manager::waitForMctpPacket(std::shared_ptr<spdmcpp::MctpIoClass> mctpIo)
{
    afMctpEvent->async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this, mctpIo](const boost::system::error_code& ec) {
        if (ec)
        {
            lg2::error("SPDM AF_MCTP wait failed: {ERR}", "ERR", ec.message());
            return;
        }
        mctpCallback(EPOLLIN, *mctpIo);
        waitForMctpPacket(mctpIo);
    });
}

void Manager::addOrUpdateResponder(const Endpoint& endpoint)
{
    if (!ensureTransport())
    {
        return;
    }

    if (auto existing = responders.find(endpoint.eid);
        existing != responders.end() && existing->second)
    {
        lg2::debug("NVMe SPDM responder already exists; ignoring duplicate "
                   "discovery for EID {EID}",
                   "EID", static_cast<int>(endpoint.eid));
        return;
    }

    const auto path = makeResponderPath(endpoint.inventoryPath, endpoint.eid);
    try
    {
        lg2::info("Creating NVMe SPDM responder object: EID {EID}, D-Bus path "
                  "{RESPONDER_PATH}, MCTP path {MCTP_PATH}, inventory path "
                  "{INVENTORY_PATH}",
                  "EID", static_cast<int>(endpoint.eid), "RESPONDER_PATH", path,
                  "MCTP_PATH", endpoint.mctpPath.str, "INVENTORY_PATH",
                  endpoint.inventoryPath.str);
        auto responder = std::make_shared<Responder>(*this, path, endpoint);
        responders[endpoint.eid] = responder;
        responder->discover();
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error(
            "Failed to create NVMe SPDM responder; preserving service: EID "
            "{EID}, D-Bus path {RESPONDER_PATH}, error {ERR}",
            "EID", static_cast<int>(endpoint.eid), "RESPONDER_PATH", path,
            "ERR", e.what());
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Unexpected NVMe SPDM responder creation failure; preserving "
            "service: EID {EID}, D-Bus path {RESPONDER_PATH}, error {ERR}",
            "EID", static_cast<int>(endpoint.eid), "RESPONDER_PATH", path,
            "ERR", e.what());
    }
}

void Manager::removeResponder(uint8_t eid)
{
    auto it = responders.find(eid);
    if (it == responders.end())
    {
        return;
    }
    lg2::info("Removing NVMe SPDM responder object for EID {EID}", "EID",
              static_cast<int>(eid));
    responders.erase(it);
}

void Manager::removeAllResponders()
{
    responders.clear();
}

void Manager::mctpCallback(uint32_t revents, spdmcpp::MctpIoClass& mctpIo)
{
    if ((revents & EPOLLIN) == 0)
    {
        return;
    }

    auto rs = mctpIo.read(packetBuffer);
    if (rs != spdmcpp::RetStat::OK)
    {
        lg2::error("SPDM MCTP read failed");
        return;
    }

    uint8_t eid = 0;
    spdmcpp::TransportClass::LayerState lay;
    rs = spdmcpp::MctpTransportClass::peekEid(packetBuffer, lay, eid);
    if (rs != spdmcpp::RetStat::OK)
    {
        lg2::warning("SPDM MCTP packet decode failed: {ERR}", "ERR",
                     spdmcpp::get_cstr(rs));
        return;
    }

    auto it = responders.find(eid);
    if (it == responders.end() || !it->second)
    {
        lg2::warning("SPDM response received for unknown EID {EID}", "EID",
                     static_cast<int>(eid));
        return;
    }

    spdmcpp::EventReceiveClass event(packetBuffer);
    it->second->handleEvent(event);
}

} // namespace nvme::spdm
