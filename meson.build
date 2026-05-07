project(
    'dbus-Drives',
    'cpp',
    default_options: [
        'warning_level=3',
        'werror=true',
        'buildtype=debugoptimized',
        'cpp_std=c++23',
    ],
    license: 'Apache-2.0',
    version: '0.1',
    meson_version: '>=1.1.1',
)

# Get the inkernel_mctp option
inkernel_mctp = get_option('inkernel_mctp')

# Get the firmware inventory option
firmware_inventory = get_option('firmware_inventory')

# Get the NVMe-MI sensors option
nvme_mi_sensors = get_option('nvme_mi_sensors')

# Get the platform drive prefix
platform_drive_prefix = get_option('platform_drive_prefix')

add_project_arguments(
    '-Wno-psabi',
    '-Wuninitialized',
    '-DBOOST_SYSTEM_NO_DEPRECATED',
    '-DBOOST_ERROR_CODE_HEADER_ONLY',
    '-DBOOST_NO_RTTI',
    '-DBOOST_NO_TYPEID',
    '-DBOOST_ALL_NO_LIB',
    '-DBOOST_ALLOW_DEPRECATED_HEADERS',
    '-DBOOST_ASIO_HAS_THREADS',
    '-DCONFIG_LIBMCTP',
    language: 'cpp',
)

# Add INKERNEL_MCTP define based on the option
if inkernel_mctp
    add_project_arguments('-DINKERNEL_MCTP', language: 'cpp')
endif

# Add FIRMWARE_INVENTORY define based on the option
if firmware_inventory
    add_project_arguments('-DFIRMWARE_INVENTORY', language: 'cpp')
endif

# Add NVME_MI_SENSORS define based on the option
if nvme_mi_sensors
    add_project_arguments('-DNVME_MI_SENSORS', language: 'cpp')
endif

# Add PLATFORM_DRIVE_PREFIX define
add_project_arguments(
    '-DPLATFORM_DRIVE_PREFIX="' + platform_drive_prefix + '"',
    language: 'cpp',
)

cpp = meson.get_compiler('cpp')


sdbusplus = dependency('sdbusplus', fallback: ['sdbusplus', 'sdbusplus_dep'])

phosphor_logging_dep = dependency(
    'phosphor-logging',
    fallback: ['phosphor-logging', 'phosphor_logging_dep'],
)

if cpp.has_header('nlohmann/json.hpp')
    nlohmann_json = declare_dependency()
else
    subproject('nlohmann-json')
    nlohmann_json = declare_dependency(
        include_directories: include_directories(
            'subprojects/nlohmann-json/single_include',
            'subprojects/nlohmann-json/single_include/nlohmann',
        ),
    )
endif

libnvme_dep = dependency('libnvme')
libnvme_mi_dep = dependency('libnvme-mi')

systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    pkgconfig: 'systemdsystemunitdir',
    pkgconfig_define: ['prefix', get_option('prefix')],
)
threads = dependency('threads')

default_deps = [
    nlohmann_json,
    phosphor_logging_dep,
    sdbusplus,
    libnvme_dep,
    libnvme_mi_dep,
]

if nvme_mi_sensors
    default_deps += dependency('nvidia-tal', required: true)
endif

subdir('include')
subdir('service_files')
subdir('src')
subdir('tool')
