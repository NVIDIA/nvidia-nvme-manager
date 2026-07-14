unit_files = [['nvidia-nvme-manager.service']]

if get_option('mux_manager').enabled()
    unit_files += [['nvme-mux-manager.service']]
endif

foreach tuple : unit_files
    configure_file(
        copy: true,
        input: tuple[0],
        install: true,
        install_dir: systemd_system_unit_dir,
        output: tuple[0],
    )
endforeach
