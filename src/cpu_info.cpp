#include "cpu_info.hpp"

#include <linux/ioctl.h>
#include <linux/types.h>

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/property.hpp>

#include <filesystem>

#include "iomanip"

extern "C"
{
#include "apml.h"
#include "apml64Config.h"
#include "esmi_cpuid_msr.h"
#include "esmi_mailbox.h"
#include "esmi_rmi.h"
#include "esmi_tsi.h"
#include "i2c/smbus.h"
#include "linux/i2c-dev.h"

#include <unistd.h>
}

#define COMMAND_LEN (3)
#define MAX_RETRY 30

#define CMD_BUFF_LEN 256
#define FNAME_LEN 128

// AMPL command
#define EAX_VAL 1
#define EAX_DATA_LEN_1 4
#define EAX_DATA_LEN_2 8
#define EAX_DATA_LEN_3 16
#define EAX_DATA_LEN_4 20
#define EAX_MASK_MAGIC_1 0xf
#define EAX_MASK_MAGIC_2 0xff
#define EAX_MASK_MAGIC_3 0x10
#define APML_SLEEP 10

// PPIN logic
#define LOWER_PINBITS 8
#define MASKNO_14BITS 0x00003FFF
#define DEV_LENGTH 4
#define MASKNO_MON_YEAR 0x001FC000
#define DATECODE_SHIFT 14
#define MONTH_VAL 10
#define LOTNUM_SHIFT 21
#define LOTNUM_LENGTH 7
#define MAX_ALPHA_NUM 37
#define MAX_ALPHA_LENGTH 27
#define ALPHA_CHAR_CONVER_VAL 64
#define DIGIT_CONVER_VAL 21
#define HEX "0x"
#define LENGTH_DIV 2

const std::string P0_Present = "P0_PRESENT_L";
const std::string P1_Present = "P1_PRESENT_L";

CpuInfoDataHolder* CpuInfoDataHolder::instance = 0;

uint8_t p0_info = 0;
uint8_t p1_info = 1;
uint32_t eax;
uint32_t ebx;
uint32_t ecx;
uint32_t edx;

// Init CPU Information using OOB library
void CpuInfo::collect_cpu_information(uint8_t soc_num)
{
    if (connect_apml_get_family_model_step(soc_num))
    {
        set_general_info();
        get_cpu_base_freq(soc_num);
        get_ppin_fuse(soc_num);
        get_threads_per_core_and_soc(soc_num);
        get_microcode_rev(soc_num);
        get_opn(soc_num);
    }
}

int CpuInfo::getGPIOValue(const std::string& name)
{
    int value;
    gpiod::line gpioLine;

    // Find the GPIO line
    gpioLine = gpiod::find_line(name);
    if (!gpioLine)
    {
        sd_journal_print(LOG_ERR, "Can't find line: %s \n", name.c_str());
        return -1;
    }
    try
    {
        gpioLine.request(
            {__FUNCTION__, gpiod::line_request::DIRECTION_INPUT, 0});
    }
    catch (std::system_error& exc)
    {
        sd_journal_print(LOG_ERR, "Error setting gpio as Input: %s \n",
                         name.c_str());
        return -1;
    }

    try
    {
        value = gpioLine.get_value();
    }
    catch (std::system_error& exc)
    {
        sd_journal_print(LOG_ERR, "Error getting gpio value for: %s \n",
                         name.c_str());
        return -1;
    }

    return value;
}

// Call Apml library to get the CPU Info
bool CpuInfo::connect_apml_get_family_model_step(uint8_t soc_num)
{
    int retry = 0;
    oob_status_t ret;
    uint16_t family_id;
    uint16_t model_id;
    uint16_t step_id;
    uint16_t ext_family;
    uint16_t ext_model;
    uint16_t cpuPresence;
    int core_id = 0;
    ebx = 0;
    edx = 0;
    eax = EAX_VAL;
    ecx = 0;

    boost::asio::io_context io;
    boost::asio::steady_timer timer(io);

    try
    {
        std::function<void(const boost::system::error_code&)> retry_handler;
        retry_handler = [&](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }

            ret = esmi_oob_cpuid(soc_num, core_id, &eax, &ebx, &ecx, &edx);
            if (ret != 0 && retry < MAX_RETRY)
            {
                retry++;
                sd_journal_print(
                    LOG_ERR,
                    "Retry count %d: Failed to read the CPU info from APML\n",
                    retry);

                timer.expires_after(std::chrono::seconds(APML_SLEEP));
                timer.async_wait(retry_handler); // recurse asynchronously
            }
            else
            {
                io.stop();
            }
        };

        // Start first attempt immediately
        timer.expires_after(std::chrono::seconds(0));
        timer.async_wait(retry_handler);

        io.run();

        std::string processor_presence =
            (soc_num == 0) ? P0_Present : P1_Present;
        cpuPresence = getGPIOValue(processor_presence);
        if (cpuPresence == 1)
        {
            // set false -Absent if GPIO value is high
            present(false);
            sd_journal_print(LOG_INFO, "Warning : %d CPU is absent \n",
                             soc_num);
            return false;
        }
        else
        {
            present(true);
        }

        if (ret != 0)
        {
            sd_journal_print(LOG_ERR,
                             "Error : Unable to get the CPU info from APML \n");
        }
        else
        {
            char cpuid[CMD_BUFF_LEN] = {0};

            ext_family = ((eax >> EAX_DATA_LEN_4) & EAX_MASK_MAGIC_2);
            effectiveFamily(ext_family);

            cpuid[CMD_BUFF_LEN] = {0};
            family_id = ((eax >> EAX_DATA_LEN_2) & EAX_MASK_MAGIC_1) +
                        ext_family;
            sprintf(cpuid, "%x (%d)", family_id, family_id);
            std::string family_str(cpuid);
            family(family_str);

            ext_model = ((eax >> EAX_DATA_LEN_3) & EAX_MASK_MAGIC_1);
            effectiveModel(ext_model);

            cpuid[CMD_BUFF_LEN] = {0};
            model_id = ext_model * EAX_MASK_MAGIC_3 +
                       ((eax >> EAX_DATA_LEN_1) & EAX_MASK_MAGIC_1);
            sprintf(cpuid, "%x (%d)", model_id, model_id);
            std::string model_str(cpuid);
            model(model_str);

            step_id = eax & EAX_MASK_MAGIC_1;
            step(step_id);

            cpuid[CMD_BUFF_LEN] = {0};
            sprintf(cpuid, "%d", soc_num);
            std::string socket_str(cpuid);
            socket(socket_str);

            return true;
        }
    }
    catch (std::exception& e)
    {
        sd_journal_print(LOG_ERR,
                         "Error getting CPU Model, Family and Step value \n");
        return false;
    }

    return false;
}

// Get the OPN
void CpuInfo::get_opn(uint8_t soc_num)
{
    int32_t cpuid_fn = 1;
    uint32_t cpuid_extd_fn = 0;
    uint32_t thread_ind = 0;
    uint32_t eax_value, ebx_value, ecx_value, edx_value;
    u_int8_t write_opn_data[OPN_LENGTH] = {0};
    char OpnChar[OPN_LENGTH] = {0};

    cpuid_fn = CPUID_Fn8000002;
    if (read_register(soc_num, thread_ind, cpuid_fn, cpuid_extd_fn, &eax_value,
                      &ebx_value, &ecx_value, &edx_value))
    {
        // eax
        write_opn_data[0] = (eax_value & MASK_TWO_BYTES);
        write_opn_data[1] =
            get_reg_offset_conv(eax_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[2] =
            get_reg_offset_conv(eax_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[3] =
            get_reg_offset_conv(eax_value, SHIFT_24, MASK_BYTE_4);
        // ebx
        write_opn_data[4] = (ebx_value & MASK_TWO_BYTES);
        write_opn_data[5] =
            get_reg_offset_conv(ebx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[6] =
            get_reg_offset_conv(ebx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[7] =
            get_reg_offset_conv(ebx_value, SHIFT_24, MASK_BYTE_4);
        // ecx
        write_opn_data[8] = (ecx_value & MASK_TWO_BYTES);
        write_opn_data[9] =
            get_reg_offset_conv(ecx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[10] =
            get_reg_offset_conv(ecx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[11] =
            get_reg_offset_conv(ecx_value, SHIFT_24, MASK_BYTE_4);
        // edx
        write_opn_data[12] = (edx_value & MASK_TWO_BYTES);
        write_opn_data[13] =
            get_reg_offset_conv(edx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[14] =
            get_reg_offset_conv(edx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[15] =
            get_reg_offset_conv(edx_value, SHIFT_24, MASK_BYTE_4);
    }
    else
    {
        sd_journal_print(LOG_ERR,
                         "Failed to read 0x80000002 register value \n");
        return;
    }

    cpuid_fn = CPUID_Fn8000003;
    if (read_register(soc_num, thread_ind, cpuid_fn, cpuid_extd_fn, &eax_value,
                      &ebx_value, &ecx_value, &edx_value))
    {
        // eax
        write_opn_data[16] = (eax_value & MASK_TWO_BYTES);
        write_opn_data[17] =
            get_reg_offset_conv(eax_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[18] =
            get_reg_offset_conv(eax_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[19] =
            get_reg_offset_conv(eax_value, SHIFT_24, MASK_BYTE_4);
        // ebx
        write_opn_data[20] = (ebx_value & MASK_TWO_BYTES);
        write_opn_data[21] =
            get_reg_offset_conv(ebx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[22] =
            get_reg_offset_conv(ebx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[23] =
            get_reg_offset_conv(ebx_value, SHIFT_24, MASK_BYTE_4);
        // ecx
        write_opn_data[24] = (ecx_value & MASK_TWO_BYTES);
        write_opn_data[25] =
            get_reg_offset_conv(ecx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[26] =
            get_reg_offset_conv(ecx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[27] =
            get_reg_offset_conv(ecx_value, SHIFT_24, MASK_BYTE_4);
        // edx
        write_opn_data[28] = (edx_value & MASK_TWO_BYTES);
        write_opn_data[29] =
            get_reg_offset_conv(edx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[30] =
            get_reg_offset_conv(edx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[31] =
            get_reg_offset_conv(edx_value, SHIFT_24, MASK_BYTE_4);
    }
    else
    {
        sd_journal_print(LOG_ERR,
                         "Failed to read 0x80000003 register value \n");
        return;
    }

    cpuid_fn = CPUID_Fn8000004;
    if (read_register(soc_num, thread_ind, cpuid_fn, cpuid_extd_fn, &eax_value,
                      &ebx_value, &ecx_value, &edx_value))
    {
        write_opn_data[32] = (eax_value & MASK_TWO_BYTES);
        write_opn_data[33] =
            get_reg_offset_conv(eax_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[34] =
            get_reg_offset_conv(eax_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[35] =
            get_reg_offset_conv(eax_value, SHIFT_24, MASK_BYTE_4);
        // ebx
        write_opn_data[36] = (ebx_value & MASK_TWO_BYTES);
        write_opn_data[37] =
            get_reg_offset_conv(ebx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[38] =
            get_reg_offset_conv(ebx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[39] =
            get_reg_offset_conv(ebx_value, SHIFT_24, MASK_BYTE_4);
        // ecx
        write_opn_data[40] = (ecx_value & MASK_TWO_BYTES);
        write_opn_data[41] =
            get_reg_offset_conv(ecx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[42] =
            get_reg_offset_conv(ecx_value, SHIFT_16, MASK_BYTE_3);
        write_opn_data[43] =
            get_reg_offset_conv(ecx_value, SHIFT_24, MASK_BYTE_4);
        // edx
        write_opn_data[44] = (edx_value & MASK_TWO_BYTES);
        write_opn_data[45] =
            get_reg_offset_conv(edx_value, SHIFT_8, MASK_BYTE_2);
        write_opn_data[46] =
            get_reg_offset_conv(edx_value, SHIFT_16, MASK_BYTE_3);
    }
    else
    {
        sd_journal_print(LOG_ERR,
                         "Failed to read 0x80000004 register value \n");
        return;
    }

    // convert hex to ascii
    for (int i = 0; i < OPN_LENGTH; i++)
    {
        OpnChar[i] = (char)write_opn_data[i];
    }

    // convert char to opn string
    std::string opn_str(OpnChar);
    sd_journal_print(LOG_INFO, "OPN string # %s \n", opn_str.c_str());

    // set the value in DBUS
    partNumber(opn_str);
}

// Read register thru apml lib
bool CpuInfo::read_register(uint8_t soc_num, uint32_t thread_ind,
                            uint32_t cpuid_fn, uint32_t cpuid_extd_fn,
                            uint32_t* eax_value, uint32_t* ebx_value,
                            uint32_t* ecx_value, uint32_t* edx_value)
{
    bool ret = false;
    if (OOB_SUCCESS == esmi_oob_cpuid_eax(soc_num, thread_ind, cpuid_fn,
                                          cpuid_extd_fn, eax_value))
    {
        if (OOB_SUCCESS == esmi_oob_cpuid_ebx(soc_num, thread_ind, cpuid_fn,
                                              cpuid_extd_fn, ebx_value))
        {
            if (OOB_SUCCESS == esmi_oob_cpuid_ecx(soc_num, thread_ind, cpuid_fn,
                                                  cpuid_extd_fn, ecx_value))
            {
                if (OOB_SUCCESS ==
                    esmi_oob_cpuid_edx(soc_num, thread_ind, cpuid_fn,
                                       cpuid_extd_fn, edx_value))
                {
                    ret = true;
                }
                else
                {
                    sd_journal_print(LOG_ERR, "Error reading register eax \n");
                }
            }
            else
            {
                sd_journal_print(LOG_ERR, "Error reading register ecx \n");
            }
        }
        else
        {
            sd_journal_print(LOG_ERR, "Error reading register ebx \n");
        }
    }
    else
    {
        sd_journal_print(LOG_ERR, "Error reading register eax \n");
    }

    return ret;
}
// get byte value
u_int8_t CpuInfo::get_reg_offset_conv(uint32_t reg, uint32_t offset,
                                      uint32_t flag)
{
    return ((reg & flag) >> offset);
}

void CpuInfo::set_general_info()
{
    manufacturer("AMD");
    vendorId("AuthenticAMD");
}

// Get processor threads per Core and Socket
void CpuInfo::get_threads_per_core_and_soc(uint8_t soc_num)
{
    uint32_t threads_per_core, threads_per_soc;
    bool isthreadcall_pass;
    oob_status_t ret;
    try
    {
        ret = esmi_get_threads_per_socket(soc_num, &threads_per_soc);
        if (ret)
        {
            sd_journal_print(LOG_ERR,
                             "esmi_get_threads_per_socket call failed \n");
        }
        else
        {
            threadCount(threads_per_soc);
            isthreadcall_pass = true;
        }
        usleep(APML_SLEEP);

        ret = esmi_get_threads_per_core(soc_num, &threads_per_core);
        if (ret)
        {
            sd_journal_print(LOG_ERR,
                             "esmi_get_threads_per_core call failed \n");
        }
        else
        {
            if (isthreadcall_pass)
            {
                uint32_t TotalCores = threads_per_soc / threads_per_core;
                coreCount(TotalCores);
            }
        }
    }
    catch (std::exception& e)
    {
        sd_journal_print(LOG_ERR, "Error getting Thread and Socket \n");
        return;
    }
}

void CpuInfo::get_cpu_base_freq(uint8_t soc_num)
{
    uint32_t buffer;
    oob_status_t ret;
    try
    {
        ret = esmi_oob_read_mailbox(soc_num, READ_BMC_CPU_BASE_FREQUENCY, 0,
                                    &buffer);
        if (ret != OOB_SUCCESS)
        {
            sd_journal_print(LOG_ERR, "read bmc cpu base freq failed \n");
            return;
        }
    }
    catch (std::exception& e)
    {
        sd_journal_print(LOG_ERR, "Error getting CPU Base Freq value \n");
        return;
    }

    maxSpeedInMhz(buffer);
}

// Get PPIN then we need to Decode to get Serial Number
void CpuInfo::get_ppin_fuse(uint8_t soc_num)
{
    oob_status_t ret;
    uint64_t data = 0;

    ret = read_ppin_fuse(soc_num, &data);
    if (ret)
    {
        sd_journal_print(LOG_ERR, "Failed to read PPIN fuse data\n");
    }
    else
    {
        id(data);
        if (data != 0)
        {
            decode_PPIN(data);
        }
    }
}

void CpuInfo::get_microcode_rev(uint8_t soc_num)
{
    uint32_t ucode;
    oob_status_t ret;
    try
    {
        ret = esmi_oob_read_mailbox(soc_num, READ_UCODE_REVISION, 0, &ucode);
        if (ret)
        {
            sd_journal_print(LOG_ERR, "Failed to read ucode revision\n");
            return;
        }
        sd_journal_print(LOG_INFO, "|ucode revision  | 0x%-32x |\n", ucode);
        microcode(ucode);
    }
    catch (std::exception& e)
    {
        sd_journal_print(LOG_ERR, "Error getting microcode: %s \n", e.what());
    }
}

// function to decode Marking Month - last Digit of making Year and Unit # in
// lot
void CpuInfo::decode_datemonth_unitlot(char* ppinstr,
                                       std::string& datemonthlotstr)
{
    std::string ppin_str(ppinstr);
    size_t len = strlen(ppinstr);
    // get lower PPIN
    int offset = len - LOWER_PINBITS;
    std::string lower32ppinstr = ppin_str.substr(offset, LOWER_PINBITS);
    lower32ppinstr = HEX + lower32ppinstr;

    // get upper PPIN
    std::string upper32ppinstr = ppin_str.substr(0, (len - LOWER_PINBITS));
    upper32ppinstr = HEX + upper32ppinstr;

    // convert hex string to actual number
    unsigned int upper32ppin;
    std::stringstream ss1;
    ss1 << std::hex << upper32ppinstr;
    ss1 >> upper32ppin;

    unsigned int lower32ppin;
    std::stringstream ss2;
    ss2 << std::hex << lower32ppinstr;
    ss2 >> lower32ppin;

    // Bits 0-13 are Dev Number
    // mask out all 14 bits
    int devnum = (lower32ppin & MASKNO_14BITS);
    // add leading zeros to the string if less then 4 digit
    std::string devnumstr = std::to_string(devnum);
    std::ostringstream ss3;
    ss3 << std::setw(DEV_LENGTH) << std::setfill('0') << devnumstr;
    devnumstr = ss3.str();

    // Bits 14-20 are Month/Year
    int datecode = (lower32ppin & MASKNO_MON_YEAR);
    datecode = (datecode >> DATECODE_SHIFT);

    int month = (datecode / MONTH_VAL);
    month = month + 1;

    int year = datecode % MONTH_VAL;
    std::string monthstr = "";
    if (months_map.find(month) != months_map.end())
    {
        monthstr = months_map.find(month)->second;
    }

    datemonthlotstr = monthstr + std::to_string(year) + devnumstr;
}

// decode marked lot number char length is 7 -Fuse/mak lot #
void CpuInfo::decode_lotstring(char* ppinstr, std::string& markedlotstr)
{
    std::string ppin_str(ppinstr);
    uint64_t full64ppin;
    uint64_t markedlot;
    uint64_t converter_num;
    uint64_t decodechar_num;

    // convert hex string to actual number
    std::string ppin_hexstr = HEX + ppin_str;
    std::stringstream ss;
    ss << std::hex << ppin_hexstr;
    ss >> full64ppin;

    // Bits 21-57 are Marked Lot Number
    markedlot = full64ppin >> LOTNUM_SHIFT;

    converter_num = markedlot;
    char currentchar[CMD_BUFF_LEN] = {0};

    // Now convert Marked Lot through Alpha Numeric 37 decoding
    // marked lot is 7 char string
    for (int i = 0; i < LOTNUM_LENGTH; i++)
    {
        decodechar_num = converter_num % MAX_ALPHA_NUM;
        converter_num = converter_num / MAX_ALPHA_NUM;
        if (decodechar_num < MAX_ALPHA_LENGTH)
        {
            decodechar_num = decodechar_num + ALPHA_CHAR_CONVER_VAL;
            currentchar[i] = decodechar_num;
        }
        else
        {
            decodechar_num = decodechar_num + DIGIT_CONVER_VAL;
            currentchar[i] = decodechar_num;
        }
    }
    // reverse the char buffer to get lot number
    int len, temp, itrlen;
    len = strlen(currentchar);
    itrlen = len / LENGTH_DIV;
    for (int i = 0; i < itrlen; i++)
    {
        temp = currentchar[i];
        currentchar[i] = currentchar[len - i - 1];
        currentchar[len - i - 1] = temp;
    }
    markedlotstr = currentchar;
}

// decode PPIN to get SN
void CpuInfo::decode_PPIN(uint64_t data)
{
    char ppinstr[CMD_BUFF_LEN] = {0};
    std::string markedlotstr;
    std::string datemonthlotstr;
    std::string serialnumstr;

    sprintf(ppinstr, "0%lx", data);
    decode_lotstring(ppinstr, markedlotstr);
    sd_journal_print(LOG_INFO, "Mark Lot string # %s \n", markedlotstr.c_str());

    decode_datemonth_unitlot(ppinstr, datemonthlotstr);
    sd_journal_print(LOG_INFO, "Month:Year:#Unitlot %s \n",
                     datemonthlotstr.c_str());

    // serial Number = lotstring + month + year + devnum
    serialnumstr = markedlotstr + datemonthlotstr;

    serialNumber(serialnumstr.c_str());

    return;
}
