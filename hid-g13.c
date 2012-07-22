/***************************************************************************
 *   Copyright (C) 2009 by Rick L. Vinyard, Jr.                            *
 *   rvinyard@cs.nmsu.edu                                                  *
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

#define G13_NAME "Logitech G13"

/* Key defines */
#define G13_KEYS 35
#define G13_KEYMAP_SIZE (G13_KEYS*3)

/* Framebuffer defines */
#define G13FB_NAME "g13fb"
#define G13FB_WIDTH (160)
#define G13FB_LINE_LENGTH (160/8)
#define G13FB_HEIGHT (43)
#define G13FB_SIZE (G13FB_LINE_LENGTH*G13FB_HEIGHT)

#define G13FB_UPDATE_RATE_LIMIT (20)
#define G13FB_UPDATE_RATE_DEFAULT (10)

/*
 * The native G13 format uses vertical bits. Therefore the number of bytes
 * needed to represent the first column is 43/8 (rows/bits) rounded up.
 * Additionally, the format requires a padding of 32 bits in front of the
 * image data.
 *
 * Therefore the vbitmap size must be:
 *   160 * ceil(43/8) + 32 = 160 * 6 + 32 = 992
 */
#define G13_VBITMAP_SIZE (992)

/* Backlight defaults */
#define G13_DEFAULT_RED (0)
#define G13_DEFAULT_GREEN (255)
#define G13_DEFAULT_BLUE (0)

#define LED_COUNT 7

/* LED array indices */
#define G13_LED_M1 0
#define G13_LED_M2 1
#define G13_LED_M3 2
#define G13_LED_MR 3
#define G13_LED_BL_R 4
#define G13_LED_BL_G 5
#define G13_LED_BL_B 6

#define G13_REPORT_4_INIT	0x00
#define G13_REPORT_4_FINALIZE	0x01

#define G13_READY_SUBSTAGE_1 0x01
#define G13_READY_SUBSTAGE_2 0x02
#define G13_READY_SUBSTAGE_3 0x04
#define G13_READY_STAGE_1    0x07
#define G13_READY_SUBSTAGE_4 0x08
#define G13_READY_SUBSTAGE_5 0x10
#define G13_READY_STAGE_2    0x1F
#define G13_READY_SUBSTAGE_6 0x20
#define G13_READY_SUBSTAGE_7 0x40
#define G13_READY_STAGE_3    0x7F

#define G13_RESET_POST 0x01
#define G13_RESET_MESSAGE_1 0x02
#define G13_RESET_READY 0x03

/* Per device data structure */
struct g13_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* core state */
	u8 rgb[3];
	u8 led;

	/* LED stuff */
	struct led_classdev *led_cdev[LED_COUNT];

	/* Housekeeping stuff */
	struct completion ready;
	int ready_stages;
	int need_reset;
};

/* Convenience macros */
/* Convenience macros */
#define hid_get_g13data(hdev) \
	((struct g13_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g19data(dev) \
	((struct g13_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices
 *
 * Key        Index
 * ---------  ------
 * G1-G22     0-21
 * FUNC       22
 * LCD1       23
 * LCD2       24
 * LCD3       25
 * LCD4       26
 * M1         27
 * M2         28
 * M3         29
 * MR         30
 * BTN_LEFT   31
 * BTN_DOWN   32
 * BTN_STICK  33
 * LIGHT      34
 */
static const unsigned int g13_default_key_map[G13_KEYS] = {
/* first row g1 - g7 */
KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7,
/* second row g8 - g11 */
KEY_F8, KEY_F9, KEY_F10, KEY_F11,
/* second row g12 - g14 */
KEY_F12, KEY_F13, KEY_F14,
/* third row g15 - g19 */
KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19,
/* fourth row g20 - g22 */
KEY_F20, KEY_F21, KEY_F22,
/* next, light left, light center left, light center right, light right */
/* BTN_0, BTN_1, BTN_2, BTN_3, BTN_4, */
KEY_OK, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_RIGHT,
/* M1, M2, M3, MR */
KEY_PROG1, KEY_PROG2, KEY_PROG3, KEY_RECORD ,
/* button left, button down, button stick, light */
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, KEY_KBDILLUMTOGGLE
};


static DEVICE_ATTR(fb_node, 0444, gfb_fb_node_show, NULL);
static DEVICE_ATTR(fb_update_rate, 0666,
		   gfb_fb_update_rate_show,
		   gfb_fb_update_rate_store);

static void g13_led_send(struct hid_device *hdev)
{
	struct g13_data *g13data = hid_get_g13data(hdev);

	g13data->led_report->field[0]->value[0] = g13data->led&0x0F;
	g13data->led_report->field[0]->value[1] = 0x00;
	g13data->led_report->field[0]->value[2] = 0x00;
	g13data->led_report->field[0]->value[3] = 0x00;

	usbhid_submit_report(hdev, g13data->led_report, USB_DIR_OUT);
}

static void g13_led_set(struct led_classdev *led_cdev,
                        enum led_brightness value,
                        int led_num)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g13_data *g13data;
	u8 mask;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g13data = hid_get_g13data(hdev);

	mask = 0x01<<led_num;
	if (value)
		g13data->led |= mask;
	else
		g13data->led &= ~mask;

	g13_led_send(hdev);
}

static void g13_led_m1_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g13_led_set(led_cdev, value, G13_LED_M1);
}

static void g13_led_m2_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g13_led_set(led_cdev, value, G13_LED_M2);
}

static void g13_led_m3_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g13_led_set(led_cdev, value, G13_LED_M3);
}

static void g13_led_mr_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g13_led_set(led_cdev, value, G13_LED_MR);
}

static enum led_brightness g13_led_brightness_get(struct led_classdev *led_cdev)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g13_data *g13data;
	int value = 0;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g13data = hid_get_g13data(hdev);

	if (led_cdev == g13data->led_cdev[G13_LED_M1])
		value = g13data->led & 0x01;
	else if (led_cdev == g13data->led_cdev[G13_LED_M2])
		value = g13data->led & 0x02;
	else if (led_cdev == g13data->led_cdev[G13_LED_M3])
		value = g13data->led & 0x04;
	else if (led_cdev == g13data->led_cdev[G13_LED_MR])
		value = g13data->led & 0x08;
	else
		dev_info(dev, G13_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g13_rgb_send(struct hid_device *hdev)
{
	struct g13_data *g13data = hid_get_g13data(hdev);

	g13data->backlight_report->field[0]->value[0] = g13data->rgb[0];
	g13data->backlight_report->field[0]->value[1] = g13data->rgb[1];
	g13data->backlight_report->field[0]->value[2] = g13data->rgb[2];
	g13data->backlight_report->field[0]->value[3] = 0x00;

	usbhid_submit_report(hdev, g13data->backlight_report, USB_DIR_OUT);
}

static void g13_led_bl_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g13_data *g13data;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g13data = hid_get_g13data(hdev);

	if (led_cdev == g13data->led_cdev[G13_LED_BL_R])
		g13data->rgb[0] = value;
	else if (led_cdev == g13data->led_cdev[G13_LED_BL_G])
		g13data->rgb[1] = value;
	else if (led_cdev == g13data->led_cdev[G13_LED_BL_B])
		g13data->rgb[2] = value;

	g13_rgb_send(hdev);
}

static enum led_brightness g13_led_bl_brightness_get(struct led_classdev *led_cdev)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g13_data *g13data;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g13data = hid_get_g13data(hdev);

	if (led_cdev == g13data->led_cdev[G13_LED_BL_R])
		return g13data->rgb[0];
	else if (led_cdev == g13data->led_cdev[G13_LED_BL_G])
		return g13data->rgb[1];
	else if (led_cdev == g13data->led_cdev[G13_LED_BL_B])
		return g13data->rgb[2];
	else
		dev_info(dev, G13_NAME " error retrieving LED brightness\n");

	return LED_OFF;
}

static const struct led_classdev g13_led_cdevs[LED_COUNT] = {
	{
		.brightness_set		= g13_led_m1_brightness_set,
		.brightness_get		= g13_led_brightness_get,
	},
	{
		.brightness_set		= g13_led_m2_brightness_set,
		.brightness_get		= g13_led_brightness_get,
	},
	{
		.brightness_set		= g13_led_m3_brightness_set,
		.brightness_get		= g13_led_brightness_get,
	},
	{
		.brightness_set		= g13_led_mr_brightness_set,
		.brightness_get		= g13_led_brightness_get,
	},
	{
		.brightness_set		= g13_led_bl_brightness_set,
		.brightness_get		= g13_led_bl_brightness_get,
	},
	{
		.brightness_set		= g13_led_bl_brightness_set,
		.brightness_get		= g13_led_bl_brightness_get,
	},
	{
		.brightness_set		= g13_led_bl_brightness_set,
		.brightness_get		= g13_led_bl_brightness_get,
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
static void g13_notify_keymap_switched(struct gcommon_data * gdata, 
                                       unsigned int index)
{
        struct g13_data * g13data = hid_get_g13data(gdata->hdev);

        g13data->led = 1 << index;
        g13_led_send(gdata->hdev);
}

static ssize_t g13_name_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
        unsigned long irq_flags;
	struct gcommon_data *gdata = dev_get_gdata(dev);
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

static ssize_t g13_name_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
        unsigned long irq_flags;
	struct gcommon_data *gdata = dev_get_gdata(dev);
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

static DEVICE_ATTR(name, 0666, g13_name_show, g13_name_store);

static void g13_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g13_data *g13data = hid_get_g13data(hdev);

	if (which == G13_REPORT_4_INIT) {
		g13data->feature_report_4->field[0]->value[0] = 0x02;
		g13data->feature_report_4->field[0]->value[1] = 0x00;
		g13data->feature_report_4->field[0]->value[2] = 0x00;
		g13data->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G13_REPORT_4_FINALIZE) {
		g13data->feature_report_4->field[0]->value[0] = 0x02;
		g13data->feature_report_4->field[0]->value[1] = 0x80;
		g13data->feature_report_4->field[0]->value[2] = 0x00;
		g13data->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	usbhid_submit_report(hdev, g13data->feature_report_4, USB_DIR_OUT);
}

/*
 * The "minor" attribute
 */
static ssize_t g13_minor_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct gcommon_data *gdata = dev_get_gdata(dev);

	return sprintf(buf, "%d\n", gdata->hdev->minor);
}

static DEVICE_ATTR(minor, 0444, g13_minor_show, NULL);

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *g13_attrs[] = {
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
static struct attribute_group g13_attr_group = {
	.attrs = g13_attrs,
};


static void g13_raw_event_process_input(struct hid_device *hdev,
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
		if (input_data->curkeymap != 0 && raw_data[6] & 0x20)
			ginput_set_keymap_index(gdata, 0);
		else if (input_data->curkeymap != 1 && raw_data[6] & 0x40)
			ginput_set_keymap_index(gdata, 1);
		else if (input_data->curkeymap != 2 && raw_data[6] & 0x80)
			ginput_set_keymap_index(gdata, 2);
	}

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		/* Keys G1 through G8 */
		scancode = i;
		value = raw_data[3] & mask;
		ginput_handle_key_event(gdata, scancode, value);

		/* Keys G9 through G16 */
		scancode = i + 8;
		value = raw_data[4] & mask;
		ginput_handle_key_event(gdata, scancode, value);

		/* Keys G17 through G22 */
		scancode = i + 16;
		value = raw_data[5] & mask;
		if (i <= 5)
			ginput_handle_key_event(gdata, scancode, value);

		/* Keys FUNC through M3 */
		scancode = i + 22;
		value = raw_data[6] & mask;
		ginput_handle_key_event(gdata, scancode, value);

		/* Keys MR through LIGHT */
		scancode = i + 30;
		value = raw_data[7] & mask;
		if (i <= 4)
			ginput_handle_key_event(gdata, scancode, value);
	}

	input_report_abs(idev, ABS_X, raw_data[1]);
	input_report_abs(idev, ABS_Y, raw_data[2]);
	input_sync(idev);
}

static int g13_raw_event(struct hid_device *hdev,
			 struct hid_report *report,
			 u8 *raw_data, int size)
{
        unsigned long irq_flags;

	/*
 	 * On initialization receive a 258 byte message with
	 * data = 6 0 255 255 255 255 255 255 255 255 ...
	 */
	struct gcommon_data *gdata = dev_get_gdata(&hdev->dev);
	struct g13_data *g13data = gdata->data;

	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (unlikely(g13data->need_reset)) {
		g13_rgb_send(hdev);
		g13_led_send(hdev);
		g13data->need_reset = 0;
		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	if (unlikely(g13data->ready_stages != G13_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g13data->ready_stages & G13_READY_SUBSTAGE_1))
				g13data->ready_stages |= G13_READY_SUBSTAGE_1;
			else if (g13data->ready_stages & G13_READY_SUBSTAGE_4 &&
				 !(g13data->ready_stages & G13_READY_SUBSTAGE_5)
				)
				g13data->ready_stages |= G13_READY_SUBSTAGE_5;
			else if (g13data->ready_stages & G13_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g13data->ready_stages |= G13_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g13data->ready_stages & G13_READY_SUBSTAGE_2))
				g13data->ready_stages |= G13_READY_SUBSTAGE_2;
			else
				g13data->ready_stages |= G13_READY_SUBSTAGE_3;
			break;
		}

		if (g13data->ready_stages == G13_READY_STAGE_1 ||
		    g13data->ready_stages == G13_READY_STAGE_2 ||
		    g13data->ready_stages == G13_READY_STAGE_3)
			complete_all(&g13data->ready);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	if (likely(report->id == 1)) {
		g13_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}

static void g13_initialize_keymap(struct gcommon_data *gdata)
{
	int i;

	for (i = 0; i < G13_KEYS; i++) {
		gdata->input_data.keycode[i] = g13_default_key_map[i];
		__set_bit(gdata->input_data.keycode[i], gdata->input_dev->keybit);
	}

	__clear_bit(KEY_RESERVED, gdata->input_dev->keybit);
}

static int g13_probe(struct hid_device *hdev,
		     const struct hid_device_id *id)
{
        unsigned long irq_flags;
	int error;
        struct gcommon_data *gdata;
	struct g13_data *g13data;
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

	dev_dbg(&hdev->dev, "Logitech G13 HID hardware probe...");

	/* Get the usb device to send the start report on */
	intf = to_usb_interface(hdev->dev.parent);
	usbdev = interface_to_usbdev(intf);

	/*
	 * Let's allocate the g13 data structure, set some reasonable
	 * defaults, and associate it with the device
	 */
	gdata = kzalloc(sizeof(struct gcommon_data), GFP_KERNEL);
	if (gdata == NULL) {
		dev_err(&hdev->dev, "can't allocate space for Logitech G13 device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g13data = kzalloc(sizeof(struct g13_data), GFP_KERNEL);
	if (g13data == NULL) {
		dev_err(&hdev->dev, "can't allocate space for Logitech G13 device attributes\n");
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
        gdata->data = g13data;

	spin_lock_init(&gdata->lock);

	init_completion(&g13data->ready);

	gdata->hdev = hdev;

	hid_set_drvdata(hdev, gdata);

	dbg_hid("Preparing to parse " G13_NAME " hid reports\n");

	/* Parse the device reports and start it up */
	error = hid_parse(hdev);
	if (error) {
		dev_err(&hdev->dev, G13_NAME " device report parse failed\n");
		error = -EINVAL;
		goto err_cleanup_g13data;
	}

	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT | HID_CONNECT_HIDINPUT_FORCE);
	if (error) {
		dev_err(&hdev->dev, G13_NAME " hardware start failed\n");
		error = -EINVAL;
		goto err_cleanup_g13data;
	}

	dbg_hid(G13_NAME " claimed: %d\n", hdev->claimed);

	error = hdev->ll_driver->open(hdev);
	if (error) {
		dev_err(&hdev->dev, G13_NAME " failed to open input interrupt pipe for key and joystick events\n");
		error = -EINVAL;
		goto err_cleanup_g13data;
	}

	/* Set up the input device for the key I/O */
	gdata->input_dev = input_allocate_device();
	if (gdata->input_dev == NULL) {
		dev_err(&hdev->dev, G13_NAME " error initializing the input device");
		error = -ENOMEM;
		goto err_cleanup_g13data;
	}

	input_set_drvdata(gdata->input_dev, gdata);

	gdata->input_dev->name = G13_NAME;
	gdata->input_dev->phys = hdev->phys;
	gdata->input_dev->uniq = hdev->uniq;
	gdata->input_dev->id.bustype = hdev->bus;
	gdata->input_dev->id.vendor = hdev->vendor;
	gdata->input_dev->id.product = hdev->product;
	gdata->input_dev->id.version = hdev->version;
	gdata->input_dev->dev.parent = hdev->dev.parent;
	gdata->input_dev->keycode = gdata->input_data.keycode;
	gdata->input_dev->keycodemax = G13_KEYMAP_SIZE;
	gdata->input_dev->keycodesize = sizeof(int);
	gdata->input_dev->setkeycode = ginput_setkeycode;
	gdata->input_dev->getkeycode = ginput_getkeycode;

	input_set_capability(gdata->input_dev, EV_ABS, ABS_X);
	input_set_capability(gdata->input_dev, EV_ABS, ABS_Y);
	input_set_capability(gdata->input_dev, EV_MSC, MSC_SCAN);
	input_set_capability(gdata->input_dev, EV_KEY, KEY_UNKNOWN);
	gdata->input_dev->evbit[0] |= BIT_MASK(EV_REP);

	/* 4 center values */
	input_set_abs_params(gdata->input_dev, ABS_X, 0, 0xff, 0, 4);
	input_set_abs_params(gdata->input_dev, ABS_Y, 0, 0xff, 0, 4);

        gdata->input_data.notify_keymap_switched = g13_notify_keymap_switched;

        error = ginput_alloc(gdata, G13_KEYS);
        if (error) {
		dev_err(&hdev->dev, G13_NAME " error allocating memory for the input device");
                goto err_cleanup_input_dev;
        }

	g13_initialize_keymap(gdata);

	error = input_register_device(gdata->input_dev);
	if (error) {
		dev_err(&hdev->dev, G13_NAME " error registering the input device");
		error = -EINVAL;
		goto err_cleanup_input_dev_data;
	}

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev, "no feature report found\n");
		error = -ENODEV;
		goto err_cleanup_input_dev_reg;
	}
	dbg_hid(G13_NAME " feature report found\n");

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x04:
			g13data->feature_report_4 = report;
			break;
		case 0x05:
			g13data->led_report = report;
			break;
		case 0x06:
			g13data->start_input_report = report;
			break;
		case 0x07:
			g13data->backlight_report = report;
			break;
		default:
			break;
		}
		dbg_hid(G13_NAME " Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	if (list_empty(output_report_list)) {
		dev_err(&hdev->dev, "no output report found\n");
		error = -ENODEV;
		goto err_cleanup_input_dev_reg;
	}
	dbg_hid(G13_NAME " output report found\n");

	list_for_each_entry(report, output_report_list, list) {
		dbg_hid(G13_NAME " output report %d found size=%u maxfield=%u\n", report->id, report->size, report->maxfield);
		if (report->maxfield > 0) {
			dbg_hid(G13_NAME " offset=%u size=%u count=%u type=%u\n",
			       report->field[0]->report_offset,
			       report->field[0]->report_size,
			       report->field[0]->report_count,
			       report->field[0]->report_type);
		}
		switch (report->id) {
		case 0x03:
			g13data->output_report_3 = report;
			break;
		}
	}

	dbg_hid("Found all reports\n");

	/* Create the LED structures */
	for (i = 0; i < LED_COUNT; i++) {
		g13data->led_cdev[i] = kzalloc(sizeof(struct led_classdev), GFP_KERNEL);
		if (g13data->led_cdev[i] == NULL) {
			dev_err(&hdev->dev, G13_NAME " error allocating memory for led %d", i);
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		/* Set the accessor functions by copying from template*/
		*(g13data->led_cdev[i]) = g13_led_cdevs[i];

		/*
		 * Allocate memory for the LED name
		 *
		 * Since led_classdev->name is a const char* we'll use an
		 * intermediate until the name is formatted with sprintf().
		 */
		led_name = kzalloc(sizeof(char)*15, GFP_KERNEL);
		if (led_name == NULL) {
			dev_err(&hdev->dev, G13_NAME " error allocating memory for led %d name", i);
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		switch (i) {
		case 0:
		case 1:
		case 2:
			sprintf(led_name, "g13_%d:red:m%d", hdev->minor, i+1);
			break;
		case 3:
			sprintf(led_name, "g13_%d:red:mr", hdev->minor);
			break;
		case 4:
			sprintf(led_name, "g13_%d:red:bl", hdev->minor);
			break;
		case 5:
			sprintf(led_name, "g13_%d:green:bl", hdev->minor);
			break;
		case 6:
			sprintf(led_name, "g13_%d:blue:bl", hdev->minor);
			break;
		}
		g13data->led_cdev[i]->name = led_name;
	}

	for (i = 0; i < LED_COUNT; i++) {
		led_num = i;
		error = led_classdev_register(&hdev->dev, g13data->led_cdev[i]);
		if (error < 0) {
			dev_err(&hdev->dev, G13_NAME " error registering led %d", i);
			error = -EINVAL;
			goto err_cleanup_registered_leds;
		}
	}

	gdata->gfb_data = gfb_probe(hdev, GFB_PANEL_TYPE_160_43_1);
	if (gdata->gfb_data == NULL) {
		dev_err(&hdev->dev, G13_NAME " error registering framebuffer\n");
		goto err_cleanup_registered_leds;
	}

	dbg_hid("Waiting for G13 to activate\n");

	/* Add the sysfs attributes */
	error = sysfs_create_group(&(hdev->dev.kobj), &g13_attr_group);
	if (error) {
		dev_err(&hdev->dev, G13_NAME " failed to create sysfs group attributes\n");
		goto err_cleanup_registered_leds;
	}

	/*
	 * Wait here for stage 1 (substages 1-3) to complete
	 */
	wait_for_completion_timeout(&g13data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g13data->ready_stages != G13_READY_STAGE_1) {
		dev_warn(&hdev->dev, G13_NAME " hasn't completed stage 1 yet, forging ahead with initialization\n");
		/* Force the stage */
		g13data->ready_stages = G13_READY_STAGE_1;
	}
	init_completion(&g13data->ready);
	g13data->ready_stages |= G13_READY_SUBSTAGE_4;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g13_feature_report_4_send(hdev, G13_REPORT_4_INIT);
	usbhid_submit_report(hdev, g13data->start_input_report, USB_DIR_IN);
	wait_for_completion_timeout(&g13data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g13data->ready_stages != G13_READY_STAGE_2) {
		dev_warn(&hdev->dev, G13_NAME " hasn't completed stage 2 yet, forging ahead with initialization\n");
		/* Force the stage */
		g13data->ready_stages = G13_READY_STAGE_2;
	}
	init_completion(&g13data->ready);
	g13data->ready_stages |= G13_READY_SUBSTAGE_6;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Clear the LEDs
	 */
	g13_led_send(hdev);

	g13data->rgb[0] = G13_DEFAULT_RED;
	g13data->rgb[1] = G13_DEFAULT_GREEN;
	g13data->rgb[2] = G13_DEFAULT_BLUE;
	g13_rgb_send(hdev);

	/*
	 * Send the finalize report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g13_feature_report_4_send(hdev, G13_REPORT_4_FINALIZE);
	usbhid_submit_report(hdev, g13data->start_input_report, USB_DIR_IN);
	usbhid_submit_report(hdev, g13data->start_input_report, USB_DIR_IN);
	wait_for_completion_timeout(&g13data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (g13data->ready_stages != G13_READY_STAGE_3) {
		dev_warn(&hdev->dev, G13_NAME " hasn't completed stage 3 yet, forging ahead with initialization\n");
		/* Force the stage */
		g13data->ready_stages = G13_READY_STAGE_3;
	} else {
		dbg_hid(G13_NAME " stage 3 complete\n");
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	ginput_set_keymap_switching(gdata, 1);

	dbg_hid("G13 activated and initialized\n");

	/* Everything went well */
	return 0;

err_cleanup_registered_leds:
	for (i = 0; i < led_num; i++)
		led_classdev_unregister(g13data->led_cdev[i]);

err_cleanup_led_structs:
	for (i = 0; i < LED_COUNT; i++) {
		if (g13data->led_cdev[i] != NULL) {
			if (g13data->led_cdev[i]->name != NULL)
				kfree(g13data->led_cdev[i]->name);
			kfree(g13data->led_cdev[i]);
		}
	}

err_cleanup_input_dev_reg:
	input_unregister_device(gdata->input_dev);

err_cleanup_input_dev_data:
        ginput_free(gdata);

err_cleanup_input_dev:
	input_free_device(gdata->input_dev);

err_cleanup_g13data:
	/* Make sure we clean up the allocated data structure */
	kfree(g13data);

err_cleanup_gdata:
	/* Make sure we clean up the allocated data structure */
	kfree(gdata);

err_no_cleanup:

	hid_set_drvdata(hdev, NULL);

	return error;
}

static void g13_remove(struct hid_device *hdev)
{
	struct gcommon_data *gdata;
	struct g13_data *g13data;
	int i;

	/* Get the internal g13 data buffer */
	gdata = hid_get_drvdata(hdev);
        g13data = gdata->data;

	input_unregister_device(gdata->input_dev);
        ginput_free(gdata);

	kfree(gdata->name);

	/* Clean up the leds */
	for (i = 0; i < LED_COUNT; i++) {
		led_classdev_unregister(g13data->led_cdev[i]);
		kfree(g13data->led_cdev[i]->name);
		kfree(g13data->led_cdev[i]);
	}

	gfb_remove(gdata->gfb_data);


	hdev->ll_driver->close(hdev);

	hid_hw_stop(hdev);

	sysfs_remove_group(&(hdev->dev.kobj), &g13_attr_group);

//	/* Get the internal g13 data buffer */
//	data = hid_get_drvdata(hdev);
//
//	input_unregister_device(data->input_dev);
//
//	kfree(data->name);
//
//	/* Clean up the leds */
//	for (i = 0; i < LED_COUNT; i++) {
//		led_classdev_unregister(data->led_cdev[i]);
//		kfree(data->led_cdev[i]->name);
//		kfree(data->led_cdev[i]);
//	}
//
//	gfb_remove(data->gfb_data);

	/* Finally, clean up the g13 data itself */
	kfree(g13data);
	kfree(gdata);
}

static void __UNUSED g13_post_reset_start(struct hid_device *hdev)
{
        unsigned long irq_flags;
	struct gcommon_data *gdata = hid_get_gdata(hdev);
	struct g13_data *g13data = gdata->data;

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g13data->need_reset = 1;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static const struct hid_device_id g13_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G13)
	},
	{ }
};
MODULE_DEVICE_TABLE(hid, g13_devices);

static struct hid_driver g13_driver = {
	.name			= "hid-g13",
	.id_table		= g13_devices,
	.probe			= g13_probe,
	.remove			= g13_remove,
	.raw_event		= g13_raw_event,
};

static int __init g13_init(void)
{
	return hid_register_driver(&g13_driver);
}

static void __exit g13_exit(void)
{
	hid_unregister_driver(&g13_driver);
}

module_init(g13_init);
module_exit(g13_exit);
MODULE_DESCRIPTION("Logitech G13 HID Driver");
MODULE_AUTHOR("Rick L Vinyard Jr (rvinyard@cs.nmsu.edu)");
MODULE_LICENSE("GPL");
