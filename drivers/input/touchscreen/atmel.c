/* drivers/input/touchscreen/atmel.c - ATMEL Touch driver
 *
 * Copyright (C) 2009 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <mach/board.h>
#include <asm/mach-types.h>
#include <linux/atmel_qt602240.h>
#include <linux/jiffies.h>
#include <mach/msm_hsusb.h>
#if !defined(CONFIG_ARCH_MSM8X60)
#include <mach/htc_battery.h>
#else
#include <mach/cable_detect.h>
#endif
#include <linux/stat.h>
#include <linux/pl_sensor.h>

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
#include <linux/leds-pm8058.h>
#include <linux/ctype.h>
#endif

#define ATMEL_EN_SYSFS
#define ATMEL_I2C_RETRY_TIMES 10

/* config_setting */
#define NONE                                    0
#define CONNECTED                               1

/* anti-touch calibration */
#define RECALIB_NEED                            0
#define RECALIB_NG                              1
#define RECALIB_UNLOCK                          2
#define RECALIB_DONE                            3
#define ATCHCAL_DELAY                           200
#define SAFE_TIMEOUT                            500

struct atmel_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct workqueue_struct *atmel_wq;
	struct workqueue_struct *atmel_delayed_wq;
	struct work_struct check_delta_work;
	struct delayed_work unlock_work;
	uint8_t flag_htc_event;
	uint8_t report_both;
	int (*power) (int on);
	uint8_t unlock_attr;
	struct early_suspend early_suspend;
	struct info_id_t *id;
	struct object_t *object_table;
	uint8_t finger_count;
	uint16_t abs_x_min;
	uint16_t abs_x_max;
	uint16_t abs_y_min;
	uint16_t abs_y_max;
	uint8_t abs_pressure_min;
	uint8_t abs_pressure_max;
	uint8_t abs_width_min;
	uint8_t abs_width_max;
	uint8_t first_pressed;
	uint8_t valid_pressed_cnt;
	uint8_t cal_after_unlock;
	uint8_t debug_log_level;
	struct atmel_finger_data finger_data[10];
	uint8_t high_res_x_en;
	uint8_t high_res_y_en;
	uint8_t finger_type;
	uint8_t finger_support;
	uint16_t finger_pressed;
	uint8_t face_suppression;
	uint8_t grip_suppression;
	uint8_t noise_status[2];
	uint16_t *filter_level;
	uint8_t calibration_confirm;
	uint64_t timestamp;
	unsigned long valid_press_timeout;
	unsigned long safe_unlock_timeout;
	struct atmel_config_data config_setting[2];
	struct atmel_finger_data pre_finger_data[10];
	uint8_t repeat_flag;
	int8_t wlc_config[7];
	uint8_t wlc_freq[5];
	uint8_t wlc_status;
	int8_t noise_config[3];
	uint8_t call_tchthr[2];
	uint8_t locking_config[1];
	uint8_t status;
	uint8_t GCAF_sample;
	uint8_t *GCAF_level;
	uint8_t noisethr;
	uint8_t noisethr_config;
	uint8_t diag_command;
	uint8_t psensor_status;
	uint8_t *ATCH_EXT;
	int pre_data[11];
#ifdef ATMEL_EN_SYSFS
	struct device dev;
#endif
};

static struct atmel_ts_data *private_ts;

#ifdef CONFIG_HAS_EARLYSUSPEND
static void atmel_ts_early_suspend(struct early_suspend *h);
static void atmel_ts_late_resume(struct early_suspend *h);
#endif

static void restore_normal_threshold(struct atmel_ts_data *ts);
static void confirm_calibration(struct atmel_ts_data *ts, uint8_t recal, uint8_t reason);
static void multi_input_report(struct atmel_ts_data *ts);

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
/* Sweep to wake values are 
 * 0 = no sweep2wake
 * 1 = sweep2wake with no backlight
 * 2 = sweep2wake with backlight
 */
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_DISABLED
int s2w_switch = 0;
int s2w_temp = 0;
#elif defined(CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_ENABLED)
int s2w_switch = 1;
int s2w_temp = 1;
#elif defined(CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_ENABLED_WITH_BACKLIGHT)
int s2w_switch = 2;
int s2w_temp = 2;
#endif

bool scr_suspended = false, exec_count = true, s2w_switch_changed = false;
bool scr_on_touch = false, led_exec_count = false, barrier[2] = {false, false};
static struct input_dev * sweep2wake_pwrdev;
static struct led_classdev * sweep2wake_leddev;
int barrier1 = 0, barrier2 = 0, barrier3 = 0, barrier4 = 0;
static DEFINE_MUTEX(pwrlock);

typedef struct {
	int	x;
   	char	name[10];
} button;

static button buttons[] = {{140, "HOME"},
			{400, "MENU"},
			{645, "BACK"},
			{915, "SEARCH"}};
				
int s2w_startbutton = 0;
int s2w_endbutton = 0;

int sweep2wake_buttonset(const char * button_name) {
	int i = 0;	
	int future_button = -1;	
	char temp_button_name[2] = "";
	char temp_button_name_from_array[2] = "";

	strncpy(temp_button_name,button_name,1);
	temp_button_name[1] = '\0';

	for (i = 0; i < sizeof(buttons)/sizeof(button); i++)
	{
		strncpy(temp_button_name_from_array,buttons[i].name,1);
		temp_button_name_from_array[1] = '\0';

		if (strcmp(temp_button_name,temp_button_name_from_array) == 0)
			future_button = buttons[i].x;
		
		temp_button_name_from_array[0] = tolower(temp_button_name_from_array[0]);

		if (strcmp(temp_button_name,temp_button_name_from_array) == 0)
			future_button = buttons[i].x;
	}

	return future_button;
}

extern void sweep2wake_setdev(struct input_dev * input_device) {
	sweep2wake_pwrdev = input_device;
	return;
}
EXPORT_SYMBOL(sweep2wake_setdev);

extern void sweep2wake_setleddev(struct led_classdev * led_dev) {
	sweep2wake_leddev = led_dev;
	return;
}
EXPORT_SYMBOL(sweep2wake_setleddev);

static void sweep2wake_presspwr(struct work_struct * sweep2wake_presspwr_work) {
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(100);
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(100);
	mutex_unlock(&pwrlock);
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

void sweep2wake_pwrtrigger(void) {
	if (mutex_trylock(&pwrlock)) {
		schedule_work(&sweep2wake_presspwr_work);
	}
	return;
}
#endif

static int i2c_atmel_read(struct i2c_client *client, uint16_t address, uint8_t *data, uint8_t length)
{
	int retry;
	uint8_t addr[2];

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 2,
			.buf = addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		}
	};
	addr[0] = address & 0xFF;
	addr[1] = (address >> 8) & 0xFF;

	for (retry = 0; retry < ATMEL_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 2) == 2)
			break;
		msleep(10);
	}
	if (retry == ATMEL_I2C_RETRY_TIMES) {
		printk(KERN_ERR "[TP]TOUCH_ERR: i2c_read_block retry over %d\n",
			ATMEL_I2C_RETRY_TIMES);
		return -EIO;
	}
	return 0;

}

static int i2c_atmel_write(struct i2c_client *client, uint16_t address, uint8_t *data, uint8_t length)
{
	int retry, loop_i;
	uint8_t buf[length + 2];

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = length + 2,
			.buf = buf,
		}
	};

	buf[0] = address & 0xFF;
	buf[1] = (address >> 8) & 0xFF;

	for (loop_i = 0; loop_i < length; loop_i++)
		buf[loop_i + 2] = data[loop_i];

	for (retry = 0; retry < ATMEL_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1)
			break;
		msleep(10);
	}

	if (retry == ATMEL_I2C_RETRY_TIMES) {
		printk(KERN_ERR "[TP]TOUCH_ERR: i2c_write_block retry over %d\n",
			ATMEL_I2C_RETRY_TIMES);
		return -EIO;
	}
	return 0;

}

static int i2c_atmel_write_byte_data(struct i2c_client *client, uint16_t address, uint8_t value)
{
	i2c_atmel_write(client, address, &value, 1);
	return 0;
}

static uint16_t get_object_address(struct atmel_ts_data *ts, uint8_t object_type)
{
	uint8_t loop_i;
	for (loop_i = 0; loop_i < ts->id->num_declared_objects; loop_i++) {
		if (ts->object_table[loop_i].object_type == object_type)
			return ts->object_table[loop_i].i2c_address;
	}
	return 0;
}
static uint8_t get_object_size(struct atmel_ts_data *ts, uint8_t object_type)
{
	uint8_t loop_i;
	for (loop_i = 0; loop_i < ts->id->num_declared_objects; loop_i++) {
		if (ts->object_table[loop_i].object_type == object_type)
			return ts->object_table[loop_i].size;
	}
	return 0;
}

static uint8_t get_rid(struct atmel_ts_data *ts, uint8_t object_type)
{
	uint8_t loop_i;
	for (loop_i = 0; loop_i < ts->id->num_declared_objects; loop_i++) {
		if (ts->object_table[loop_i].object_type == object_type)
			return ts->object_table[loop_i].report_ids;
	}
	return 0;
}

#ifdef ATMEL_EN_SYSFS
static ssize_t atmel_gpio_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	struct atmel_ts_data *ts_data;
	struct atmel_i2c_platform_data *pdata;

	ts_data = private_ts;
	pdata = ts_data->client->dev.platform_data;

	ret = gpio_get_value(pdata->gpio_irq);
	printk(KERN_DEBUG "[TP]GPIO_TP_INT_N=%d\n", pdata->gpio_irq);
	sprintf(buf, "GPIO_TP_INT_N=%d\n", ret);
	ret = strlen(buf) + 1;
	return ret;
}
static DEVICE_ATTR(gpio, S_IRUGO, atmel_gpio_show, NULL);

static ssize_t atmel_reset(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int i;
	struct atmel_ts_data *ts;
	struct atmel_i2c_platform_data *pdata;

	ts = private_ts;
	pdata = ts->client->dev.platform_data;

	if (sscanf(buf, "%d", &i) == 1 && i < 2) {
		if (pdata->gpio_rst) {
			gpio_set_value(pdata->gpio_rst, 0);
			msleep(5);
			gpio_set_value(pdata->gpio_rst, 1);
			msleep(40);

			pr_info("[TP] Reset Atmel Chip\n");
		} else
			pr_info("[TP] Reset pin NOT ASSIGN\n");
	} else
		pr_info("[TP] Parameter Error\n");

	return count;
}
static DEVICE_ATTR(reset, (S_IWUSR|S_IRUGO), NULL, atmel_reset);

static ssize_t atmel_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	struct atmel_ts_data *ts_data;
	ts_data = private_ts;
	sprintf(buf, "%s_x%4.4X_x%4.4X\n", "ATMEL",
		ts_data->id->family_id, ts_data->id->version);
	ret = strlen(buf) + 1;
	return ret;
}

static DEVICE_ATTR(vendor, S_IRUGO, atmel_vendor_show, NULL);

static uint16_t atmel_reg_addr;

static ssize_t atmel_register_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	uint8_t ptr[1] = { 0 };
	struct atmel_ts_data *ts_data;
	ts_data = private_ts;
	if (i2c_atmel_read(ts_data->client, atmel_reg_addr, ptr, 1) < 0) {
		printk(KERN_WARNING "[TP]%s: read fail\n", __func__);
		return ret;
	}
	ret += sprintf(buf, "addr: %d, data: %d\n", atmel_reg_addr, ptr[0]);
	return ret;
}

static ssize_t atmel_register_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	struct atmel_ts_data *ts_data;
	char buf_tmp[4], buf_zero[200];
	uint8_t write_da;

	ts_data = private_ts;
	memset(buf_tmp, 0x0, sizeof(buf_tmp));
	if ((buf[0] == 'r' || buf[0] == 'w') && buf[1] == ':' &&
		(buf[5] == ':' || buf[5] == '\n')) {
		memcpy(buf_tmp, buf + 2, 3);
		atmel_reg_addr = simple_strtol(buf_tmp, NULL, 10);
		printk(KERN_DEBUG "[TP]read addr: 0x%X\n", atmel_reg_addr);
		if (!atmel_reg_addr) {
			printk(KERN_WARNING "[TP]%s: string to number fail\n",
								__func__);
			return count;
		}
		printk(KERN_DEBUG "[TP]%s: set atmel_reg_addr is: %d\n",
						__func__, atmel_reg_addr);
		if (buf[0] == 'w' && buf[5] == ':' && buf[9] == '\n') {
			memcpy(buf_tmp, buf + 6, 3);
			write_da = simple_strtol(buf_tmp, NULL, 10);
			printk(KERN_DEBUG "[TP]write addr: 0x%X, data: 0x%X\n",
						atmel_reg_addr, write_da);
			ret = i2c_atmel_write_byte_data(ts_data->client,
						atmel_reg_addr, write_da);
			if (ret < 0) {
				printk(KERN_WARNING "[TP]%s: write fail(%d)\n",
								__func__, ret);
			}
		}
	}
	if ((buf[0] == '0') && (buf[1] == ':') && (buf[5] == ':')) {
		memcpy(buf_tmp, buf + 2, 3);
		atmel_reg_addr = simple_strtol(buf_tmp, NULL, 10);
		memcpy(buf_tmp, buf + 6, 3);
		memset(buf_zero, 0x0, sizeof(buf_zero));
		ret = i2c_atmel_write(ts_data->client, atmel_reg_addr,
			buf_zero, simple_strtol(buf_tmp, NULL, 10) - atmel_reg_addr + 1);
		if (buf[9] == 'r') {
			i2c_atmel_write_byte_data(ts_data->client,
				get_object_address(ts_data, GEN_COMMANDPROCESSOR_T6) +
				T6_CFG_BACKUPNV, 0x55);
			i2c_atmel_write_byte_data(ts_data->client,
				get_object_address(ts_data, GEN_COMMANDPROCESSOR_T6) +
				T6_CFG_RESET, 0x11);
		}
	}

	return count;
}

static DEVICE_ATTR(register, (S_IWUSR|S_IRUGO),
	atmel_register_show, atmel_register_store);

static ssize_t atmel_regdump_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int count = 0, ret_t = 0;
	struct atmel_ts_data *ts_data;
	uint16_t loop_i, startAddr, endAddr;
	uint8_t numObj;
	uint8_t ptr[1] = { 0 };

	ts_data = private_ts;
	if (!ts_data->id->num_declared_objects)
		return count;
	numObj = ts_data->id->num_declared_objects - 1;
	startAddr = get_object_address(ts_data, GEN_POWERCONFIG_T7);
	endAddr = get_object_address(ts_data, SPT_CTECONFIG_T28);
	endAddr += get_object_size(ts_data, SPT_CTECONFIG_T28) - 1;
	if (ts_data->id->version >= 0x14) {
		for (loop_i = startAddr; loop_i <= endAddr; loop_i++) {
			ret_t = i2c_atmel_read(ts_data->client, loop_i, ptr, 1);
			if (ret_t < 0) {
				printk(KERN_WARNING "[TP]dump fail, addr: %d\n",
								loop_i);
			}
			count += sprintf(buf + count, "addr[%3d]: %3d, ",
								loop_i , *ptr);
			if (((loop_i - startAddr) % 4) == 3)
				count += sprintf(buf + count, "\n");
		}
		count += sprintf(buf + count, "\n");
	}
	return count;
}

#ifdef DEBUG
static void regdump_to_kernel(void)
{
	int count = 0, ret_t = 0;
	struct atmel_ts_data *ts_data;
	char buf[80];
	uint16_t loop_i, startAddr, endAddr;
	uint8_t numObj;
	uint8_t ptr[1] = { 0 };

	ts_data = private_ts;
	if (!ts_data->id->num_declared_objects)
		return;
	numObj = ts_data->id->num_declared_objects - 1;
	startAddr = get_object_address(ts_data, GEN_POWERCONFIG_T7);
	endAddr = get_object_address(ts_data, SPT_CTECONFIG_T28);
	endAddr += get_object_size(ts_data, SPT_CTECONFIG_T28) - 1;
	if (ts_data->id->version >= 0x14) {
		for (loop_i = startAddr; loop_i <= endAddr; loop_i++) {
			ret_t = i2c_atmel_read(ts_data->client, loop_i, ptr, 1);
			if (ret_t < 0) {
				printk(KERN_WARNING "[TP]dump fail, addr: %d\n",
								loop_i);
			}
			count += sprintf(buf + count, "addr[%3d]: %3d, ",
								loop_i , *ptr);
			if (((loop_i - startAddr) % 4) == 3) {
				printk(KERN_INFO "%s\n", buf);
				count = 0;
			}
		}
		printk(KERN_INFO "%s\n", buf);
	}
	return;
}
#else
static void regdump_to_kernel(void) { }
#endif


static ssize_t atmel_regdump_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct atmel_ts_data *ts_data;
	ts_data = private_ts;
	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		ts_data->debug_log_level = buf[0] - '0';

	return count;

}

static DEVICE_ATTR(regdump, (S_IWUSR|S_IRUGO),
	atmel_regdump_show, atmel_regdump_dump);

static ssize_t atmel_debug_level_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct atmel_ts_data *ts_data;
	size_t count = 0;
	ts_data = private_ts;

	count += sprintf(buf, "%d\n", ts_data->debug_log_level);

	return count;
}

static ssize_t atmel_debug_level_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct atmel_ts_data *ts_data;
	ts_data = private_ts;
	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		ts_data->debug_log_level = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(debug_level, (S_IWUSR|S_IRUGO),
	atmel_debug_level_show, atmel_debug_level_dump);

static ssize_t atmel_diag_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct atmel_ts_data *ts_data;
	size_t count = 0;
	uint8_t data[T37_PAGE_SIZE];
	uint8_t loop_i, loop_j;
	int16_t rawdata;
	int x, y;
	ts_data = private_ts;
	memset(data, 0x0, sizeof(data));

	if (ts_data->diag_command != T6_CFG_DIAG_CMD_DELTAS &&
		ts_data->diag_command != T6_CFG_DIAG_CMD_REF)
		return count;

	i2c_atmel_write_byte_data(ts_data->client,
		get_object_address(ts_data, GEN_COMMANDPROCESSOR_T6) + T6_CFG_DIAG,
		ts_data->diag_command);

	x = T28_CFG_MODE0_X + ts_data->config_setting[NONE].config_T28[T28_CFG_MODE];
	y = T28_CFG_MODE0_Y - ts_data->config_setting[NONE].config_T28[T28_CFG_MODE];
	count += sprintf(buf, "Channel: %d * %d\n", x, y);

	for (loop_i = 0; loop_i < 4; loop_i++) {
		for (loop_j = 0;
			!(data[T37_MODE] == ts_data->diag_command && data[T37_PAGE] == loop_i) && loop_j < 10; loop_j++) {
			msleep(5);
			i2c_atmel_read(ts_data->client,
				get_object_address(ts_data, DIAGNOSTIC_T37), data, 2);
		}
		if (loop_j == 10)
			printk(KERN_ERR "[TP]TOUCH_ERR:%s: Diag data not ready\n", __func__);

		i2c_atmel_read(ts_data->client,
			get_object_address(ts_data, DIAGNOSTIC_T37) +
			T37_DATA, data, T37_PAGE_SIZE);
		for (loop_j = 0; loop_j < T37_PAGE_SIZE - 1; loop_j += 2) {
			if ((loop_i * 64 + loop_j / 2) >= (x * y)) {
				count += sprintf(buf + count, "\n");
				return count;
			} else {
				rawdata = data[loop_j+1] << 8 | data[loop_j];
				count += sprintf(buf + count, "%6d", rawdata);
				if (((loop_i * 64 + loop_j / 2) % y) == (y - 1))
					count += sprintf(buf + count, "\n");
			}
		}
		i2c_atmel_write_byte_data(ts_data->client,
			get_object_address(ts_data, GEN_COMMANDPROCESSOR_T6) +
			T6_CFG_DIAG, T6_CFG_DIAG_CMD_PAGEUP);

	}

	return count;
}

static ssize_t atmel_diag_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct atmel_ts_data *ts_data;
	ts_data = private_ts;
	if (buf[0] == '1')
		ts_data->diag_command = T6_CFG_DIAG_CMD_DELTAS;
	if (buf[0] == '2')
		ts_data->diag_command = T6_CFG_DIAG_CMD_REF;

	return count;
}

static DEVICE_ATTR(diag, (S_IWUSR|S_IRUGO),
	atmel_diag_show, atmel_diag_dump);

static ssize_t atmel_unlock_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct atmel_ts_data *ts_data;
	int unlock = -1;
	ts_data = private_ts;

	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		unlock = buf[0] - '0';

	printk(KERN_INFO "[TP]unlock change to %d\n", unlock);

	if ((unlock == 2 || unlock == 3) && ts_data->id->version >= 0x20 &&
		(ts_data->first_pressed || ts_data->finger_count) &&
		ts_data->pre_data[0] < RECALIB_UNLOCK &&
		ts_data->unlock_attr) {

		ts_data->valid_press_timeout = jiffies + msecs_to_jiffies(15);
		if (ts_data->finger_count == 0)
			ts_data->valid_pressed_cnt = 1;
		else /* unlock direction: left to right */
			ts_data->valid_pressed_cnt = 0;

		ts_data->cal_after_unlock = 0;
		ts_data->pre_data[0] = RECALIB_UNLOCK;
		restore_normal_threshold(ts_data);
		i2c_atmel_write_byte_data(ts_data->client,
			get_object_address(ts_data, GEN_ACQUISITIONCONFIG_T8) +
			T8_CFG_ATCHCALST, 0x01);
		if (time_after(jiffies, ts_data->safe_unlock_timeout))
			queue_delayed_work(ts_data->atmel_delayed_wq, &ts_data->unlock_work,
				msecs_to_jiffies(ATCHCAL_DELAY));
		else
			printk(KERN_INFO "[TP]unsafe unlock, give up delta check\n");
	}

	return count;
}

static DEVICE_ATTR(unlock, (S_IWUSR|S_IRUGO),
	NULL, atmel_unlock_store);

static ssize_t atmel_htc_event_read(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct atmel_ts_data *ts_data;
	size_t count = 0;
	ts_data = private_ts;

	count += sprintf(buf, "%d\n", ts_data->flag_htc_event);

	return count;
}

static ssize_t atmel_htc_event_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct atmel_ts_data *ts_data;
	ts_data = private_ts;
	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		ts_data->flag_htc_event = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(htc_event, (S_IWUSR|S_IRUGO), atmel_htc_event_read, atmel_htc_event_write);

static ssize_t atmel_info_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	count += sprintf(buf, "Both report & MTSize & new filter & INT thread \n");
	count += sprintf(buf + count, "2012.02.29.\n");
	return count;
}

static DEVICE_ATTR(info, S_IRUGO, atmel_info_show, NULL);

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
static ssize_t atmel_sweep2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	if (s2w_switch == s2w_temp )
		count += sprintf(buf, "%d\n", s2w_switch);
	else
		count += sprintf(buf, "%d->%d\n", s2w_switch, s2w_temp);

	return count;
}

static ssize_t atmel_sweep2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '2' && buf[1] == '\n')
		if (s2w_switch != buf[0] - '0') {
			s2w_temp = buf[0] - '0';
			if (scr_suspended == false)
				s2w_switch = s2w_temp;
			else 
				s2w_switch_changed = true;
		}

	if (s2w_temp == 0) 
		printk(KERN_INFO "[sweep2wake]: Disabled.\n");
	else if (s2w_temp == 1)
		printk(KERN_INFO "[sweep2wake]: Enabled without Backlight.\n");
	else if (s2w_temp == 2)
		printk(KERN_INFO "[sweep2wake]: Enabled with Backlight.\n");

	return count;
}

static DEVICE_ATTR(sweep2wake, (S_IWUSR|S_IRUGO),
	atmel_sweep2wake_show, atmel_sweep2wake_dump);

static ssize_t atmel_sweep2wake_availablebuttons_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i = 0;
	size_t count = 0;

	for (i = 0; i < sizeof(buttons)/sizeof(button); i++)
	{
		count += sprintf(&buf[count], "%s ",buttons[i].name);
	}

	count += sprintf(&buf[count], "\n");

	return count;
}
static DEVICE_ATTR(sweep2wake_availablebuttons, S_IRUGO,
	atmel_sweep2wake_availablebuttons_show, NULL);

static ssize_t atmel_sweep2wake_startbutton_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i = 0;
	size_t count = 0;
	bool found = false;

	for (i = 0; i < sizeof(buttons)/sizeof(button); i++)
	{
		if (s2w_startbutton == buttons[i].x) {				
			count += sprintf(buf, "%s\n",buttons[i].name);
			found = true;
		}
	}
	
	if (!found) 
		count += sprintf(buf, "%s\n","UNKNOWN");

	return count;
}

static ssize_t atmel_sweep2wake_startbutton_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int s2w_tempbutton = 0;

	s2w_tempbutton = sweep2wake_buttonset(buf);
	if (s2w_tempbutton == -1)
		return -EINVAL;;

	if ( s2w_tempbutton == s2w_endbutton )
		return count;

	if ( s2w_tempbutton > s2w_endbutton ) {
		s2w_startbutton = s2w_endbutton;	
		s2w_endbutton = s2w_tempbutton;
	} else 
		s2w_startbutton = s2w_tempbutton;

	barrier1 = s2w_startbutton - 100; //0;
	barrier2 = ((s2w_endbutton - s2w_startbutton) / 4) + s2w_startbutton; //333;
	barrier3 = (((s2w_endbutton - s2w_startbutton) / 4) * 3) + s2w_startbutton; //667;
	barrier4 = s2w_endbutton + 100; //1000;

	return count;
}

static DEVICE_ATTR(sweep2wake_startbutton, (S_IWUSR|S_IRUGO),
	atmel_sweep2wake_startbutton_show, atmel_sweep2wake_startbutton_dump);

static ssize_t atmel_sweep2wake_endbutton_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i = 0;
	size_t count = 0;
	bool found = false;

	for (i = 0; i < sizeof(buttons)/sizeof(button); i++)
	{
		if (s2w_endbutton == buttons[i].x) {				
			count += sprintf(buf, "%s\n",buttons[i].name);
			found = true;
		}
	}
	
	if (!found) 
		count += sprintf(buf, "%s\n","UNKNOWN");

	return count;
}

static ssize_t atmel_sweep2wake_endbutton_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int s2w_tempbutton = 0;

	s2w_tempbutton = sweep2wake_buttonset(buf);
	if (s2w_tempbutton == -1)
		return -EINVAL;;

	if ( s2w_tempbutton == s2w_startbutton )
		return count;

	if ( s2w_tempbutton < s2w_startbutton ) {
		s2w_endbutton = s2w_startbutton;	
		s2w_startbutton = s2w_tempbutton;
	} else 
		s2w_endbutton = s2w_tempbutton;

	barrier1 = s2w_startbutton - 100; //0;
	barrier2 = ((s2w_endbutton - s2w_startbutton) / 4) + s2w_startbutton; //333;
	barrier3 = (((s2w_endbutton - s2w_startbutton) / 4) * 3) + s2w_startbutton; //667;
	barrier4 = s2w_endbutton + 100; //1000;

	return count;
}

static DEVICE_ATTR(sweep2wake_endbutton, (S_IWUSR|S_IRUGO),
	atmel_sweep2wake_endbutton_show, atmel_sweep2wake_endbutton_dump);
#endif


static struct kobject *android_touch_kobj;

static int atmel_touch_sysfs_init(void)
{
	int ret;
	android_touch_kobj = kobject_create_and_add("android_touch", NULL);
	if (android_touch_kobj == NULL) {
		printk(KERN_ERR "[TP]TOUCH_ERR: subsystem_register failed\n");
		ret = -ENOMEM;
		return ret;
	}
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake_startbutton.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake_endbutton.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake_availablebuttons.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}
#endif
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_gpio.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file gpio failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_vendor.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file vendor failed\n");
		return ret;
	}
	atmel_reg_addr = 0;
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_register.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file register failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_regdump.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file regdump failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_debug_level.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file debug_level failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_diag.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file diag failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_unlock.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file unlock failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_htc_event.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file htc_event failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_info.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file info failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_reset.attr);
	if (ret) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create_file RESET failed\n");
		return ret;
	}
	return 0;
}

static void atmel_touch_sysfs_deinit(void)
{
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake_startbutton.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake_endbutton.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake_availablebuttons.attr);
#endif
	sysfs_remove_file(android_touch_kobj, &dev_attr_info.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_htc_event.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_unlock.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_diag.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_debug_level.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_regdump.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_register.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_vendor.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_gpio.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_reset.attr);
	kobject_del(android_touch_kobj);
}

#endif
static int check_delta(struct atmel_ts_data*ts)
{
	int8_t data[T37_DATA + T37_PAGE_SIZE];
	uint8_t loop_i;
	int16_t rawdata, count = 0;

	memset(data, 0xFF, sizeof(data));
	i2c_atmel_write_byte_data(ts->client,
		get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
		T6_CFG_DIAG, T6_CFG_DIAG_CMD_DELTAS);

	for (loop_i = 0;
		!(data[T37_MODE] == T6_CFG_DIAG_CMD_DELTAS && data[T37_PAGE] == T37_PAGE_NUM0) && loop_i < 10; loop_i++) {
		msleep(5);
		i2c_atmel_read(ts->client,
			get_object_address(ts, DIAGNOSTIC_T37), data, 2);
	}
	if (loop_i == 10)
		printk(KERN_ERR "[TP]TOUCH_ERR:%s: Diag data not ready\n", __func__);

	i2c_atmel_read(ts->client,
		get_object_address(ts, DIAGNOSTIC_T37),
		data, T37_DATA + T37_PAGE_SIZE);
	if (data[T37_MODE] == T6_CFG_DIAG_CMD_DELTAS &&
		data[T37_PAGE] == T37_PAGE_NUM0) {
		for (loop_i = T37_DATA;
			loop_i < (T37_DATA + T37_PAGE_SIZE - 1); loop_i += 2) {
			rawdata = data[loop_i+1] << 8 | data[loop_i];
			if (abs(rawdata) > 50)
				count++;
		}
		if (count > 32)
			return 1;
	}
	return 0;
}

static int check_delta_full(struct atmel_ts_data *ts,
		uint8_t delta, uint8_t percent, uint8_t print_log)
{
	int8_t data[T37_DATA + T37_PAGE_SIZE];
	uint8_t loop_i, loop_j;
	uint8_t cnt, pos_cnt, neg_cnt, node_thr_cnt;
	uint8_t x, y;
	int16_t rawdata;
	int check_result = 0;

	cnt = pos_cnt = neg_cnt = 0;
	i2c_atmel_write_byte_data(ts->client,
		get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
		T6_CFG_DIAG, T6_CFG_DIAG_CMD_DELTAS);

	x = T28_CFG_MODE0_X + ts->config_setting[NONE].config_T28[T28_CFG_MODE];
	y = T28_CFG_MODE0_Y - ts->config_setting[NONE].config_T28[T28_CFG_MODE];
	node_thr_cnt = (x * y) * percent / 100;

	for (loop_i = 0; loop_i < 4; loop_i++) {
		memset(data, 0xFF, sizeof(data));
		for (loop_j = 0;
			!(data[T37_MODE] == T6_CFG_DIAG_CMD_DELTAS && data[T37_PAGE] == loop_i) && loop_j < 10; loop_j++) {
			msleep(5);
			i2c_atmel_read(ts->client,
				get_object_address(ts, DIAGNOSTIC_T37), data, 2);
		}
		if (loop_j == 10)
			printk(KERN_ERR "[TP]TOUCH_ERR:%s: Diag data not ready\n", __func__);

		i2c_atmel_read(ts->client,
			get_object_address(ts, DIAGNOSTIC_T37),
			data, T37_DATA + T37_PAGE_SIZE);
		for (loop_j = T37_DATA;
			loop_j < (T37_DATA + T37_PAGE_SIZE - 1); loop_j += 2) {
			rawdata = data[loop_j+1] << 8 | data[loop_j];
			cnt++;
			if (rawdata > delta)
				pos_cnt++;
			else if (rawdata < -(delta))
				neg_cnt++;

			if (cnt >= x * y)
				break;
		}
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
			T6_CFG_DIAG, T6_CFG_DIAG_CMD_PAGEUP);
	}

	check_result = (pos_cnt + neg_cnt) * 100 / (x * y);
	printk(KERN_INFO "[TP]DeltaCheck=%d (D=%d,P=%d) \n", check_result, delta, percent);

	if (pos_cnt + neg_cnt > node_thr_cnt) {
		if (print_log)
			printk(KERN_INFO "[TP]channels C=%d P=%d N=%d T=%d\n",
				cnt, pos_cnt, neg_cnt, node_thr_cnt);
		return 1;
	}

	return 0;
}

static void check_calibration(struct atmel_ts_data*ts)
{
	uint8_t data[T37_DATA + T37_TCH_FLAG_SIZE];
	uint8_t loop_i, loop_j, x_limit = 0, check_mask, tch_ch = 0, atch_ch = 0;

	memset(data, 0xFF, sizeof(data));
	i2c_atmel_write_byte_data(ts->client,
		get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
		T6_CFG_DIAG, T6_CFG_DIAG_CMD_TCH);

	for (loop_i = 0;
		!(data[T37_MODE] == T6_CFG_DIAG_CMD_TCH && data[T37_PAGE] == T37_PAGE_NUM0) && loop_i < 10; loop_i++) {
		msleep(5);
		i2c_atmel_read(ts->client,
			get_object_address(ts, DIAGNOSTIC_T37), data, 2);
	}

	if (loop_i == 10)
		printk(KERN_ERR "[TP]TOUCH_ERR:%s: Diag data not ready\n", __func__);

	i2c_atmel_read(ts->client, get_object_address(ts, DIAGNOSTIC_T37), data,
		T37_DATA + T37_TCH_FLAG_SIZE);
	if (data[T37_MODE] == T6_CFG_DIAG_CMD_TCH &&
		data[T37_PAGE] == T37_PAGE_NUM0) {
		x_limit = T28_CFG_MODE0_X + ts->config_setting[NONE].config_T28[T28_CFG_MODE];
		x_limit = x_limit << 1;
		if (x_limit <= 40) {
			for (loop_i = 0; loop_i < x_limit; loop_i += 2) {
				for (loop_j = 0; loop_j < BITS_PER_BYTE; loop_j++) {
					check_mask = BIT_MASK(loop_j);
					if (data[T37_DATA + T37_TCH_FLAG_IDX + loop_i] &
						check_mask)
						tch_ch++;
					if (data[T37_DATA + T37_TCH_FLAG_IDX + loop_i + 1] &
						check_mask)
						tch_ch++;
					if (data[T37_DATA + T37_ATCH_FLAG_IDX + loop_i] &
						check_mask)
						atch_ch++;
					if (data[T37_DATA + T37_ATCH_FLAG_IDX + loop_i + 1] &
						check_mask)
						atch_ch++;
				}
			}
		}
	}
	i2c_atmel_write_byte_data(ts->client,
		get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
		T6_CFG_DIAG, T6_CFG_DIAG_CMD_PAGEUP);

	if (tch_ch && (atch_ch == 0)) {
		if (jiffies > (ts->timestamp + HZ/2) && (ts->calibration_confirm == 1)) {
			ts->calibration_confirm = 2;
			printk(KERN_INFO "%s: calibration confirm\n", __func__);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) + T8_CFG_ATCHCALST,
				ts->config_setting[ts->status].config_T8[T8_CFG_ATCHCALST]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
				T8_CFG_ATCHCALSTHR,
				ts->config_setting[ts->status].config_T8[T8_CFG_ATCHCALSTHR]);
		}
		if (ts->calibration_confirm < 2)
			ts->calibration_confirm = 1;
		ts->timestamp = jiffies;
	} else if ((tch_ch - 25) <= atch_ch && (tch_ch || atch_ch)) {
		ts->calibration_confirm = 0;
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
			T6_CFG_CALIBRATE, 0x55);
	}
}

static void restore_normal_threshold(struct atmel_ts_data *ts)
{
	if (ts->call_tchthr[0]) {
		if (ts->wlc_status && ts->wlc_config[0])
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_TCHTHR,
				ts->wlc_config[WLC_TCHTHR]);
		else if (ts->config_setting[ts->status].config[0])
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_TCHTHR,
				ts->config_setting[ts->status].config[CB_TCHTHR]);
		else if (ts->config_setting[ts->status].config_T9 != NULL)
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_TCHTHR,
				ts->config_setting[ts->status].config_T9[T9_CFG_TCHTHR]);
		else
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_TCHTHR,
				ts->config_setting[NONE].config_T9[T9_CFG_TCHTHR]);

		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
			T8_CFG_ATCHCALSTHR,
			ts->config_setting[ts->status].config_T8[T8_CFG_ATCHCALSTHR]);
	}
	if (ts->locking_config[0]) {
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
			T9_CFG_MRGTHR,
			ts->config_setting[NONE].config_T9[T9_CFG_MRGTHR]);
	}
}

static void confirm_calibration(struct atmel_ts_data *ts,
		uint8_t recal, uint8_t reason)
{
	uint8_t ATCH_NOR[4] = {0, 1, 0, 0};

	i2c_atmel_write(ts->client,
		get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
		T8_CFG_ATCHCALST, ATCH_NOR, 4);
	ts->pre_data[0] = RECALIB_DONE;
	if (recal)
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
			T6_CFG_CALIBRATE, 0x55);
	printk(KERN_INFO "[TP]calibration confirm %sby %s\n",
		recal ? "with recal " : "",
		(reason == 0) ? "position" :
		(reason == 1) ? "clicks" :
		(reason == 2) ? "delta" :
		(reason == 3) ? "suspend" : "unknown");
}

static void msg_process_finger_data(struct atmel_ts_data *ts,
				struct atmel_finger_data *fdata, uint8_t *data, uint8_t idx)
{
	if (!ts->high_res_x_en)
		fdata->x = data[T9_MSG_XPOSMSB] << 2 | data[T9_MSG_XYPOSLSB] >> 6;
	else
		fdata->x = data[T9_MSG_XPOSMSB] << 4 | data[T9_MSG_XYPOSLSB] >> 4;
	if (!ts->high_res_y_en)
		fdata->y = data[T9_MSG_YPOSMSB] << 2 | (data[T9_MSG_XYPOSLSB] & 0x0C) >> 2;
	else
		fdata->y = data[T9_MSG_YPOSMSB] << 4 | (data[T9_MSG_XYPOSLSB] & 0x0F);
	fdata->w = data[T9_MSG_TCHAREA];
	fdata->z = data[T9_MSG_TCHAMPLITUDE];
	ts->repeat_flag = 0;
	if (ts->finger_count == 1) {
		if ((ts->pre_finger_data[idx].x == fdata->x) && (ts->pre_finger_data[idx].y == fdata->y)
			&& (ts->pre_finger_data[idx].w == fdata->w) && (ts->pre_finger_data[idx].z == fdata->z))
			ts->repeat_flag = 1;
	}
	ts->pre_finger_data[idx].x = fdata->x;
	ts->pre_finger_data[idx].y = fdata->y;
	ts->pre_finger_data[idx].w = fdata->w;
	ts->pre_finger_data[idx].z = fdata->z;
}

static void msg_process_multitouch(struct atmel_ts_data *ts, uint8_t *data, uint8_t idx)
{
	if (ts->calibration_confirm < 2 && ts->id->version == 0x16)
		check_calibration(ts);

	msg_process_finger_data(ts, &ts->finger_data[idx], data, idx);
	if (data[T9_MSG_STATUS] & T9_MSG_STATUS_RELEASE) {
		if (ts->finger_pressed & BIT(idx)) {
			if (data[T9_MSG_STATUS] & T9_MSG_STATUS_MOVE) {
				if (ts->repeat_flag == 0) {
					multi_input_report(ts);
					input_sync(ts->input_dev);
				}
			}
		}
		if (ts->grip_suppression & BIT(idx))
			ts->grip_suppression &= ~BIT(idx);
		if (ts->finger_pressed & BIT(idx)) {
			if (!ts->finger_count)
				printk(KERN_ERR "[TP]TOUCH_ERR: finger count has reached zero\n");
			else
				ts->finger_count--;
			ts->finger_pressed &= ~BIT(idx);
			if (!ts->first_pressed) {
				if (!ts->finger_count)
					ts->first_pressed = 1;
				printk(KERN_INFO "[TP]E%d@%d,%d\n",
					idx + 1, ts->finger_data[idx].x, ts->finger_data[idx].y);
			}
			if (ts->id->version >= 0x20 && ts->pre_data[0] < RECALIB_DONE) {
				if (ts->finger_count == 0) {
					if (ts->pre_data[0] == RECALIB_NEED &&
						!ts->unlock_attr && idx == 0 &&
						ts->finger_data[idx].y > 750 &&
						ts->finger_data[idx].y - ts->pre_data[idx+1] > 135) {
							restore_normal_threshold(ts);
							confirm_calibration(ts, 1, 0);
					} else if (ts->pre_data[0] == RECALIB_UNLOCK &&
						ts->unlock_attr && idx == 0 &&
						time_after(jiffies, ts->valid_press_timeout)) {
						ts->valid_pressed_cnt++;
						if (ts->valid_pressed_cnt > 2) {
							cancel_delayed_work_sync(&ts->unlock_work);
							if (ts->pre_data[0] == RECALIB_UNLOCK)
								confirm_calibration(ts, 0, 1);
						}
					} else if (ts->pre_data[0] == RECALIB_NG)
						ts->pre_data[0] = RECALIB_NEED;
				} else {
					if (ts->pre_data[0] < RECALIB_UNLOCK)
						i2c_atmel_write_byte_data(ts->client,
							get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
							T6_CFG_CALIBRATE, 0x55);
				}
			}
		}
	} else if ((data[T9_MSG_STATUS] & (T9_MSG_STATUS_DETECT|T9_MSG_STATUS_PRESS)) &&
		!(ts->finger_pressed & BIT(idx))) {
		if (ts->filter_level[0]) {
			if (ts->finger_data[idx].x < ts->filter_level[FL_XLOGRIPMIN] ||
				ts->finger_data[idx].x > ts->filter_level[FL_XHIGRIPMAX])
				ts->grip_suppression |= BIT(idx);
			else if ((ts->finger_data[idx].x < ts->filter_level[FL_XLOGRIPMAX] ||
				ts->finger_data[idx].x > ts->filter_level[FL_XHIGRIPMIN]) &&
				(ts->grip_suppression & BIT(idx)))
				ts->grip_suppression |= BIT(idx);
			else if (ts->finger_data[idx].x > ts->filter_level[FL_XLOGRIPMAX] &&
				ts->finger_data[idx].x < ts->filter_level[FL_XHIGRIPMIN])
				ts->grip_suppression &= ~BIT(idx);
		}
		if (!(ts->grip_suppression & BIT(idx))) {
			if (!ts->first_pressed)
				printk(KERN_INFO "[TP]S%d@%d,%d\n",
					idx + 1, ts->finger_data[idx].x, ts->finger_data[idx].y);
			if (ts->finger_count >= ts->finger_support)
				printk(KERN_ERR "[TP]TOUCH_ERR: finger count has reached max\n");
			else
				ts->finger_count++;
			ts->finger_pressed |= BIT(idx);
			if (ts->id->version >= 0x20 && ts->pre_data[0] < RECALIB_DONE) {
				if (ts->pre_data[0] < RECALIB_UNLOCK) {
					ts->pre_data[idx + 1] = ts->finger_data[idx].y;
					if (ts->finger_count == ts->finger_support)
						i2c_atmel_write_byte_data(ts->client,
							get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
							T6_CFG_CALIBRATE, 0x55);
					else if (ts->finger_count > 1 &&
						ts->pre_data[0] == RECALIB_NEED)
						ts->pre_data[0] = RECALIB_NG;
				} else if (ts->pre_data[0] == RECALIB_UNLOCK && ts->unlock_attr)
					if (ts->finger_count > 1)
						ts->valid_pressed_cnt = 0;
			}
		}
	}
}

static void msg_process_multitouch_legacy(struct atmel_ts_data *ts, uint8_t *data, uint8_t idx)
{
	/* for issue debug only */
	if ((data[T9_MSG_STATUS] & (T9_MSG_STATUS_PRESS|T9_MSG_STATUS_RELEASE)) ==
		(T9_MSG_STATUS_PRESS|T9_MSG_STATUS_RELEASE))
		printk(KERN_INFO "[TP] x60 ISSUE happened: %x, %x, %x, %x, %x, %x, %x\n",
			data[0], data[1], data[2], data[3], data[4], data[5], data[6]);

	msg_process_finger_data(ts, &ts->finger_data[idx], data, idx);
	if ((data[T9_MSG_STATUS] & T9_MSG_STATUS_RELEASE) &&
		(ts->finger_pressed & BIT(idx))) {
		if (data[T9_MSG_STATUS] & T9_MSG_STATUS_MOVE) {
			multi_input_report(ts);
			input_sync(ts->input_dev);
		}
		ts->finger_count--;
		ts->finger_pressed &= ~BIT(idx);
	} else if ((data[T9_MSG_STATUS] & (T9_MSG_STATUS_DETECT|T9_MSG_STATUS_PRESS)) &&
		!(ts->finger_pressed & BIT(idx))) {
		ts->finger_count++;
		ts->finger_pressed |= BIT(idx);
	}
}

static void msg_process_noisesuppression(struct atmel_ts_data *ts, uint8_t *data)
{
	uint8_t loop_i;

	if (ts->status == CONNECTED && data[T22_MSG_GCAFDEPTH] >= ts->GCAF_sample) {
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_POWERCONFIG_T7) +
			T7_CFG_IDLEACQINT, 0x08);
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_POWERCONFIG_T7) +
			T7_CFG_ACTVACQINT, 0x08);
		for (loop_i = 0; loop_i < 5; loop_i++) {
			if (ts->GCAF_sample < ts->GCAF_level[loop_i]) {
				ts->GCAF_sample = ts->GCAF_level[loop_i];
				break;
			}
		}
		if (loop_i == 5)
			ts->GCAF_sample += 24;
		if (ts->GCAF_sample >= 63) {
			ts->GCAF_sample = 63;
			if (ts->noise_config[0]) {
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
					T9_CFG_TCHTHR, ts->noise_config[NC_TCHTHR]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
					T9_CFG_TCHDI, ts->noise_config[NC_TCHDI]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
					T22_CFG_NOISETHR, ts->noise_config[NC_NOISETHR]);
			} else {
				if (ts->config_setting[CONNECTED].config[0])
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_NOISETHR, ts->config_setting[CONNECTED].config[CB_NOISETHR]);
				else
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_NOISETHR, ts->config_setting[CONNECTED].config_T22[T22_CFG_NOISETHR]);
			}
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
				T8_CFG_ATCHCALSTHR, 0x1);
		}
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, SPT_CTECONFIG_T28) +
			T28_CFG_ACTVGCAFDEPTH, ts->GCAF_sample);
	}
	if (data[T22_MSG_STATUS] & (T22_MSG_STATUS_FHERR|T22_MSG_STATUS_GCAFERR) &&
		ts->GCAF_sample == 63) {
		ts->noisethr_config += 3;
		if (ts->noisethr && ts->noisethr_config > ts->noisethr)
			ts->noisethr_config = ts->noisethr;
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
			T22_CFG_NOISETHR, ts->noisethr_config);
	}
}

static void compatible_input_report(uint8_t report_both, struct input_dev *idev,
				struct atmel_finger_data *fdata, uint8_t press, uint8_t last)
{
	if (!press) {
		if (report_both == REPORT_BOTH_DATA) {
			input_report_abs(idev, ABS_MT_AMPLITUDE, 0);
			input_report_abs(idev, ABS_MT_POSITION, BIT(31));
		}
		input_mt_sync(idev);
	} else {
		if (report_both == REPORT_BOTH_DATA) {
			input_report_abs(idev, ABS_MT_AMPLITUDE, fdata->z << 16 | fdata->w);
			input_report_abs(idev, ABS_MT_POSITION,
				(last ? BIT(31) : 0) | fdata->x << 16 | fdata->y);
		}
		input_report_abs(idev, ABS_MT_PRESSURE, fdata->z);
		input_report_abs(idev, ABS_MT_TOUCH_MAJOR, fdata->z);
		input_report_abs(idev, ABS_MT_WIDTH_MAJOR, fdata->w);
		input_report_abs(idev, ABS_MT_POSITION_X, fdata->x);
		input_report_abs(idev, ABS_MT_POSITION_Y, fdata->y);
		input_mt_sync(idev);
	}
}

static void htc_input_report(struct input_dev *idev,
				struct atmel_finger_data *fdata, uint8_t press, uint8_t last)
{
	if (!press) {
		input_report_abs(idev, ABS_MT_AMPLITUDE, 0);
		input_report_abs(idev, ABS_MT_POSITION, BIT(31));
	} else {
		input_report_abs(idev, ABS_MT_AMPLITUDE, fdata->z << 16 | fdata->w);
		input_report_abs(idev, ABS_MT_POSITION,
			(last ? BIT(31) : 0) | fdata->x << 16 | fdata->y);
	}
}

static void multi_input_report(struct atmel_ts_data *ts)
{
	uint8_t loop_i, finger_report = 0;

	for (loop_i = 0; loop_i < ts->finger_support; loop_i++) {
		if (ts->finger_pressed & BIT(loop_i)) {
			if (ts->id->version >= 0x15) {
				if (ts->flag_htc_event == 0) {
					compatible_input_report(ts->report_both, ts->input_dev, &ts->finger_data[loop_i],
						1, (ts->finger_count == ++finger_report));
				} else {
					htc_input_report(ts->input_dev, &ts->finger_data[loop_i],
						1, (ts->finger_count == ++finger_report));
				}
			} else {
				compatible_input_report(ts->report_both, ts->input_dev, &ts->finger_data[loop_i],
					1, (ts->finger_count == ++finger_report));
			}
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
			//left -> right
			if ((s2w_switch > 0) && (scr_suspended == true) && (ts->finger_count == 1)) {
				if ((barrier[0] == true) ||
				   ((ts->finger_data[loop_i].x > barrier1) &&
				    (ts->finger_data[loop_i].x < barrier2) &&
				    (ts->finger_data[loop_i].y > 950))) {
					if ((led_exec_count == true) && (scr_on_touch == false) && (s2w_switch == 2)) {
						pm8058_drvx_led_brightness_set(sweep2wake_leddev, 255);
						printk(KERN_INFO "[sweep2wake]: activated button backlight.\n");
						led_exec_count = false;
					}
					barrier[0] = true;
					if ((barrier[1] == true) ||
					   ((ts->finger_data[loop_i].x > barrier2) &&
					    (ts->finger_data[loop_i].x < barrier3) &&
					    (ts->finger_data[loop_i].y > 950))) {
						barrier[1] = true;
						if ((ts->finger_data[loop_i].x > barrier3) &&
						    (ts->finger_data[loop_i].y > 950)) {
							if (exec_count) {
								printk(KERN_INFO "[sweep2wake]: POWER ON.\n");
								sweep2wake_pwrtrigger();
								exec_count = false;
								break;
							}
						}
					}
				}
			//right -> left
			} else if ((s2w_switch > 0) && (scr_suspended == false) && (ts->finger_count == 1)) {
				scr_on_touch=true;
				if ((barrier[0] == true) ||
				   ((ts->finger_data[loop_i].x < barrier4) &&
			    	    (ts->finger_data[loop_i].x > barrier3) &&
				    (ts->finger_data[loop_i].y > 950))) {
					barrier[0] = true;
					if ((barrier[1] == true) ||
					   ((ts->finger_data[loop_i].x < barrier3) &&
					    (ts->finger_data[loop_i].x > barrier2) &&
					    (ts->finger_data[loop_i].y > 950))) {
						barrier[1] = true;
						if ((ts->finger_data[loop_i].x < barrier2) &&
						    (ts->finger_data[loop_i].y > 950)) {
							if (exec_count) {
								printk(KERN_INFO "[sweep2wake]: POWER OFF.\n");
								sweep2wake_pwrtrigger();
								exec_count = false;
								break;
							}
						}
					}
				}
			}
#endif
			if (ts->debug_log_level & 0x2)
				printk(KERN_INFO "Finger %d=> X:%d, Y:%d w:%d, z:%d, F:%d\n",
					loop_i + 1,
					ts->finger_data[loop_i].x, ts->finger_data[loop_i].y,
					ts->finger_data[loop_i].w, ts->finger_data[loop_i].z,
					ts->finger_count);
		}
	}
}

static irqreturn_t atmel_irq_thread(int irq, void *ptr)
{
	int ret;
	struct atmel_ts_data *ts = ptr;
	uint8_t data[7];
	int8_t report_type;
	uint8_t loop_i, loop_j, msg_byte_num = 7;

	memset(data, 0x0, sizeof(data));

	ret = i2c_atmel_read(ts->client, get_object_address(ts,
		GEN_MESSAGEPROCESSOR_T5), data, 7);

	if (ts->debug_log_level & 0x1) {
		printk(KERN_INFO "[TP]");
		for (loop_i = 0; loop_i < 7; loop_i++)
			printk(KERN_INFO "0x%2.2X ", data[loop_i]);
		printk("\n");
	}

	if (ts->id->version >= 0x15) {
		report_type = data[MSG_RID] - ts->finger_type;
		if (report_type >= 0 && report_type < ts->finger_support) {
			msg_process_multitouch(ts, data, report_type);
		} else {
			if (data[MSG_RID] == get_rid(ts, GEN_COMMANDPROCESSOR_T6)) {
				if ((data[T6_MSG_STATUS] & T6_MSG_STATUS_CAL) &&
					ts->unlock_attr) {
					if (ts->pre_data[0] == RECALIB_UNLOCK) {
						ts->valid_pressed_cnt = 0;
						ts->cal_after_unlock = 1;
						ts->valid_press_timeout = jiffies +
							msecs_to_jiffies(15 + ts->finger_count * 5);
					} else if (ts->pre_data[0] < RECALIB_UNLOCK) {
						ts->safe_unlock_timeout = jiffies +
							msecs_to_jiffies(SAFE_TIMEOUT);
					}
				}
				printk(KERN_INFO "[TP]");
				msg_byte_num = 5;
			} else if (data[MSG_RID] == get_rid(ts, PROCI_GRIPFACESUPPRESSION_T20)) {
				if (ts->calibration_confirm < 2 && ts->id->version == 0x16)
					check_calibration(ts);
				ts->face_suppression = data[T20_MSG_STATUS];
				printk(KERN_INFO "[TP]Touch Face suppression %s: ",
					ts->face_suppression ? "Active" : "Inactive");
				msg_byte_num = 2;
			} else if (data[MSG_RID] == get_rid(ts, PROCG_NOISESUPPRESSION_T22)) {
				if (data[T22_MSG_STATUS] == T22_MSG_STATUS_GCAFCHG) { /* reduce message print */
					printk(KERN_INFO "[TP]Touch Noise suppression: ");
					msg_byte_num = 4;
				} else {
					printk(KERN_INFO "[TP]Touch Noise suppression: ");
					msg_byte_num = 4;
					msg_process_noisesuppression(ts, data);
				}
			} else
				printk(KERN_INFO "[TP]Touch Unhandled: ");

			if (data[MSG_RID] != 0xFF) {
				for (loop_j = 0; loop_j < msg_byte_num; loop_j++)
					printk("0x%2.2X ", data[loop_j]);
				if (msg_byte_num)
					printk("\n");
			}
			return IRQ_HANDLED;
		}

		if (!ts->finger_count || ts->face_suppression) {
			ts->finger_pressed = 0;
			ts->finger_count = 0;
			if (ts->flag_htc_event == 0)
				compatible_input_report(ts->report_both, ts->input_dev, NULL, 0, 1);
			else
				htc_input_report(ts->input_dev, NULL, 0, 1);

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
		 	/* if finger released, reset count & barriers */
			if ((((ts->finger_count > 0)?1:0) == 0) && (s2w_switch > 0)) {
				if ((s2w_switch == 2) &&
			    	(scr_suspended == true) &&
			    	(led_exec_count == false) &&
			    	(scr_on_touch == false) &&
			    	(exec_count == true)) {
					pm8058_drvx_led_brightness_set(sweep2wake_leddev, 0);
					printk(KERN_INFO "[sweep2wake]: deactivated button backlight.\n");
				}
				exec_count = true;
				led_exec_count = true;
				barrier[0] = false;
				barrier[1] = false;
				scr_on_touch = false;
				printk(KERN_INFO "[sweep2wake]: Finger released, reseting vars.\n");
			}
#endif

			if (ts->debug_log_level & 0x2)
				printk(KERN_INFO "Finger leave\n");
		} else {
			multi_input_report(ts);
		}
		input_sync(ts->input_dev);
	} else { /*read one message one time */
		report_type = data[MSG_RID] - ts->finger_type;
		if (report_type >= 0 && report_type < ts->finger_support) {
			msg_process_multitouch_legacy(ts, data, report_type);
			if (!ts->finger_count) {
				ts->finger_pressed = 0;
				compatible_input_report(ts->report_both, ts->input_dev, NULL, 0, 1);
				if (ts->debug_log_level & 0x2)
					printk(KERN_INFO "Finger leave\n");
			} else {
				multi_input_report(ts);
			}
			input_sync(ts->input_dev);
		} else
			printk(KERN_INFO"RAW data: %x, %x, %x, %x, %x, %x, %x\n",
				data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
	}
	return IRQ_HANDLED;
}

static void atmel_ts_check_delta_work_func(struct work_struct *work)
{
	struct atmel_ts_data *ts;
	uint8_t delta = 0;

	ts = container_of(work, struct atmel_ts_data, check_delta_work);

	i2c_atmel_read(ts->client, get_object_address(ts,
		TOUCH_MULTITOUCHSCREEN_T9) + T9_CFG_TCHTHR, &delta, 1);
	delta = (delta >> 2) << 3;
	if (ts->id->version >= 0x20 && check_delta_full(ts, delta, 50, 1)) {
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
			T8_CFG_ATCHCALST, ts->ATCH_EXT[0]);
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
			T8_CFG_ATCHCALSTHR, ts->ATCH_EXT[1]);
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
			T8_CFG_ATCHFRCCALTHR, 16);
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
			T8_CFG_ATCHFRCCALRATIO, 240);
		msleep(1);
		i2c_atmel_write_byte_data(ts->client,
			get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
			T6_CFG_CALIBRATE, 0x55);
	}
}

static void atmel_ts_unlock_work_func(struct work_struct *work)
{
	struct atmel_ts_data *ts;
	uint8_t delta = 0;
	int ret;

	ts = container_of(work, struct atmel_ts_data, unlock_work.work);
	if (ts->pre_data[0] != RECALIB_UNLOCK || ts->cal_after_unlock)
		goto give_up;
	else {
		if (ts->finger_count)
			ret = 1;
		else {
			i2c_atmel_read(ts->client, get_object_address(ts,
				TOUCH_MULTITOUCHSCREEN_T9) + T9_CFG_TCHTHR, &delta, 1);
			delta = (delta >> 2) << 3;
			ret = check_delta_full(ts, delta, 2, 0);
		}
		if (ts->pre_data[0] != RECALIB_UNLOCK || ts->cal_after_unlock)
			goto give_up;
		else {
			if (ret == 0)
				confirm_calibration(ts, 0, 2);
			else /* retry, schedule next work */
				queue_delayed_work(ts->atmel_delayed_wq, &ts->unlock_work,
					msecs_to_jiffies(ATCHCAL_DELAY));
		}
	}

	return;

give_up:
	printk(KERN_INFO "[TP]give up delta check\n");
}

static int psensor_tp_status_handler_func(struct notifier_block *this,
	unsigned long status, void *unused)
{
	struct atmel_ts_data *ts;

	ts = private_ts;
	printk(KERN_INFO "[TP]psensor status %d -> %lu\n",
		ts->psensor_status, status);
	if (ts->psensor_status == 0) {
		if (status == 1)
			ts->psensor_status = status;
		else
			ts->psensor_status = 0;
	} else
		ts->psensor_status = status;

	return NOTIFY_OK;
}

static int wlc_tp_status_handler_func(struct notifier_block *this,
	unsigned long connect_status, void *unused)
{
	struct atmel_ts_data *ts;
	int wlc_status;

	wlc_status = connect_status ? CONNECTED : NONE;
	printk(KERN_INFO "[TP]wireless charger %d\n", wlc_status);

	ts = private_ts;
	if (ts->status)
		printk(KERN_INFO "[TP]ambigurous wireless charger state\n");

	if (wlc_status != ts->wlc_status) {
		ts->wlc_status = wlc_status ? CONNECTED : NONE;
		if (!ts->status && ts->wlc_config[0]) {
			if (ts->wlc_status) {
				if (ts->wlc_freq[1])
					i2c_atmel_write(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_FREQ,
						ts->wlc_freq, 5);
				i2c_atmel_write(ts->client,
					get_object_address(ts, GEN_POWERCONFIG_T7),
					ts->wlc_config,
					get_object_size(ts, GEN_POWERCONFIG_T7));
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
					T9_CFG_TCHTHR,
					ts->wlc_config[WLC_TCHTHR]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
					T22_CFG_NOISETHR,
					ts->wlc_config[WLC_NOISETHR]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28) +
					T28_CFG_IDLEGCAFDEPTH,
					ts->wlc_config[WLC_IDLEGCAFDEPTH]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28) +
					T28_CFG_ACTVGCAFDEPTH,
					ts->wlc_config[WLC_ACTVGCAFDEPTH]);
			} else {
				if (ts->wlc_freq[1])
					i2c_atmel_write(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_FREQ,
						ts->config_setting[NONE].config_T22 + T22_CFG_FREQ, 5);
				i2c_atmel_write(ts->client,
					get_object_address(ts, GEN_POWERCONFIG_T7),
					ts->config_setting[NONE].config_T7,
					get_object_size(ts, GEN_POWERCONFIG_T7));
				if (ts->config_setting[CONNECTED].config[0]) {
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
						T9_CFG_TCHTHR,
						ts->config_setting[NONE].config[CB_TCHTHR]);
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_NOISETHR,
						ts->config_setting[NONE].config[CB_NOISETHR]);
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, SPT_CTECONFIG_T28) +
						T28_CFG_IDLEGCAFDEPTH,
						ts->config_setting[NONE].config[CB_IDLEGCAFDEPTH]);
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, SPT_CTECONFIG_T28) +
						T28_CFG_ACTVGCAFDEPTH,
						ts->config_setting[NONE].config[CB_ACTVGCAFDEPTH]);
				} else {
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
						T9_CFG_TCHTHR,
						ts->config_setting[NONE].config_T9[T9_CFG_TCHTHR]);
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_NOISETHR,
						ts->config_setting[NONE].config_T22[T22_CFG_NOISETHR]);
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, SPT_CTECONFIG_T28) +
						T28_CFG_IDLEGCAFDEPTH,
						ts->config_setting[NONE].config_T28[T28_CFG_IDLEGCAFDEPTH]);
					i2c_atmel_write_byte_data(ts->client,
						get_object_address(ts, SPT_CTECONFIG_T28) +
						T28_CFG_ACTVGCAFDEPTH,
						ts->config_setting[NONE].config_T28[T28_CFG_ACTVGCAFDEPTH]);
				}
			}
		}
		regdump_to_kernel();
	}

	return NOTIFY_OK;
}

static void cable_tp_status_handler_func(int connect_status)
{
	struct atmel_ts_data *ts;
	ts = private_ts;

#if defined(CONFIG_ARCH_MSM8X60)
	if (connect_status == 4 || (connect_status == 0 && ts->wlc_status)) {
		wlc_tp_status_handler_func(NULL, connect_status == 4 ? 1 : 0, NULL);
		return;
	}
#endif

	printk(KERN_INFO "[TP]cable change to %d\n", connect_status);

	if (connect_status != ts->status) {
		ts->status = connect_status ? CONNECTED : NONE;
		if (!ts->status && ts->wlc_status)
			printk(KERN_INFO "[TP]ambigurous wireless charger state\n");
		if (ts->config_setting[CONNECTED].config[0]) {
			if (ts->status && ts->wlc_status) {
				if (ts->wlc_freq[1])
					i2c_atmel_write(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_FREQ,
						ts->config_setting[NONE].config_T22 + T22_CFG_FREQ, 5);
				i2c_atmel_write(ts->client,
					get_object_address(ts, GEN_POWERCONFIG_T7),
					ts->config_setting[CONNECTED].config_T7,
					get_object_size(ts, GEN_POWERCONFIG_T7));
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
					T22_CFG_NOISETHR,
					ts->config_setting[NONE].config[CB_NOISETHR]);
				ts->noisethr_config =
					ts->config_setting[CONNECTED].config[CB_NOISETHR];

				printk(KERN_INFO "[TP]cable %s overrides wireless charger\n",
					ts->status ? "in" : "out");
				ts->wlc_status = NONE;
			}
			if (ts->status == CONNECTED && ts->id->version < 0x20) {
				ts->calibration_confirm = 2;
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
					T8_CFG_ATCHCALST,
					ts->config_setting[ts->status].config_T8[T8_CFG_ATCHCALST]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
					T8_CFG_ATCHCALSTHR ,
					ts->config_setting[ts->status].config_T8[T8_CFG_ATCHCALSTHR]);
			}
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_TCHTHR,
				ts->config_setting[ts->status].config[CB_TCHTHR]);
			if (ts->status == NONE) {
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
					T22_CFG_NOISETHR,
					ts->config_setting[NONE].config[CB_NOISETHR]);
				ts->noisethr_config =
					ts->config_setting[CONNECTED].config[CB_NOISETHR];
			}
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, SPT_CTECONFIG_T28) +
				T28_CFG_IDLEGCAFDEPTH,
				ts->config_setting[ts->status].config[CB_IDLEGCAFDEPTH]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, SPT_CTECONFIG_T28) +
				T28_CFG_ACTVGCAFDEPTH,
				ts->config_setting[ts->status].config[CB_ACTVGCAFDEPTH]);
			ts->GCAF_sample =
				ts->config_setting[CONNECTED].config[CB_ACTVGCAFDEPTH];
		} else {
			if (ts->config_setting[CONNECTED].config_T7 != NULL)
				i2c_atmel_write(ts->client,
					get_object_address(ts, GEN_POWERCONFIG_T7),
					ts->config_setting[ts->status].config_T7,
					get_object_size(ts, GEN_POWERCONFIG_T7));
			else if (ts->wlc_status) {
				i2c_atmel_write(ts->client,
					get_object_address(ts, GEN_POWERCONFIG_T7),
					ts->config_setting[NONE].config_T7,
					get_object_size(ts, GEN_POWERCONFIG_T7));
			}
			if (ts->config_setting[CONNECTED].config_T8 != NULL) {
				if (ts->pre_data[0] == RECALIB_DONE && ts->id->version >= 0x20)
					i2c_atmel_write(ts->client,
						get_object_address(ts, GEN_ACQUISITIONCONFIG_T8),
						ts->config_setting[CONNECTED].config_T8,
						get_object_size(ts, GEN_ACQUISITIONCONFIG_T8) - 4);
				else
					i2c_atmel_write(ts->client,
						get_object_address(ts, GEN_ACQUISITIONCONFIG_T8),
						ts->config_setting[CONNECTED].config_T8,
						get_object_size(ts, GEN_ACQUISITIONCONFIG_T8));
			}
			if (ts->config_setting[ts->status].config_T9 != NULL) {
				i2c_atmel_write(ts->client,
					get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9),
					ts->config_setting[ts->status].config_T9,
					get_object_size(ts, TOUCH_MULTITOUCHSCREEN_T9));
				i2c_atmel_write(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22),
					ts->config_setting[ts->status].config_T22,
					get_object_size(ts, PROCG_NOISESUPPRESSION_T22));
				ts->noisethr_config =
					ts->config_setting[ts->status].config_T22[8];
				i2c_atmel_write(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28),
					ts->config_setting[ts->status].config_T28,
					get_object_size(ts, SPT_CTECONFIG_T28));
				ts->GCAF_sample =
					ts->config_setting[ts->status].config_T28[T28_CFG_ACTVGCAFDEPTH];
			} else if (ts->wlc_status) {
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
					T9_CFG_TCHTHR,
					ts->config_setting[NONE].config_T9[T9_CFG_TCHTHR]);
				if (ts->wlc_freq[1])
					i2c_atmel_write(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
						T22_CFG_FREQ,
						ts->config_setting[NONE].config_T22 + T22_CFG_FREQ, 5);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
					T22_CFG_NOISETHR,
					ts->config_setting[NONE].config_T22[T22_CFG_NOISETHR]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28) +
					T28_CFG_IDLEGCAFDEPTH,
					ts->config_setting[NONE].config_T28[T28_CFG_IDLEGCAFDEPTH]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28) +
					T28_CFG_ACTVGCAFDEPTH,
					ts->config_setting[NONE].config_T28[T28_CFG_ACTVGCAFDEPTH]);
			}

			if (ts->wlc_status) {
				printk(KERN_INFO "[TP]cable %s overrides wireless charger\n",
					ts->status ? "in" : "out");
				ts->wlc_status = NONE;
			}
		}
		regdump_to_kernel();
	}
}

static int read_object_table(struct atmel_ts_data *ts)
{
	uint8_t i, type_count = 0;
	uint8_t data[6];
	memset(data, 0x0, sizeof(data));

	ts->object_table = kzalloc(sizeof(struct object_t)*ts->id->num_declared_objects, GFP_KERNEL);
	if (ts->object_table == NULL) {
		printk(KERN_ERR "[TP]TOUCH_ERR: allocate object_table failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < ts->id->num_declared_objects; i++) {
		i2c_atmel_read(ts->client, i * 6 + 0x07, data, 6);
		ts->object_table[i].object_type = data[OBJ_TABLE_TYPE];
		ts->object_table[i].i2c_address =
			data[OBJ_TABLE_LSB] | data[OBJ_TABLE_MSB] << 8;
		ts->object_table[i].size = data[OBJ_TABLE_SIZE] + 1;
		ts->object_table[i].instances = data[OBJ_TABLE_INSTANCES];
		ts->object_table[i].num_report_ids = data[OBJ_TABLE_RIDS];
		if (data[OBJ_TABLE_RIDS]) {
			ts->object_table[i].report_ids = type_count + 1;
			type_count += data[OBJ_TABLE_RIDS];
		}
		if (data[OBJ_TABLE_TYPE] == TOUCH_MULTITOUCHSCREEN_T9)
			ts->finger_type = ts->object_table[i].report_ids;
		printk(KERN_INFO
			"[TP]Type: %2.2X, Start: %4.4X, Size: %2X, Instance: %2X, RD#: %2X, %2X\n",
			ts->object_table[i].object_type , ts->object_table[i].i2c_address,
			ts->object_table[i].size, ts->object_table[i].instances,
			ts->object_table[i].num_report_ids, ts->object_table[i].report_ids);
	}

	return 0;
}

#if !defined(CONFIG_ARCH_MSM8X60)
static struct t_usb_status_notifier cable_status_handler = {
	.name = "usb_tp_connected",
	.func = cable_tp_status_handler_func,
};

static struct notifier_block wlc_status_handler = {
	.notifier_call = wlc_tp_status_handler_func,
};
#else
static struct t_cable_status_notifier cable_status_handler = {
    .name = "usb_tp_connected",
    .func = cable_tp_status_handler_func,
};
#endif

static struct notifier_block psensor_status_handler = {
	.notifier_call = psensor_tp_status_handler_func,
};

static int atmel_ts_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct atmel_ts_data *ts;
	struct atmel_i2c_platform_data *pdata;
	int ret = 0, intr = 0;
	uint8_t loop_i;
	struct i2c_msg msg[2];
	uint8_t data[16];
	uint8_t CRC_check = 0;
#if defined(CONFIG_ARCH_MSM8X60)
	int cable_connect_type = 0;
#endif
	uint16_t x_range;
	uint16_t y_range;
	printk(KERN_INFO "%s:[TP]enter\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		printk(KERN_ERR "[TP]TOUCH_ERR: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	ts = kzalloc(sizeof(struct atmel_ts_data), GFP_KERNEL);
	if (ts == NULL) {
		printk(KERN_ERR "[TP]TOUCH_ERR: allocate atmel_ts_data failed\n");
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}

	ts->atmel_wq = create_singlethread_workqueue("atmel_wq");
	if (!ts->atmel_wq) {
		printk(KERN_ERR "TOUCH_ERR: create workqueue atmel_wq failed\n");
		ret = -ENOMEM;
		goto err_create_atmel_wq_failed;
	}

	ts->atmel_delayed_wq = create_singlethread_workqueue("atmel_delayed_wq");
	if (!ts->atmel_delayed_wq) {
		printk(KERN_ERR "[TP]TOUCH_ERR: create workqueue atmel_delayed_wq failed\n");
		ret = -ENOMEM;
		goto err_create_atmel_delayed_wq_failed;
	}

	INIT_WORK(&ts->check_delta_work, atmel_ts_check_delta_work_func);
	INIT_DELAYED_WORK(&ts->unlock_work, atmel_ts_unlock_work_func);
	ts->client = client;
	i2c_set_clientdata(client, ts);
	pdata = client->dev.platform_data;

	if (pdata) {
		ts->power = pdata->power;
		intr = pdata->gpio_irq;
	} else {
		printk(KERN_ERR "[TP]TOUCH_ERR: pdata is NULL\n");
		goto err_get_platform_data_fail;
	}

	if (ts->power)
		ret = ts->power(1);

	for (loop_i = 0; loop_i < 10; loop_i++) {
		if (!gpio_get_value(intr))
			break;
		msleep(10);
	}

	if (loop_i == 10)
		printk(KERN_INFO "[TP]No Messages after reset\n");

	/* read message*/
	msg[0].addr = ts->client->addr;
	msg[0].flags = I2C_M_RD;
	msg[0].len = 7;
	msg[0].buf = data;
	ret = i2c_transfer(client->adapter, msg, 1);

	if (ret < 0) {
		printk(KERN_INFO "[TP]No Atmel chip inside\n");
		goto err_detect_failed;
	}
#if !defined(CONFIG_ARCH_MSM8X60)
	if (ts->power)
		ret = ts->power(2);
#endif

	printk(KERN_INFO
		"[TP]0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X\n",
		data[0], data[1], data[2], data[3], data[4], data[5], data[6]);

	if (data[MSG_RID] == 0x01 &&
		(data[T6_MSG_STATUS] & (T6_MSG_STATUS_SIGERR|T6_MSG_STATUS_COMSERR))) {
		printk(KERN_ERR "[TP]TOUCH_ERR: init err: %x\n", data[T6_MSG_STATUS]);
		goto err_detect_failed;
	} else {
		for (loop_i = 0; loop_i < 10; loop_i++) {
			if (gpio_get_value(intr)) {
				printk(KERN_INFO "[TP]No more message\n");
				break;
			}
			ret = i2c_transfer(client->adapter, msg, 1);
			printk(KERN_INFO
				"[TP]0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X\n",
				data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
			msleep(10);
		}
	}

	/* Read the info block data. */
	ts->id = kzalloc(sizeof(struct info_id_t), GFP_KERNEL);
	if (ts->id == NULL) {
		printk(KERN_ERR "[TP]TOUCH_ERR: allocate info_id_t failed\n");
		goto err_alloc_failed;
	}
	ret = i2c_atmel_read(client, 0x00, data, 7);

	ts->id->family_id = data[INFO_BLK_FID];
	ts->id->variant_id = data[INFO_BLK_VID];
	ts->id->version = data[INFO_BLK_VER];
	if (ts->id->family_id == 0x80 && ts->id->variant_id == 0x10) {
		if (ts->id->version == 0x10)
			ts->id->version += 6;
	}
	ts->id->build = data[INFO_BLK_BUILD];
	ts->id->matrix_x_size = data[INFO_BLK_XSIZE];
	ts->id->matrix_y_size = data[INFO_BLK_YSIZE];
	ts->id->num_declared_objects = data[INFO_BLK_OBJS];

	printk(KERN_INFO
		"[TP]info block: 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X\n",
		ts->id->family_id, ts->id->variant_id,
		ts->id->version, ts->id->build,
		ts->id->matrix_x_size, ts->id->matrix_y_size,
		ts->id->num_declared_objects);

	/* Read object table. */
	ret = read_object_table(ts);
	if (ret < 0)
		goto err_alloc_failed;

	if (pdata) {
		while (pdata->version > ts->id->version)
			pdata++;
		if (pdata->source) {
			i2c_atmel_write_byte_data(client,
				get_object_address(ts, SPT_GPIOPWM_T19) + T19_CFG_CTRL,
										T19_CFG_CTRL_ENABLE |
										T19_CFG_CTRL_RPTEN |
										T19_CFG_CTRL_FORCERPT);
			for (loop_i = 0; loop_i < 10; loop_i++) {
				if (!gpio_get_value(intr))
					break;
				msleep(10);
			}
			if (loop_i == 10)
				printk(KERN_ERR "[TP]TOUCH_ERR: No Messages when check source\n");
			for (loop_i = 0; loop_i < 100; loop_i++) {
				i2c_atmel_read(ts->client, get_object_address(ts,
					GEN_MESSAGEPROCESSOR_T5), data, 2);
				if (data[MSG_RID] == get_rid(ts, SPT_GPIOPWM_T19)) {
					while (((data[T19_MSG_STATUS] >> 3) & 0x1) != pdata->source)
						pdata++;
					break;
				}
			}
		}

		if ((pdata->config_T9[T9_CFG_NUMTOUCH] > 0) && (pdata->config_T9[T9_CFG_NUMTOUCH] <= 10)) {
			ts->finger_support = pdata->config_T9[T9_CFG_NUMTOUCH];
		} else {
			printk(KERN_INFO "[TP]T9_CFG_NUMTOUCH=%d is over range (1~10)!!\n", pdata->config_T9[T9_CFG_NUMTOUCH]);
			goto err_alloc_failed;
		}

		x_range = ((uint8_t)(pdata->config_T9[T9_CFG_XRANGE + 1]) << 8) +
					(uint8_t)(pdata->config_T9[T9_CFG_XRANGE]);
		y_range = ((uint8_t)(pdata->config_T9[T9_CFG_YRANGE + 1]) << 8) +
					(uint8_t)(pdata->config_T9[T9_CFG_YRANGE]);
		if ((pdata->config_T9[T9_CFG_ORIENT] & 0x1) == 0) {
			if (x_range >= 1024)
				ts->high_res_x_en = 1;
			if (y_range >= 1024)
				ts->high_res_y_en = 1;
		} else { /* Switches the X and Y */
			if (x_range >= 1024)
				ts->high_res_y_en = 1;
			if (y_range >= 1024)
				ts->high_res_x_en = 1;
		}
		printk(KERN_INFO
			"[TP]finger_type: %d, max finger: %d%s%s\n",
			ts->finger_type, ts->finger_support,
			ts->high_res_x_en ? ", x: 12-bit" : "",
			ts->high_res_y_en ? ", y: 12-bit" : "");

		/* infoamtion block CRC check */
		if (pdata->object_crc[0]) {
			ret = i2c_atmel_write_byte_data(client,
						get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
						T6_CFG_CALIBRATE, 0x55);
			for (loop_i = 0; loop_i < 10; loop_i++) {
				if (!gpio_get_value(intr)) {
					ret = i2c_atmel_read(ts->client, get_object_address(ts,
								GEN_MESSAGEPROCESSOR_T5), data, 5);
					if (data[MSG_RID] == get_rid(ts, GEN_COMMANDPROCESSOR_T6))
						break;
				}
				msleep(10);
			}
			if (loop_i == 10)
				printk(KERN_INFO "[TP]No checksum read\n");
			else {
				for (loop_i = 0; loop_i < 3; loop_i++) {
					if (pdata->object_crc[loop_i] !=
						data[T6_MSG_CHECKSUM + loop_i]) {
						printk(KERN_INFO
							"[TP]CRC Error: DRV=0x%2.2X,0x%2.2X,0x%2.2X  NV=0x%2.2X,0x%2.2X,0x%2.2X\n",
							pdata->object_crc[0],
							pdata->object_crc[1],
							pdata->object_crc[2],
							data[T6_MSG_CHECKSUM + 0],
							data[T6_MSG_CHECKSUM + 1],
							data[T6_MSG_CHECKSUM + 2]);
						break;
					}
				}
				if (loop_i == 3) {
					printk(KERN_INFO "[TP]CRC passed: ");
					for (loop_i = 0; loop_i < 3; loop_i++)
						printk("0x%2.2X ", pdata->object_crc[loop_i]);
					printk("\n");
					CRC_check = 1;
				}
			}
		}
		ts->abs_x_min = pdata->abs_x_min;
		ts->abs_x_max = pdata->abs_x_max;
		ts->abs_y_min = pdata->abs_y_min;
		ts->abs_y_max = pdata->abs_y_max;
		ts->abs_pressure_min = pdata->abs_pressure_min;
		ts->abs_pressure_max = pdata->abs_pressure_max;
		ts->abs_width_min = pdata->abs_width_min;
		ts->abs_width_max = pdata->abs_width_max;
		ts->GCAF_level = pdata->GCAF_level;
		if (ts->id->version >= 0x20)
			ts->ATCH_EXT = &pdata->config_T8[T8_CFG_ATCHCALST];
		ts->filter_level = pdata->filter_level;
		ts->unlock_attr = pdata->unlock_attr;
		ts->report_both = pdata->report_both;

#if !defined(CONFIG_ARCH_MSM8X60)
		if (usb_get_connect_type())
			ts->status = CONNECTED;
		else if (htc_is_wireless_charger())
			ts->wlc_status = CONNECTED;
#else
		cable_connect_type = cable_get_connect_type();
		if (cable_connect_type == 4)
			ts->wlc_status = CONNECTED;
		else if (cable_connect_type != 0)
			ts->status = CONNECTED;
#endif

		ts->config_setting[NONE].config_T7
			= ts->config_setting[CONNECTED].config_T7
			= pdata->config_T7;
		ts->config_setting[NONE].config_T8
			= ts->config_setting[CONNECTED].config_T8
			= pdata->config_T8;
		ts->config_setting[NONE].config_T9 = pdata->config_T9;
		ts->config_setting[NONE].config_T22 = pdata->config_T22;
		ts->config_setting[NONE].config_T28 = pdata->config_T28;

		if (pdata->wlc_config[0])
			for (loop_i = 0; loop_i < 7; loop_i++)
				ts->wlc_config[loop_i] = pdata->wlc_config[loop_i];

		if (pdata->wlc_freq[1])
			for (loop_i = 0; loop_i < 5; loop_i++)
				ts->wlc_freq[loop_i] = pdata->wlc_freq[loop_i];

		if (pdata->noise_config[0])
			for (loop_i = 0; loop_i < 3; loop_i++)
				ts->noise_config[loop_i] = pdata->noise_config[loop_i];

		if (pdata->cable_config[0]) {
			ts->config_setting[NONE].config[CB_TCHTHR] =
				pdata->config_T9[T9_CFG_TCHTHR];
			ts->config_setting[NONE].config[CB_NOISETHR] =
				pdata->config_T22[T22_CFG_NOISETHR];
			ts->config_setting[NONE].config[CB_IDLEGCAFDEPTH] =
				pdata->config_T28[T28_CFG_IDLEGCAFDEPTH];
			ts->config_setting[NONE].config[CB_ACTVGCAFDEPTH] =
				pdata->config_T28[T28_CFG_ACTVGCAFDEPTH];
			for (loop_i = 0; loop_i < 4; loop_i++)
				ts->config_setting[CONNECTED].config[loop_i] =
					pdata->cable_config[loop_i];
			ts->GCAF_sample =
				ts->config_setting[CONNECTED].config[CB_ACTVGCAFDEPTH];
			if (ts->id->version >= 0x20)
				ts->noisethr = pdata->cable_config[CB_TCHTHR] -
					pdata->config_T9[T9_CFG_TCHHYST];
			else
				ts->noisethr = pdata->cable_config[CB_TCHTHR];
			ts->noisethr_config =
				ts->config_setting[CONNECTED].config[CB_NOISETHR];
		} else {
			if (pdata->cable_config_T7[0])
				ts->config_setting[CONNECTED].config_T7 =
					pdata->cable_config_T7;
			if (pdata->cable_config_T8[0])
				ts->config_setting[CONNECTED].config_T8 =
					pdata->cable_config_T8;
			if (pdata->cable_config_T9[0]) {
				ts->config_setting[CONNECTED].config_T9 =
					pdata->cable_config_T9;
				ts->config_setting[CONNECTED].config_T22 =
					pdata->cable_config_T22;
				ts->config_setting[CONNECTED].config_T28 =
					pdata->cable_config_T28;
				ts->GCAF_sample =
					ts->config_setting[CONNECTED].config_T28[T28_CFG_ACTVGCAFDEPTH];
			}
			if (ts->status == CONNECTED && pdata->cable_config_T9[0])
				ts->noisethr = (ts->id->version >= 0x20) ?
					pdata->cable_config_T9[T9_CFG_TCHTHR] - pdata->cable_config_T9[T9_CFG_TCHHYST] :
					pdata->cable_config_T9[T9_CFG_TCHTHR];
			else
				ts->noisethr = (ts->id->version >= 0x20) ?
					pdata->config_T9[T9_CFG_TCHTHR] - pdata->config_T9[T9_CFG_TCHHYST] :
					pdata->config_T9[T9_CFG_TCHTHR];
			ts->noisethr_config = pdata->cable_config_T22[T22_CFG_NOISETHR];

		}

		if (pdata->call_tchthr[0])
			for (loop_i = 0; loop_i < 2; loop_i++)
				ts->call_tchthr[loop_i] = pdata->call_tchthr[loop_i];

		if (pdata->locking_config[0])
			ts->locking_config[0] = pdata->locking_config[0];

		if (!CRC_check) {
			printk(KERN_INFO "[TP]Config reload\n");

			i2c_atmel_write(ts->client, get_object_address(ts, SPT_CTECONFIG_T28),
				pdata->config_T28, get_object_size(ts, SPT_CTECONFIG_T28));

			ret = i2c_atmel_write_byte_data(client,
						get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
						T6_CFG_BACKUPNV, 0x55);
			msleep(10);

			ret = i2c_atmel_write_byte_data(client,
						get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
						T6_CFG_RESET, 0x11);
			msleep(100);

			i2c_atmel_write(ts->client,
				get_object_address(ts, GEN_COMMANDPROCESSOR_T6),
				pdata->config_T6,
				get_object_size(ts, GEN_COMMANDPROCESSOR_T6));
			i2c_atmel_write(ts->client,
				get_object_address(ts, GEN_POWERCONFIG_T7),
				pdata->config_T7,
				get_object_size(ts, GEN_POWERCONFIG_T7));
			i2c_atmel_write(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8),
				pdata->config_T8,
				get_object_size(ts, GEN_ACQUISITIONCONFIG_T8));
			i2c_atmel_write(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9),
				pdata->config_T9,
				get_object_size(ts, TOUCH_MULTITOUCHSCREEN_T9));
			i2c_atmel_write(ts->client,
				get_object_address(ts, TOUCH_KEYARRAY_T15),
				pdata->config_T15,
				get_object_size(ts, TOUCH_KEYARRAY_T15));
			i2c_atmel_write(ts->client,
				get_object_address(ts, SPT_COMCONFIG_T18),
				pdata->config_T18,
				get_object_size(ts, SPT_COMCONFIG_T18));
			i2c_atmel_write(ts->client,
				get_object_address(ts, SPT_GPIOPWM_T19),
				pdata->config_T19,
				get_object_size(ts, SPT_GPIOPWM_T19));
			i2c_atmel_write(ts->client,
				get_object_address(ts, PROCI_GRIPFACESUPPRESSION_T20),
				pdata->config_T20,
				get_object_size(ts, PROCI_GRIPFACESUPPRESSION_T20));
			i2c_atmel_write(ts->client,
				get_object_address(ts, PROCG_NOISESUPPRESSION_T22),
				pdata->config_T22,
				get_object_size(ts, PROCG_NOISESUPPRESSION_T22));
			i2c_atmel_write(ts->client,
				get_object_address(ts, TOUCH_PROXIMITY_T23),
				pdata->config_T23,
				get_object_size(ts, TOUCH_PROXIMITY_T23));
			i2c_atmel_write(ts->client,
				get_object_address(ts, PROCI_ONETOUCHGESTUREPROCESSOR_T24),
				pdata->config_T24,
				get_object_size(ts, PROCI_ONETOUCHGESTUREPROCESSOR_T24));
			i2c_atmel_write(ts->client,
				get_object_address(ts, SPT_SELFTEST_T25),
				pdata->config_T25,
				get_object_size(ts, SPT_SELFTEST_T25));
			if (ts->id->version < 0x20)
				i2c_atmel_write(ts->client,
					get_object_address(ts, PROCI_TWOTOUCHGESTUREPROCESSOR_T27),
					pdata->config_T27,
					get_object_size(ts, PROCI_TWOTOUCHGESTUREPROCESSOR_T27));
			i2c_atmel_write(ts->client,
				get_object_address(ts, SPT_CTECONFIG_T28),
				pdata->config_T28,
				get_object_size(ts, SPT_CTECONFIG_T28));

			ret = i2c_atmel_write_byte_data(client,
						get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
						T6_CFG_BACKUPNV, 0x55);

			for (loop_i = 0; loop_i < 10; loop_i++) {
				if (!gpio_get_value(intr))
					break;
				printk(KERN_INFO "[TP]wait for Message(%d)\n", loop_i + 1);
				msleep(10);
			}

			i2c_atmel_read(client,
				get_object_address(ts, GEN_MESSAGEPROCESSOR_T5), data, 7);
			printk(KERN_INFO
				"[TP]0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X 0x%2.2X\n",
				data[0], data[1], data[2], data[3], data[4], data[5], data[6]);

			ret = i2c_atmel_write_byte_data(client,
						get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
						T6_CFG_RESET, 0x11);
			msleep(100);
		}

		if (ts->status == CONNECTED) {
			printk(KERN_INFO "[TP]set cable config\n");
			if (ts->config_setting[CONNECTED].config[0]) {
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
					T9_CFG_TCHTHR,
					ts->config_setting[CONNECTED].config[CB_TCHTHR]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28) +
					T28_CFG_IDLEGCAFDEPTH,
					ts->config_setting[CONNECTED].config[CB_IDLEGCAFDEPTH]);
				i2c_atmel_write_byte_data(ts->client,
					get_object_address(ts, SPT_CTECONFIG_T28) +
					T28_CFG_ACTVGCAFDEPTH,
					ts->config_setting[CONNECTED].config[CB_ACTVGCAFDEPTH]);
			} else {
				if (ts->config_setting[CONNECTED].config_T7 != NULL)
					i2c_atmel_write(ts->client,
						get_object_address(ts, GEN_POWERCONFIG_T7),
						ts->config_setting[CONNECTED].config_T7,
						get_object_size(ts, GEN_POWERCONFIG_T7));
				if (ts->config_setting[CONNECTED].config_T8 != NULL)
					i2c_atmel_write(ts->client,
						get_object_address(ts, GEN_ACQUISITIONCONFIG_T8),
						ts->config_setting[CONNECTED].config_T8,
						get_object_size(ts, GEN_ACQUISITIONCONFIG_T8));
				if (ts->config_setting[CONNECTED].config_T9 != NULL) {
					i2c_atmel_write(ts->client,
						get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9),
						ts->config_setting[CONNECTED].config_T9,
						get_object_size(ts, TOUCH_MULTITOUCHSCREEN_T9));
					i2c_atmel_write(ts->client,
						get_object_address(ts, PROCG_NOISESUPPRESSION_T22),
						ts->config_setting[CONNECTED].config_T22,
						get_object_size(ts, PROCG_NOISESUPPRESSION_T22));
					i2c_atmel_write(ts->client,
						get_object_address(ts, SPT_CTECONFIG_T28),
						ts->config_setting[CONNECTED].config_T28,
						get_object_size(ts, SPT_CTECONFIG_T28));
				}
			}
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
				T6_CFG_CALIBRATE, 0x55);
		} else if (ts->wlc_status == CONNECTED) {
			printk(KERN_INFO "[TP]set wireless charger config\n");
			if (ts->wlc_freq[1])
				i2c_atmel_write(ts->client,
					get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
					T22_CFG_FREQ,
					ts->wlc_freq, 5);
			i2c_atmel_write(ts->client,
				get_object_address(ts, GEN_POWERCONFIG_T7),
				ts->wlc_config,
				get_object_size(ts, GEN_POWERCONFIG_T7));
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_TCHTHR,
				ts->wlc_config[WLC_TCHTHR]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, PROCG_NOISESUPPRESSION_T22) +
				T22_CFG_NOISETHR,
				ts->wlc_config[WLC_NOISETHR]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, SPT_CTECONFIG_T28) +
				T28_CFG_IDLEGCAFDEPTH,
				ts->wlc_config[WLC_IDLEGCAFDEPTH]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, SPT_CTECONFIG_T28) +
				T28_CFG_ACTVGCAFDEPTH,
				ts->wlc_config[WLC_ACTVGCAFDEPTH]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
				T6_CFG_CALIBRATE, 0x55);
		}

		if (ts->id->version >= 0x20 && ts->call_tchthr[0]) {
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) + T9_CFG_TCHTHR,
				ts->call_tchthr[ts->status]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) + T8_CFG_ATCHCALSTHR,
				ts->call_tchthr[ts->status] - 5);
		}

		if (ts->id->version >= 0x20 && ts->locking_config[0]) {
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_MRGTHR,
				ts->locking_config[0]);
		}
	}
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		dev_err(&client->dev, "[TP]TOUCH_ERR: Failed to allocate input device\n");
		goto err_input_dev_alloc_failed;
	}
	ts->input_dev->name = "atmel-touchscreen";
	ts->input_dev->mtsize = ts->finger_support;

	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_KEY, ts->input_dev->evbit);
	set_bit(EV_ABS, ts->input_dev->evbit);

	set_bit(KEY_BACK, ts->input_dev->keybit);
	set_bit(KEY_HOME, ts->input_dev->keybit);
	set_bit(KEY_MENU, ts->input_dev->keybit);
	set_bit(KEY_SEARCH, ts->input_dev->keybit);

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
				ts->abs_x_min, ts->abs_x_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
				ts->abs_y_min, ts->abs_y_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR,
				ts->abs_pressure_min, ts->abs_pressure_max,
				0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR,
				ts->abs_width_min, ts->abs_width_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE,
				ts->abs_pressure_min, ts->abs_pressure_max,
				0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_AMPLITUDE,
				0, ((ts->abs_pressure_max << 16) | ts->abs_width_max), 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION,
				0, (BIT(31) | (ts->abs_x_max << 16) | ts->abs_y_max), 0, 0);

	ret = input_register_device(ts->input_dev);
	if (ret) {
		dev_err(&client->dev,
			"[TP]TOUCH_ERR: Unable to register %s input device\n",
			ts->input_dev->name);
		goto err_input_register_device_failed;
	}

	private_ts = ts;
	ret = request_threaded_irq(client->irq, NULL, atmel_irq_thread,
		IRQF_TRIGGER_LOW | IRQF_ONESHOT, client->name, ts);

	if (ret)
		dev_err(&client->dev, "[TP]TOUCH_ERR: request_irq failed\n");

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 1;
	ts->early_suspend.suspend = atmel_ts_early_suspend;
	ts->early_suspend.resume = atmel_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

#ifdef ATMEL_EN_SYSFS
	atmel_touch_sysfs_init();
#endif

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_START
	s2w_startbutton = sweep2wake_buttonset(CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_START);
#endif /* CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_START */
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_END
	s2w_endbutton = sweep2wake_buttonset(CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_END);
#endif /* CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE_END */

	barrier1 = s2w_startbutton - 100; //0;
	barrier2 = ((s2w_endbutton - s2w_startbutton) / 4) + s2w_startbutton; //333;
	barrier3 = (((s2w_endbutton - s2w_startbutton) / 4) * 3) + s2w_startbutton; //667;
	barrier4 = s2w_endbutton + 100; //1000;
#endif /* CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE */

	dev_info(&client->dev, "[TP]Start touchscreen %s in interrupt mode\n",
			ts->input_dev->name);

#if !defined(CONFIG_ARCH_MSM8X60)
	usb_register_notifier(&cable_status_handler);
	if (pdata->wlc_config[0])
		register_notifier_wireless_charger(&wlc_status_handler);
#else
	cable_detect_register_notifier(&cable_status_handler);
#endif
	register_notifier_by_psensor(&psensor_status_handler);

	ts->flag_htc_event = 0;
	printk(KERN_INFO "%s:[TP]done\n", __func__);
	return 0;

err_input_register_device_failed:
	input_free_device(ts->input_dev);

err_input_dev_alloc_failed:
err_alloc_failed:
err_detect_failed:
err_get_platform_data_fail:
	destroy_workqueue(ts->atmel_delayed_wq);
err_create_atmel_delayed_wq_failed:
	destroy_workqueue(ts->atmel_wq);

err_create_atmel_wq_failed:
	kfree(ts);

err_alloc_data_failed:
err_check_functionality_failed:

	return ret;
}

static int atmel_ts_remove(struct i2c_client *client)
{
	struct atmel_ts_data *ts = i2c_get_clientdata(client);

#ifdef ATMEL_EN_SYSFS
	atmel_touch_sysfs_deinit();
#endif

	unregister_early_suspend(&ts->early_suspend);
	free_irq(client->irq, ts);

	destroy_workqueue(ts->atmel_delayed_wq);
	destroy_workqueue(ts->atmel_wq);
	input_unregister_device(ts->input_dev);
	kfree(ts);

	return 0;
}

static int atmel_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct atmel_ts_data *ts = i2c_get_clientdata(client);

	printk(KERN_INFO "%s:[TP]enter\n", __func__);

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
    	scr_suspended = true;
	if (s2w_switch > 0) {
		enable_irq_wake(client->irq);
		printk(KERN_INFO "[sweep2wake]: suspend but keep interupt wake going.\n");
		if (s2w_switch == 2) {
			//ensure backlight is turned off
			pm8058_drvx_led_brightness_set(sweep2wake_leddev, 0);
			printk(KERN_INFO "[sweep2wake]: deactivated button backlight.\n");
		}
 	} else {
#endif
    		disable_irq(client->irq);
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
	}
#endif

	cancel_delayed_work_sync(&ts->unlock_work);
	if (ts->pre_data[0] == RECALIB_UNLOCK && ts->psensor_status)
		confirm_calibration(ts, 0, 3);
	cancel_work_sync(&ts->check_delta_work);

	ts->finger_pressed = 0;
	ts->finger_count = 0;
	ts->first_pressed = 0;

	if (ts->id->version >= 0x20 && ts->psensor_status == 0) {
		ts->pre_data[0] = RECALIB_NEED;
		i2c_atmel_write(client,
			get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) + T8_CFG_ATCHCALST,
			ts->ATCH_EXT, 4);
	}

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
  	if (s2w_switch == 0) {
#endif
		i2c_atmel_write_byte_data(client,
			get_object_address(ts, GEN_POWERCONFIG_T7) + T7_CFG_IDLEACQINT, 0x0);
		i2c_atmel_write_byte_data(client,
			get_object_address(ts, GEN_POWERCONFIG_T7) + T7_CFG_ACTVACQINT, 0x0);
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
 	}
#endif

	printk(KERN_INFO "%s:[TP]done\n", __func__);
	return 0;
}

static int atmel_ts_resume(struct i2c_client *client)
{
	struct atmel_ts_data *ts = i2c_get_clientdata(client);

	printk(KERN_INFO "%s:[TP]enter\n", __func__);

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
    	scr_suspended = false;
	if (s2w_switch > 0) {
    		disable_irq_wake(client->irq);
		printk(KERN_INFO "[sweep2wake]: resume but disable interupt wake.\n");
  	}
#endif

	if (ts->id->version >= 0x20 && ts->pre_data[0] == RECALIB_NEED) {
		if (ts->call_tchthr[0] && ts->psensor_status == 2 && !ts->wlc_status) {
			printk(KERN_INFO "[TP]raise touch threshold\n");
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) + T9_CFG_TCHTHR,
				ts->call_tchthr[ts->status]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) + T8_CFG_ATCHCALSTHR,
				ts->call_tchthr[ts->status] - 5);
		}
		if (ts->locking_config[0]) {
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, TOUCH_MULTITOUCHSCREEN_T9) +
				T9_CFG_MRGTHR,
				ts->locking_config[0]);
		}
	}

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
	if (s2w_switch == 0) {
#endif
		if (!ts->status && ts->wlc_status && ts->wlc_config[0])
			i2c_atmel_write(ts->client,
				get_object_address(ts, GEN_POWERCONFIG_T7),
				ts->wlc_config,
				get_object_size(ts, GEN_POWERCONFIG_T7));
		else
			i2c_atmel_write(ts->client,
				get_object_address(ts, GEN_POWERCONFIG_T7),
				ts->config_setting[ts->status].config_T7,
				get_object_size(ts, GEN_POWERCONFIG_T7));
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
  	}
#endif

	if (ts->id->version == 0x16) {
		if (ts->config_setting[CONNECTED].config[0] && ts->status &&
			!check_delta(ts)) {
			ts->calibration_confirm = 2;
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
				T8_CFG_ATCHCALST,
				ts->config_setting[CONNECTED].config_T8[T8_CFG_ATCHCALST]);
			i2c_atmel_write_byte_data(ts->client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
				T8_CFG_ATCHCALSTHR,
				ts->config_setting[CONNECTED].config_T8[T8_CFG_ATCHCALSTHR]);
		} else {
			ts->calibration_confirm = 0;
			msleep(1);
			i2c_atmel_write_byte_data(client,
				get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
				T6_CFG_CALIBRATE, 0x55);
			i2c_atmel_write_byte_data(client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
				T8_CFG_ATCHCALST, 0x0);
			i2c_atmel_write_byte_data(client,
				get_object_address(ts, GEN_ACQUISITIONCONFIG_T8) +
				T8_CFG_ATCHCALSTHR, 0x0);
		}
	} else {
		if (ts->pre_data[0] != RECALIB_NEED) {
			printk(KERN_INFO "[TP]resume in call, psensor status %d\n",
				ts->psensor_status);
			queue_work(ts->atmel_wq, &ts->check_delta_work);
		} else {
			msleep(1);
			i2c_atmel_write_byte_data(client,
				get_object_address(ts, GEN_COMMANDPROCESSOR_T6) +
				T6_CFG_CALIBRATE, 0x55);
		}
	}

#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
  	if (s2w_switch == 0) {
#endif
		enable_irq(client->irq);
#ifdef CONFIG_TOUCHSCREEN_ATMEL_SWEEP2WAKE
  	}

	if (s2w_switch_changed == true) {
		s2w_switch = s2w_temp;
		s2w_switch_changed = false;
	}
#endif
	printk(KERN_INFO "%s:[TP]done\n", __func__);
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void atmel_ts_early_suspend(struct early_suspend *h)
{
	struct atmel_ts_data *ts;
	ts = container_of(h, struct atmel_ts_data, early_suspend);
	atmel_ts_suspend(ts->client, PMSG_SUSPEND);
}

static void atmel_ts_late_resume(struct early_suspend *h)
{
	struct atmel_ts_data *ts;
	ts = container_of(h, struct atmel_ts_data, early_suspend);
	atmel_ts_resume(ts->client);
}
#endif

static const struct i2c_device_id atml_ts_i2c_id[] = {
	{ ATMEL_QT602240_NAME, 0 },
	{ }
};

static struct i2c_driver atmel_ts_driver = {
	.id_table = atml_ts_i2c_id,
	.probe = atmel_ts_probe,
	.remove = atmel_ts_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = atmel_ts_suspend,
	.resume = atmel_ts_resume,
#endif
	.driver = {
			.name = ATMEL_QT602240_NAME,
	},
};

static int __devinit atmel_ts_init(void)
{
	printk(KERN_INFO "atmel_ts_init():\n");
	return i2c_add_driver(&atmel_ts_driver);
}

static void __exit atmel_ts_exit(void)
{
	i2c_del_driver(&atmel_ts_driver);
}

module_init(atmel_ts_init);
module_exit(atmel_ts_exit);

MODULE_DESCRIPTION("ATMEL Touch driver");
MODULE_LICENSE("GPL");

