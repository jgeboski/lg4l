/***************************************************************************
 *   Copyright (C) 2010 by Alistair Buxton                                 *
 *   a.j.buxton@gmail.com                                                  *
 *   based on hid-g13.c                                                    *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This driver is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   General Public License for more details.                              *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this software. If not see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
#include <linux/fb.h>
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/leds.h>
#include <linux/completion.h>
#include <linux/version.h>

#include "hid-ids.h"
#include "usbhid/usbhid.h"

#include "hid-gcommon.h"

#ifdef __GNUC__
#define __UNUSED __attribute__ ((unused))
#else
#define __UNUSED
#endif

#define G15_NAME "Logitech G15v2"

/* Key defines */
#define G15_KEYS 16
#define G15_KEYMAP_SIZE (G15_KEYS*3)

/* Backlight defaults */
#define G15_DEFAULT_RED (0)
#define G15_DEFAULT_GREEN (255)
#define G15_DEFAULT_BLUE (0)

/* LED array indices */
#define G15_LED_M1 0
#define G15_LED_M2 1
#define G15_LED_M3 2
#define G15_LED_MR 3
#define G15_LED_BL_KEYS 4
#define G15_LED_BL_SCREEN 5
#define G15_LED_BL_CONTRAST 6 /* HACK ALERT contrast is nothing like a LED */

#define G15_REPORT_4_INIT	0x00
#define G15_REPORT_4_FINALIZE	0x01

#define G15_READY_SUBSTAGE_1 0x01
#define G15_READY_SUBSTAGE_2 0x02
#define G15_READY_SUBSTAGE_3 0x04
#define G15_READY_STAGE_1    0x07
#define G15_READY_SUBSTAGE_4 0x08
#define G15_READY_SUBSTAGE_5 0x10
#define G15_READY_STAGE_2    0x1F
#define G15_READY_SUBSTAGE_6 0x20
#define G15_READY_SUBSTAGE_7 0x40
#define G15_READY_STAGE_3    0x7F

#define G15_RESET_POST 0x01
#define G15_RESET_MESSAGE_1 0x02
#define G15_RESET_READY 0x03

/* Per device data structure */
struct g15_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* core state */
	u8 keys_bl;
	u8 screen_bl;
	u8 screen_contrast;
	u8 led;

	/* LED stuff */
	struct led_classdev *led_cdev[7];

	/* Housekeeping stuff */
	struct completion ready;
	int ready_stages;
	int need_reset;
};

/* Convenience macros */
#define hid_get_g15data(hdev) \
	((struct g15_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g15data(dev) \
	((struct g15_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices
 */
static const unsigned int g15_default_key_map[G15_KEYS] = {
KEY_F1,
KEY_F2,
KEY_F3,
KEY_F4,
KEY_F5,
KEY_F6,
KEY_PROG1,
KEY_PROG2,
KEY_KBDILLUMTOGGLE, /* Light */
KEY_LEFT, /* L2 */
KEY_UP, /* L3 */
KEY_DOWN, /* L4 */
KEY_RIGHT, /* L5 */
KEY_PROG3, /* M3 */
KEY_RECORD, /* MR */
KEY_OK /* L1 */
};

static DEVICE_ATTR(fb_node, 0444, gfb_fb_node_show, NULL);

static DEVICE_ATTR(fb_update_rate, 0666,
		   gfb_fb_update_rate_show,
		   gfb_fb_update_rate_store);

static void g15_msg_send(struct hid_device *hdev, u8 msg, u8 value1, u8 value2)
{
	struct g15_data *g15data = hid_get_g15data(hdev);

	g15data->led_report->field[0]->value[0] = msg;
	g15data->led_report->field[0]->value[1] = value1;
	g15data->led_report->field[0]->value[2] = value2;

	usbhid_submit_report(hdev, g15data->led_report, USB_DIR_OUT);
}

static void g15_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value,
			 int led_num)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g15_data *g15data;
	u8 mask;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g15data = hid_get_g15data(hdev);

	mask = 0x01<<led_num;
	if (value)
		g15data->led |= mask;
	else
		g15data->led &= ~mask;

	g15_msg_send(hdev, 0x04, ~(g15data->led), 0);
}

static void g15_led_m1_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g15_led_set(led_cdev, value, G15_LED_M1);
}

static void g15_led_m2_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g15_led_set(led_cdev, value, G15_LED_M2);
}

static void g15_led_m3_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g15_led_set(led_cdev, value, G15_LED_M3);
}

static void g15_led_mr_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g15_led_set(led_cdev, value, G15_LED_MR);
}

static enum led_brightness g15_led_brightness_get(struct led_classdev *led_cdev)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g15_data *g15data;
	int value = 0;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g15data = hid_get_g15data(hdev);

	if (led_cdev == g15data->led_cdev[G15_LED_M1])
		value = g15data->led & 0x01;
	else if (led_cdev == g15data->led_cdev[G15_LED_M2])
		value = g15data->led & 0x02;
	else if (led_cdev == g15data->led_cdev[G15_LED_M3])
		value = g15data->led & 0x04;
	else if (led_cdev == g15data->led_cdev[G15_LED_MR])
		value = g15data->led & 0x08;
	else
		dev_info(dev, G15_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g15_led_bl_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g15_data *g15data;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g15data = hid_get_g15data(hdev);

	if (led_cdev == g15data->led_cdev[G15_LED_BL_KEYS]) {
		if (value > 2)
			value = 2;
		g15data->keys_bl = value;
		g15_msg_send(hdev, 0x01, g15data->keys_bl, 0);
	} else if (led_cdev == g15data->led_cdev[G15_LED_BL_SCREEN]) {
		if (value > 2)
			value = 2;
		g15data->screen_bl = value<<4;
		g15_msg_send(hdev, 0x02, g15data->screen_bl, 0);
	} else if (led_cdev == g15data->led_cdev[G15_LED_BL_CONTRAST]) {
		if (value > 63)
			value = 63;
		g15data->screen_contrast = value;
		g15_msg_send(hdev, 32, 129, g15data->screen_contrast);
	} else
		dev_info(dev, G15_NAME " error retrieving LED brightness\n");

}

static enum led_brightness g15_led_bl_get(struct led_classdev *led_cdev)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g15_data *g15data;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g15data = hid_get_g15data(hdev);

	if (led_cdev == g15data->led_cdev[G15_LED_BL_KEYS])
		return g15data->keys_bl;
	else if (led_cdev == g15data->led_cdev[G15_LED_BL_SCREEN])
		return g15data->screen_bl;
	else if (led_cdev == g15data->led_cdev[G15_LED_BL_CONTRAST])
		return g15data->screen_contrast;
	else
		dev_info(dev, G15_NAME " error retrieving LED brightness\n");

	return LED_OFF;
}

static const struct led_classdev g15_led_cdevs[7] = {
	{
		.brightness_set		= g15_led_m1_brightness_set,
		.brightness_get		= g15_led_brightness_get,
	},
	{
		.brightness_set		= g15_led_m2_brightness_set,
		.brightness_get		= g15_led_brightness_get,
	},
	{
		.brightness_set		= g15_led_m3_brightness_set,
		.brightness_get		= g15_led_brightness_get,
	},
	{
		.brightness_set		= g15_led_mr_brightness_set,
		.brightness_get		= g15_led_brightness_get,
	},
	{
		.brightness_set		= g15_led_bl_set,
		.brightness_get		= g15_led_bl_get,
	},
	{
		.brightness_set		= g15_led_bl_set,
		.brightness_get		= g15_led_bl_get,
	},
	{
		.brightness_set		= g15_led_bl_set,
		.brightness_get		= g15_led_bl_get,
	},
};

static DEVICE_ATTR(keymap_index, 0666,
		   ginput_keymap_index_show,
		   ginput_keymap_index_store);

static DEVICE_ATTR(keymap, 0666, 
                   ginput_keymap_show,
                   ginput_keymap_store);

static DEVICE_ATTR(keymap_switching, 0644,
		   ginput_keymap_switching_show,
		   ginput_keymap_switching_store);

/* change leds when the keymap was changed */
static void g15_notify_keymap_switched(struct gcommon_data * gdata, 
                                       unsigned int index)
{
        struct g15_data * g15data = gdata->data;

        g15data->led = 1 << index;
        g15_msg_send(gdata->hdev, 4, ~g15data->led, 0);
}


static ssize_t g15_name_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	unsigned long irq_flags;
	struct gcommon_data *gdata = dev_get_drvdata(dev);
	int result;

	if (gdata->name == NULL) {
		buf[0] = 0x00;
		return 1;
	}

	spin_lock_irqsave(&gdata->lock, irq_flags);
	result = sprintf(buf, "%s", gdata->name);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return result;
}

static ssize_t g15_name_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long irq_flags;
	struct gcommon_data *gdata = dev_get_drvdata(dev);
	size_t limit = count;
	char *end;

	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (gdata->name != NULL) {
		kfree(gdata->name);
		gdata->name = NULL;
	}

	end = strpbrk(buf, "\n\r");
	if (end != NULL)
		limit = end - buf;

	if (end != buf) {

		if (limit > 100)
			limit = 100;

		gdata->name = kzalloc(limit+1, GFP_ATOMIC);

		strncpy(gdata->name, buf, limit);
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return count;
}

static DEVICE_ATTR(name, 0666, g15_name_show, g15_name_store);

static void g15_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g15_data *gdata = hid_get_g15data(hdev);

	if (which == G15_REPORT_4_INIT) {
		gdata->feature_report_4->field[0]->value[0] = 0x02;
		gdata->feature_report_4->field[0]->value[1] = 0x00;
		gdata->feature_report_4->field[0]->value[2] = 0x00;
		gdata->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G15_REPORT_4_FINALIZE) {
		gdata->feature_report_4->field[0]->value[0] = 0x02;
		gdata->feature_report_4->field[0]->value[1] = 0x80;
		gdata->feature_report_4->field[0]->value[2] = 0x00;
		gdata->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	usbhid_submit_report(hdev, gdata->feature_report_4, USB_DIR_OUT);
}

/*
 * The "minor" attribute
 */
static ssize_t g15_minor_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct gcommon_data *gdata = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", gdata->hdev->minor);
}

static DEVICE_ATTR(minor, 0444, g15_minor_show, NULL);

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *g15_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_keymap_index.attr,
	&dev_attr_keymap_switching.attr,
	&dev_attr_keymap.attr,
	&dev_attr_minor.attr,
	&dev_attr_fb_update_rate.attr,
	&dev_attr_fb_node.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

/*
 * An unnamed attribute group will put all of the attributes directly in
 * the kobject directory.  If we specify a name, a subdirectory will be
 * created for the attributes with the directory being the name of the
 * attribute group.
 */
static struct attribute_group g15_attr_group = {
	.attrs = g15_attrs,
};

static void g15_raw_event_process_input(struct hid_device *hdev,
					struct gcommon_data *gdata,
					u8 *raw_data)
{
	struct input_dev *idev = gdata->input_dev;
        struct ginput_data *input_data = &gdata->input_data;
	int scancode;
	int value;
	int i;
	int mask;

	/*
	 * We'll check for the M* keys being pressed before processing
	 * the remainder of the key data. That way the new keymap will
	 * be loaded if there is a keymap switch.
	 */
	if (unlikely(input_data->keymap_switching)) {
		if (input_data->curkeymap != 0 && raw_data[1] & 0x40)
			ginput_set_keymap_index(gdata, 0);
		else if (input_data->curkeymap != 1 && raw_data[1] & 0x80)
			ginput_set_keymap_index(gdata, 1);
		else if (input_data->curkeymap != 2 && raw_data[2] & 0x20)
			ginput_set_keymap_index(gdata, 2);
	}

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		scancode = i;
		value = raw_data[1] & mask;
		ginput_handle_key_event(gdata, scancode, value);

		scancode = i + 8;
		value = raw_data[2] & mask;
		ginput_handle_key_event(gdata, scancode, value);
	}

	input_sync(idev);
}

static int g15_raw_event(struct hid_device *hdev,
			 struct hid_report *report,
			 u8 *raw_data, int size)
{
	/*
	* On initialization receive a 258 byte message with
	* data = 6 0 255 255 255 255 255 255 255 255 ...
	*/
	unsigned long irq_flags;
	struct gcommon_data *gdata = dev_get_gdata(&hdev->dev);
	struct g15_data *g15data = gdata->data;

	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (unlikely(g15data->need_reset)) {
		g15_msg_send(hdev, 4, ~g15data->led, 0);
		g15data->need_reset = 0;
		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	if (unlikely(g15data->ready_stages != G15_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g15data->ready_stages & G15_READY_SUBSTAGE_1))
				g15data->ready_stages |= G15_READY_SUBSTAGE_1;
			else if (g15data->ready_stages & G15_READY_SUBSTAGE_4 &&
				 !(g15data->ready_stages & G15_READY_SUBSTAGE_5)
				)
				g15data->ready_stages |= G15_READY_SUBSTAGE_5;
			else if (g15data->ready_stages & G15_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g15data->ready_stages |= G15_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g15data->ready_stages & G15_READY_SUBSTAGE_2))
				g15data->ready_stages |= G15_READY_SUBSTAGE_2;
			else
				g15data->ready_stages |= G15_READY_SUBSTAGE_3;
			break;
		}

		if (g15data->ready_stages == G15_READY_STAGE_1 ||
		    g15data->ready_stages == G15_READY_STAGE_2 ||
		    g15data->ready_stages == G15_READY_STAGE_3)
			complete_all(&g15data->ready);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	if (likely(report->id == 2)) {
		g15_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}

static void g15_initialize_keymap(struct gcommon_data *gdata)
{
	int i;

	for (i = 0; i < G15_KEYS; i++) {
		gdata->input_data.keycode[i] = g15_default_key_map[i];
		__set_bit(gdata->input_data.keycode[i], gdata->input_dev->keybit);
	}

	__clear_bit(KEY_RESERVED, gdata->input_dev->keybit);
}

static int g15_probe(struct hid_device *hdev,
		     const struct hid_device_id *id)
{
	unsigned long irq_flags;
	int error;
	struct gcommon_data *gdata;
	struct g15_data *g15data;
	int i;
	int led_num;
	struct usb_interface *intf;
	struct usb_device *usbdev;
	struct list_head *feature_report_list =
		&hdev->report_enum[HID_FEATURE_REPORT].report_list;
	struct list_head *output_report_list =
			&hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report;
	char *led_name;

	dev_dbg(&hdev->dev, "Logitech G15v2 HID hardware probe...");

	/* Get the usb device to send the start report on */
	intf = to_usb_interface(hdev->dev.parent);
	usbdev = interface_to_usbdev(intf);

	/*
	 * Let's allocate the g15 data structure, set some reasonable
	 * defaults, and associate it with the device
	 */
	gdata = kzalloc(sizeof(struct g15_data), GFP_KERNEL);
	if (gdata == NULL) {
		dev_err(&hdev->dev, "can't allocate space for Logitech G15v2 device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g15data = kzalloc(sizeof(struct g15_data), GFP_KERNEL);
	if (g15data == NULL) {
		dev_err(&hdev->dev, "can't allocate space for Logitech G15 device attributes\n");
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
        gdata->data = g15data;

	spin_lock_init(&gdata->lock);

	init_completion(&g15data->ready);

	gdata->hdev = hdev;

	hid_set_drvdata(hdev, gdata);

	dbg_hid("Preparing to parse " G15_NAME " hid reports\n");

	/* Parse the device reports and start it up */
	error = hid_parse(hdev);
	if (error) {
		dev_err(&hdev->dev, G15_NAME " device report parse failed\n");
		error = -EINVAL;
		goto err_cleanup_g15data;
	}

	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT | HID_CONNECT_HIDINPUT_FORCE);
	if (error) {
		dev_err(&hdev->dev, G15_NAME " hardware start failed\n");
		error = -EINVAL;
		goto err_cleanup_g15data;
	}

	dbg_hid(G15_NAME " claimed: %d\n", hdev->claimed);

	error = hdev->ll_driver->open(hdev);
	if (error) {
		dev_err(&hdev->dev, G15_NAME " failed to open input interrupt pipe for key and joystick events\n");
		error = -EINVAL;
		goto err_cleanup_g15data;
	}

	/* Set up the input device for the key I/O */
	gdata->input_dev = input_allocate_device();
	if (gdata->input_dev == NULL) {
		dev_err(&hdev->dev, G15_NAME " error initializing the input device");
		error = -ENOMEM;
		goto err_cleanup_g15data;
	}

	input_set_drvdata(gdata->input_dev, gdata);

	gdata->input_dev->name = G15_NAME;
	gdata->input_dev->phys = hdev->phys;
	gdata->input_dev->uniq = hdev->uniq;
	gdata->input_dev->id.bustype = hdev->bus;
	gdata->input_dev->id.vendor = hdev->vendor;
	gdata->input_dev->id.product = hdev->product;
	gdata->input_dev->id.version = hdev->version;
	gdata->input_dev->dev.parent = hdev->dev.parent;
	gdata->input_dev->keycode = gdata->input_data.keycode;
	gdata->input_dev->keycodemax = G15_KEYMAP_SIZE;
	gdata->input_dev->keycodesize = sizeof(int);
	gdata->input_dev->setkeycode = ginput_setkeycode;
	gdata->input_dev->getkeycode = ginput_getkeycode;

	input_set_capability(gdata->input_dev, EV_KEY, KEY_UNKNOWN);
	gdata->input_dev->evbit[0] |= BIT_MASK(EV_REP);

        gdata->input_data.notify_keymap_switched = g15_notify_keymap_switched;

        error = ginput_alloc(gdata, G15_KEYS);
        if (error) {
		dev_err(&hdev->dev, G15_NAME " error allocating memory for the input device");
                goto err_cleanup_input_dev;
        }

	g15_initialize_keymap(gdata);

	error = input_register_device(gdata->input_dev);
	if (error) {
		dev_err(&hdev->dev, G15_NAME " error registering the input device");
		error = -EINVAL;
		goto err_cleanup_input_dev_data;
	}

	dbg_hid(KERN_INFO G15_NAME " allocated framebuffer\n");

	dbg_hid(KERN_INFO G15_NAME " allocated deferred IO structure\n");

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev, "no feature report found\n");
		error = -ENODEV;
		goto err_cleanup_input_dev_reg;
	}
	dbg_hid(G15_NAME " feature report found\n");

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x02: /* G15 has only one feature report 0x02 */
			g15data->feature_report_4 
                          = g15data->led_report
                          = g15data->start_input_report
                          = g15data->backlight_report
                          = report;
			break;
		default:
			break;
		}
		dbg_hid(G15_NAME " Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	if (list_empty(output_report_list)) {
		dev_err(&hdev->dev, "no output report found\n");
		error = -ENODEV;
		goto err_cleanup_input_dev_reg;
	}
	dbg_hid(G15_NAME " output report found\n");

	list_for_each_entry(report, output_report_list, list) {
		dbg_hid(G15_NAME " output report %d found size=%u maxfield=%u\n", report->id, report->size, report->maxfield);
		if (report->maxfield > 0) {
			dbg_hid(G15_NAME " offset=%u size=%u count=%u type=%u\n",
			       report->field[0]->report_offset,
			       report->field[0]->report_size,
			       report->field[0]->report_count,
			       report->field[0]->report_type);
		}
		switch (report->id) {
		case 0x03:
			g15data->output_report_3 = report;
			break;
		}
	}

	dbg_hid("Found all reports\n");

	/* Create the LED structures */
	for (i = 0; i < 7; i++) {
		g15data->led_cdev[i] = kzalloc(sizeof(struct led_classdev), GFP_KERNEL);
		if (g15data->led_cdev[i] == NULL) {
			dev_err(&hdev->dev, G15_NAME " error allocating memory for led %d", i);
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		/* Set the accessor functions by copying from template*/
		*(g15data->led_cdev[i]) = g15_led_cdevs[i];

		/*
		 * Allocate memory for the LED name
		 *
		 * Since led_classdev->name is a const char* we'll use an
		 * intermediate until the name is formatted with sprintf().
		 */
		led_name = kzalloc(sizeof(char)*30, GFP_KERNEL);
		if (led_name == NULL) {
			dev_err(&hdev->dev, G15_NAME " error allocating memory for led %d name", i);
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		switch (i) {
		case 0:
		case 1:
		case 2:
			sprintf(led_name, "g15v2_%d:red:m%d", hdev->minor, i+1);
			break;
		case 3:
			sprintf(led_name, "g15v2_%d:blue:mr", hdev->minor);
			break;
		case 4:
			sprintf(led_name, "g15v2_%d:orange:keys", hdev->minor);
			break;
		case 5:
			sprintf(led_name, "g15v2_%d:white:screen", hdev->minor);
			break;
		case 6:
			sprintf(led_name, "g15v2_%d:contrast:screen", hdev->minor);
			break;
		}
		g15data->led_cdev[i]->name = led_name;
	}

	for (i = 0; i < 7; i++) {
		led_num = i;
		error = led_classdev_register(&hdev->dev, g15data->led_cdev[i]);
		if (error < 0) {
			dev_err(&hdev->dev, G15_NAME " error registering led %d\n", i);
			error = -EINVAL;
			goto err_cleanup_registered_leds;
		}
	}

	gdata->gfb_data = gfb_probe(hdev, GFB_PANEL_TYPE_160_43_1);
	if (gdata->gfb_data == NULL) {
		dev_err(&hdev->dev, G15_NAME " error registering framebuffer\n");
		goto err_cleanup_registered_leds;
	}

	dbg_hid("Waiting for G15v2 to activate\n");

	/* Add the sysfs attributes */
	error = sysfs_create_group(&(hdev->dev.kobj), &g15_attr_group);
	if (error) {
		dev_err(&hdev->dev, G15_NAME " failed to create sysfs group attributes\n");
		goto err_cleanup_registered_leds;
	}

	/*
	 * Wait here for stage 1 (substages 1-3) to complete
	 */
	wait_for_completion_timeout(&g15data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g15data->ready_stages != G15_READY_STAGE_1) {
		dev_warn(&hdev->dev, G15_NAME " hasn't completed stage 1 yet, forging ahead with initialization\n");
		/* Force the stage */
		g15data->ready_stages = G15_READY_STAGE_1;
	}
	init_completion(&g15data->ready);
	g15data->ready_stages |= G15_READY_SUBSTAGE_4;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g15_feature_report_4_send(hdev, G15_REPORT_4_INIT);
	usbhid_submit_report(hdev, g15data->start_input_report, USB_DIR_IN);
	wait_for_completion_timeout(&g15data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g15data->ready_stages != G15_READY_STAGE_2) {
		dev_warn(&hdev->dev, G15_NAME " hasn't completed stage 2 yet, forging ahead with initialization\n");
		/* Force the stage */
		g15data->ready_stages = G15_READY_STAGE_2;
	}
	init_completion(&g15data->ready);
	g15data->ready_stages |= G15_READY_SUBSTAGE_6;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Clear the LEDs
	 */
	g15_msg_send(hdev, 4, ~g15data->led, 0);

	/*
	 * Send the finalize report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g15_feature_report_4_send(hdev, G15_REPORT_4_FINALIZE);
	usbhid_submit_report(hdev, g15data->start_input_report, USB_DIR_IN);
	usbhid_submit_report(hdev, g15data->start_input_report, USB_DIR_IN);
	wait_for_completion_timeout(&g15data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (g15data->ready_stages != G15_READY_STAGE_3) {
		dev_warn(&hdev->dev, G15_NAME " hasn't completed stage 3 yet, forging ahead with initialization\n");
		/* Force the stage */
		g15data->ready_stages = G15_READY_STAGE_3;
	} else {
		dbg_hid(G15_NAME " stage 3 complete\n");
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	ginput_set_keymap_switching(gdata, 1);

	dbg_hid("G15v2 activated and initialized\n");

	/* Everything went well */
	return 0;

err_cleanup_registered_leds:
	for (i = 0; i < led_num; i++)
		led_classdev_unregister(g15data->led_cdev[i]);

err_cleanup_led_structs:
	for (i = 0; i < 7; i++) {
		if (g15data->led_cdev[i] != NULL) {
			if (g15data->led_cdev[i]->name != NULL)
				kfree(g15data->led_cdev[i]->name);
			kfree(g15data->led_cdev[i]);
		}
	}

err_cleanup_input_dev_reg:
	input_unregister_device(gdata->input_dev);

err_cleanup_input_dev_data:
        ginput_free(gdata);

err_cleanup_input_dev:
	input_free_device(gdata->input_dev);

err_cleanup_g15data:
	kfree(g15data);

err_cleanup_gdata:
	/* Make sure we clean up the allocated data structure */
	kfree(gdata);

err_no_cleanup:

	hid_set_drvdata(hdev, NULL);

	return error;
}

static void g15_remove(struct hid_device *hdev)
{
	int i;
	struct gcommon_data *gdata = hid_get_drvdata(hdev);
	struct g15_data *g15data = gdata->data;

	input_unregister_device(gdata->input_dev);
        ginput_free(gdata);

	kfree(gdata->name);

	/* Clean up the leds */
	for (i = 0; i < 7; i++) {
		led_classdev_unregister(g15data->led_cdev[i]);
		kfree(g15data->led_cdev[i]->name);
		kfree(g15data->led_cdev[i]);
	}

	gfb_remove(gdata->gfb_data);

	hdev->ll_driver->close(hdev);

	hid_hw_stop(hdev);

	sysfs_remove_group(&(hdev->dev.kobj), &g15_attr_group);

	/* Finally, clean up the g15 data itself */
	kfree(g15data);
	kfree(gdata);
}

static void __UNUSED g15_post_reset_start(struct hid_device *hdev)
{
	unsigned long irq_flags;
	struct gcommon_data *gdata = hid_get_gdata(hdev);
	struct g15_data *g15data = gdata->data;

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g15data->need_reset = 1;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static const struct hid_device_id g15_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G15V2_LCD)
	},
	{ }
};
MODULE_DEVICE_TABLE(hid, g15_devices);

static struct hid_driver g15_driver = {
	.name			= "hid-g15v2",
	.id_table		= g15_devices,
	.probe			= g15_probe,
	.remove			= g15_remove,
	.raw_event		= g15_raw_event,
};

static int __init g15_init(void)
{
	return hid_register_driver(&g15_driver);
}

static void __exit g15_exit(void)
{
	hid_unregister_driver(&g15_driver);
}

module_init(g15_init);
module_exit(g15_exit);
MODULE_DESCRIPTION("Logitech G15v2 HID Driver");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_LICENSE("GPL");
