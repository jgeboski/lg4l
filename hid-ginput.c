/***************************************************************************
 *   Copyright (C) 2010 by Alistair Buxton                                 *
 *   a.j.buxton@gmail.com                                                  *
 *   based on hid-g13.c                                                    *
 *                                                                         *
 *   Copyright (C) 2011 by Ciprian Ciubotariu <cheepeero@gmx.net>          *
 *   - factored macro keys input code from hid-gNNN.c                      *
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

#include <linux/module.h>
#include <linux/input.h>
#include <linux/hid.h>

#include "hid-gcommon.h"


#define input_get_gdata(idev) \
	((struct gcommon_data *)(input_get_drvdata(idev)))
#define input_get_idata(idev) \
	((struct ginput_data *) &(input_get_gdata(idev)->input_data))



int ginput_alloc(struct gcommon_data * gdata, int key_count)
{
  struct ginput_data * input_data = &gdata->input_data;
  input_data->key_count = key_count;

  input_data->keycode = kzalloc(key_count * sizeof(int), GFP_KERNEL);
  if (input_data->keycode == NULL)
    goto err_keycode;

  input_data->scancode_state = kzalloc(3 * key_count * sizeof(int), GFP_KERNEL);
  if (input_data->scancode_state == NULL)
    goto err_scancode_state;

  return 0;

err_scancode_state:
  kfree(input_data->keycode);

err_keycode:
  return -ENOMEM;
}
EXPORT_SYMBOL_GPL(ginput_alloc);

void ginput_free(struct gcommon_data * gdata)
{
  kfree(gdata->input_data.scancode_state);
  kfree(gdata->input_data.keycode);
}
EXPORT_SYMBOL_GPL(ginput_free);


/* provide the keycode for a scancode using the current keymap */
int ginput_get_keycode(struct input_dev * dev,
                       unsigned int scancode,
                       unsigned int * keycode)
{
	int retval;	
	
        struct input_keymap_entry ke = {
		.flags    = 0,
		.len      = sizeof(scancode),
		.index    = scancode,
	};

        /* don't demote the scancode from unsigned int to __u8 */
        *((unsigned int *) &ke.scancode) = scancode;

	retval   = input_get_keycode(dev, &ke);
	*keycode = ke.keycode;
	
	return retval;
}

void ginput_handle_key_event(struct gcommon_data *gdata,
                             int scancode,
                             int value)
{
        struct input_dev * idev = gdata->input_dev;
        struct ginput_data * idata = &gdata->input_data;
	int error;
	int keycode;
	int offset;

	offset = idata->key_count * idata->curkeymap;

	error = ginput_get_keycode(idev, scancode+offset, &keycode);

	if (unlikely(error)) {
		dev_warn(&idev->dev, "%s error in ginput_get_keycode(): scancode=%d\n", 
                         gdata->name, scancode);
		return;
	}

	/* Only report mapped keys */
	if (keycode != KEY_RESERVED) {
		input_report_key(idev, keycode, value);
	}
	/* Or report MSC_SCAN on keypress of an unmapped key */
	else if (idata->scancode_state[scancode] == 0 && value) {
		input_event(idev, EV_MSC, MSC_SCAN, scancode);
	}

	idata->scancode_state[scancode] = value;
}
EXPORT_SYMBOL_GPL(ginput_handle_key_event);


/* set a keycode in the current keymap (kernel callback) */
int ginput_setkeycode(struct input_dev * dev,
                      const struct input_keymap_entry * ke,
                      unsigned int * old_keycode)
{
	unsigned long irq_flags;
	int i;
	struct gcommon_data *gdata = input_get_gdata(dev);
	struct ginput_data *idata = &gdata->input_data;
        unsigned int * scancode = (unsigned int *) ke->scancode;

	if (*scancode >= dev->keycodemax)
		return -EINVAL;

	spin_lock_irqsave(&gdata->lock, irq_flags);

	*old_keycode = idata->keycode[*scancode];
	idata->keycode[*scancode] = ke->keycode;

	__clear_bit(*old_keycode, dev->keybit);
	__set_bit(ke->keycode, dev->keybit);

	for (i = 0; i < dev->keycodemax; i++) {
		if (idata->keycode[i] == *old_keycode) {
			__set_bit(*old_keycode, dev->keybit);
			break; /* Setting the bit twice is useless, so break*/
		}
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return 0;
}
EXPORT_SYMBOL_GPL(ginput_setkeycode);


/* helper for ginput_setkeycode */
static int ginput_setkeycode_internal(struct input_dev * dev,
                                      unsigned int scancode,
                                      unsigned int keycode) 
{
  struct input_keymap_entry ke;
  unsigned int old_keycode;

  ke.keycode = keycode;
  *((unsigned int *) ke.scancode) = scancode;

  return ginput_setkeycode(dev, &ke, &old_keycode);
}

/* read a keycode from the current keymap (kernel callback) */
int ginput_getkeycode(struct input_dev *dev,
                      struct input_keymap_entry *ke)
{
	struct gcommon_data *gdata = input_get_gdata(dev);
        unsigned int * scancode = (unsigned int *) ke->scancode;

	if (!dev->keycodesize)
		return -EINVAL;

	if (*scancode >= dev->keycodemax)
		return -EINVAL;

	ke->keycode = gdata->input_data.keycode[*scancode];

	return 0;
}
EXPORT_SYMBOL_GPL(ginput_getkeycode);



/*
 * The "keymap" attribute
 */
ssize_t ginput_keymap_index_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
	struct gcommon_data *gdata = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", gdata->input_data.curkeymap);
}
EXPORT_SYMBOL_GPL(ginput_keymap_index_show);

ssize_t ginput_set_keymap_index(struct gcommon_data *gdata, unsigned k)
{
	int scancode;
	int offset_old;
	int offset_new;
	int keycode_old;
	int keycode_new;

	struct input_dev *idev = gdata->input_dev;
        struct ginput_data *idata = &gdata->input_data;

	if (k > 2)
		return -EINVAL;

	/*
	 * Release all the pressed keys unless the new keymap has the same key
	 * in the same scancode position.
	 *
	 * Also, clear the scancode state unless the new keymap has the same
	 * key in the same scancode position.
	 *
	 * This allows a keycode mapped to the same scancode in two different
	 * keymaps to remain pressed without a key up code when the keymap is
	 * switched.
	 */
	offset_old = idata->key_count * idata->curkeymap;
	offset_new = idata->key_count * k;
	for (scancode = 0; scancode < idata->key_count; scancode++) {
		keycode_old = idata->keycode[offset_old+scancode];
		keycode_new = idata->keycode[offset_new+scancode];
		if (keycode_old != keycode_new) {
			if (keycode_old != KEY_RESERVED)
				input_report_key(idev, keycode_old, 0);
			idata->scancode_state[scancode] = 0;
		}
	}

	idata->curkeymap = k;

	if (idata->keymap_switching && idata->notify_keymap_switched) {
                (*idata->notify_keymap_switched)(gdata, k);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ginput_set_keymap_index);

ssize_t ginput_keymap_index_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
	int i;
	unsigned k;
	ssize_t set_result;

	struct gcommon_data *gdata = dev_get_drvdata(dev);

	i = sscanf(buf, "%u", &k);
	if (i != 1) {
        	dev_warn(dev, "%s unrecognized input: %s", gdata->input_dev->name, buf);
		return -1;
	}

	set_result = ginput_set_keymap_index(gdata, k);

	if (set_result < 0)
		return set_result;

	return count;
}
EXPORT_SYMBOL_GPL(ginput_keymap_index_store);

/*
 * The "keycode" attribute
 */
ssize_t ginput_keymap_show(struct device *dev,
                           struct device_attribute *attr,
                           char *buf)
{
	int offset = 0;
	int result;
	int scancode;
	int keycode;
	int error;

	struct gcommon_data *gdata = dev_get_drvdata(dev);

        int keymap_size = 3 * gdata->input_data.key_count;

	for (scancode = 0; scancode < keymap_size; scancode++) {
		error = ginput_get_keycode(gdata->input_dev, scancode, &keycode);
		if (error) {
			dev_warn(dev, "%s error accessing scancode %d\n",
				 gdata->input_dev->name, scancode);
			continue;
		}

		result = sprintf(buf+offset, "0x%03x 0x%04x\n",
				 scancode, keycode);
		if (result < 0)
			return -EINVAL;
		offset += result;
	}

	return offset+1;
}
EXPORT_SYMBOL_GPL(ginput_keymap_show);

ssize_t ginput_keymap_store(struct device *dev,
                            struct device_attribute *attr,
                            const char *buf, size_t count)
{
	int scanned;
	int consumed;
        unsigned int scancd;
        unsigned int keycd;
	int error;
	int set = 0;
	int gkey;
	int index;
	int good;

	struct gcommon_data *gdata = dev_get_drvdata(dev);
        struct ginput_data *idata = &gdata->input_data;

	do {
		good = 0;

		/* Look for scancode keycode pair in hex */
		scanned = sscanf(buf, "%x %x%n", &scancd, &keycd, &consumed);
		if (scanned == 2) {
			buf += consumed;
			error = ginput_setkeycode_internal(gdata->input_dev, scancd, keycd);
			if (error)
				goto err_input_setkeycode;
			set++;
			good = 1;
		} else {
			/*
			 * Look for Gkey keycode pair and assign to current
			 * keymap
			 */
			scanned = sscanf(buf, "G%d %x%n", &gkey, &keycd, &consumed);
			if (scanned == 2 && gkey > 0 && gkey <= idata->key_count) {
				buf += consumed;
				scancd = idata->curkeymap * idata->key_count + gkey - 1;
				error = ginput_setkeycode_internal(gdata->input_dev, scancd, keycd);
				if (error)
					goto err_input_setkeycode;
				set++;
				good = 1;
			} else {
				/*
				 * Look for Gkey-index keycode pair and assign
				 * to indexed keymap
				 */
				scanned = sscanf(buf, "G%d-%d %x%n", &gkey, &index, &keycd, &consumed);
				if (scanned == 3 &&
				    gkey > 0 && gkey <= idata->key_count &&
				    index >= 0 && index <= 2) {
					buf += consumed;
					scancd = index * idata->key_count + gkey - 1;
					error = ginput_setkeycode_internal(gdata->input_dev, scancd, keycd);
					if (error)
						goto err_input_setkeycode;
					set++;
					good = 1;
				}
			}
		}

	} while (good);

	if (set == 0) {
        	dev_warn(dev, "%s unrecognized keycode input: %s", 
                         gdata->name, buf);
		return -1;
	}

	return count;

err_input_setkeycode:
	dev_warn(dev, "%s error setting scancode %d to keycode %d\n", 
                 gdata->name, scancd, keycd);
	return error;
}
EXPORT_SYMBOL_GPL(ginput_keymap_store);

/*
 * The "keymap_switching" attribute
 */
ssize_t ginput_keymap_switching_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
	struct gcommon_data *gdata = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", gdata->input_data.keymap_switching);
}
EXPORT_SYMBOL_GPL(ginput_keymap_switching_show);

ssize_t ginput_set_keymap_switching(struct gcommon_data *gdata, unsigned k)
{
        struct ginput_data * idata = &gdata->input_data;
	idata->keymap_switching = k;

	if (idata->keymap_switching && idata->notify_keymap_switched) {
                (*idata->notify_keymap_switched)(gdata, k);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ginput_set_keymap_switching);

ssize_t ginput_keymap_switching_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
	int i;
	unsigned k;
	ssize_t set_result;

	struct gcommon_data *gdata = dev_get_drvdata(dev);

	i = sscanf(buf, "%u", &k);
	if (i != 1) {
        	dev_warn(dev, "%s unrecognized input: %s", 
                         gdata->name, buf);
		return -1;
	}

	set_result = ginput_set_keymap_switching(gdata, k);

	if (set_result < 0)
		return set_result;

	return count;
}
EXPORT_SYMBOL_GPL(ginput_keymap_switching_store);

MODULE_DESCRIPTION("Logitech G-Series HID Input Driver helpers");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_AUTHOR("Thomas Berger (tbe@boreus.de)");
MODULE_AUTHOR("Ciprian Ciubotariu (cheepeero@gmx.net)");
MODULE_LICENSE("GPL v2");
