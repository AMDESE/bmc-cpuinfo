#include "cpu_info.hpp"

/* Venice Platform IDs */
constexpr int CONGO = 128;     // 0x80
constexpr int CONGO_1 = 129;   // 0x81
constexpr int CONGO_2 = 134;   // 0x86
constexpr int MOROCCO = 130;   // 0x82
constexpr int MOROCCO_1 = 131; // 0x83
constexpr int MOROCCO_2 = 135; // 0x87
constexpr int KENYA = 132;     // 0x84
constexpr int NIGERIA = 133;   // 0x85

constexpr uint8_t SOCKET_1 = 1;
constexpr uint8_t SOCKET_2 = 2;
constexpr uint8_t INDEX_3 = 3;

uint8_t getSocketInfo()
{
    char data[INDEX_3];
    std::stringstream ss;
    uint8_t cpuCount = 0;
    uint32_t boardId;

    oob_status_t ret = OOB_MAILBOX_CMD_UNKNOWN;

    std::unique_ptr<FILE, void (*)(FILE*)> pipe(
        popen("/sbin/fw_printenv -n board_id", "r"), [](FILE* f) {
            if (f)
                pclose(f); // Custom deleter to call pclose
        });

    if (!pipe)
    {
        throw std::runtime_error("Failed to read the boardID");
    }

    if (fgets(data, sizeof(data), pipe.get()) != nullptr)
    {
        {
            ss << std::hex << (std::string)data;
            ss >> boardId;

            if ((boardId == MOROCCO) || (boardId == MOROCCO_1) ||
                (boardId == MOROCCO_2) || (boardId == NIGERIA))
            {
                cpuCount = SOCKET_2;
            }
            else if ((boardId == CONGO) || (boardId == CONGO_1) ||
                     (boardId == CONGO_2) || (boardId == KENYA))
            {
                cpuCount = SOCKET_1;
            }
            else
            {
                throw std::runtime_error("Failed to find the correct board ID");
            }
        }
    }
    return cpuCount;
}

int main()
{
    CpuInfoDataHolder* cpuinfoDataHolderObj =
        cpuinfoDataHolderObj->getInstance();

    int ret = 0;
    std::string intfName;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Start cpu info service...");

    uint8_t cpuCount = getSocketInfo();

    sd_event* event = nullptr;
    ret = sd_event_default(&event);
    if (ret < 0)
    {
        sd_journal_print(LOG_ERR,
                         "Error creating a default sd_event handler \n");
        return ret;
    }
    EventPtr eventP{event};
    event = nullptr;

    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    bus.request_name(DBUS_SERVICE_NAME);

    sdbusplus::server::manager_t inventory{bus,
                                           "/xyz/openbmc_project/inventory"};
    sdbusplus::server::manager_t manager0{bus, DBUS_P0_OBJECT_NAME};
    CpuInfo cpuInfo{bus, DBUS_P0_OBJECT_NAME, eventP, 0};

    if (cpuCount == SOCKET_2)
    {
        sdbusplus::server::manager_t managaer1{bus, DBUS_P1_OBJECT_NAME};
        CpuInfo cpuInfo{bus, DBUS_P1_OBJECT_NAME, eventP, 1};
    }

    try
    {
        bus.attach_event(eventP.get(), SD_EVENT_PRIORITY_NORMAL);
        ret = sd_event_loop(eventP.get());
        if (ret < 0)
        {
            sd_journal_print(
                LOG_ERR, "Error occurred during the sd_event_loop %d \n", ret);
        }
    }
    catch (std::exception& e)
    {
        // phosphor::logging::log<phosphor::logging::level::ERR>(e.what());
        sd_journal_print(LOG_ERR,
                         "Exception occurred during the sd_event_loop %s \n",
                         e.what());
        return -1;
    }

    return 0;
}
