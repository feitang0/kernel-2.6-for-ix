#include <linux/dmi.h>

/*
 * Some BIOSes may not present P-state related ACPI information
 * to the OS/driver on cpufreq capable machines on purpose.
 * Do not throw a firmware bug exception for these.
 */
static int bios_with_pstate_cap = -1;

static const struct dmi_system_id bios_cap_dmi_table[] = {
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant DL365"),
		},
	},
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant DL385"),
		},
	},
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant BL465c"),
		},
	},
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant BL495c"),
		},
	},
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant DL585"),
		},
	},
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant BL685c"),
		},
	},
	{
		.ident = "HP ProLiant",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ProLiant DL785"),
		},
	},
	{ }
};

int dmi_check_amd_bios_driven(void)
{
	if (bios_with_pstate_cap == -1)
		bios_with_pstate_cap = dmi_check_system(bios_cap_dmi_table);
	return bios_with_pstate_cap;
}
