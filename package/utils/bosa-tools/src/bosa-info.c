// SPDX-License-Identifier: GPL-2.0-only
/*
 * bosa-info / bosa-ctl: Optical Transceiver (BOSA / SFF-8472) DDMI Utility
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define I2C_DEV_PATH		"/dev/i2c-0"
#define BOSA_I2C_ADDR_A0	0x50
#define BOSA_I2C_ADDR_A2	0x51
#define SYSFS_PON_DIR		"/sys/devices/platform/soc/92040000.pon"

struct bosa_identity {
	char vendor[17];
	char part_number[17];
	char revision[5];
	char serial[17];
	char date_code[9];
};

struct bosa_thresholds {
	double temp_hi_alarm, temp_lo_alarm;
	double temp_hi_warn, temp_lo_warn;
	double vcc_hi_alarm, vcc_lo_alarm;
	double vcc_hi_warn, vcc_lo_warn;
	double bias_hi_alarm, bias_lo_alarm;
	double tx_hi_alarm, tx_lo_alarm;
	double rx_hi_alarm, rx_lo_alarm;
};

struct bosa_ddmi {
	double temperature_c;
	double voltage_v;
	double bias_ma;
	double tx_power_uw;
	double rx_power_uw;
	double tx_power_dbm;
	double rx_power_dbm;
	uint8_t status_control;
	uint8_t alarm_flags[2];
	uint8_t warn_flags[2];
	bool laser_enabled;
	char optical_state[8];
};

static int read_i2c_block(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD, .len = len, .buf = buf }
	};
	struct i2c_rdwr_ioctl_data data = {
		.msgs = msgs,
		.nmsgs = 2
	};

	return ioctl(fd, I2C_RDWR, &data);
}

static void read_sysfs_string(const char *name, char *out, size_t max)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", SYSFS_PON_DIR, name);
	f = fopen(path, "r");
	if (!f) {
		snprintf(out, max, "N/A");
		return;
	}
	if (fgets(out, max, f)) {
		char *nl = strchr(out, '\n');

		if (nl)
			*nl = '\0';
	} else {
		snprintf(out, max, "N/A");
	}
	fclose(f);
}

static bool get_sysfs_laser_enable(void)
{
	char buf[16] = {0};

	read_sysfs_string("laser_enable", buf, sizeof(buf));
	return buf[0] == '1';
}

static int set_sysfs_laser_enable(bool enable)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "%s/laser_enable", SYSFS_PON_DIR);
	f = fopen(path, "w");
	if (!f) {
		perror("Failed to open sysfs laser_enable");
		return -1;
	}
	fprintf(f, "%d\n", enable ? 1 : 0);
	fclose(f);
	printf("Optical TX Laser is now %s.\n", enable ? "ENABLED" : "DISABLED (Safety Hold)");
	return 0;
}

static void parse_identity(const uint8_t *a0, struct bosa_identity *id)
{
	int i;

	memcpy(id->vendor, a0 + 20, 16);
	id->vendor[16] = '\0';
	for (i = 15; i >= 0 && id->vendor[i] == ' '; i--)
		id->vendor[i] = '\0';

	memcpy(id->part_number, a0 + 40, 16);
	id->part_number[16] = '\0';
	for (i = 15; i >= 0 && id->part_number[i] == ' '; i--)
		id->part_number[i] = '\0';

	memcpy(id->revision, a0 + 56, 4);
	id->revision[4] = '\0';
	for (i = 3; i >= 0 && id->revision[i] == ' '; i--)
		id->revision[i] = '\0';

	memcpy(id->serial, a0 + 68, 16);
	id->serial[16] = '\0';
	for (i = 15; i >= 0 && id->serial[i] == ' '; i--)
		id->serial[i] = '\0';

	memcpy(id->date_code, a0 + 84, 8);
	id->date_code[8] = '\0';
}

static void parse_thresholds(const uint8_t *a2, struct bosa_thresholds *th)
{
	th->temp_hi_alarm = ((int16_t)((a2[0] << 8) | a2[1])) / 256.0;
	th->temp_lo_alarm = ((int16_t)((a2[2] << 8) | a2[3])) / 256.0;
	th->temp_hi_warn  = ((int16_t)((a2[4] << 8) | a2[5])) / 256.0;
	th->temp_lo_warn  = ((int16_t)((a2[6] << 8) | a2[7])) / 256.0;

	th->vcc_hi_alarm  = (((a2[8] << 8) | a2[9]) * 100) / 1000000.0;
	th->vcc_lo_alarm  = (((a2[10] << 8) | a2[11]) * 100) / 1000000.0;
	th->vcc_hi_warn   = (((a2[12] << 8) | a2[13]) * 100) / 1000000.0;
	th->vcc_lo_warn   = (((a2[14] << 8) | a2[15]) * 100) / 1000000.0;

	th->bias_hi_alarm = (((a2[16] << 8) | a2[17]) * 2) / 1000.0;
	th->bias_lo_alarm = (((a2[18] << 8) | a2[19]) * 2) / 1000.0;

	th->tx_hi_alarm   = (((a2[24] << 8) | a2[25]) * 100) / 1000.0;
	th->tx_lo_alarm   = (((a2[26] << 8) | a2[27]) * 100) / 1000.0;

	th->rx_hi_alarm   = (((a2[32] << 8) | a2[33]) * 100) / 1000.0;
	th->rx_lo_alarm   = (((a2[34] << 8) | a2[35]) * 100) / 1000.0;
}

static void parse_ddmi(const uint8_t *a2, struct bosa_ddmi *d)
{
	int16_t raw_temp = (int16_t)((a2[96] << 8) | a2[97]);
	uint16_t raw_vcc  = (a2[98] << 8) | a2[99];
	uint16_t raw_bias = (a2[100] << 8) | a2[101];
	uint16_t raw_tx   = (a2[102] << 8) | a2[103];
	uint16_t raw_rx   = (a2[104] << 8) | a2[105];

	d->temperature_c = raw_temp / 256.0;
	d->voltage_v     = (raw_vcc * 100) / 1000000.0;
	d->bias_ma       = (raw_bias * 2) / 1000.0;
	d->tx_power_uw   = (raw_tx * 100) / 1000.0;
	d->rx_power_uw   = (raw_rx * 100) / 1000.0;

	d->tx_power_dbm  = d->tx_power_uw > 0 ? 10.0 * log10(d->tx_power_uw / 1000.0) : -40.0;
	d->rx_power_dbm  = d->rx_power_uw > 0 ? 10.0 * log10(d->rx_power_uw / 1000.0) : -40.0;

	d->status_control = a2[110];
	d->alarm_flags[0] = a2[112];
	d->alarm_flags[1] = a2[113];
	d->warn_flags[0]  = a2[116];
	d->warn_flags[1]  = a2[117];

	d->laser_enabled = get_sysfs_laser_enable();
	read_sysfs_string("optical_state", d->optical_state, sizeof(d->optical_state));
}

static void print_table(const struct bosa_identity *id,
			const struct bosa_thresholds *th,
			const struct bosa_ddmi *d)
{
	printf("=================================================================\n");
	printf("      ZTE ZX279125 Optical Transceiver (BOSA DDMI) Telemetry\n");
	printf("=================================================================\n");
	printf("Transceiver Model  : %s %s (Rev %s)\n", id->vendor, id->part_number, id->revision);
	printf("Transceiver Serial : %s (Date: %s)\n", id->serial, id->date_code);
	printf("Optical MAC State  : %s\n", d->optical_state);
	printf("Laser Hardware Lock: %s\n", d->laser_enabled ? "ENABLED [TX ACTIVE]" : "DISABLED [SAFETY HOLD ACTIVE]");
	printf("-----------------------------------------------------------------\n");
	printf(" Parameter          Value            Thresholds [Alarm / Warn]\n");
	printf("-----------------------------------------------------------------\n");
	printf(" Temperature     :  %6.2f °C         [Lo: %.1f / Hi: %.1f °C]\n",
	       d->temperature_c, th->temp_lo_alarm, th->temp_hi_alarm);
	printf(" Supply Voltage  :  %6.3f V          [Lo: %.2f / Hi: %.2f V]\n",
	       d->voltage_v, th->vcc_lo_alarm, th->vcc_hi_alarm);
	printf(" Laser Bias Curr :  %6.3f mA         [Alarm Limit: %.1f mA]\n",
	       d->bias_ma, th->bias_hi_alarm);
	printf(" Optical TX Power:  %6.2f µW         (%.2f dBm)\n",
	       d->tx_power_uw, d->tx_power_dbm);
	printf(" Optical RX Power:  %6.2f µW         (%.2f dBm)\n",
	       d->rx_power_uw, d->rx_power_dbm);
	printf("-----------------------------------------------------------------\n");
	printf("Alarms & Status  : TX Soft Disable = %s | LOS = %s\n",
	       (d->status_control & (1 << 6)) ? "YES" : "NO",
	       d->rx_power_uw < 1.0 ? "YES (No Optical Signal Detected)" : "NO");
	printf("=================================================================\n");
}

static void print_json(const struct bosa_identity *id,
		       const struct bosa_thresholds *th,
		       const struct bosa_ddmi *d)
{
	printf("{\n");
	printf("  \"identity\": {\n");
	printf("    \"vendor\": \"%s\",\n", id->vendor);
	printf("    \"part_number\": \"%s\",\n", id->part_number);
	printf("    \"revision\": \"%s\",\n", id->revision);
	printf("    \"serial\": \"%s\",\n", id->serial);
	printf("    \"date_code\": \"%s\"\n", id->date_code);
	printf("  },\n");
	printf("  \"telemetry\": {\n");
	printf("    \"optical_state\": \"%s\",\n", d->optical_state);
	printf("    \"laser_enabled\": %s,\n", d->laser_enabled ? "true" : "false");
	printf("    \"temperature_c\": %.2f,\n", d->temperature_c);
	printf("    \"voltage_v\": %.3f,\n", d->voltage_v);
	printf("    \"bias_ma\": %.3f,\n", d->bias_ma);
	printf("    \"tx_power_uw\": %.2f,\n", d->tx_power_uw);
	printf("    \"tx_power_dbm\": %.2f,\n", d->tx_power_dbm);
	printf("    \"rx_power_uw\": %.2f,\n", d->rx_power_uw);
	printf("    \"rx_power_dbm\": %.2f,\n", d->rx_power_dbm);
	printf("    \"los\": %s\n", d->rx_power_uw < 1.0 ? "true" : "false");
	printf("  },\n");
	printf("  \"thresholds\": {\n");
	printf("    \"temp_hi_alarm\": %.1f,\n", th->temp_hi_alarm);
	printf("    \"temp_lo_alarm\": %.1f,\n", th->temp_lo_alarm);
	printf("    \"vcc_hi_alarm\": %.2f,\n", th->vcc_hi_alarm);
	printf("    \"vcc_lo_alarm\": %.2f,\n", th->vcc_lo_alarm);
	printf("    \"bias_hi_alarm\": %.1f\n", th->bias_hi_alarm);
	printf("  }\n");
	printf("}\n");
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS] [COMMAND]\n\n", prog);
	printf("Commands:\n");
	printf("  status            Display optical transceiver DDMI status (default)\n");
	printf("  laser on          Enable optical TX laser emission\n");
	printf("  laser off         Disable optical TX laser (safety hold)\n\n");
	printf("Options:\n");
	printf("  -j, --json        Output telemetry in JSON format\n");
	printf("  -w, --watch [SEC] Continuously monitor and refresh every SEC seconds (default: 2)\n");
	printf("  -h, --help        Show this help message\n");
}

int main(int argc, char **argv)
{
	bool json_mode = false;
	int watch_interval = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			return 0;
		}
		if (!strcmp(argv[i], "laser") && i + 1 < argc) {
			if (!strcmp(argv[i + 1], "on"))
				return set_sysfs_laser_enable(true);
			if (!strcmp(argv[i + 1], "off"))
				return set_sysfs_laser_enable(false);
		}
		if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--json"))
			json_mode = true;
		if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch")) {
			watch_interval = 2;
			if (i + 1 < argc && atoi(argv[i + 1]) > 0) {
				watch_interval = atoi(argv[i + 1]);
				i++;
			}
		}
	}

	do {
		int fd = open(I2C_DEV_PATH, O_RDWR);
		uint8_t a0[128] = {0};
		uint8_t a2[128] = {0};
		struct bosa_identity id = {0};
		struct bosa_thresholds th = {0};
		struct bosa_ddmi ddmi = {0};

		if (fd < 0) {
			perror("Failed to open I2C bus (" I2C_DEV_PATH ")");
			return 1;
		}

		if (read_i2c_block(fd, BOSA_I2C_ADDR_A0, 0, a0, sizeof(a0)) < 0) {
			perror("I2C read failed on A0h (0x50)");
			close(fd);
			return 1;
		}

		if (read_i2c_block(fd, BOSA_I2C_ADDR_A2, 0, a2, sizeof(a2)) < 0) {
			perror("I2C read failed on A2h (0x51)");
			close(fd);
			return 1;
		}
		close(fd);

		parse_identity(a0, &id);
		parse_thresholds(a2, &th);
		parse_ddmi(a2, &ddmi);

		if (watch_interval > 0)
			printf("\033[H\033[2J");

		if (json_mode)
			print_json(&id, &th, &ddmi);
		else
			print_table(&id, &th, &ddmi);

		if (watch_interval > 0)
			sleep(watch_interval);
	} while (watch_interval > 0);

	return 0;
}
