#include "cpu_info.hpp"

#include "iomanip"
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/error.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/property.hpp>
#include <gpiod.hpp>
#include <filesystem>
#include <linux/types.h>
#include <linux/ioctl.h>

extern "C" {
#include <unistd.h>
#include "linux/i2c-dev.h"
#include "i2c/smbus.h"
#include "apml64Config.h"
#include "esmi_cpuid_msr.h"
#include "apml.h"
#include "esmi_mailbox.h"
#include "esmi_rmi.h"
#include "esmi_cpuid_msr.h"
#include "esmi_tsi.h"
#include "esmi_mailbox_int.h"
}

#define COMMAND_BOARD_ID    ("/sbin/fw_printenv -n board_id")
#define COMMAND_LEN         3
#define MAX_RETRY           20

#define CMD_BUFF_LEN     256
#define FNAME_LEN        128

// AMPL command
#define EAX_VAL 1
#define EAX_DATA_LEN_1 4
#define EAX_DATA_LEN_2 8
#define EAX_DATA_LEN_3 16
#define EAX_DATA_LEN_4 20
#define EAX_MASK_MAGIC_1 0xf
#define EAX_MASK_MAGIC_2 0xff
#define EAX_MASK_MAGIC_3 0x10
#define APML_SLEEP 10000
#define MUX_SLEEP 5

// PPIN logic
#define LOWER_PINBITS 8
#define MASKNO_14BITS 0x00003FFF
#define DEV_LENGTH 4
#define MASKNO_MON_YEAR 0x001FC000
#define DATECODE_SHIFT 14
#define MONTH_VAL 10
#define LOTNUM_SHIFT 21
#define LOTNUM_LENGHT 7
#define MAX_ALPHA_NUM 37
#define MAX_ALPHA_LENGTH 27
#define ALPHA_CHAR_CONVER_VAL 64
#define DIGIT_CONVER_VAL 21
#define HEX "0x"

// Platform Type
constexpr auto ONYX_SLT     = 61;   //0x3D
constexpr auto ONYX_1       = 64;   //0x40
constexpr auto ONYX_2       = 65;   //0x41
constexpr auto ONYX_3       = 66;   //0x42
constexpr auto ONYX_FR4     = 82;   //0x52
constexpr auto QUARTZ_DAP   = 62;   //0x3E
constexpr auto QUARTZ_1     = 67;   //0x43
constexpr auto QUARTZ_2     = 68;   //0x44
constexpr auto QUARTZ_3     = 69;   //0x45
constexpr auto QUARTZ_FR4   = 81;   //0x51
constexpr auto RUBY_1       = 70;   //0x46
constexpr auto RUBY_2       = 71;   //0x47
constexpr auto RUBY_3       = 72;   //0x48
constexpr auto TITANITE_1   = 73;   //0x49
constexpr auto TITANITE_2   = 74;   //0x4A
constexpr auto TITANITE_3   = 75;   //0x4B
constexpr auto TITANITE_4   = 76;   //0x4C
constexpr auto TITANITE_5   = 77;   //0x4D
constexpr auto TITANITE_6   = 78;   //0x4E
//SP6 PLATFORMS
constexpr auto SHALE_1      = 98;   //0x62
constexpr auto SHALE_2      = 101;  //0x65
constexpr auto SHALE_3      = 89;   //0x59
constexpr auto CINNABAR     = 99;   //0x63
constexpr auto SUNSTONE_1   = 97;   //0x61
constexpr auto SUNSTONE_2   = 100;  //0x64

constexpr auto P0_PATH = "/xyz/openbmc_project/inventory/system/processor/P0";
constexpr auto P1_PATH = "/xyz/openbmc_project/inventory/system/processor/P1";

CpuInfoDataHolder* CpuInfoDataHolder::instance = 0;

uint8_t p0_info = 0;
uint8_t p1_info = 1;
uint32_t eax;
uint32_t ebx;
uint32_t ecx;
uint32_t edx;

// Init CPU Information using OOB library
void CpuInfo::collect_cpu_information()
{

  for(uint8_t soc_num = 0; soc_num < num_of_proc;  soc_num++)
  {
     if (connect_apml_get_family_model_step(soc_num))
     {
        set_general_info(soc_num);
        get_threads_per_core_and_soc(soc_num);
        get_cpu_base_freq(soc_num);
        get_ppin_fuse(soc_num);
        get_microcode_rev(soc_num);
     }
  }

}
//Call Apml library to get the CPU Info
bool CpuInfo::connect_apml_get_family_model_step(uint8_t soc_num )
{
    int retry = 0;
    oob_status_t ret;
    uint32_t family_id;
    uint32_t model_id;
    uint32_t step_id;
    int core_id = 0;
    uint16_t freq;
    ebx = 0;
    edx = 0;
    eax = EAX_VAL;
    ecx = 0;
    try
    {
      while(retry < MAX_RETRY)
      {
        ret = esmi_oob_cpuid(soc_num, core_id, &eax, &ebx, &ecx, &edx);
        if(ret != 0)
        {
          sleep(MUX_SLEEP);
          retry++;
        }
        else
        {
          break;
        }
      }//end of retry

      if(ret != 0)
      {
        sd_journal_print(LOG_ERR, "Error : Unable to get the CPU info from APML \n" );
      }
      else
      {
        char cpuid[CMD_BUFF_LEN];
        family_id = ((eax >> EAX_DATA_LEN_2) & EAX_MASK_MAGIC_1) + ((eax >> EAX_DATA_LEN_4) & EAX_MASK_MAGIC_2);
        sprintf(cpuid, "%x (%d)", family_id, family_id);
        set_cpu_string_value(soc_num, cpuid, "Family", CPU_INTERFACE);
        model_id = ((eax >> EAX_DATA_LEN_3) & EAX_MASK_MAGIC_1) * EAX_MASK_MAGIC_3 + ((eax >> EAX_DATA_LEN_1) & EAX_MASK_MAGIC_1);
        sprintf(cpuid, "%x (%d)", model_id, model_id);
        set_cpu_string_value(soc_num, cpuid, "Model", CPU_INTERFACE);
        step_id = eax & EAX_MASK_MAGIC_1 ;
        sprintf(cpuid, "%x (%d)", step_id, step_id);
        set_cpu_string_value(soc_num, cpuid, "Step", CPU_INTERFACE);
        sprintf(cpuid, "%d", soc_num);
        set_cpu_string_value(soc_num, cpuid, "Socket", CPU_INTERFACE);

        return true;
      }
    }
    catch (std::exception& e)
    {
       sd_journal_print(LOG_ERR, "Error getting CPU Model, Family and Step value \n");
       return false;
    }

    return false;
}
void CpuInfo::set_general_info(uint8_t soc_num)
{
    set_cpu_string_value(soc_num, "AMD", "Manufacturer", ASSET_INTERFACE);
    set_cpu_string_value(soc_num, "AuthenticAMD", "VendorId", CPU_INTERFACE);

}
//Get processor threads per Core and Socket
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
        sd_journal_print(LOG_ERR, "esmi_get_threads_per_socket call failed \n");
      }
      else 
      {
        set_cpu_int16_value(soc_num, threads_per_soc, "ThreadCount", CPU_INTERFACE);
        isthreadcall_pass = true;
      }
      usleep(APML_SLEEP);

      ret = esmi_get_threads_per_core(soc_num, &threads_per_core);
      if (ret)
      {
        sd_journal_print(LOG_ERR, "esmi_get_threads_per_core call failed \n");
      }
      else
      {
	if(isthreadcall_pass)
	{
	  uint32_t TotalCores = threads_per_soc / threads_per_core; 
          set_cpu_int16_value(soc_num, TotalCores, "CoreCount", CPU_INTERFACE);
	}
      }

    }
    catch (std::exception& e)
    {
       sd_journal_print(LOG_ERR, "Error getting Thread and Socket \n");
       return ;
    }
}

void CpuInfo::get_cpu_base_freq(uint8_t soc_num)
{
    uint32_t  buffer, value;
    oob_status_t ret;
    try
    {
       ret = esmi_oob_read_mailbox(soc_num, READ_BMC_CPU_BASE_FREQUENCY, 0, &buffer);
       if (ret != OOB_SUCCESS) {
            sd_journal_print(LOG_ERR, "read bmc cpu base freq failed \n");
            return;
       }
     }
     catch (std::exception& e)
     {
        sd_journal_print(LOG_ERR, "Error getting CPU Base Freq value \n");
        return ;
     }
     set_cpu_int_value(soc_num, buffer, "MaxSpeedInMhz", CPU_INTERFACE);
}
//Get PPIN then we need to Decode to get Serial Number
void CpuInfo::get_ppin_fuse(uint8_t soc_num)
{
    uint32_t buffer;
    oob_status_t ret;
    uint64_t data = 0;
    char cpuid[CMD_BUFF_LEN];
    try
    {
      // Read lower 32 bit PPIN data
      ret = esmi_oob_read_mailbox(soc_num, READ_PPIN_FUSE, LO_WORD_REG, &buffer);
      if (!ret)
      {
          data = buffer;
          // Read higher 32 bit PPIN data
          ret = esmi_oob_read_mailbox(soc_num, READ_PPIN_FUSE, HI_WORD_REG, &buffer);
          if (!ret)
          {
            data |= ((uint64_t)buffer << 32);
            //now decode PPIN to get SN
            decode_PPIN(soc_num, data);

          }
      }
   }
   catch (std::exception& e)
   {
      sd_journal_print(LOG_ERR, "Error getting PPN value \n");
      return ;
   }
}
void CpuInfo::get_microcode_rev(uint8_t soc_num)
{
    uint32_t ucode;
    oob_status_t ret;
    ret = esmi_oob_read_mailbox(soc_num, READ_UCODE_REVISION, 0, &ucode);
    if (ret) {
        sd_journal_print(LOG_ERR,"Failed to read ucode revision\n");
        return;
    }
    sd_journal_print(LOG_INFO,"|ucode revision  | 0x%-32x |\n", ucode);
    //set the Dbus value
    char microid[CMD_BUFF_LEN];
    sprintf(microid, "0x%x",ucode);
    set_cpu_string_value(soc_num, microid, "Microcode", CPU_INTERFACE);
}
//get the platform ID
bool CpuInfo::getPlatformID()
{
    FILE *pf;
    char data[COMMAND_LEN];
    std::stringstream ss;
    // Setup pipe for reading and execute to get u-boot environment
    pf = popen(COMMAND_BOARD_ID,"r");
    if(pf > 0)
    {   // no error
        if (fgets(data, COMMAND_LEN , pf) != NULL)
        {
            ss << std::hex << (std::string)data;
            ss >> board_id;
        }
        pclose(pf);
        if ( board_id > 0 || board_id < 0xFF )
        {
            switch (board_id)
            {
                case ONYX_SLT:
                case ONYX_1 ... ONYX_3:
                case ONYX_FR4:
                case RUBY_1 ... RUBY_3:
                case SHALE_1:
                case SHALE_2:
                case SHALE_3:
                case CINNABAR:
                case SUNSTONE_1:
                case SUNSTONE_2:
                    num_of_proc = 1;
                    break;
                case QUARTZ_DAP:
                case QUARTZ_1 ... QUARTZ_3:
                case QUARTZ_FR4:
                case TITANITE_1 ... TITANITE_6:
                    num_of_proc = 2;
                    break;
                default:
                    num_of_proc = 1;
                    break;
            }//switch
            return true;
        }
    }
    else
    {
        sd_journal_print(LOG_ERR, "Failed to open command stream \n");
    }
    return false;
}

std::string CpuInfo::get_interface(uint8_t enum_val )
{
    return enum_str[enum_val];
}
//Set the CPU DBus value
void CpuInfo::set_cpu_string_value(uint8_t soc_num, char *value, std::string property_name, uint8_t enum_val)
{
    sd_journal_print(LOG_INFO, "Set the DBUS Property of %s \n", property_name.c_str());
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    boost::system::error_code ec;
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    if (soc_num == 0)
    {
      conn->async_method_call(
          [this](boost::system::error_code ec) {
              if (ec)
              {
                  sd_journal_print(LOG_ERR, "Failed to set CPU value in dbus interface \n");
              }
          },
          "xyz.openbmc_project.Inventory.Manager",
          P0_PATH,
          "org.freedesktop.DBus.Properties", "Set",
          get_interface(enum_val), property_name,
          std::variant<std::string>(value));
    }
    else if (soc_num == 1)
    {
       conn->async_method_call(
          [this](boost::system::error_code ec) {
              if (ec)
              {
                  sd_journal_print(LOG_ERR, "Failed to set CPU value in dbus interface \n" );
              }
          },
          "xyz.openbmc_project.Inventory.Manager",
          P1_PATH,
          "org.freedesktop.DBus.Properties", "Set",
          get_interface(enum_val), property_name,
          std::variant<std::string>(value));
    }
}
void CpuInfo::set_cpu_int_value(uint8_t soc_num, uint32_t value, std::string property_name, uint8_t enum_val)
{
    sd_journal_print(LOG_INFO,"Set the DBUS Property of %s \n", property_name.c_str());
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    boost::system::error_code ec;
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    if (soc_num == 0)
    {
      conn->async_method_call(
          [this](boost::system::error_code ec) {
              if (ec)
              {
                  sd_journal_print(LOG_ERR, "Failed to set CPU value in dbus interface \n");
              }
          },
          "xyz.openbmc_project.Inventory.Manager",
          P0_PATH,
          "org.freedesktop.DBus.Properties", "Set",
          get_interface(enum_val), property_name,
          std::variant<uint32_t>(value));
    }
    else if (soc_num == 1)
    {
       conn->async_method_call(
          [this](boost::system::error_code ec) {
              if (ec)
              {
                  sd_journal_print(LOG_ERR, "Failed to set CPU value in dbus interface \n");
              }
          },
          "xyz.openbmc_project.Inventory.Manager",
          P1_PATH,
          "org.freedesktop.DBus.Properties", "Set",
          get_interface(enum_val), property_name,
          std::variant<uint32_t>(value));
    }
}
void CpuInfo::set_cpu_int16_value(uint8_t soc_num, uint16_t value, std::string property_name, uint8_t enum_val)
{
    sd_journal_print(LOG_INFO,"Set the DBUS Property of %s \n", property_name.c_str());
    sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
    boost::system::error_code ec;
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    if (soc_num == 0)
    {
      conn->async_method_call(
          [this](boost::system::error_code ec) {
              if (ec)
              {
                  sd_journal_print(LOG_ERR, "Failed to set CPU value in dbus interface \n");
              }
          },
          "xyz.openbmc_project.Inventory.Manager",
          P0_PATH,
          "org.freedesktop.DBus.Properties", "Set",
          get_interface(enum_val), property_name,
          std::variant<uint16_t>(value));
    }
    else if (soc_num == 1)
    {
       conn->async_method_call(
          [this](boost::system::error_code ec) {
              if (ec)
              {
                  sd_journal_print(LOG_ERR, "Failed to set CPU value in dbus interface \n");
              }
          },
          "xyz.openbmc_project.Inventory.Manager",
          P1_PATH,
          "org.freedesktop.DBus.Properties", "Set",
          get_interface(enum_val), property_name,
          std::variant<uint16_t>(value));
    }
}
//function to decode Marking Month - last Digit of making Year and Unit # in lot
void CpuInfo::decode_datemonth_unitlot(char* ppinstr, std::string& datemonthlotstr)
{
    std::string ppin_str(ppinstr);
    size_t len = strlen(ppinstr);
    // get lower PPIN
    int offset = len - LOWER_PINBITS;
    std::string lower32ppinstr = ppin_str.substr(offset, LOWER_PINBITS);
    lower32ppinstr = HEX + lower32ppinstr;

    //get upper PPIN
    std::string upper32ppinstr = ppin_str.substr (0, (len - LOWER_PINBITS));
    upper32ppinstr = HEX + upper32ppinstr;

    //convert hex string to actual number
    unsigned int upper32ppin;
    std::stringstream ss1;
    ss1 << std::hex << upper32ppinstr;
    ss1 >> upper32ppin;

    unsigned int lower32ppin;
    std::stringstream ss2;
    ss2 << std::hex << lower32ppinstr;
    ss2 >> lower32ppin;

    //Bits 0-13 are Dev Number
    //mask out all 14 bits
    int devnum = (lower32ppin & MASKNO_14BITS);
    //add leading zeros to the string if less then 4 digit
    std::string devnumstr = std::to_string(devnum);
    std::ostringstream ss3;
    ss3 << std::setw(DEV_LENGTH) << std::setfill('0') << devnumstr;
    devnumstr = ss3.str();

    //Bits 14-20 are Month/Year
    int datecode = (lower32ppin & MASKNO_MON_YEAR);
        datecode = (datecode >> DATECODE_SHIFT);

    int month = (datecode / MONTH_VAL);
        month = month + 1;

    int year = datecode  % MONTH_VAL;

    std::string monthstr;
    for (auto itr = months_map.find(month); itr != months_map.end(); itr++)
    {
      monthstr = itr->second;
    }

    datemonthlotstr = monthstr + std::to_string(year) + devnumstr;
}
//decode marked lot number char length is 7 -Fuse/mak lot #
void CpuInfo::decode_lotstring(char* ppinstr, std::string& markedlotstr)
{
    std::string ppin_str(ppinstr);
    uint64_t full64ppin;
    uint64_t markedlot;
    uint64_t converter_num;
    uint64_t decodechar_num;

    //convert hex string to actual number
    std::string ppin_hexstr = HEX + ppin_str;
    std::stringstream ss;
    ss << std::hex << ppin_hexstr;
    ss >> full64ppin;

    //Bits 21-57 are Marked Lot Number
    markedlot = full64ppin >> LOTNUM_SHIFT;

    converter_num = markedlot;
    char currentchar[CMD_BUFF_LEN];
    char lotstring[CMD_BUFF_LEN];

    //Now convert Marked Lot through Alpha Numeric 37 decoding
    //marked lot is 7 char string
    for (int i=0; i < LOTNUM_LENGHT; i++)
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
       lotstring[i] = currentchar[i];
    }
    markedlotstr = lotstring;
}
//decode PPIN to get SN
void CpuInfo::decode_PPIN(uint8_t soc_num, uint64_t data)
{
    char ppinstr[CMD_BUFF_LEN];
    std::string markedlotstr;
    std::string datemonthlotstr;
    std::string serialnumstr;

    sprintf(ppinstr, "%llx", data);
    sd_journal_print(LOG_INFO, "PPIN Fuse : %s \n", ppinstr);

    decode_lotstring(ppinstr, markedlotstr);
    sd_journal_print(LOG_INFO, "Mark Lot string # %s \n", markedlotstr.c_str());

    decode_datemonth_unitlot(ppinstr, datemonthlotstr);
    sd_journal_print(LOG_INFO, "Month:Year:#Unitlot %s \n", datemonthlotstr.c_str());

    // Serial Number = lotstring + month + year + devnum
    serialnumstr = markedlotstr + datemonthlotstr;

    //now convert string to Char buffer to set in DBus
    char serialnum_buffer[serialnumstr.length() + 1];
    strcpy(serialnum_buffer, serialnumstr.c_str());

    //set the Dbus property
    set_cpu_string_value(soc_num, serialnum_buffer, "SerialNumber", ASSET_INTERFACE);

    return;
}

