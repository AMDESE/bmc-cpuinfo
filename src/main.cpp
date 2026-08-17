#include <pthread.h>

#include <csignal>
#include <iostream>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

#include "cpu_info.hpp"

constexpr uint8_t SOCKET_1 = 1;
constexpr uint8_t SOCKET_2 = 2;

uint8_t getSocketInfo()
{
    const std::string filePath = "/var/lib/platform-config/platform.json";

    try
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            sd_journal_print(LOG_ERR,
                             "Failed to open %s, default to SOCKET_1",
                             filePath.c_str());
            return SOCKET_1;
        }

        nlohmann::json jsonData;
        file >> jsonData;

        if (!jsonData.contains("CpuCount") ||
            !jsonData["CpuCount"].is_number())
        {
            sd_journal_print(LOG_ERR,
                             "CpuCount missing/invalid in JSON, default to SOCKET_1");
            return SOCKET_1;
        }

        int cpuCount = jsonData["CpuCount"].get<int>();

        if (cpuCount == SOCKET_2)
        {
            return SOCKET_2;
        }

        // Explicit handling for all other cases
        return SOCKET_1;
    }
    catch (const std::exception& e)
    {
        sd_journal_print(LOG_ERR,
                         "Exception parsing %s: %s, default to SOCKET_1",
                         filePath.c_str(), e.what());
        return SOCKET_1;
    }
}

int main()
{
    CpuInfoDataHolder* cpuinfoDataHolderObj =
        cpuinfoDataHolderObj->getInstance();

    int ret = 0;
    std::string intfName;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Start cpu info service...");

    // Block SIGTERM/SIGINT in this (and inherited worker) thread(s) so they are
    // delivered synchronously to the sd_event loop instead of terminating the
    // process via the default disposition. This must happen before any worker
    // threads are spawned so they inherit the mask.
    sigset_t signalMask;
    sigemptyset(&signalMask);
    sigaddset(&signalMask, SIGTERM);
    sigaddset(&signalMask, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &signalMask, nullptr) != 0)
    {
        sd_journal_print(LOG_ERR, "Failed to block SIGTERM/SIGINT\n");
    }

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

    // Dispatcher used by worker threads to marshal all D-Bus setter calls back
    // onto this (the event-loop) thread; a bus connection must not be
    // accessed concurrently from multiple threads.
    EventLoopDispatcher dispatcher;
    ret = dispatcher.init(eventP.get());
    if (ret < 0)
    {
        sd_journal_print(LOG_ERR, "Failed to init event loop dispatcher %d\n",
                         ret);
        return ret;
    }

    // Exit the event loop cleanly (return 0) on SIGTERM/SIGINT. A NULL handler
    // makes sd-event call sd_event_exit() with code 0, so a commanded stop
    // (e.g. s5-state-mgr on host power-off, or BMC reboot) is a clean shutdown
    // rather than a signal-kill that systemd would report as a failure.
    (void)sd_event_add_signal(eventP.get(), nullptr, SIGTERM, nullptr, nullptr);
    (void)sd_event_add_signal(eventP.get(), nullptr, SIGINT, nullptr, nullptr);

    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    bus.request_name(DBUS_SERVICE_NAME);

    sdbusplus::server::manager_t inventory{bus,
                                           "/xyz/openbmc_project/inventory"};
    sdbusplus::server::manager_t manager0{bus, DBUS_P0_OBJECT_NAME};
    CpuInfo cpuInfo0{bus, DBUS_P0_OBJECT_NAME, eventP, 0, &dispatcher};

    std::optional<sdbusplus::server::manager_t> manager1;
    std::optional<CpuInfo> cpuInfo1;

    if (cpuCount == SOCKET_2)
    {
        manager1.emplace(bus, DBUS_P1_OBJECT_NAME);
        cpuInfo1.emplace(bus, DBUS_P1_OBJECT_NAME, eventP, 1, &dispatcher);
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
