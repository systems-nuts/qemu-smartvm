#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "exec/cpu-common.h"
#include "exec/address-spaces.h"
#include "exec/ramlist.h"
#include "exec/ramblock.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"
#include "qemu/option.h"
#include "qapi/qmp/qdict.h"
#include "standard-headers/linux/virtio_ids.h"

/* For libnuma functions */
#include <numa.h>
#include <numaif.h>

#include "hw/virtio/virtio-numa-migrate.h"

#define QUEUE_SIZE 16
#define PAGE_BITS  12
#define PAGE_SIZE  (1 << PAGE_BITS)

/* NUMA Migration states */
typedef enum {
    NUMA_MIGRATE_IDLE = 0,
    NUMA_MIGRATE_IN_PROGRESS,
    NUMA_MIGRATE_COMPLETE,
    NUMA_MIGRATE_FAILED
} NUMAMigrateState;

static VirtioNUMAMigrate *g_numa_migrate_dev = NULL;

static const VMStateDescription vmstate_virtio_numa_migrate = {
    .name = "virtio-numa-migrate",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

#if 0
static struct VirtioNUMAMigrateReq *virtio_numa_migrate_get_request(
        VirtioNUMAMigrate *s, VirtQueue *vq)
{
    struct VirtioNUMAMigrateReq *req = virtqueue_pop(vq, 
                                      sizeof(struct VirtioNUMAMigrateReq));
    if (req) {
        req->vq = vq;
        req->dev = s;
    }
    return req;
}

static void virtio_numa_migrate_free_request(struct VirtioNUMAMigrateReq *req)
{
    g_free(req);
}
#endif

static void *virtio_numa_gpa2hva(hwaddr addr, uint64_t size, Error **errp)
{
    MemoryRegionSection mrs = memory_region_find(get_system_memory(),
                                                addr, size);

    if (!mrs.mr) {
        error_setg(errp, "No memory is mapped at address 0x%" HWADDR_PRIx, addr);
        return NULL;
    }

    if (!memory_region_is_ram(mrs.mr) && !memory_region_is_romd(mrs.mr)) {
        error_setg(errp, "Memory at address 0x%" HWADDR_PRIx " is not RAM", addr);
        memory_region_unref(mrs.mr);
        return NULL;
    }

    return qemu_map_ram_ptr(mrs.mr->ram_block, mrs.offset_within_region);
}

static void virtio_notify_guest_migration(VirtioNUMAMigrate *s, 
                                         hwaddr gpa_start, 
                                         size_t size, 
                                         int target_node)
{
    VirtQueue *vq = s->notify_vq;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    struct VirtioNUMANotify notify;
    VirtQueueElement *elem;
    size_t sz;
    

    memset(&notify, 0, sizeof(notify));
    notify.gpa_start = cpu_to_le64(gpa_start);
    notify.size = cpu_to_le64(size);
    notify.target_node = cpu_to_le32(target_node);
    notify.status = cpu_to_le32(s->migration_state);

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));

    if (!elem) {
        error_report_once(
            "no buffer available in event queue to report event");
        return;
    }
    
    if (iov_size(elem->in_sg, elem->in_num) < sizeof(notify)) {
        virtio_error(vdev, "error buffer of wrong size");
        virtqueue_detach_element(vq, elem, 0);
        g_free(elem);
        return;
    }

    sz = iov_from_buf(elem->in_sg, elem->in_num, 0,
                      &notify, sizeof(notify));
    assert(sz == sizeof(notify));

    virtqueue_push(vq, elem, sz);
    virtio_notify(vdev, vq);
    g_free(elem);

}

static int do_numa_migrate_pages(VirtioNUMAMigrate *s, 
                                hwaddr gpa_start, 
                                size_t size, 
                                int target_node)
{
    Error *local_err = NULL;
    void *hva_start;
    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int *nodes, *status;
    void **pages;
    int i, ret = 0;
    
    /* Check if libnuma is available and initialized */
    if (numa_available() < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "NUMA support not available\n");
        return -ENOSYS;
    }
    
    /* Verify target node exists */
    if (target_node < 0 || target_node >= numa_max_node() + 1 || 
        !numa_bitmask_isbitset(numa_nodes_ptr, target_node)) {
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid NUMA node %d\n", target_node);
        return -EINVAL;
    }
    
    hva_start = virtio_numa_gpa2hva(gpa_start, size, &local_err);
    if (!hva_start) {
        qemu_log_mask(LOG_GUEST_ERROR, "Failed to map GPA to HVA: %s\n", 
                     error_get_pretty(local_err));
        error_free(local_err);
        return -EINVAL;
    }
    
    pages = g_new(void *, npages);
    nodes = g_new(int, npages);
    status = g_new(int, npages);
    
    for (i = 0; i < npages; i++) {
        pages[i] = hva_start + i * PAGE_SIZE;
        nodes[i] = target_node;
    }
    
    qemu_log("Migrating %zu pages starting at GPA 0x%" HWADDR_PRIx " to NUMA node %d\n",
             npages, gpa_start, target_node);
    
    s->migration_state = NUMA_MIGRATE_IN_PROGRESS;
    
    /* Use the actual numa_move_pages syscall from libnuma */
    ret = numa_move_pages(0, /* Current process */
                         npages,
                         pages,
                         nodes,
                         status,
                         0 /* flags */);
    
    /* Check migration status */
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "NUMA migration failed: %s\n", strerror(errno));
        s->migration_state = NUMA_MIGRATE_FAILED;
        ret = -errno;
    } else {
        s->migration_state = NUMA_MIGRATE_COMPLETE;
        qemu_log("NUMA migration completed successfully\n");
        
        /* Log the migration status for each page */
        for (i = 0; i < npages; i++) {
            if (status[i] < 0) {
                qemu_log("Page %d migration failed: %s\n", 
                         i, strerror(-status[i]));
            } else if (status[i] != target_node) {
                qemu_log("Page %d migrated to unexpected node %d (target: %d)\n", 
                         i, status[i], target_node);
            }
        }
    }
    
    g_free(pages);
    g_free(nodes);
    g_free(status);
    
    return ret;
}

/* Monitor command handler */
void hmp_numa_migrate(Monitor *mon, const QDict *qdict)
{
    hwaddr gpa_start;
    size_t size;
    int target_node;
    int ret;
    
    if (!g_numa_migrate_dev) {
        monitor_printf(mon, "Error: virtio-numa-migrate device not available\n");
        return;
    }
    
    gpa_start = qdict_get_int(qdict, "gpa");
    size = qdict_get_int(qdict, "size");
    target_node = qdict_get_int(qdict, "node");
    
    /* Validate node id using libnuma */
    if (numa_available() < 0) {
        monitor_printf(mon, "Error: NUMA support not available\n");
        return;
    }
    
    if (target_node < 0 || target_node >= numa_max_node() + 1 || 
        !numa_bitmask_isbitset(numa_nodes_ptr, target_node)) {
        monitor_printf(mon, "Error: Invalid NUMA node %d\n", target_node);
        return;
    }
    
    ret = do_numa_migrate_pages(g_numa_migrate_dev, gpa_start, size, target_node);
    if (ret < 0) {
        monitor_printf(mon, "NUMA migration failed: %s\n", strerror(-ret));
    } else {
        monitor_printf(mon, "NUMA migration initiated for GPA range 0x%" HWADDR_PRIx 
                      " - 0x%" HWADDR_PRIx " to node %d\n", 
                      gpa_start, gpa_start + size - 1, target_node);
    }
    
    /* Notify guest about migration */
    virtio_notify_guest_migration(g_numa_migrate_dev, gpa_start, size, target_node);
}

#if 0
static void virtio_numa_migrate_handle_notify(VirtIODevice *vdev, VirtQueue *vq)
{
    struct VirtioNUMAMigrateReq *req;
    VirtioNUMAMigrate *s = VIRTIO_NUMA_MIGRATE(vdev);

    while ((req = virtio_numa_migrate_get_request(s, vq))) {
        /* Just acknowledge and free the request - 
           notification is initiated from the QEMU side */
        virtqueue_push(req->vq, &req->elem, 0);
        virtio_notify(vdev, req->vq);
        virtio_numa_migrate_free_request(req);
    }
}
#endif

static uint64_t virtio_numa_migrate_get_features(VirtIODevice *vdev, 
                                               uint64_t features,
                                               Error **errp)
{
    qemu_log("virtio-numa-migrate: get features\n");
    return features;
}

static void virtio_numa_migrate_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtioNUMAMigrate *s = VIRTIO_NUMA_MIGRATE(dev);
    int ret;

    /* Initialize device state */
    s->migration_state = NUMA_MIGRATE_IDLE;
    
    ret = event_notifier_init(&s->host_notifier, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Cannot initialize event notifier");
        return;
    }
    
    /* Initialize virtio device */
    virtio_init(vdev, VIRTIO_ID_NUMA_MIGRATE, 0);
    
    /* Add notification queue */
    s->notify_vq = virtio_add_queue(vdev, QUEUE_SIZE, NULL);
    
    /* Save the device pointer for monitor command access */
    g_numa_migrate_dev = s;
    
    qemu_log("virtio-numa-migrate: device realized\n");
}

static void virtio_numa_migrate_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtioNUMAMigrate *s = VIRTIO_NUMA_MIGRATE(dev);
    
    /* Clear global reference */
    if (g_numa_migrate_dev == s) {
        g_numa_migrate_dev = NULL;
    }
    
    event_notifier_cleanup(&s->host_notifier);
    
    virtio_del_queue(vdev, 0);
    virtio_cleanup(vdev);
    
    qemu_log("virtio-numa-migrate: device unrealized\n");
}

static void virtio_numa_migrate_instance_init(Object *obj)
{
    qemu_log("virtio-numa-migrate: instance initialized\n");
}

static void virtio_numa_migrate_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    
    dc->vmsd = &vmstate_virtio_numa_migrate;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    
    vdc->realize = virtio_numa_migrate_realize;
    vdc->unrealize = virtio_numa_migrate_unrealize;
    vdc->get_features = virtio_numa_migrate_get_features;
}

static const TypeInfo virtio_numa_migrate_info = {
    .name = TYPE_VIRTIO_NUMA_MIGRATE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtioNUMAMigrate),
    .instance_init = virtio_numa_migrate_instance_init,
    .class_init = virtio_numa_migrate_class_init
};

static void virtio_numa_migrate_register_types(void)
{
    type_register_static(&virtio_numa_migrate_info);
}

type_init(virtio_numa_migrate_register_types)
