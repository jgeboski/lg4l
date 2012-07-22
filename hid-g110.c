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

#define G110_NAME "Logitech G110"

/* Key defines */
#define G110_KEYS 17
#define G110_KEYMAP_SIZE (G110_KEYS*3)

/* Backlight defaults */
#define G110_DEFAULT_RED (0)
#define G110_DEFAULT_BLUE (255)

/* LED array indices */
#define G110_LED_M1 0
#define G110_LED_M2 1
#define G110_LED_M3 2
#define G110_LED_MR 3
#define G110_LED_BL_R 4
#define G110_LED_BL_B 5

#define G110_REPORT_4_INIT	0x00
#define G110_REPORT_4_FINALIZE	0x01

#define G110_READY_SUBSTAGE_1 0x01
#define G110_READY_SUBSTAGE_2 0x02
#define G110_READY_SUBSTAGE_3 0x04
#define G110_READY_STAGE_1    0x07
#define G110_READY_SUBSTAGE_4 0x08
#define G110_READY_SUBSTAGE_5 0x10
#define G110_READY_STAGE_2    0x1F
#define G110_READY_SUBSTAGE_6 0x20
#define G110_READY_SUBSTAGE_7 0x40
#define G110_READY_STAGE_3    0x7F

#define G110_RESET_POST 0x01
#define G110_RESET_MESSAGE_1 0x02
#define G110_RESET_READY 0x03

/* Per device data structure */
struct g110_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* core state */
	u8 backlight_rb[2];
	u8 led;

	/* none standard buttons stuff */
	u8 ep1keys[2];
	struct urb *ep1_urb;
	spinlock_t ep1_urb_lock;

	/* LED stuff */
	struct led_classdev *led_cdev[6];

	/* Housekeeping stuff */
	struct completion ready;
	int ready_stages;
	int need_reset;
};

/* Convenience macros */
#define hid_get_g110data(hdev) \
	((struct g110_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g110data(dev) \
	((struct g110_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices
 *
 * Key        Index
 * ---------  ------
 * G1-G12     0-11
 * M1         12
 * M2         13
 * M3         14
 * MR         15
 * LIGHT      19
 */
static const unsigned int g110_default_key_map[G110_KEYS] = {
  KEY_F1, KEY_F2, KEY_F3, KEY_F4,
  KEY_F5, KEY_F6, KEY_F7, KEY_F8,
  KEY_F9, KEY_F10, KEY_F11, KEY_F12,
  /* M1, M2, M3, MR */
  KEY_PROG1, KEY_PROG2, KEY_PROG3, KEY_RECORD,
  KEY_KBDILLUMTOGGLE
};

static void g110_led_send(struct hid_device *hdev)
{
	struct g110_data *g110data = hid_get_g110data(hdev);

	g110data->led_report->field[0]->value[0] = g110data->led&0xFF;

	usbhid_submit_report(hdev, g110data->led_report, USB_DIR_OUT);
}

static void g110_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value,
			 int led_num)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g110_data *g110data;
	u8 mask;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g110data = hid_get_g110data(hdev);

	mask = 0x01<<led_num;
	if (value)
		g110data->led |= mask;
	else
		g110data->led &= ~mask;

	g110_led_send(hdev);
}

static void g110_led_m1_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g110_led_set(led_cdev, value, G110_LED_M1);
}

static void g110_led_m2_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g110_led_set(led_cdev, value, G110_LED_M2);
}

static void g110_led_m3_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g110_led_set(led_cdev, value, G110_LED_M3);
}

static void g110_led_mr_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	g110_led_set(led_cdev, value, G110_LED_MR);
}

static enum led_brightness g110_led_brightness_get(struct led_classdev *led_cdev)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g110_data *g110data;
	int value = 0;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g110data = hid_get_g110data(hdev);

	if (led_cdev == g110data->led_cdev[G110_LED_M1])
		value = g110data->led & 0x80;
	else if (led_cdev == g110data->led_cdev[G110_LED_M2])
		value = g110data->led & 0x40;
	else if (led_cdev == g110data->led_cdev[G110_LED_M3])
		value = g110data->led & 0x20;
	else if (led_cdev == g110data->led_cdev[G110_LED_MR])
		value = g110data->led & 0x10;
	else
		dev_info(dev, G110_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g110_rgb_send(struct hid_device *hdev)
{
	struct g110_data *g110data = hid_get_g110data(hdev);

    /*
     * Unlike the other keyboards, the G110 only has 2 LED backlights (red and
     * blue). Rather than just setting intensity on each, the keyboard instead
     * has a single intensity value, and a second value to specify how red/blue
     * the backlight should be. This weird logic converts the two intensity
     * values from the user into an intensity/colour value suitable for the
     * keyboard.
     *
     * Additionally, the intensity is only valid from 0x00 - 0x0f (rather than
     * 0x00 - 0xff). I decided to keep accepting 0x00 - 0xff as input, and I
     * just >>4 to make it fit.
     */

    // These are just always zero from what I can tell
	g110data->backlight_report->field[0]->value[1] = 0x00;
	g110data->backlight_report->field[0]->value[2] = 0x00;

    // If the intensities are the same, "colour" is 0x80
    if ( g110data->backlight_rb[0] == g110data->backlight_rb[1] ) {
        g110data->backlight_report->field[0]->value[0] = 0x80;
        g110data->backlight_report->field[1]->value[0] = g110data->backlight_rb[0]>>4;
    }
    // If the blue value is higher
    else if ( g110data->backlight_rb[1] > g110data->backlight_rb[0] ) {
        g110data->backlight_report->field[0]->value[0] = 0xff - ( 0x80 * g110data->backlight_rb[0] ) / g110data->backlight_rb[1];
        g110data->backlight_report->field[1]->value[0] = g110data->backlight_rb[1]>>4;
    }
    // If the red value is higher
    else {
        g110data->backlight_report->field[0]->value[0] = ( 0x80 * g110data->backlight_rb[1] ) / g110data->backlight_rb[0];
        g110data->backlight_report->field[1]->value[0] = g110data->backlight_rb[0]>>4;
    }

	usbhid_submit_report(hdev, g110data->backlight_report, USB_DIR_OUT);
}

static void g110_led_bl_brightness_set(struct led_classdev *led_cdev,
                                       enum led_brightness value)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g110_data *g110data;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g110data = hid_get_g110data(hdev);

	if (led_cdev == g110data->led_cdev[G110_LED_BL_R])
		g110data->backlight_rb[0] = value;
	else if (led_cdev == g110data->led_cdev[G110_LED_BL_B])
		g110data->backlight_rb[1] = value;

	g110_rgb_send(hdev);
}

static enum led_brightness g110_led_bl_brightness_get(struct led_classdev *led_cdev)
{
	struct device *dev;
	struct hid_device *hdev;
	struct g110_data *g110data;
	int value = 0;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	hdev = container_of(dev, struct hid_device, dev);

	/* Get the underlying data value */
	g110data = hid_get_g110data(hdev);

	if (led_cdev == g110data->led_cdev[G110_LED_BL_R])
		value = g110data->backlight_rb[0];
	else if (led_cdev == g110data->led_cdev[G110_LED_BL_B])
		value = g110data->backlight_rb[1];
	else
		dev_info(dev, G110_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}


static const struct led_classdev g110_led_cdevs[6] = {
	{
		.brightness_set		= g110_led_m1_brightness_set,
		.brightness_get		= g110_led_brightness_get,
	},
	{
		.brightness_set		= g110_led_m2_brightness_set,
		.brightness_get		= g110_led_brightness_get,
	},
	{
		.brightness_set		= g110_led_m3_brightness_set,
		.brightness_get		= g110_led_brightness_get,
	},
	{
		.brightness_set		= g110_led_mr_brightness_set,
		.brightness_get		= g110_led_brightness_get,
	},
	{
		.brightness_set		= g110_led_bl_brightness_set,
		.brightness_get		= g110_led_bl_brightness_get,
	},
	{
		.brightness_set		= g110_led_bl_brightness_set,
		.brightness_get		= g110_led_bl_brightness_get,
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
static void g110_notify_keymap_switched(struct gcommon_data * gdata, 
                                        unsigned int index)
{
        struct g110_data * g110data = gdata->data;

        g110data->led = 1 << index;
        g110_led_send(gdata->hdev);
}


static ssize_t g110_name_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
	struct gcommon_data *gdata = dev_get_drvdata(dev);
	int result;

	if (gdata->name == NULL) {
		buf[0] = 0x00;
		return 1;
	}

	spin_lock(&gdata->lock);
	result = sprintf(buf, "%s", gdata->name);
	spin_unlock(&gdata->lock);

	return result;
}

static ssize_t g110_name_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
        struct gcommon_data *gdata = dev_get_drvdata(dev);
	size_t limit = count;
	char *end;

	spin_lock(&gdata->lock);

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

	spin_unlock(&gdata->lock);

	return count;
}

static DEVICE_ATTR(name, 0666, g110_name_show, g110_name_store);

static void g110_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g110_data *g110data = hid_get_g110data(hdev);

	if (which == G110_REPORT_4_INIT) {
		g110data->feature_report_4->field[0]->value[0] = 0x02;
		g110data->feature_report_4->field[0]->value[1] = 0x00;
		g110data->feature_report_4->field[0]->value[2] = 0x00;
		g110data->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G110_REPORT_4_FINALIZE) {
		g110data->feature_report_4->field[0]->value[0] = 0x02;
		g110data->feature_report_4->field[0]->value[1] = 0x80;
		g110data->feature_report_4->field[0]->value[2] = 0x00;
		g110data->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	usbhid_submit_report(hdev, g110data->feature_report_4, USB_DIR_OUT);
}

/*
 * The "minor" attribute
 */
static ssize_t g110_minor_show(struct device *dev,
                               struct device_attribute *attr,
                               char *buf)
{
	struct gcommon_data *gdata = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", gdata->hdev->minor);
}

static DEVICE_ATTR(minor, 0444, g110_minor_show, NULL);

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *g110_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_keymap_index.attr,
	&dev_attr_keymap_switching.attr,
	&dev_attr_keymap.attr,
	&dev_attr_minor.attr,
	NULL,	 /* need to NULL terminate the list of attributes */
};

/*
 * An unnamed attribute group will put all of the attributes directly in
 * the kobject directory.  If we specify a name, a subdirectory will be
 * created for the attributes with the directory being the name of the
 * attribute group.
 */
static struct attribute_group g110_attr_group = {
	.attrs = g110_attrs,
};


static void g110_raw_event_process_input(struct hid_device *hdev,
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
		if (input_data->curkeymap != 0 && raw_data[2] & 0x10)
			ginput_set_keymap_index(gdata, 0);
		else if (input_data->curkeymap != 1 && raw_data[2] & 0x20)
			ginput_set_keymap_index(gdata, 1);
		else if (input_data->curkeymap != 2 && raw_data[2] & 0x40)
			ginput_set_keymap_index(gdata, 2);
	}

	raw_data[3] &= 0xBF; /* bit 6 is always on */

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		/* Keys G1 through G8 */
		scancode = i;
		value = raw_data[1] & mask;
		ginput_handle_key_event(gdata, scancode, value);

		/* Keys G9 through MR */
		scancode = i + 8;
		value = raw_data[2] & mask;
		ginput_handle_key_event(gdata, scancode, value);

		/* Key Light Only */
		if(i == 0) {
			scancode = i + 16;
			value = raw_data[3] & mask;
			ginput_handle_key_event(gdata, scancode, value);
		}

	}

	input_sync(idev);
}

static int g110_raw_event(struct hid_device *hdev,
                          struct hid_report *report,
                          u8 *raw_data, int size)
{
	/*
	* On initialization receive a 258 byte message with
	* data = 6 0 255 255 255 255 255 255 255 255 ...
	*/
	struct gcommon_data *gdata = dev_get_gdata(&hdev->dev);
	struct g110_data *g110data = gdata->data;

	spin_lock(&gdata->lock);

	if (unlikely(g110data->need_reset)) {
		g110_rgb_send(hdev);
		g110_led_send(hdev);
		g110data->need_reset = 0;
		spin_unlock(&gdata->lock);
		return 1;
	}

	if (unlikely(g110data->ready_stages != G110_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g110data->ready_stages & G110_READY_SUBSTAGE_1))
				g110data->ready_stages |= G110_READY_SUBSTAGE_1;
			else if (g110data->ready_stages & G110_READY_SUBSTAGE_4 &&
				 !(g110data->ready_stages & G110_READY_SUBSTAGE_5)
				)
				g110data->ready_stages |= G110_READY_SUBSTAGE_5;
			else if (g110data->ready_stages & G110_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g110data->ready_stages |= G110_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g110data->ready_stages & G110_READY_SUBSTAGE_2))
				g110data->ready_stages |= G110_READY_SUBSTAGE_2;
			else
				g110data->ready_stages |= G110_READY_SUBSTAGE_3;
			break;
		}

		if (g110data->ready_stages == G110_READY_STAGE_1 ||
		    g110data->ready_stages == G110_READY_STAGE_2 ||
		    g110data->ready_stages == G110_READY_STAGE_3)
			complete_all(&g110data->ready);

		spin_unlock(&gdata->lock);
		return 1;
	}

	spin_unlock(&gdata->lock);

	if (likely(report->id == 2)) {
		g110_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}

static void g110_initialize_keymap(struct gcommon_data *gdata)
{
	int i;

	for (i = 0; i < G110_KEYS; i++) {
		gdata->input_data.keycode[i] = g110_default_key_map[i];
		__set_bit(gdata->input_data.keycode[i], gdata->input_dev->keybit);
	}

	__clear_bit(KEY_RESERVED, gdata->input_dev->keybit);
}

/* Unlock the urb so we can reuse it */
static void g110_ep1_urb_completion(struct urb *urb)
{
	struct hid_device *hdev = urb->context;
	struct gcommon_data *gdata = hid_get_gdata(hdev);
        struct g110_data *g110data = gdata->data;
	struct input_dev *idev = gdata->input_dev;
	int i;

	for (i = 0; i < 8; i++)
		ginput_handle_key_event(gdata, 24+i, g110data->ep1keys[0]&(1<<i));

	input_sync(idev);

	usb_submit_urb(urb, GFP_ATOMIC);
}

static int g110_ep1_read(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_device *usb_dev;
	struct g110_data *g110data = hid_get_g110data(hdev);

	struct usb_host_endpoint *ep;
	unsigned int pipe;
	int retval = 0;

	/* Get the usb device to send the image on */
	intf = to_usb_interface(hdev->dev.parent);
	usb_dev = interface_to_usbdev(intf);

	pipe = usb_rcvintpipe(usb_dev, 0x01);
	ep = (usb_pipein(pipe) ? usb_dev->ep_in : usb_dev->ep_out)[usb_pipeendpoint(pipe)];

	if (unlikely(!ep))
		return -EINVAL;

	usb_fill_int_urb(g110data->ep1_urb, usb_dev, pipe, g110data->ep1keys, 2,
			 g110_ep1_urb_completion, NULL, 10);
	g110data->ep1_urb->context = hdev;
	g110data->ep1_urb->actual_length = 0;

	retval = usb_submit_urb(g110data->ep1_urb, GFP_KERNEL);

	return retval;
}

static int g110_probe(struct hid_device *hdev,
		     const struct hid_device_id *id)
{
	int error;
	struct gcommon_data *gdata;
	struct g110_data *g110data;
	int i;
	int led_num;
	struct usb_interface *intf;
	struct usb_device *usbdev;
	struct list_head *feature_report_list =
		&hdev->report_enum[HID_FEATURE_REPORT].report_list;
	struct hid_report *report;
	char *led_name;

	dev_dbg(&hdev->dev, "Logitech G110 HID hardware probe...");

	/* Get the usb device to send the start report on */
	intf = to_usb_interface(hdev->dev.parent);
	usbdev = interface_to_usbdev(intf);

	/*
	 * Let's allocate the g110 data structure, set some reasonable
	 * defaults, and associate it with the device
	 */
	gdata = kzalloc(sizeof(struct gcommon_data), GFP_KERNEL);
	if (gdata == NULL) {
		dev_err(&hdev->dev, "can't allocate space for Logitech G110 device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g110data = kzalloc(sizeof(struct g110_data), GFP_KERNEL);
	if (g110data == NULL) {
		dev_err(&hdev->dev, "can't allocate space for Logitech G110 device attributes\n");
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
        gdata->data = g110data;

	spin_lock_init(&gdata->lock);

	init_completion(&g110data->ready);

	gdata->hdev = hdev;

	g110data->ep1_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (g110data->ep1_urb == NULL) {
		dev_err(&hdev->dev, G110_NAME ": ERROR: can't alloc ep1 urb stuff\n");
		error = -ENOMEM;
		goto err_cleanup_g110data;
	}

	hid_set_drvdata(hdev, gdata);

	dbg_hid("Preparing to parse " G110_NAME " hid reports\n");

	/* Parse the device reports and start it up */
	error = hid_parse(hdev);
	if (error) {
		dev_err(&hdev->dev, G110_NAME " device report parse failed\n");
		error = -EINVAL;
		goto err_cleanup_ep1_urb;
	}

	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT | HID_CONNECT_HIDINPUT_FORCE);
	if (error) {
		dev_err(&hdev->dev, G110_NAME " hardware start failed\n");
		error = -EINVAL;
		goto err_cleanup_ep1_urb;
	}

	dbg_hid(G110_NAME " claimed: %d\n", hdev->claimed);

	error = hdev->ll_driver->open(hdev);
	if (error) {
		dev_err(&hdev->dev, G110_NAME " failed to open input interrupt pipe for key and joystick events\n");
		error = -EINVAL;
		goto err_cleanup_ep1_urb;
	}

	/* Set up the input device for the key I/O */
	gdata->input_dev = input_allocate_device();
	if (gdata->input_dev == NULL) {
		dev_err(&hdev->dev, G110_NAME " error initializing the input device");
		error = -ENOMEM;
		goto err_cleanup_ep1_urb;
	}

	input_set_drvdata(gdata->input_dev, gdata);

	gdata->input_dev->name = G110_NAME;
	gdata->input_dev->phys = hdev->phys;
	gdata->input_dev->uniq = hdev->uniq;
	gdata->input_dev->id.bustype = hdev->bus;
	gdata->input_dev->id.vendor = hdev->vendor;
	gdata->input_dev->id.product = hdev->product;
	gdata->input_dev->id.version = hdev->version;
	gdata->input_dev->dev.parent = hdev->dev.parent;
	gdata->input_dev->keycode = gdata->input_data.keycode;
	gdata->input_dev->keycodemax = G110_KEYMAP_SIZE;
	gdata->input_dev->keycodesize = sizeof(int);
	gdata->input_dev->setkeycode = ginput_setkeycode;
	gdata->input_dev->getkeycode = ginput_getkeycode;

	input_set_capability(gdata->input_dev, EV_KEY, KEY_UNKNOWN);
	gdata->input_dev->evbit[0] |= BIT_MASK(EV_REP);

        gdata->input_data.notify_keymap_switched = g110_notify_keymap_switched;

        error = ginput_alloc(gdata, G110_KEYS);
        if (error) {
		dev_err(&hdev->dev, G110_NAME " error allocating memory for the input device");
                goto err_cleanup_input_dev;
        }

	g110_initialize_keymap(gdata);

	error = input_register_device(gdata->input_dev);
	if (error) {
		dev_err(&hdev->dev, G110_NAME " error registering the input device");
		error = -EINVAL;
		goto err_cleanup_input_dev_data;
	}

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev, "no feature report found\n");
		error = -ENODEV;
		goto err_cleanup_input_dev_reg;
	}
	dbg_hid(G110_NAME " feature report found\n");

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x03:
			g110data->feature_report_4 = report;
			g110data->start_input_report = report;
			g110data->led_report = report;
			break;
		case 0x07:
			g110data->backlight_report = report;
			break;
		default:
			break;
		}
		dbg_hid(G110_NAME " Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	dbg_hid("Found all reports\n");

	/* Create the LED structures */
	for (i = 0; i < 6; i++) {
		g110data->led_cdev[i] = kzalloc(sizeof(struct led_classdev), GFP_KERNEL);
		if (g110data->led_cdev[i] == NULL) {
			dev_err(&hdev->dev, G110_NAME " error allocating memory for led %d", i);
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		/* Set the accessor functions by copying from template*/
		*(g110data->led_cdev[i]) = g110_led_cdevs[i];

		/*
		 * Allocate memory for the LED name
		 *
		 * Since led_classdev->name is a const char* we'll use an
		 * intermediate until the name is formatted with sprintf().
		 */
		led_name = kzalloc(sizeof(char)*20, GFP_KERNEL);
		if (led_name == NULL) {
			dev_err(&hdev->dev, G110_NAME " error allocating memory for led %d name", i);
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		switch (i) {
		case 0:
		case 1:
		case 2:
			sprintf(led_name, "g110_%d:orange:m%d", hdev->minor, i+1);
			break;
		case 3:
			sprintf(led_name, "g110_%d:red:mr", hdev->minor);
			break;
		case 4:
			sprintf(led_name, "g110_%d:red:bl", hdev->minor);
			break;
		case 5:
			sprintf(led_name, "g110_%d:blue:bl", hdev->minor);
			break;
		}
		g110data->led_cdev[i]->name = led_name;
	}

	for (i = 0; i < 6; i++) {
		led_num = i;
		error = led_classdev_register(&hdev->dev, g110data->led_cdev[i]);
		if (error < 0) {
			dev_err(&hdev->dev, G110_NAME " error registering led %d", i);
			error = -EINVAL;
			goto err_cleanup_registered_leds;
		}
	}

	dbg_hid("Waiting for G110 to activate\n");

	/* Add the sysfs attributes */
	error = sysfs_create_group(&(hdev->dev.kobj), &g110_attr_group);
	if (error) {
		dev_err(&hdev->dev, G110_NAME " failed to create sysfs group attributes\n");
		goto err_cleanup_registered_leds;
	}

	/*
	 * Wait here for stage 1 (substages 1-3) to complete
	 */
	wait_for_completion_timeout(&g110data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock(&gdata->lock);
	if (g110data->ready_stages != G110_READY_STAGE_1) {
		dev_warn(&hdev->dev, G110_NAME " hasn't completed stage 1 yet, forging ahead with initialization\n");
		/* Force the stage */
		g110data->ready_stages = G110_READY_STAGE_1;
	}
	init_completion(&g110data->ready);
	g110data->ready_stages |= G110_READY_SUBSTAGE_4;
	spin_unlock(&gdata->lock);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g110_feature_report_4_send(hdev, G110_REPORT_4_INIT);
	usbhid_submit_report(hdev, g110data->start_input_report, USB_DIR_IN);
	wait_for_completion_timeout(&g110data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock(&gdata->lock);
	if (g110data->ready_stages != G110_READY_STAGE_2) {
		dev_warn(&hdev->dev, G110_NAME " hasn't completed stage 2 yet, forging ahead with initialization\n");
		/* Force the stage */
		g110data->ready_stages = G110_READY_STAGE_2;
	}
	init_completion(&g110data->ready);
	g110data->ready_stages |= G110_READY_SUBSTAGE_6;
	spin_unlock(&gdata->lock);

	/*
	 * Clear the LEDs
	 */
	g110_led_send(hdev);

	g110data->backlight_rb[0] = G110_DEFAULT_RED;
	g110data->backlight_rb[1] = G110_DEFAULT_BLUE;
	g110_rgb_send(hdev);

	/*
	 * Send the finalize report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g110_feature_report_4_send(hdev, G110_REPORT_4_FINALIZE);
	usbhid_submit_report(hdev, g110data->start_input_report, USB_DIR_IN);
	usbhid_submit_report(hdev, g110data->start_input_report, USB_DIR_IN);
	wait_for_completion_timeout(&g110data->ready, HZ);

	/* Protect data->ready_stages before checking whether we're ready to proceed */
	spin_lock(&gdata->lock);

	if (g110data->ready_stages != G110_READY_STAGE_3) {
		dev_warn(&hdev->dev, G110_NAME " hasn't completed stage 3 yet, forging ahead with initialization\n");
		/* Force the stage */
		g110data->ready_stages = G110_READY_STAGE_3;
	} else {
		dbg_hid(G110_NAME " stage 3 complete\n");
	}

	spin_unlock(&gdata->lock);

	ginput_set_keymap_switching(gdata, 1);

	g110_ep1_read(hdev);

	dbg_hid("G110 activated and initialized\n");

	/* Everything went well */
	return 0;

err_cleanup_registered_leds:
	for (i = 0; i < led_num; i++)
		led_classdev_unregister(g110data->led_cdev[i]);

err_cleanup_led_structs:
	for (i = 0; i < 6; i++) {
		if (g110data->led_cdev[i] != NULL) {
			if (g110data->led_cdev[i]->name != NULL)
				kfree(g110data->led_cdev[i]->name);
			kfree(g110data->led_cdev[i]);
		}
	}

err_cleanup_input_dev_reg:
	input_unregister_device(gdata->input_dev);

err_cleanup_input_dev_data:
        ginput_free(gdata);

err_cleanup_input_dev:
	input_free_device(gdata->input_dev);

err_cleanup_ep1_urb:
	usb_free_urb(g110data->ep1_urb);

err_cleanup_g110data:
	kfree(g110data);

err_cleanup_gdata:
	/* Make sure we clean up the allocated data structure */
	kfree(gdata);

err_no_cleanup:

	hid_set_drvdata(hdev, NULL);

	return error;
}

static void g110_remove(struct hid_device *hdev)
{
	struct gcommon_data *gdata;
	struct g110_data *g110data;
	int i;

	/* Get the internal g110 data buffer */
	gdata = hid_get_drvdata(hdev);
        g110data = gdata->data;

	input_unregister_device(gdata->input_dev);
        ginput_free(gdata);

	kfree(gdata->name);

	/* Clean up the leds */
	for (i = 0; i < 6; i++) {
		led_classdev_unregister(g110data->led_cdev[i]);
		kfree(g110data->led_cdev[i]->name);
		kfree(g110data->led_cdev[i]);
	}

	hdev->ll_driver->close(hdev);

	hid_hw_stop(hdev);

	sysfs_remove_group(&(hdev->dev.kobj), &g110_attr_group);

	usb_free_urb(g110data->ep1_urb);


	/* Finally, clean up the g110 data itself */
	kfree(g110data);
	kfree(gdata);
}

static void __UNUSED g110_post_reset_start(struct hid_device *hdev)
{
	struct gcommon_data *gdata = hid_get_gdata(hdev);
        struct g110_data *g110data = gdata->data;

	spin_lock(&gdata->lock);
	g110data->need_reset = 1;
	spin_unlock(&gdata->lock);
}

static const struct hid_device_id g110_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G110)
	},
	{ }
};
MODULE_DEVICE_TABLE(hid, g110_devices);

static struct hid_driver g110_driver = {
	.name			= "hid-g110",
	.id_table		= g110_devices,
	.probe			= g110_probe,
	.remove			= g110_remove,
	.raw_event		= g110_raw_event,
};

static int __init g110_init(void)
{
	return hid_register_driver(&g110_driver);
}

static void __exit g110_exit(void)
{
	hid_unregister_driver(&g110_driver);
}

module_init(g110_init);
module_exit(g110_exit);
MODULE_DESCRIPTION("Logitech G110 HID Driver");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_LICENSE("GPL");
