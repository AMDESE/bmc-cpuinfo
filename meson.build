project(
  'cpu-info',
  'cpp',
  default_options: [
    'warning_level=3',
    'werror=true',
    'cpp_std=c++23'
  ],
  license: 'Apache-2.0',
  version: '1.0',
)

inc_dirs = include_directories('inc', '.')

dbus_service_name = 'xyz.openbmc_project.Inventory.Item.Cpu_info'
dbus_p0_object_name = '/xyz/openbmc_project/inventory/system/processor/P0'
dbus_p1_object_name = '/xyz/openbmc_project/inventory/system/processor/P1'
dbus_intf_name = 'xyz.openbmc_project.Inventory.Item.Cpu'

add_project_arguments('-DDBUS_P0_OBJECT_NAME="' + dbus_p0_object_name + '"',
                      '-DDBUS_P1_OBJECT_NAME="' + dbus_p1_object_name + '"',
                      '-DDBUS_INTF_NAME="' + dbus_intf_name + '"',
                      '-DDBUS_SERVICE_NAME="' + dbus_service_name+ '"',
                      language : 'cpp')

src_files = files('src/cpu_info.cpp', 'src/main.cpp')
service_files = files('service_files/xyz.openbmc_project.Inventory.Item.Cpu_info.service')

sdbusplusplus_dep = dependency('sdbusplus')
logging_dep = dependency('phosphor-logging')
dbusinterface_dep = dependency('phosphor-dbus-interfaces')

executable(
  'cpu-info',
  src_files,
  include_directories : include_directories('inc'),
  dependencies : [sdbusplusplus_dep, logging_dep, dbusinterface_dep],
  link_args : ['-lstdc++fs', '-lphosphor_dbus', '-lapml64', '-li2c', '-lpthread', '-lm', '-lgpiodcxx'],
  install: true,
  install_dir: get_option('bindir'))

systemd = dependency('systemd')

install_data(
  service_files,
  install_dir: systemd.get_pkgconfig_variable('systemdsystemunitdir')
)
