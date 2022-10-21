#include "cpu_info.hpp"

int main()
{
    CpuInfoDataHolder* cpuinfoDataHolderObj =
        cpuinfoDataHolderObj->getInstance();

    int ret = 0;
    std::string intfName;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Start cpu info service...");

    sd_event* event = nullptr;
    ret = sd_event_default(&event);
    if (ret < 0)
    {
	sd_journal_print(LOG_ERR, "Error creating a default sd_event handler \n");
        return ret;
    }
    EventPtr eventP{event};
    event = nullptr;

    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    sdbusplus::server::manager_t m{bus, DBUS_OBJECT_NAME};

    intfName = DBUS_INTF_NAME;
    bus.request_name(intfName.c_str());

    CpuInfo cpuInfo{bus, DBUS_OBJECT_NAME, eventP};

    try
    {
        bus.attach_event(eventP.get(), SD_EVENT_PRIORITY_NORMAL);
        ret = sd_event_loop(eventP.get());
        if (ret < 0)
        {
	  sd_journal_print(LOG_ERR, "Error occurred during the sd_event_loop %d \n", ret);
        }
    }
    catch (std::exception& e)
    {
        //phosphor::logging::log<phosphor::logging::level::ERR>(e.what());
	sd_journal_print(LOG_ERR, "Exception occurred during the sd_event_loop %s \n", e.what());
        return -1;
    }
   
    return 0;
}
