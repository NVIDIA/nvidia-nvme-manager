conf_data = configuration_data()
conf_data.set_quoted(
    'PLATFORM_DRIVE_LOCATION',
    get_option('platform_drive_location'),
)
conf_data.set('DRIVE_SANITIZE_TIME', get_option('drive_sanitize_time'))
conf_data.set('IDENTIFY_RSP_LENGTH', get_option('identify_rsp_length'))
conf_data.set_quoted(
    'PLATFORM_DRIVE_PREFIX',
    get_option('platform_drive_prefix'),
)
conf_data.set_quoted('DRIVE_STATE_FILE', get_option('drive_state_file'))
conf_data.set('BOOT_PROGRESS_TIMEOUT', get_option('boot_progress_timeout'))
conf_data.set_quoted(
    'NVME_UPDATE_SERVICE_PREFIX',
    get_option('nvme_update_service_prefix'),
)
configure_file(
    input: 'nvme-mi_config.h.in',
    output: 'nvme-mi_config.h',
    configuration: conf_data,
)

