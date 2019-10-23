#ifndef __LIBWR_SHW_SFPLIB_H
#define __LIBWR_SHW_SFPLIB_H

/* note each led contains green and orange part */
#define SFP_LED_WRMODE_SLAVE	(1) /* green */ 
#define SFP_LED_WRMODE_OTHER	(2) /* orange */
#define SFP_LED_WRMODE_MASTER	(3) /* yellow */
#define SFP_LED_WRMODE_OFF	(4) /* to off entire WRMODE LED */
#define SFP_LED_WRMODE1	(1 << 0)
#define SFP_LED_WRMODE2	(1 << 1)
#define SFP_LED_SYNCED	(1 << 2)
#define SFP_TX_DISABLE	(1 << 3)

#define shw_sfp_set_led_synced(num, status)	\
	shw_sfp_set_generic(num, status, SFP_LED_SYNCED)

#define shw_sfp_set_tx_disable(num, status)	\
	shw_sfp_set_generic(num, status, SFP_TX_DISABLE)

#define SFP_FLAG_CLASS_DATA	(1 << 0)
#define SFP_FLAG_DEVICE_DATA	(1 << 1)
#define SFP_FLAG_1GbE		(1 << 2) /* SFP is 1GbE */
#define SFP_FLAG_IN_DB		(1 << 3) /* SFP is present in data base */

#define SFP_SPEED_1Gb		0x0D /* Speed of SFP in 100MB/s. According to
				      * SFF-8472.PDF: By convention 1.25 Gb/s
				      * should be rounded up to 0Dh (13 in
				      * units of 100 MBd) for Ethernet
				      * 1000BASE-X. */
#define SFP_SPEED_1Gb_10   0x0A /* Unfortunatelly the above is not always true,
              * e.g. Cisco copper SFP (MGBT1) says simply 10 and not 13.*/

#define SFP_DIAGNOSTIC_IMPLEMENTED (1 << 6) /* Digital diagnostic monitoring
					       implemented. "1" for compliance
					       with SFF-8472 */
#define SFP_ADDR_CHANGE_REQ (1 << 2) /* Bit 2 indicates whether or not it is
					necessary for the host to perform an
					address change sequence before
					accessing information at 2-wire serial
					address A2h. */


struct shw_sfp_caldata {
	uint32_t flags;
	/*
	 * Part number used to identify it. Serial number because we
	 * may specify per-specimen delays, but it is not used at this
	 * point in time
	 */
	char vendor_name[16];
	char part_num[16];
	char vendor_serial[16];
	/* Callibration data */
	double alpha;
	int delta_tx_ps; /* "delta" of this SFP type WRT calibration type */
	int delta_rx_ps;
	/* wavelengths, used to get alpha from fiber type */
	int tx_wl;
	int rx_wl;
	/* and link as a list */
	struct shw_sfp_caldata *next;
};

struct shw_sfp_header {
	uint8_t id;
	uint8_t ext_id;
	uint8_t connector;
	uint8_t transciever[8];
	uint8_t encoding;
	uint8_t br_nom;
	uint8_t reserved1;
	uint8_t length1;	/* Link length supported for 9/125 mm fiber (km) */
	uint8_t length2;	/* Link length supported for 9/125 mm fiber (100m) */
	uint8_t length3;	/* Link length supported for 50/125 mm fiber (10m) */
	uint8_t length4;	/* Link length supported for 62.5/125 mm fiber (10m) */
	uint8_t length5;	/* Link length supported for copper (1m) */
	uint8_t length6;	/* Link length supported on OM3 (1m) */
	uint8_t vendor_name[16];
	uint8_t reserved3;	/* This is now a field named transceiver */
	uint8_t vendor_oui[3];
	uint8_t vendor_pn[16];
	uint8_t vendor_rev[4];
	uint8_t tx_wavelength[2];
	uint8_t reserved4;
	uint8_t cc_base;

	/* extended ID fields start here */
	uint8_t options[2];
	uint8_t br_max;
	uint8_t br_min;
	uint8_t vendor_serial[16];
	uint8_t date_code[8];
	uint8_t diagnostic_monitoring_type;
	uint8_t enhanced_options;
	uint8_t sff_8472_compliance;
	uint8_t cc_ext;
} __attribute__ ((packed));

struct shw_sfp_dom {
/* Treshold values, 0 - 55 */
	uint8_t temp_high_alarm[2];
	uint8_t temp_low_alarm[2];
	uint8_t temp_high_warn[2];
	uint8_t temp_low_warn[2];
	uint8_t volt_high_alarm[2];
	uint8_t volt_low_alarm[2];
	uint8_t volt_high_warn[2];
	uint8_t volt_low_warn[2];
	uint8_t bias_high_alarm[2];
	uint8_t bias_low_alarm[2];
	uint8_t bias_high_warn[2];
	uint8_t bias_low_warn[2];
	uint8_t tx_pow_high_alarm[2];
	uint8_t tx_pow_low_alarm[2];
	uint8_t tx_pow_high_warn[2];
	uint8_t tx_pow_low_warn[2];
	uint8_t rx_pow_high_alarm[2];
	uint8_t rx_pow_log_alarm[2];
	uint8_t rx_power_high_warn[2];
	uint8_t rx_power_low_warn[2];
	uint8_t unalloc0[16];
/* Calibration data, 56-91 */
	uint8_t cal_rx_pwr4[4];
	uint8_t cal_rx_pwr3[4];
	uint8_t cal_rx_pwr2[4];
	uint8_t cal_rx_pwr1[4];
	uint8_t cal_rx_pwr0[4];
	uint8_t cal_tx_i_slope[2];
	uint8_t cal_tx_i_offset[2];
	uint8_t cal_tx_pow_slope[2];
	uint8_t cal_tx_pow_offset[2];
	uint8_t cal_T_slope[2];
	uint8_t cal_T_offset[2];
	uint8_t cal_V_slope[2];
	uint8_t cal_V_offset[2];
/* Unallocated and checksum, 92-95 */
	uint8_t cal_unalloc[3];
	uint8_t CC_DMI;
/* Real Time Diagnostics, 96-111 */
	uint8_t temp[2];
	uint8_t vcc[2];
	uint8_t tx_bias[2];
	uint8_t tx_pow[2];
	uint8_t rx_pow[2];
	uint8_t rtd_unalloc0[4];
	uint8_t OSCB;
	uint8_t rtd_unalloc1;
/* Alarms and Warnings, 112 - 117 */
	uint8_t alw[6];
/* Extended Module Control/Status bytes 118 - 119 */
	uint8_t emcsb[2];
/* Vendor locations 120 - 127 */
	uint8_t vendor_locations[8];
/* User data 128 - 247 */
	uint8_t dom_user[120];
/* Vendor specific control function locations 248 - 255 */
	uint8_t vendor_functions[8];
} __attribute__ ((packed));

/* Public API */

/*
 * Scan all ports for plugged in SFP's. The return value is a bitmask
 * of all the ports with detected SFP's (bits 0-17 are valid).
 */
uint32_t shw_sfp_module_scan(void);

/* Set/get the 4 GPIO's connected to PCA9554's for a particular SFP */
void shw_sfp_gpio_set(int num, uint8_t state);
uint8_t shw_sfp_gpio_get(int num);

static inline void shw_sfp_set_generic(int num, int status, int type)
{
	uint8_t state;
	state = shw_sfp_gpio_get(num);
	if (status)
		state |= type;
	else
		state &= ~type;
	shw_sfp_gpio_set(num, state);
}

/* Load the db from dot-config to internal structures */
int shw_sfp_read_db(void);

/* Read and verify the header all at once. returns -1 on failure */
int shw_sfp_read_verify_header(int num, struct shw_sfp_header *head);

/* Read the SFP diagnostics page */
int shw_sfp_read_dom(int num, struct shw_sfp_dom *dom);

/* Update the SFP diagnostics page */
int shw_sfp_update_dom(int num, struct shw_sfp_dom *dom);

/* Decode and print the SFP real time diagnostics */
void shw_sfp_print_dom(struct shw_sfp_dom * dom);

/* Dump the SFP diagnostics page in hex */
void shw_sfp_dom_dump(struct shw_sfp_dom * dom);

/* return NULL if no data found */
struct shw_sfp_caldata *shw_sfp_get_cal_data(int num,
					     struct shw_sfp_header *head);

#endif /* __LIBWR_SHW_SFPLIB_H */
