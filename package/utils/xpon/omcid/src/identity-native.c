// SPDX-License-Identifier: GPL-2.0-only
/* Normalization from net/xpon/omci/identity.c; no factory-data writes. */
#include "native.h"
static size_t omci_trim_input(const u8 **data, size_t len)
{
	while (len && isspace(**data)) {
		(*data)++;
		len--;
	}
	while (len && (data[0][len - 1] == '\0' ||
		       data[0][len - 1] == 0xff ||
		       isspace(data[0][len - 1])))
		len--;

	return len;
}

static int omci_hex_compact(const u8 *data, size_t len, u8 *output,
			    size_t output_len)
{
	size_t digits = 0;
	size_t i;
	int high = -1;

	if (len >= 4 && !strncasecmp((const char *)data, "hex:", 4)) {
		data += 4;
		len -= 4;
	} else if (len >= 2 && data[0] == '0' &&
		   (data[1] == 'x' || data[1] == 'X')) {
		data += 2;
		len -= 2;
	}

	memset(output, 0, output_len);
	for (i = 0; i < len; i++) {
		int value;

		if (data[i] == ':' || data[i] == '-' || data[i] == '.' ||
		    data[i] == '_' || isspace(data[i]))
			continue;
		value = hex_to_bin(data[i]);
		if (value < 0)
			return -EINVAL;
		if (high < 0) {
			high = value;
		} else {
			if (digits / 2 >= output_len)
				return -E2BIG;
			output[digits / 2] = (high << 4) | value;
			high = -1;
		}
		digits++;
	}

	if (high >= 0 || digits != output_len * 2)
		return -EINVAL;

	return 0;
}

static bool omci_all_hex(const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (hex_to_bin(data[i]) < 0)
			return false;
	return true;
}

static int omci_normalize_serial(const u8 *data, size_t len, u8 serial[8])
{
	const u8 *trimmed = data;
	u8 suffix[4];
	int ret;


	switch (len) {
	case 8:
		memcpy(serial, data, 8);
		return 0;
	case 16:
		if (omci_all_hex(trimmed, 16))
			return omci_hex_compact(trimmed, 16, serial, 8);
		break;
	}
	len = omci_trim_input(&trimmed, len);
	if (len == 12 && omci_all_hex(trimmed + 4, 8)) {
		ret = omci_hex_compact(trimmed + 4, 8, suffix, sizeof(suffix));
		if (ret)
			return ret;
		memcpy(serial, trimmed, 4);
		memcpy(serial + 4, suffix, sizeof(suffix));
		return 0;
	}

	return omci_hex_compact(trimmed, len, serial, 8);
}

static int omci_normalize_vendor(const u8 *data, size_t len, u8 vendor[4])
{
	const u8 *trimmed = data;

	if (len == 4) {
		memcpy(vendor, data, 4);
		return 0;
	}
	len = omci_trim_input(&trimmed, len);
	return omci_hex_compact(trimmed, len, vendor, 4);
}

static int omci_normalize_password(const u8 *data, size_t len,
				   u8 password[10])
{
	const u8 *trimmed = data;

	if (len == 10) {
		memcpy(password, data, 10);
		return 0;
	}
	len = omci_trim_input(&trimmed, len);
	if (!len)
		return -EINVAL;
	if (len == 20 || (len > 4 && !strncasecmp((const char *)trimmed, "hex:", 4)))
		return omci_hex_compact(trimmed, len, password, 10);
	if (len > 10)
		return -E2BIG;

	memset(password, 0, 10);
	memcpy(password, trimmed, len);
	return 0;
}

static int omci_normalize_string(const u8 *data, size_t len, u8 *output,
				 size_t output_len)
{
	const u8 *trimmed = data;

	len = omci_trim_input(&trimmed, len);
	if (!len || len > output_len)
		return -EINVAL;
	memset(output, 0, output_len);
	memcpy(output, trimmed, len);
	return 0;
}

/* LOID fields may be empty: an unset stock record reads back as zeros. */
static int omci_normalize_optional_string(const u8 *data, size_t len,
					  u8 *output, size_t output_len)
{
	const u8 *trimmed = data;

	if (!omci_trim_input(&trimmed, len)) {
		memset(output, 0, output_len);
		return 0;
	}
	return omci_normalize_string(data, len, output, output_len);
}

int omci_identity_normalize_config(u16 key, const void *value, size_t len,
				   u8 *output, size_t *output_len)
{
	size_t required;
	int ret;

	if (!value || !output || !output_len)
		return -EINVAL;
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		required = 8;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_serial(value, len, output);
		break;
	case OMCI_CONFIG_VENDOR_ID:
		required = 4;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_vendor(value, len, output);
		break;
	case OMCI_CONFIG_PASSWORD:
		required = 10;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_password(value, len, output);
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		required = OMCI_HARDWARE_VERSION_LEN;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_string(value, len, output, required);
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		required = OMCI_OLT_EQUIPMENT_ID_LEN;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_string(value, len, output, required);
		break;
	case OMCI_CONFIG_LOID:
		required = OMCI_LOID_LEN;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_optional_string(value, len, output,
						     required);
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		required = OMCI_LOID_PASSWORD_LEN;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_optional_string(value, len, output,
						     required);
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (ret)
		return ret;
	*output_len = required;
	return 0;
}
