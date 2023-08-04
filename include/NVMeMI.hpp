#include <sdbusplus/asio/connection.hpp>

#include <span>

extern "C"
{
#include <libnvme-mi.h>
}

class NVMeMI
{
 public:
    NVMeMI(std::vector<uint8_t>, uint8_t eid);
    ~NVMeMI();

    void subsystemHealthStatusPoll(
        std::function<void(nvme_mi_nvm_ss_health_status*)>&& cb);

    void adminIdentify(
        uint16_t cntid,
        std::function<void(nvme_id_ctrl*)>&& cb);

  private:
    nvme_root_t root;
    nvme_mi_ep_t ep;
};