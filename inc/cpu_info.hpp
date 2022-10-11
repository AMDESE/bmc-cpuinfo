#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <phosphor-logging/elog-errors.hpp>
#include <xyz/openbmc_project/Collection/DeleteAll/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Cpu/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>

const static constexpr char *CpuInfoName =
    "CpuInfo";
const static constexpr char *CpuInfoEnableName =
    "CpuInfoEnable";

class CpuInfoDataHolder
{
    static CpuInfoDataHolder *instance;
    CpuInfoDataHolder()
    {
    }

  public:
    static CpuInfoDataHolder *getInstance()
    {
        if (!instance)
            instance = new CpuInfoDataHolder;
        return instance;
    }

    const static constexpr char *PropertiesIntf =
        "org.freedesktop.DBus.Properties";
    const static constexpr char *HostStatePathPrefix =
        "/xyz/openbmc_project/state/host0";
};

struct EventDeleter
{
    void operator()(sd_event *event) const
    {
        event = sd_event_unref(event);
    }
};

using EventPtr = std::unique_ptr<sd_event, EventDeleter>;
namespace StateServer = sdbusplus::xyz::openbmc_project::State::server;

enum dbus_interface { CPU_INTERFACE, ASSET_INTERFACE } ;
static const char *enum_str[] = { "xyz.openbmc_project.Inventory.Item.Cpu", "xyz.openbmc_project.Inventory.Decorator.Asset" };

struct CpuInfo
{
    CpuInfoDataHolder *cpuinfoDataHolderObj =
        cpuinfoDataHolderObj->getInstance();

    CpuInfo(sdbusplus::bus::bus &bus, const char *path, EventPtr &event) :
        bus(bus),
        propertiesChangedCpuInfoValue(
            bus,
            sdbusplus::bus::match::rules::type::signal() +
                sdbusplus::bus::match::rules::member("PropertiesChanged") +
                sdbusplus::bus::match::rules::path(
                "/xyz/openbmc_project/inventory/system/processor/P0") +
                sdbusplus::bus::match::rules::argN(0, "xyz.openbmc_project.Inventory.Item.Cpu") +
                sdbusplus::bus::match::rules::interface(
                    cpuinfoDataHolderObj->PropertiesIntf),
            [this](sdbusplus::message::message &msg) {
                std::string objectName;
                std::map<std::string, std::variant<uint32_t,bool>> msgData;
                msg.read(objectName, msgData);
                //TO DO - in case if we need to check any DBus Property event
            }),
        propertiesChangedSignalCurrentHostState(
            bus,
            sdbusplus::bus::match::rules::type::signal() +
                sdbusplus::bus::match::rules::member("PropertiesChanged") +
                sdbusplus::bus::match::rules::path(
                    cpuinfoDataHolderObj->HostStatePathPrefix)  +
                sdbusplus::bus::match::rules::interface(
                    cpuinfoDataHolderObj->PropertiesIntf),
            [this](sdbusplus::message::message &msg) {
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
                     //TO DO - in case if we need to check any DBus Property event
                    }
                }
        })
    {
       phosphor::logging::log<phosphor::logging::level::INFO>("cpu service stated...");
       getPlatformID();
       collect_cpu_information();
    }
    ~CpuInfo()
    {
    }

  private:

    sdbusplus::bus::bus &bus;
    sdbusplus::bus::match_t propertiesChangedCpuInfoValue;
    sdbusplus::bus::match_t propertiesChangedSignalCurrentHostState;
    std::string get_interface(uint8_t enum_val);
    int num_of_proc = 1;
    unsigned int board_id = 0;

    // oob-lib functions
    bool getPlatformID();
    void collect_cpu_information();
    void set_general_info(uint8_t soc_num);
    bool connect_apml_get_family_model_step(uint8_t soc_num);
    void get_threads_per_core_and_soc(uint8_t soc_num);
    void get_cpu_base_freq(uint8_t soc_num);
    void get_ppin_fuse(uint8_t soc_num);
    void get_microcode_rev(uint8_t soc_num);

    //DBUS functions
    void set_cpu_string_value(uint8_t soc_num, char *value, std::string property_name, uint8_t enum_val);
    void set_cpu_int_value(uint8_t soc_num, uint32_t value, std::string property_name, uint8_t enum_val);
    void set_cpu_int16_value(uint8_t soc_num, uint16_t value, std::string property_name, uint8_t enum_val);

};
