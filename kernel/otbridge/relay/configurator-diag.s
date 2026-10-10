	.text
	.globl OTSetupConfigurator
OTSetupConfigurator:
	lea otr_config_diagnostics,%a0
	jmp otr_setup_configurator
