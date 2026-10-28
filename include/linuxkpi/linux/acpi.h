/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACPI para drivers de Linux (L7). Aurora tiene un interprete AML limitado
 * (kernel/acpi_aml.c: evaluar rutas sin argumentos), asi que aqui no hay
 * namespace por dispositivo: ACPI_HANDLE()/ACPI_COMPANION() son NULL y todo
 * lo que evalua metodos (_DSM, _DOD, _ADR) falla con AE_NOT_FOUND. i915 sigue
 * funcionando: el opregion se lee por ASLS (config PCI), no por ACPI.
 * Pendiente (L9): exponer aml_eval con argumentos y handles por dispositivo.
 */
#ifndef _LKPI_LINUX_ACPI_H
#define _LKPI_LINUX_ACPI_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/uuid.h>
#include <linux/device.h>
#include <linux/property.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef u32 acpi_object_type;
typedef u64 acpi_integer;

#define AE_OK			0x0000
#define AE_ERROR		0x0001
#define AE_NOT_FOUND		0x0005
#define AE_NOT_EXIST		0x0006
#define AE_SUPPORT		0x0003
#define ACPI_SUCCESS(a)		(!(a))
#define ACPI_FAILURE(a)		(a)

#define ACPI_TYPE_ANY		0
#define ACPI_TYPE_INTEGER	1
#define ACPI_TYPE_STRING	2
#define ACPI_TYPE_BUFFER	3
#define ACPI_TYPE_PACKAGE	4

#define ACPI_FULL_PATHNAME	0
#define ACPI_SINGLE_NAME	1
#define ACPI_ALLOCATE_BUFFER	((size_t)-1)

#define ACPI_STATE_S0	0
#define ACPI_STATE_S3	3
#define ACPI_STATE_S4	4
#define ACPI_STATE_S5	5

struct acpi_buffer {
	size_t length;
	void *pointer;
};

union acpi_object {
	acpi_object_type type;
	struct { acpi_object_type type; u64 value; } integer;
	struct { acpi_object_type type; u32 length; char *pointer; } string;
	struct { acpi_object_type type; u32 length; u8 *pointer; } buffer;
	struct { acpi_object_type type; u32 count; union acpi_object *elements; } package;
};

struct acpi_device {
	acpi_handle handle;
	struct device dev;
};

struct acpi_bus_event {
	struct list_head node;
	char device_class[20];
	char bus_id[15];
	u32 type;
	u32 data;
};

struct acpi_resource_source { u8 index; u16 string_length; char *string_ptr; };
struct acpi_resource_i2c_serialbus {
	u8 revision_id;
	u16 slave_address;
	u32 connection_speed;
	struct acpi_resource_source resource_source;
};
struct acpi_resource { u32 type; u32 length; };

#define ACPI_FREE(p)		kfree(p)
#define ACPI_HANDLE(dev)	((acpi_handle)NULL)
#define ACPI_COMPANION(dev)	((struct acpi_device *)NULL)
#define has_acpi_companion(dev)	(false)

static inline acpi_handle acpi_device_handle(struct acpi_device *adev) { return adev ? adev->handle : NULL; }
static inline struct fwnode_handle *acpi_fwnode_handle(struct acpi_device *adev) { return NULL; }
static inline struct acpi_device *acpi_find_child_device(struct acpi_device *parent, u64 address, bool check_children)
{
	return NULL;
}
static inline acpi_status acpi_get_handle(acpi_handle parent, const char *pathname, acpi_handle *ret)
{
	return AE_NOT_FOUND;
}
static inline acpi_status acpi_get_name(acpi_handle handle, u32 name_type, struct acpi_buffer *buffer)
{
	return AE_NOT_FOUND;
}
static inline union acpi_object *acpi_evaluate_dsm(acpi_handle handle, const guid_t *guid, u64 rev, u64 func,
						   union acpi_object *argv4)
{
	return NULL;
}
static inline union acpi_object *acpi_evaluate_dsm_typed(acpi_handle handle, const guid_t *guid, u64 rev, u64 func,
							 union acpi_object *argv4, acpi_object_type type)
{
	return NULL;
}
static inline bool acpi_check_dsm(acpi_handle handle, const guid_t *guid, u64 rev, u64 funcs) { return false; }
static inline int acpi_dev_get_resources(struct acpi_device *adev, struct list_head *list,
					 int (*preproc)(struct acpi_resource *, void *), void *preproc_data)
{
	return 0;
}
static inline void acpi_dev_free_resource_list(struct list_head *list) {}
static inline int register_acpi_notifier(struct notifier_block *nb) { return 0; }
static inline int unregister_acpi_notifier(struct notifier_block *nb) { return 0; }
static inline u32 acpi_target_system_state(void) { return ACPI_STATE_S0; }

#endif
