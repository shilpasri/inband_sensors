/*
 * POWERNV debug  perf driver to export inband occ_sensors
 *
 * (C) Copyright IBM 2015
 *
 * Author: Shilpasri G Bhat <shilpa.bhat@linux.vnet.ibm.com>
 *
 * Usage:
 * Build this driver against your kernel and load the module.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/perf_event.h>

#define BE(x, s)	be_to_cpu(x, s)

typedef struct sensor {
	char name[30];
	char *unit;
	u64 paddr;
	u64 vaddr;
	u32 size;
	struct device_attribute attr;
} sensor_t;

typedef struct core {
	sensor_t *sensors;
} core_t;

struct chip {
	int id;
	char name[30];
	u64 pbase;
	u64 vbase;
	sensor_t *sensors;
	core_t	*cores;
} *chips;

sensor_t *system_sensors;

static unsigned int nr_chips, nr_system_sensors, nr_chip_sensors;
static unsigned int nr_cores_sensors, nr_cores;
static u64 power_addr;
static u64 chip_power_addr;
static u64 chip_energy_addr;
static u64 system_energy_addr;
static u64 count_addr;

static struct attribute **system_attrs;
static struct attribute ***chip_attrs;

static struct attribute_group system_attr_group = {
	.name = "system",
};

static struct attribute_group **chip_attr_group;
struct kobject *occ_sensor_kobj;

unsigned long be_to_cpu(u64 addr, u32 size)
{
	switch (size) {
	case 16:
		return __be16_to_cpu(*(u16 *)addr);
	case 32:
		return __be32_to_cpu(*(u32 *)addr);
	case 64:
		return __be64_to_cpu(*(u64 *)addr);
	}
	return 0;
}

static ssize_t sensor_attr_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	sensor_t *sensor = container_of(attr, sensor_t, attr);

	return sprintf(buf, "%lu %s\n", BE(sensor->vaddr, sensor->size),
		       sensor->unit);
}

#define add_sensor(node, var, len, reg)					   \
do {									   \
	if (of_property_read_u64(node, "reg", &var.paddr)) {		   \
		pr_info("%s node cannot read reg property\n", node->name); \
		continue;						   \
	}								   \
	reg = of_get_property(node, "reg", &len);			   \
	var.size = of_read_number(reg, 3) * 8;				   \
	if (of_property_read_string(node, "unit", &var.unit)) {		   \
		pr_info("%s node cannot read unit\n", node->name);	   \
	}								   \
	var.vaddr = (u64)phys_to_virt(var.paddr);			   \
	pr_info("Sensor : %s *(%lx) = %lu\n", node->name,		   \
		(unsigned long)var.vaddr, BE(var.vaddr, var.size));	   \
	var.attr.attr.mode = S_IRUGO;					   \
	var.attr.show = sensor_attr_show;				   \
	var.attr.store = NULL;						   \
} while (0)

static int add_system_sensor(struct device_node *snode)
{
	struct device_node *node;
	const __be32 *reg;
	int len, i = 0;

	for_each_child_of_node(snode, node) {
		add_sensor(node, system_sensors[i], len, reg);
		sprintf(system_sensors[i].name, "%s", node->name);
		system_sensors[i].attr.attr.name = system_sensors[i].name;
		if (!strcmp(node->name, "power"))
			power_addr = system_sensors[i].vaddr;

		if (!strcmp(node->name, "count"))
			count_addr = system_sensors[i].vaddr;

		if (!strcmp(node->name, "system-energy"))
			system_energy_addr = system_sensors[i].vaddr;
		i++;
	}

	return 0;
}

static int add_core_sensor(struct device_node *cnode, int chipid, int cid)
{
	const __be32 *reg;
	struct device_node *node;
	int i = 0, len;
	unsigned int id;

	if (of_property_read_u32(cnode, "ibm,core-id", &id)) {
		pr_info("Core_id not found");
		return -EINVAL;
	}
	for_each_child_of_node(cnode, node) {
		add_sensor(node, chips[chipid].cores[cid].sensors[i], len, reg);
		sprintf(chips[chipid].cores[cid].sensors[i].name, "core%d-%s",
			cid+1, node->name);
		chips[chipid].cores[cid].sensors[i].attr.attr.name =
			chips[chipid].cores[cid].sensors[i].name;
		i++;
	}
	return 0;
}

static int add_chip_sensor(struct device_node *chip_node)
{
	const __be32 *reg;
	u32 len;
	struct device_node *node;
	int i, j, k, rc = 0;
	u32 id = 0;

	if (of_property_read_u32(chip_node, "ibm,chip-id", &id)) {
		pr_err("Chip not found\n");
		goto out;
	}
	for (i = 0; i < nr_chips; i++)
		if (chips[i].id == id)
			break;

	if (of_property_read_u64(chip_node, "reg", &chips[i].pbase)) {
		pr_err("Chip Homer sensor offset not found\n");
		rc = -ENODEV;
		goto out;
	}

	chips[i].vbase = (u64)phys_to_virt(chips[i].pbase);
	pr_info("i = %d Chip %d sensor pbase= %lx, vbase = %lx (%lx)\n", i,
		 chips[i].id, (unsigned long)chips[i].pbase,
		 (unsigned long)chips[i].vbase, BE(chips[i].vbase+4, 16));

	j = k = 0;
	for_each_child_of_node(chip_node, node) {
		if (!strcmp(node->name, "core")) {
			add_core_sensor(node, i, k++);
			continue;
		}
		add_sensor(node, chips[i].sensors[j], len, reg);
		sprintf(chips[i].sensors[j].name, "%s", node->name);
		chips[i].sensors[j].attr.attr.name = chips[i].sensors[j].name;
		if (!strcmp(node->name, "power"))
			chip_power_addr = chips[i].sensors[j].vaddr;
		if (!strcmp(node->name, "chip-energy"))
			chip_energy_addr = chips[i].sensors[j].vaddr;
		j++;
	}
out:
	return rc;
}


static int init_chip(void)
{
	unsigned int chip[256];
	unsigned int cpu, i, j, k, l;
	unsigned int prev_chip_id = UINT_MAX;
	struct device_node *sensor_node, *node;
	int rc = 0;

	for_each_possible_cpu(cpu) {
		unsigned int id = cpu_to_chip_id(cpu);

		if (prev_chip_id != id) {
			prev_chip_id = id;
			chip[nr_chips++] = id;
		}
	}
	pr_info("nr_chips %d\n", nr_chips);
	chips = kcalloc(nr_chips, sizeof(struct chip), GFP_KERNEL);
	if (!chips) {
		pr_info("Out of memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < nr_chips; i++)
		chips[i].id = chip[i];

	sensor_node = of_find_node_by_path("/occ_sensors");

	if (of_property_read_u32(sensor_node, "nr_system_sensors",
				 &nr_system_sensors)) {
		pr_info("nr_system_sensors not found\n");
		return -EINVAL;
	}

	if (of_property_read_u32(sensor_node, "nr_chip_sensors",
				 &nr_chip_sensors)) {
		pr_info("nr_chip_sensors not found\n");
		return -EINVAL;
	}
	if (of_property_read_u32(sensor_node, "nr_core_sensors",
				 &nr_cores_sensors)) {
		pr_info("nr_core_sensors not found\n");
		return -EINVAL;
	}

	nr_cores = cpumask_weight(cpu_present_mask)/(8 * nr_chips);
	system_sensors = kcalloc(nr_system_sensors, sizeof(sensor_t),
				 GFP_KERNEL);
	for (i = 0; i < nr_chips; i++) {
		chips[i].sensors = kcalloc(nr_chip_sensors, sizeof(sensor_t),
					   GFP_KERNEL);
		chips[i].cores = kcalloc(nr_cores, sizeof(core_t), GFP_KERNEL);
		for (j = 0; j < nr_cores; j++)
			chips[i].cores[j].sensors = kcalloc(nr_cores_sensors,
							    sizeof(sensor_t),
							    GFP_KERNEL);
	}

	for_each_child_of_node(sensor_node, node) {
		if (!strcmp(node->name, "chip"))
			rc = add_chip_sensor(node);
		else
			rc = add_system_sensor(node);
		if (rc)
			goto out;
	}

	system_attrs = kcalloc(nr_system_sensors, sizeof(struct attribute *),
			       GFP_KERNEL);
	chip_attrs = kcalloc(nr_chip_sensors, sizeof(struct attribute **),
			     GFP_KERNEL);
	for (i = 0; i < nr_chips; i++)
		chip_attrs[i] = kcalloc(
				nr_chip_sensors + nr_cores_sensors * nr_cores,
				sizeof(struct attribute *), GFP_KERNEL);

	for (i = 0; i < nr_system_sensors; i++)
		system_attrs[i] = &system_sensors[i].attr.attr;
	system_attrs[i] = NULL;

	for (j = 0; j < nr_chips; j++) {
		i = 0;
		for (k = 0; k < nr_chip_sensors; k++, i++)
			chip_attrs[j][i] = &chips[j].sensors[k].attr.attr;
		for (k = 0; k < nr_cores; k++)
			for (l = 0; l < nr_cores_sensors; l++, i++)
				chip_attrs[j][i] =
					&chips[j].cores[k].sensors[l].attr.attr;
		chip_attrs[j][i] = NULL;
	}
	chip_attrs[j] = NULL;

	chip_attr_group = kcalloc(nr_chips, sizeof(struct attribute_group *),
				  GFP_KERNEL);
	for (i = 0; i < nr_chips; i++) {
		chip_attr_group[i] = kzalloc(sizeof(struct attribute_group),
					     GFP_KERNEL);
		sprintf(chips[i].name, "chip%d", chips[i].id);
		chip_attr_group[i]->name = chips[i].name;
		chip_attr_group[i]->attrs = chip_attrs[i];
	}
	chip_attr_group[i] = NULL;
	system_attr_group.attrs = system_attrs;
out:
	return rc;
}

static int sensor_init(void)
{
	int rc, i;

	rc = init_chip();
	if (rc)
		goto out;

	occ_sensor_kobj = kobject_create_and_add("occ_sensors",
						 &cpu_subsys.dev_root->kobj);
	rc = sysfs_create_group(occ_sensor_kobj, &system_attr_group);
	if (rc) {
		pr_info("Failed to create system attribute group\n");
		goto out;
	}

	for (i = 0; i < nr_chips; i++) {
		rc = sysfs_create_group(occ_sensor_kobj, chip_attr_group[i]);
		if (rc) {
			pr_info("Chip %d failed to create chip_attr_group\n",
				chips[i].id);
			goto out;
		}
	}
out:
	return rc;
}

static void sensor_exit(void)
{
	int i, j;

	kobject_put(occ_sensor_kobj);

	kfree(system_attrs);
	kfree(chip_attrs);
	kfree(system_sensors);
	for (i = 0; i < nr_chips; i++) {
		kfree(chip_attr_group[i]);
		kfree(chips[i].sensors);
		kfree(chips[i].cores);
		for (j = 0; j < nr_cores; j++)
			kfree(chips[i].cores[j].sensors);
	}
	kfree(chip_attr_group);
	kfree(chips);
}

module_init(sensor_init);
module_exit(sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shilpasri G Bhat <shilpa.bhat at linux.vnet.ibm.com>");
