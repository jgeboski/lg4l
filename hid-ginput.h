#ifndef GINPUT_H_INCLUDED
#define GINPUT_H_INCLUDED		1

struct gcommon_data;

struct ginput_data {
  int key_count;                /* no of keys in the kernel keymap */
  int * scancode_state;         /* length = key_count */
  int * keycode;                /* length = 3 * key_count */

  u8 curkeymap;                 /* current macro keymap index */
  u8 keymap_switching;          /* kernel keymap switch enable flag */

  /* pointer to a keymap switch notification function of the parent driver, or NULL */
  void (*notify_keymap_switched)(struct gcommon_data * gdata, 
                                 unsigned int index);
};

/* functions exposed by the module */

/* alloc/free the dynamic arrays in the input_data field of gdata */
int ginput_alloc(struct gcommon_data * gdata, int key_count);
void ginput_free(struct gcommon_data * gdata);


void ginput_handle_key_event(struct gcommon_data *gdata,
                             int scancode,
                             int value);

int ginput_setkeycode(struct input_dev * dev,
                      const struct input_keymap_entry * ke,
                      unsigned int * old_keycode);
int ginput_getkeycode(struct input_dev *dev,
                      struct input_keymap_entry *ke);

ssize_t ginput_set_keymap_index(struct gcommon_data *gdata, unsigned k);
ssize_t ginput_keymap_index_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf);
ssize_t ginput_keymap_index_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count);


ssize_t ginput_keymap_show(struct device *dev,
                           struct device_attribute *attr,
                           char *buf);
ssize_t ginput_keymap_store(struct device *dev,
                            struct device_attribute *attr,
                            const char *buf, size_t count);


ssize_t ginput_set_keymap_switching(struct gcommon_data *gdata, unsigned k);
ssize_t ginput_keymap_switching_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf);
ssize_t ginput_keymap_switching_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count);


#endif 
