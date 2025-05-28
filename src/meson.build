nvme_srcs = files('NVMeDeviceMain.cpp', 'NVMeDevice.cpp', 'NVMeMi.cpp')

nvme_deps = [ default_deps, threads ]

executable(
    'nvme',
    sources: nvme_srcs,
    dependencies: nvme_deps,
    implicit_include_directories: false,
    include_directories: '../include',
    cpp_args: ['-frtti', '-UBOOST_ASIO_NO_DEPRECATED', '-UBOOST_ASIO_DISABLE_THREADS', '-UBOOST_ASIO_HAS_IO_URING'],
    link_args: '-Wl,--gc-sections',
    install: true,
)
