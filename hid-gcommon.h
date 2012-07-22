#ifndef GCOMMON_H_INCLUDED
#define GCOMMON_H_INCLUDED		1

#include "hid-gfb.h"
#include "hid-ginput.h"

/* Private driver data common between G-series drivers 
 *
 * The model of the hid-gNNN driver is an unique driver for all
 * devices contained within the specific keyboard (framebuffer, extra keys
 * and leds). Factoring common functionalities between drivers lead to
 * separate modules needing access to common shared data.
 *
 * All functions along different modules should be able to access their 
 * specific data structures starting from this structure, attached to
 * the root hid device, by downcasting the data field to the appropriate
 * gNN_data structure.
 */
struct gcommon_data {
	char *name;                    /* name of the device */

	struct hid_device *hdev;       /* hid device */
	struct input_dev *input_dev;   /* input device */
        struct ginput_data input_data; /* keymaps of G-series extra-keys */
	struct gfb_data *gfb_data;     /* framebuffer (may be NULL) */

	spinlock_t lock;               /* global device lock */

        void *data;                    /* specific driver data */
};

/* get the common private driver data from a hid_device */
#define hid_get_gdata(hdev) \
	((struct gcommon_data *)(hid_get_drvdata(hdev)))

/* get the common private driver data from a generic device */
#define dev_get_gdata(dev) \
	((struct gcommon_data *)(dev_get_drvdata(dev)))

#endif

