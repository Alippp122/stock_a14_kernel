/*
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *  Copyright (C) 2012  Amit Daniel <amit.kachhap@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <soc/samsung/isp_cooling.h>

#include <soc/samsung/tmu.h>
#if defined(CONFIG_ECT) || defined(CONFIG_ECT_MODULE)
#include <soc/samsung/ect_parser.h>
#endif
#include "exynos_tmu.h"
#include "../thermal_core.h"

/**
 * struct isp_cooling_device - data for cooling device with isp
 * @id: unique integer value corresponding to each isp_cooling_device
 *	registered.
 * @cool_dev: thermal_cooling_device pointer to keep track of the
 *	registered cooling device.
 * @isp_state: integer value representing the current state of isp
 *	cooling	devices.
 * @isp_val: integer value representing the absolute value of the clipped
 *	fps.
 * @allowed_isp: all the isp involved for this isp_cooling_device.
 *
 * This structure is required for keeping information of each
 * isp_cooling_device registered. In order to prevent corruption of this a
 * mutex lock cooling_isp_lock is used.
 */
struct isp_cooling_device {
	int id;
	struct thermal_cooling_device *cool_dev;
	unsigned int isp_state;
	unsigned int isp_val;
};
static DEFINE_IDR(isp_idr);
static DEFINE_MUTEX(cooling_isp_lock);
static BLOCKING_NOTIFIER_HEAD(isp_notifier);

static unsigned int isp_dev_count;

struct isp_fps_table *isp_fps_table;

/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 *
 * This function will populate @id with an unique
 * id, using the idr API.
 *
 * Return: 0 on success, an error code on failure.
 */
static int get_idr(struct idr *idr, int *id)
{
	int ret;

	mutex_lock(&cooling_isp_lock);
	ret = idr_alloc(idr, NULL, 0, 0, GFP_KERNEL);
	mutex_unlock(&cooling_isp_lock);
	if (unlikely(ret < 0))
		return ret;
	*id = ret;

	return 0;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&cooling_isp_lock);
	idr_remove(idr, id);
	mutex_unlock(&cooling_isp_lock);
}

/* Below code defines functions to be used for isp as cooling device */

enum isp_cooling_property {
	GET_LEVEL,
	GET_FPS,
	GET_MAXL,
};

/**
 * get_property - fetch a property of interest for a give isp.
 * @isp: isp for which the property is required
 * @input: query parameter
 * @output: query return
 * @property: type of query (fps, level, max level)
 *
 * This is the common function to
 * 1. get maximum isp cooling states
 * 2. translate fps to cooling state
 * 3. translate cooling state to fps
 * Note that the code may be not in good shape
 * but it is written in this way in order to:
 * a) reduce duplicate code as most of the code can be shared.
 * b) make sure the logic is consistent when translating between
 *    cooling states and fps.
 *
 * Return: 0 on success, -EINVAL when invalid parameters are passed.
 */
static int get_property(unsigned int isp, unsigned long input,
			unsigned int *output,
			enum isp_cooling_property property)
{
	int i;
	unsigned long max_level = 0, level = 0;
	unsigned int fps = ISP_FPS_ENTRY_INVALID;
	int descend = -1;
	struct isp_fps_table *pos, *table =
					isp_fps_table;

	if (!output)
		return -EINVAL;

	if (!table)
		return -EINVAL;

	isp_fps_for_each_valid_entry(pos, table) {
		/* ignore duplicate entry */
		if (fps == pos->fps)
			continue;

		/* get the fps order */
		if (fps != ISP_FPS_ENTRY_INVALID && descend == -1)
			descend = fps > pos->fps;

		fps = pos->fps;
		max_level++;
	}

	/* No valid cpu fps entry */
	if (max_level == 0)
		return -EINVAL;

	/* max_level is an index, not a counter */
	max_level--;

	/* get max level */
	if (property == GET_MAXL) {
		*output = (unsigned int)max_level;
		return 0;
	}

	i = 0;
	level = (int)input;
	isp_fps_for_each_valid_entry(pos, table) {
		/* ignore duplicate entry */
		if (fps == pos->fps)
			continue;

		/* now we have a valid fps entry */
		fps = pos->fps;

		if (property == GET_LEVEL && (unsigned int)input == fps) {
			/* get level by fps */
			*output = (unsigned int)(descend ? i : (max_level - i));
			return 0;
		}
		if (property == GET_FPS && level == i) {
			/* get fps by level */
			*output = fps;
			return 0;
		}
		i++;
	}

	return -EINVAL;
}

/**
 * isp_cooling_get_level - for a give isp, return the cooling level.
 * @isp: isp for which the level is required
 * @fps: the fps of interest
 *
 * This function will match the cooling level corresponding to the
 * requested @fps and return it.
 *
 * Return: The matched cooling level on success or THERMAL_CSTATE_INVALID
 * otherwise.
 */
unsigned long isp_cooling_get_level(unsigned int isp, unsigned int fps)
{
	unsigned int val;

	if (get_property(isp, (unsigned long)fps, &val, GET_LEVEL))
		return THERMAL_CSTATE_INVALID;

	return (unsigned long)val;
}
EXPORT_SYMBOL_GPL(isp_cooling_get_level);

/**
 * isp_cooling_get_fps - for a give isp, return the fps value corresponding to cooling level.
 * @isp: isp for which the level is required
 * @level: the cooling level
 *
 * This function will match the fps value corresponding to the
 * requested @level and return it.
 *
 * Return: The matched fps value on success or ISP_FPS_INVALID otherwise.
 */
unsigned long isp_cooling_get_fps(unsigned int isp, unsigned long level)
{
	unsigned int val;

	if (get_property(isp, level, &val, GET_FPS))
		return ISP_FPS_INVALID;

	return (unsigned long)val;
}
EXPORT_SYMBOL_GPL(isp_cooling_get_fps);

/**
 * isp_apply_cooling - function to apply fps clipping.
 * @isp_device: isp_cooling_device pointer containing fps
 *	clipping data.
 * @cooling_state: value of the cooling state.
 *
 * Function used to make sure the isp layer is aware of current thermal
 * limits. The limits are applied by updating the isp policy.
 *
 * Return: 0 on success, an error code otherwise (-EINVAL in case wrong
 * cooling state).
 */
static int isp_apply_cooling(struct isp_cooling_device *isp_device,
				 unsigned long cooling_state)
{
	/* Check if the old cooling action is same as new cooling action */
	if (isp_device->isp_state == cooling_state)
		return 0;

	isp_device->isp_state = (unsigned int)cooling_state;

	blocking_notifier_call_chain(&isp_notifier, ISP_THROTTLING, &cooling_state);

	return 0;
}

/* isp cooling device callback functions are defined below */

/**
 * isp_get_max_state - callback function to get the max cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the max cooling state.
 *
 * Callback for the thermal cooling device to return the isp
 * max cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int isp_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	unsigned int count = 0;
	int ret;

	ret = get_property(0, 0, &count, GET_MAXL);

	if (count > 0)
		*state = count;

	return ret;
}

/**
 * isp_get_cur_state - callback function to get the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the current cooling state.
 *
 * Callback for the thermal cooling device to return the isp
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int isp_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct isp_cooling_device *isp_device = cdev->devdata;

	*state = isp_device->isp_state;

	return 0;
}

/**
 * isp_set_cur_state - callback function to set the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: set this variable to the current cooling state.
 *
 * Callback for the thermal cooling device to change the isp
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int isp_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct isp_cooling_device *isp_device = cdev->devdata;

	return isp_apply_cooling(isp_device, state);
}

/* Bind isp callbacks to thermal cooling device ops */
static struct thermal_cooling_device_ops const isp_cooling_ops = {
	.get_max_state = isp_get_max_state,
	.get_cur_state = isp_get_cur_state,
	.set_cur_state = isp_set_cur_state,
};

int exynos_tmu_isp_add_notifier(struct notifier_block *n)
{
	return blocking_notifier_chain_register(&isp_notifier, n);
}
EXPORT_SYMBOL_GPL(exynos_tmu_isp_add_notifier);

static int parse_ect_cooling_level(struct thermal_cooling_device *cdev,
				   char *tz_name)
{
	struct thermal_instance *instance;
	struct thermal_zone_device *tz;
	bool foundtz = false;
	void *thermal_block;
	struct ect_ap_thermal_function *function;
	int i, temperature;
	unsigned int freq;

	mutex_lock(&cdev->lock);
	list_for_each_entry(instance, &cdev->thermal_instances, cdev_node) {
		tz = instance->tz;
		if (!strncasecmp(tz_name, tz->type, THERMAL_NAME_LENGTH)) {
			foundtz = true;
			break;
		}
	}
	mutex_unlock(&cdev->lock);

	if (!foundtz)
		goto skip_ect;

	thermal_block = ect_get_block(BLOCK_AP_THERMAL);
	if (!thermal_block)
		goto skip_ect;

	function = ect_ap_thermal_get_function(thermal_block, tz_name);
	if (!function)
		goto skip_ect;

	for (i = 0; i < function->num_of_range; ++i) {
		unsigned long max_level = 0;
		int level;

		temperature = function->range_list[i].lower_bound_temperature;
		freq = function->range_list[i].max_frequency;

		instance = get_thermal_instance(tz, cdev, i);
		if (!instance) {
			pr_err("%s: (%s, %d)instance isn't valid\n", __func__, tz_name, i);
			goto skip_ect;
		}

		cdev->ops->get_max_state(cdev, &max_level);
		level = isp_cooling_get_level(0, freq);

		if (level == THERMAL_CSTATE_INVALID)
			level = max_level;

		instance->upper = level;

		pr_info("Parsed From ECT : %s: [%d] Temperature : %d, frequency : %u, level: %d\n",
			tz_name, i, temperature, freq, level);
	}
skip_ect:
	return 0;
}

/**
 * __isp_cooling_register - helper function to create isp cooling device
 * @np: a valid struct device_node to the cooling device device tree node
 * @clip_isp: ispmask of isp where the fps constraints will happen.
 *
 * This interface function registers the isp cooling device with the name
 * "thermal-isp-%x". This api can support multiple instances of isp
 * cooling devices. It also gives the opportunity to link the cooling device
 * with a device tree node, in order to bind it via the thermal DT code.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
static struct thermal_cooling_device *
__isp_cooling_register(struct device_node *np,
			   const struct cpumask *clip_isp)
{
	struct thermal_cooling_device *cool_dev;
	struct isp_cooling_device *isp_dev = NULL;
	char dev_name[THERMAL_NAME_LENGTH];
	int ret = 0;

	isp_dev = kzalloc(sizeof(struct isp_cooling_device),
			      GFP_KERNEL);
	if (!isp_dev)
		return ERR_PTR(-ENOMEM);

	ret = get_idr(&isp_idr, &isp_dev->id);
	if (ret) {
		kfree(isp_dev);
		return ERR_PTR(-EINVAL);
	}

	snprintf(dev_name, sizeof(dev_name), "thermal-isp-%d",
		 isp_dev->id);

	cool_dev = thermal_of_cooling_device_register(np, dev_name, isp_dev,
						      &isp_cooling_ops);
	if (IS_ERR(cool_dev)) {
		release_idr(&isp_idr, isp_dev->id);
		kfree(isp_dev);
		return cool_dev;
	}

	parse_ect_cooling_level(cool_dev, "ISP");

	isp_dev->cool_dev = cool_dev;
	isp_dev->isp_state = 0;
	mutex_lock(&cooling_isp_lock);

	isp_dev_count++;

	mutex_unlock(&cooling_isp_lock);

	return cool_dev;
}

/**
 * isp_cooling_register - function to create isp cooling device.
 * @clip_isp: cpumask of gpus where the fps constraints will happen.
 *
 * This interface function registers the isp cooling device with the name
 * "thermal-isp-%x". This api can support multiple instances of isp
 * cooling devices.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
isp_cooling_register(const struct cpumask *clip_isp)
{
	return __isp_cooling_register(NULL, clip_isp);
}
EXPORT_SYMBOL_GPL(isp_cooling_register);

/**
 * of_isp_cooling_register - function to create isp cooling device.
 * @np: a valid struct device_node to the cooling device device tree node
 * @clip_isp: cpumask of gpus where the fps constraints will happen.
 *
 * This interface function registers the isp cooling device with the name
 * "thermal-isp-%x". This api can support multiple instances of isp
 * cooling devices. Using this API, the isp cooling device will be
 * linked to the device tree node provided.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
of_isp_cooling_register(struct device_node *np,
			    const struct cpumask *clip_isp)
{
	if (!np)
		return ERR_PTR(-EINVAL);

	return __isp_cooling_register(np, clip_isp);
}
EXPORT_SYMBOL_GPL(of_isp_cooling_register);

/**
 * isp_cooling_unregister - function to remove isp cooling device.
 * @cdev: thermal cooling device pointer.
 *
 * This interface function unregisters the "thermal-isp-%x" cooling device.
 */
void isp_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct isp_cooling_device *isp_dev;

	if (!cdev)
		return;

	isp_dev = cdev->devdata;
	mutex_lock(&cooling_isp_lock);
	isp_dev_count--;
	mutex_unlock(&cooling_isp_lock);

	thermal_cooling_device_unregister(isp_dev->cool_dev);
	release_idr(&isp_idr, isp_dev->id);
	kfree(isp_dev);
}
EXPORT_SYMBOL_GPL(isp_cooling_unregister);

/**
 * isp_cooling_table_init - function to make ISP fps throttling table.
 *
 * Return : a valid struct isp_fps_table pointer on success,
 * on failture, it returns a corresponding ERR_PTR().
 */
static int isp_cooling_table_init(void)
{
	int ret = 0, i = 0;
#if defined(CONFIG_ECT) || defined(CONFIG_ECT_MODULE)
	void *thermal_block;
	struct ect_ap_thermal_function *function;
	int last_fps = -1, count = 0;
#endif

#if defined(CONFIG_ECT) || defined(CONFIG_ECT_MODULE)
	thermal_block = ect_get_block(BLOCK_AP_THERMAL);
	if (thermal_block == NULL) {
		pr_err("Failed to get thermal block");
		return -ENODEV;
	}

	function = ect_ap_thermal_get_function(thermal_block, "ISP");
	if (function == NULL) {
		pr_err("Failed to get ISP thermal information");
		return -ENODEV;
	}

	/* Table size can be num_of_range + 1 since last row has the value of TABLE_END */
	isp_fps_table = kzalloc(sizeof(struct isp_fps_table) * (function->num_of_range + 1), GFP_KERNEL);

	for (i = 0; i < function->num_of_range; i++) {
		if (last_fps == function->range_list[i].max_frequency)
			continue;

		isp_fps_table[count].flags = 0;
		isp_fps_table[count].driver_data = count;
		isp_fps_table[count].fps = function->range_list[i].max_frequency;
		last_fps = isp_fps_table[count].fps;

		pr_info("[ISP TMU] index : %d, fps : %d\n",
			isp_fps_table[count].driver_data, isp_fps_table[count].fps);
		count++;
	}

	if (i == function->num_of_range)
		isp_fps_table[count].fps = ISP_FPS_TABLE_END;
#else
	pr_err("[ISP cooling] could not find ECT information\n");
	ret = -EINVAL;
#endif
	return ret;
}

int exynos_isp_cooling_init(void)
{
	struct device_node *np;
	struct thermal_cooling_device *dev;
	int ret = 0;

	ret = isp_cooling_table_init();

	if (ret) {
		pr_err("Fail to initialize isp_cooling_table\n");
		return ret;
	}

	np = of_find_node_by_name(NULL, "exynos_isp_thermal");

	if (!np) {
		pr_err("Fail to find device node\n");
		return -EINVAL;
	}

	dev = of_isp_cooling_register(np, 0);

	if (IS_ERR(dev)) {
		pr_err("Fail to register isp cooling\n");
		return -EINVAL;
	}

	return ret;
}

MODULE_LICENSE("GPL");