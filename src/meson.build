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

executable(
    'nvme',
    sources: nvme_srcs,
    dependencies: nvme_deps,
    implicit_include_directories: false,
    include_directories: ['../include', '../include/sensors'],
    cpp_args: [
        '-frtti',
        '-UBOOST_ASIO_NO_DEPRECATED',
        '-UBOOST_ASIO_DISABLE_THREADS',
        '-UBOOST_ASIO_HAS_IO_URING',
    ],
    link_args: '-Wl,--gc-sections',
    install: true,
)

