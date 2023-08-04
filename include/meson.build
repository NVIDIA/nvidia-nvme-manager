conf_data = configuration_data()
conf_data.set10('VALIDATION_UNSECURE_FEATURE', get_option('validate-unsecure-feature').enabled())
conf_data.set10('INSECURE_UNRESTRICTED_Drive_OVERRIDE', get_option('insecure-Drive-override').enabled())
configure_file(input: 'nvme-mi_config.h.in',
               output: 'nvme-mi_config.h',
               configuration: conf_data)

