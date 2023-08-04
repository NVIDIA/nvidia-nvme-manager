unit_files = [
    ['xyz.openbmc_project.nvme.service'],
]

foreach tuple : unit_files
    configure_file(
        copy: true,
        input: tuple[0],
        install: true,
        install_dir: systemd_system_unit_dir,
        output: tuple[0],
    )
endforeach
