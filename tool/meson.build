executable(
    'nvme-fw-update',
    sources: 'NVMeFwUpdate.cpp',
    dependencies: default_deps,
    implicit_include_directories: false,
    include_directories: '../include',
    cpp_args: [
        '-frtti',
        '-UBOOST_ASIO_NO_DEPRECATED',
        '-UBOOST_ASIO_DISABLE_THREADS',
        '-UBOOST_ASIO_HAS_IO_URING',
    ],
    link_args: '-Wl,--gc-sections',
    install: true,
)
