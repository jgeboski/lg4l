
static int ginput_get_keycode(struct input_dev * dev,
                              unsigned int scancode,
                              unsigned int * keycode)
{
	return input_get_keycode(dev, scancode, keycode);
}

static int ginput_setkeycode(struct input_dev *dev,
                                   unsigned int scancode,
                                   unsigned int keycode)
{
	unsigned long irq_flags;
	int old_keycode;
	int i;
	struct ginput_data *data = input_get_ginputdata(dev);

	if (scancode >= dev->keycodemax)
		return -EINVAL;

	spin_lock_irqsave(data->lock, irq_flags);

	old_keycode = data->keycode[scancode];
	data->keycode[scancode] = keycode;

	__clear_bit(old_keycode, dev->keybit);
	__set_bit(keycode, dev->keybit);

	for (i = 0; i < dev->keycodemax; i++) {
		if (data->keycode[i] == old_keycode) {
			__set_bit(old_keycode, dev->keybit);
			break; /* Setting the bit twice is useless, so break*/
		}
	}

	spin_unlock_irqrestore(data->lock, irq_flags);

	return 0;
}

static int ginput_getkeycode(struct input_dev *dev,
                             unsigned int scancode,
                             unsigned int *keycode)
{
	struct ginput_data *data = input_get_ginputdata(dev);

	if (!dev->keycodesize)
		return -EINVAL;

	if (scancode >= dev->keycodemax)
		return -EINVAL;

	*keycode = data->keycode[scancode];

	return 0;
}
