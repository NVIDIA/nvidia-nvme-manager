#include <NVMeDevice.hpp>

#include <iostream>

NVMeDevice::NVMeDevice(
    boost::asio::io_service& io,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    uint8_t eid, std::vector<uint8_t> addr) :
    NvmeInterfaces(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "/xyz/openbmc_project/drive/1",
        NvmeInterfaces::action::defer_emit),std::enable_shared_from_this<NVMeDevice>(),
    objServer(objectServer), scanTimer(io)
{
    
    
    /*
    driveInterface = objectServer.add_interface(
        "/xyz/openbmc_project/drive/1",
        DriveInterface::interface);

    
    std::filesystem::path p("/xyz/openbmc_project/drive/1");
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);
    driveInterface->register_property("test2", 1);
    if (!driveInterface->initialize())
    {
        std::cerr << "error initializing interface\n";
    }
    */

    nvmeIntf = NVMeIntf::create<NVMeMi>(io, conn, addr, eid);
    intf = std::get<std::shared_ptr<NVMeMiIntf>>(nvmeIntf.getInferface());

    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(true);
}
    
std::string NVMeDevice::stripString(char *src, size_t len)
{
    std::string s;

    s.assign(src, src+len);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

std::string NVMeDevice::getManufacture(uint16_t vid)
{

    if (vid == 0x144d)
    {
        return std::string("Samsung");
    }
    else if (vid == 0x1344)
    {
        return std::string("Mircon");
    }
    else if (vid == 0x1e0f)
    {
        return std::string("Kioxia");
    }

    return "Unkown";
}

void NVMeDevice::initialize()
{
    intf->miScanCtrl([self{shared_from_this()}](
                         const std::error_code& ec,
                         const std::vector<nvme_mi_ctrl_t>& ctrlList) mutable {
        if (ec || ctrlList.size() == 0)
        {
            std::cerr << "fail to scan controllers for the nvme subsystem"
                      << (ec ? ": " + ec.message() : "") << std::endl;
            return;
        }

        auto ctrl = ctrlList.back();
        self->getIntf()->adminIdentify(
            ctrl, nvme_identify_cns::NVME_IDENTIFY_CNS_CTRL, NVME_NSID_NONE, 0,
            [self{self->shared_from_this()}](const std::error_code& ec,
                                             std::span<uint8_t> data) {
                if (ec)
                {
                    std::cerr << "fail to do Identify command" << std::endl;
                    return;
                }
                char resp[sizeof(nvme_id_ctrl)];
                int i = 0;
                for (auto d: data)
                    resp[i++] = d;
                nvme_id_ctrl* id = reinterpret_cast<nvme_id_ctrl *> (resp);

                self->sdbusplus::xyz::openbmc_project::Inventory::
                        Decorator::server::Asset::manufacturer(self->getManufacture(id->vid));
                self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                    server::Asset::serialNumber(
                        self->stripString(id->sn, sizeof(id->sn)));

                self->sdbusplus::xyz::openbmc_project::Inventory::Decorator::
                    server::Asset::model(
                        self->stripString(id->mn, sizeof(id->mn)));
            });
    });
}

void NVMeDevice::markFunctional(bool functional)
{
    driveFunctional = functional;
    if (!functional)
    {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::Fault);
    }
}

void NVMeDevice::pollDevices()
{

    scanTimer.expires_from_now(boost::posix_time::seconds(5));
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

        self->getIntf()->miSubsystemHealthStatusPoll(
            [self](__attribute__((unused))
                                       const std::error_code& err,
                                       nvme_mi_nvm_ss_health_status* ss) {

                self->sdbusplus::xyz::openbmc_project::Nvme::server::Status::driveLifeUsed(std::to_string(ss->pdlu));


                //drive failure
                if (ss->nss & 0x20)
                {

                }
                printf("NVMe MI subsys health:\n");
                printf(" smart warnings:    0x%x\n", ss->sw);
                printf(" composite temp:    %d\n", ss->ctemp);
                printf(" drive life used:   %d%%\n", ss->pdlu);
                printf(" controller status: 0x%04x\n", ss->ccs);
            });
        /*
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::Fault);
                    */

        self->pollDevices();
    });
}

NVMeDevice::~NVMeDevice()
{
    std::cout <<"~NVMEDevice"<<std::endl;
    //objServer.remove_interface(driveInterface);
}
