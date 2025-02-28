#ifndef VIRTIO_NUMA_MIGRATE_H
#define VIRTIO_NUMA_MIGRATE_H

#include "standard-headers/linux/virtio_ids.h"
#include "hw/virtio/virtio.h"
#include "qemu/event_notifier.h"

#define TYPE_VIRTIO_NUMA_MIGRATE "virtio-numa-migrate"
#define VIRTIO_NUMA_MIGRATE(obj) \
    OBJECT_CHECK(VirtioNUMAMigrate, (obj), TYPE_VIRTIO_NUMA_MIGRATE)

/* Device structure */
typedef struct VirtioNUMAMigrate {
    VirtIODevice parent_obj;
    VirtQueue *notify_vq;
    EventNotifier host_notifier;
    int migration_state;
} VirtioNUMAMigrate;


/* NUMA migration notification structure */
typedef struct VirtioNUMANotify {
    uint64_t gpa_start;     /* Starting GPA of migrated range */
    uint64_t size;          /* Size of the migrated range */
    int32_t target_node;    /* Target NUMA node */
    int32_t status;         /* Migration status */
} VirtioNUMANotify;

#endif /* VIRTIO_NUMA_MIGRATE_H */
