#include <NVMeDevice.hpp>

#include <iostream>

NVMeDevice::NVMeDevice(
    __attribute__((unused)) boost::asio::io_service& io,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    uint8_t eid, std::vector<uint8_t> addr) :
    StatusInterface(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "/xyz/openbmc_project/drive/1",
        StatusInterface::action::defer_emit),std::enable_shared_from_this<NVMeDevice>(),
    objServer(objectServer), scanTimer(io)
{
    
    
    driveInterface = objectServer.add_interface(
        "/xyz/openbmc_project/drive/1",
        DriveInterface::interface);

    
    /*
    std::filesystem::path p("/xyz/openbmc_project/drive/1");
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);
    */
    driveInterface->register_property("test2", 1);
    if (!driveInterface->initialize())
    {
        std::cerr << "error initializing interface\n";
    }
    nvmeCtx = std::make_shared<NVMeMI>(addr, eid);


    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(true);
}
    

std::string NVMeDevice::stripString(char *src, size_t len)
{
    std::string s;

    s.assign(src, src+len);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

void NVMeDevice::pollDevices()
{


    scanTimer.expires_from_now(boost::posix_time::seconds(1));
    scanTimer.async_wait([self{shared_from_this()}](
                             const boost::system::error_code errorCode) {
        if (errorCode == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        else if (errorCode)
        {
            std::cerr << "Error:" << errorCode.message() << "\n";
            return;
        }

        /*
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::Fault);
                    */

        auto ctx = self->getNVMeCtx();

        ctx->subsystemHealthStatusPoll([](nvme_mi_nvm_ss_health_status* ss) {
            printf("NVMe MI subsys health:\n");
            printf(" subsystem status:  0x%x\n", ss->nss);
            printf(" smart warnings:    0x%x\n", ss->sw);
            printf(" composite temp:    %d\n", ss->ctemp);
            printf(" drive life used:   %d%%\n", ss->pdlu);
            printf(" controller status: 0x%04x\n", ss->ccs);
        });
        ctx->adminIdentify(0, [self](nvme_id_ctrl* id) {
            printf(" PCI vendor: %04x\n", id->vid);
            if (id->vid == 0x144d)
            {
                self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                    server::Asset::manufacturer("SAMSUNG");
            }
            self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                server::Asset::serialNumber(
                    self->stripString(id->sn, sizeof(id->sn)));

            self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                server::Asset::model(self->stripString(id->mn, sizeof(id->mn)));

            self->sdbusplus::xyz::openbmc_project::Software::server::Version::
                version(self->stripString(id->fr, sizeof(id->fr)));
        });
        self->pollDevices();
    });
}

NVMeDevice::~NVMeDevice()
{
    std::cout <<"~NVMEDevice"<<std::endl;
    objServer.remove_interface(driveInterface);
}

