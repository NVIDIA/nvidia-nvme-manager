conf_data = configuration_data()
conf_data.set_quoted('PLATFORM_DRIVE_LOCATION', get_option('platform_drive_location'))
configure_file(input: 'nvme-mi_config.h.in',
               output: 'nvme-mi_config.h',
               configuration: conf_data)

