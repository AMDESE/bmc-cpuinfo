#include <fcntl.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <xyz/openbmc_project/Collection/DeleteAll/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Cpu/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#define CPUID_Fn8000002 (0x80000002)
#define CPUID_Fn8000003 (0x80000003)
#define CPUID_Fn8000004 (0x80000004)

#define MASK_BYTE_2 (0xFF00)
#define MASK_BYTE_3 (0xFF0000)
#define MASK_BYTE_4 (0xFF000000)
#define MASK_TWO_BYTES (0xFF)

#define SHIFT_24 (24)
#define SHIFT_16 (16)
#define SHIFT_8 (8)
#define OPN_LENGTH (47)
#define PARTNUMBER "PartNumber"

const static constexpr char* CpuInfoName = "CpuInfo";
const static constexpr char* CpuInfoEnableName = "CpuInfoEnable";

class CpuInfoDataHolder
{
    static CpuInfoDataHolder* instance;
    CpuInfoDataHolder() {}

  public:
    static CpuInfoDataHolder* getInstance()
    {
        if (!instance)
            instance = new CpuInfoDataHolder;
        return instance;
    }

    const static constexpr char* PropertiesIntf =
        "org.freedesktop.DBus.Properties";
    const static constexpr char* HostStatePathPrefix =
        "/xyz/openbmc_project/state/host0";
};

struct EventDeleter
{
    void operator()(sd_event* event) const
    {
        event = sd_event_unref(event);
    }
};

using EventPtr = std::unique_ptr<sd_event, EventDeleter>;
using ProcessorPtr = sdbusplus::server::xyz::openbmc_project::inventory::item::Cpu;
using Asset =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::Asset;

namespace StateServer = sdbusplus::xyz::openbmc_project::State::server;

static const std::map<int, std::string> months_map = {
    {1, "M"}, {2, "N"}, {3, "O"}, {4, "P"},  {5, "Q"},  {6, "R"},
    {7, "S"}, {8, "T"}, {9, "U"}, {10, "V"}, {11, "W"}, {12, "X"}};

struct CpuInfo :
    sdbusplus::server::object_t<
        ProcessorPtr, Asset,
        sdbusplus::xyz::openbmc_project::Inventory::server::Item>
{
    CpuInfoDataHolder* cpuinfoDataHolderObj =
        cpuinfoDataHolderObj->getInstance();

    CpuInfo(sdbusplus::bus::bus& bus, const std::string& path, EventPtr&,
            uint8_t soc_num) :
        sdbusplus::server::object_t<
            ProcessorPtr, Asset,
            sdbusplus::xyz::openbmc_project::Inventory::server::Item>(
            bus, path.c_str()),
        bus(bus), soc_num(soc_num),
        propertiesChangedSignalCurrentHostState(
            bus,
            sdbusplus::bus::match::rules::type::signal() +
                sdbusplus::bus::match::rules::member("PropertiesChanged") +
                sdbusplus::bus::match::rules::path(
                    cpuinfoDataHolderObj->HostStatePathPrefix) +
                sdbusplus::bus::match::rules::interface(
                    cpuinfoDataHolderObj->PropertiesIntf),
            [this, soc_num](sdbusplus::message::message& msg) {
                std::string objectName;
                std::map<std::string, std::variant<std::string>> msgData;
                msg.read(objectName, msgData);
                // Check if CPU was powered-on
                auto valPropMap = msgData.find("CurrentHostState");
                {
                    if (valPropMap != msgData.end())
                    {
                        StateServer::Host::HostState currentHostState =
                            StateServer::Host::convertHostStateFromString(
                                std::get<std::string>(valPropMap->second));

                        if (currentHostState !=
                            StateServer::Host::HostState::Off)
                        {
                            sd_journal_print(
                                LOG_INFO,
                                "cpu service started after bmc or host reboot... \n");
                            collect_cpu_information(soc_num);
                        }
                    }
                }
            })
    {
        sd_journal_print(LOG_DEBUG, "cpu service start... \n");
        collect_cpu_information(soc_num);
    }
    ~CpuInfo() {}

  private:
    sdbusplus::bus::bus& bus;
    uint8_t soc_num;
    sdbusplus::bus::match_t propertiesChangedSignalCurrentHostState;
    std::string get_interface(uint8_t enum_val);

    // oob-lib functions
    void collect_cpu_information(uint8_t);
    int getGPIOValue(const std::string& name);
    void set_general_info();
    bool connect_apml_get_family_model_step(uint8_t soc_num);
    void get_threads_per_core_and_soc(uint8_t soc_num);
    void get_cpu_base_freq(uint8_t soc_num);
    void get_ppin_fuse(uint8_t soc_num);
    void get_microcode_rev(uint8_t soc_num);

    // decode ppin function
    void decode_PPIN(uint64_t data);
    void decode_lotstring(char* ppinstr, std::string&);
    void decode_datemonth_unitlot(char* ppinstr, std::string& datemonthlotstr);

    // OPN functions
    void get_opn(uint8_t soc_num);
    bool read_register(uint8_t soc_num, uint32_t thread_ind, uint32_t cpuid_fn,
                       uint32_t cpuid_extd_fn, uint32_t* eax_value,
                       uint32_t* ebx_value, uint32_t* ecx_value,
                       uint32_t* edx_value);
    u_int8_t get_reg_offset_conv(uint32_t reg, uint32_t offset, uint32_t flag);
};
