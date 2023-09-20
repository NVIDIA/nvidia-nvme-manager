project(
    'dbus-Drives',
    'cpp',
    default_options: [
        'warning_level=3',
        'werror=true',
        'buildtype=debugoptimized',
        'cpp_std=c++20'
    ],
    license: 'Apache-2.0',
    version: '0.1',
    meson_version: '>=0.57.0',
)

add_project_arguments(
    '-Wno-psabi',
    '-Wuninitialized',
    '-DBOOST_SYSTEM_NO_DEPRECATED',
    '-DBOOST_ERROR_CODE_HEADER_ONLY',
    '-DBOOST_NO_RTTI',
    '-DBOOST_NO_TYPEID',
    '-DBOOST_ALL_NO_LIB',
    '-DBOOST_ASIO_DISABLE_THREADS',
    '-DBOOST_ALLOW_DEPRECATED_HEADERS',
    '-DCONFIG_LIBMCTP',
    language: 'cpp',
)

cpp = meson.get_compiler('cpp')


sdbusplus = dependency(
    'sdbusplus',
    fallback: [
        'sdbusplus',
        'sdbusplus_dep'
    ],
)

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
        )
    )
endif

libnvme_dep = dependency('libnvme')
libnvme_mi_dep = dependency('libnvme-mi')

systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    pkgconfig: 'systemdsystemunitdir',
    pkgconfig_define: ['prefix', get_option('prefix')])
threads = dependency('threads')

default_deps = [
    nlohmann_json,
    phosphor_logging_dep,
    sdbusplus,
    libnvme_dep,
    libnvme_mi_dep,
]

subdir('include')
subdir('service_files')
subdir('src')
