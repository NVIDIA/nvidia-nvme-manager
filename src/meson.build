nvme_srcs = files('NVMeDeviceMain.cpp', 'NVMeDevice.cpp', 'NVMeMI.cpp')

nvme_deps = [ default_deps, threads ]

executable(
    'nvme',
    sources: nvme_srcs,
    dependencies: nvme_deps,
    implicit_include_directories: false,
    include_directories: '../include',
    link_args: '-Wl,--gc-sections',
    install: true,
)
