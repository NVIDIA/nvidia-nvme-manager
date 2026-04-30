nvme_srcs = files(
    'NVMeDevice.cpp',
    'NVMeDeviceMain.cpp',
    'NVMeFwUpdateHandler.cpp',
    'NVMeMi.cpp',
)
if get_option('nvme_mi_sensors')
    nvme_srcs += files(
        'sensors/GetSensorConfiguration.cpp',
        'sensors/NVMeMiSensor.cpp',
        'sensors/NVMeMiSensorManager.cpp',
        'sensors/NVMeMiStatusSensor.cpp',
        'sensors/Thresholds.cpp',
    )
endif

nvme_deps = [default_deps, threads]
nvme_cpp_args = [
    '-frtti',
    '-UBOOST_ASIO_NO_DEPRECATED',
    '-UBOOST_ASIO_DISABLE_THREADS',
    '-UBOOST_ASIO_HAS_IO_URING',
]
if spdm_responder_dep.found() and mbedcrypto_dep.found() and mbedx509_dep.found() and inkernel_mctp
    if not cpp.has_header_symbol(
        'spdmcpp/config.h',
        'MCTP_IN_KERNEL',
        dependencies: spdm_responder_dep,
    )
        error(
            'NVMe SPDM requires libspdmcpp_responder built with in-kernel MCTP support',
        )
    endif
    nvme_srcs += files('NVMeSpdmResponder.cpp')
    nvme_deps += [spdm_responder_dep, mbedcrypto_dep, mbedx509_dep]
    nvme_cpp_args += ['-DNVME_SPDM']
endif

executable(
    'nvme',
    sources: nvme_srcs,
    dependencies: nvme_deps,
    implicit_include_directories: false,
    include_directories: ['../include', '../include/sensors'],
    cpp_args: nvme_cpp_args,
    link_args: '-Wl,--gc-sections',
    install: true,
)

if get_option('mux_manager').enabled()
    executable(
        'nvme-mux-manager',
        sources: 'NVMeMuxManager.cpp',
        dependencies: [nlohmann_json, phosphor_logging_dep, sdbusplus],
        implicit_include_directories: false,
        include_directories: '../include',
        link_args: '-Wl,--gc-sections',
        install: true,
    )
endif
