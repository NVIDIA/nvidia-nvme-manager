#include <NVMeMI.hpp>

#include <iostream>
#include <syslog.h>
#include <filesystem>

NVMeMI::NVMeMI(std::vector<uint8_t> addr, uint8_t eid)
{

    root = nvme_mi_create_root(stderr, LOG_DEBUG);
    if (!root) {
        std::cerr <<"failed to call nvme_mi_create_root" <<std::endl;
        return;
    }

    ep = nvme_mi_open_mctp(root, (char *) &addr.data()[1], eid);
    if (!ep) {
        std::cerr <<"fail to open MCTP endpoint "<<":" << eid <<std::endl;
        nvme_mi_free_root(root);
        return;
    }
}

void NVMeMI::subsystemHealthStatusPoll(
    std::function<void(nvme_mi_nvm_ss_health_status*)>&&
        cb)
{

    struct nvme_mi_nvm_ss_health_status ss_health;
    struct nvme_mi_read_nvm_ss_info ss_info;
    int rc;

    rc = nvme_mi_mi_read_mi_data_subsys(ep, &ss_info);
    if (rc)
    {
        std::cerr << "can't perform Read MI Data operation" << std::endl;
        return;
    }

    printf("NVMe MI subsys info:\n");
    printf(" num ports: %d\n", ss_info.nump + 1);
    printf(" major ver: %d\n", ss_info.mjr);
    printf(" minor ver: %d\n", ss_info.mnr);

    rc = nvme_mi_mi_subsystem_health_status_poll(ep, true, &ss_health);
    if (rc)
    {
        std::cerr << "can't perform Health Status Poll operation"
                    << std::endl;
        return;
    }

    cb(&ss_health);
}

void NVMeMI::adminIdentify(
   uint16_t cntid,
   std::function<void(nvme_id_ctrl *)>&& cb)
{
    struct nvme_identify_args id_args;
    struct nvme_mi_ctrl *ctrl;
	struct nvme_id_ctrl id;
    int rc;


    memset (&id_args, 0, sizeof(nvme_identify_args));
	ctrl = nvme_mi_init_ctrl(ep, cntid);
	if (!ctrl) {
		std::cerr <<"can't create controller\n";
		return ;
	}

	id_args.data = &id;
	id_args.args_size = sizeof(id_args);
	id_args.cns = NVME_IDENTIFY_CNS_CTRL;
	id_args.nsid = NVME_NSID_NONE;
	id_args.cntid = cntid;
	id_args.csi = NVME_CSI_NVM;

	/* for this example code, we can either do a full or partial identify;
	 * since we're only printing the fields before the 'rab' member,
	 * these will be equivalent, aside from the size of the MI
	 * response.
	 */
	rc = nvme_mi_admin_identify(ctrl, &id_args);

	if (rc) {
		std::cerr <<"can't perform Admin Identify command\n";
		return ;
	}

    cb(&id);
}

NVMeMI:: ~NVMeMI()
{

    nvme_mi_close(ep);
    nvme_mi_free_root(root);

}