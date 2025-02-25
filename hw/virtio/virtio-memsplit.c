#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "exec/cpu-common.h"  
#include "exec/address-spaces.h"
#include "exec/ramlist.h"
#include "exec/ramblock.h"

#include "standard-headers/linux/virtio_ids.h"

#include "virtio-memsplit.h"

#define QUEUE_SIZE 16
#define TRANSFER_END_TOKEN ~0ULL

#define PAGE_BITS        12
#define PAGE_SIZE        (1 << PAGE_BITS)
#define PAGE_OFFSET_MASK (PAGE_SIZE - 1) 

#define SYSFS_BASE            "/sys/kernel/pvm_migration"
#define START_ADDR_FILE       SYSFS_BASE "/start_addr"
#define END_ADDR_FILE         SYSFS_BASE "/end_addr"
#define PAGE_PLACEMENT        SYSFS_BASE "/page_placement"
#define PID_FILE              SYSFS_BASE "/pid"
#define TRACKING_STATE_FILE   SYSFS_BASE "/state" 
#define HVA_TO_GPA_TABLE_FILE SYSFS_BASE "/hva_to_gpa_table"
#define MIGRATION_MODE_FILE   SYSFS_BASE "/migration_mode"

static const VMStateDescription vmstate_virtio_memsplit = {
    .name = "virtio-memsplit",
    .minimum_version_id = 9,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static inline bool is_page_aligned(void *ptr) {
    return ((uint64_t)ptr) && PAGE_OFFSET_MASK == 0;
}

static struct VirtIOMemSplitReq *virtio_memsplit_get_request(VirtIOMemSplit *s, VirtQueue *vq) {
    struct VirtIOMemSplitReq *req = virtqueue_pop(vq, sizeof(struct VirtIOMemSplitReq));
    if (req) {
        req->vq = vq;
        req->dev = s;
    }
    return req;
}

static void virtio_memsplit_free_request(struct VirtIOMemSplitReq *req) {
    g_free(req);
}

static void get_gpa_log_file_path(uint64_t timestamp_ns, char *out) {
    const char *log_dir = "./logs";
    static uint64_t curr_ts = 0;
    static uint64_t i = 0;

    if (timestamp_ns != curr_ts) {
        i = 0;
    }
    curr_ts = timestamp_ns;

    sprintf(out, "%s/gpa_hva_%lu_%lu.bin", log_dir, curr_ts, i++);
}

static void *gpa2hva(hwaddr addr, uint64_t size, Error **errp) {
    Int128 gpa_region_size;
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

    gpa_region_size = int128_make64(size);
    if (int128_lt(mrs.size, gpa_region_size)) {
        error_setg(errp, "Size of memory region at 0x%" HWADDR_PRIx
                   " exceeded.", addr);
        memory_region_unref(mrs.mr);
        return NULL;
    }
    return qemu_map_ram_ptr(mrs.mr->ram_block, mrs.offset_within_region);
}

static uint64_t vtop(void *ptr, Error **errp)
{
    uint64_t pinfo;
    uint64_t ret = -1;
    uintptr_t addr = (uintptr_t) ptr;
    uintptr_t pagesize = qemu_real_host_page_size();
    off_t offset = addr / pagesize * sizeof(pinfo);
    int fd;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd == -1) {
        error_setg_errno(errp, errno, "Cannot open /proc/self/pagemap");
        return -1;
    }

    /* Force copy-on-write if necessary.  */
    qatomic_add((uint8_t *)ptr, 0);

    if (pread(fd, &pinfo, sizeof(pinfo), offset) != sizeof(pinfo)) {
        error_setg_errno(errp, errno, "Cannot read pagemap");
        goto out;
    }
    if ((pinfo & (1ull << 63)) == 0) {
        error_setg(errp, "Page not present");
        goto out;
    }
    ret = ((pinfo & 0x007fffffffffffffull) * pagesize) | (addr & (pagesize - 1));

out:
    close(fd);
    return ret;
}

static uint64_t gpa2hpa(hwaddr gpa, Error **errp) {
    void *hva = gpa2hva(gpa, 1, errp);
    if (hva == NULL) {
        return 0;
    }

    return vtop(hva, errp);
}

static ssize_t read_sysfs_file(const char *path, char *buf, size_t bufsize)
{
    int fd;
    ssize_t ret;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    ret = read(fd, buf, bufsize - 1);  // leave room for null-terminator
    if (ret < 0) {
        perror("read");
        close(fd);
        return -2;
    }

    buf[ret] = '\0'; // null-terminate
    close(fd);
    return ret;
}

static ssize_t write_sysfs_file(const char *path, const char *buf, size_t bufsize) 
{
    int fd;
    ssize_t ret;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    ret = write(fd, buf, bufsize);
    if (ret < 0) {
        perror("write");
        close(fd);
        return -2;
    }

    close(fd);
    return ret;
}

static unsigned long long get_start_addr(void) {
    char buf[64];
    ssize_t nread;
    unsigned long long start_addr;

    nread = read_sysfs_file(START_ADDR_FILE, buf, sizeof(buf));
    if (nread < 0) {
        fprintf(stderr, "Failed to read %s\n", START_ADDR_FILE);
        return 0;
    }

    sscanf(buf, "%llx", &start_addr);

    return start_addr;
}

static unsigned long long get_end_addr(void) {
    char buf[64];
    ssize_t nread;
    unsigned long long end_addr;

    nread = read_sysfs_file(END_ADDR_FILE, buf, sizeof(buf));
    if (nread < 0) {
        fprintf(stderr, "Failed to read %s\n", END_ADDR_FILE);
        return 0;
    }

    sscanf(buf, "%llx", &end_addr);

    return end_addr;
}

static bool set_tracked_addr_range(unsigned long long start, unsigned long long end) {
    char buf[64];
    ssize_t nwrite;
    sprintf(buf, "0x%llx", start);

    nwrite = write_sysfs_file(START_ADDR_FILE, buf, sizeof(buf));
    if (nwrite < 0) {
        fprintf(stderr, "Failed to write %s\n", START_ADDR_FILE);
        return false;
    }

    memset(buf, 0, sizeof(buf));
    sprintf(buf, "0x%llx", end);
    nwrite = write_sysfs_file(END_ADDR_FILE, buf, sizeof(buf));
    if (nwrite < 0) {
        fprintf(stderr, "Failed to write %s\n", END_ADDR_FILE);
        return false;
    }

    return true;
}

static bool set_tracked_pid(void) {
    char buf[64];
    ssize_t nwrite;
    int pid;

    pid = getpid();
    if (pid < 0) {
        fprintf(stderr, "getpid failed\n");
        return false;
    }

    sprintf(buf, "%d", pid);
    nwrite = write_sysfs_file(PID_FILE, buf, sizeof(buf));

    if (nwrite < 0) {
        fprintf(stderr, "Failed to write %s\n", PID_FILE);
        return false;
    }

    return true;
}

static bool is_tracking(void) {
    char buf[64];

    if (read_sysfs_file(TRACKING_STATE_FILE, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "Failed to read %s\n", TRACKING_STATE_FILE);
        return false;
    }

    if (buf[0] == '1')
        return true;
    
    return false;
}

static bool stop_tracking(void) {
    char buf[64];

    sprintf(buf, "%d", 0);

    if (write_sysfs_file(TRACKING_STATE_FILE, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "Failed to write %s\n", TRACKING_STATE_FILE);
        return false;
    }

    return true;
}

static bool set_migration_mode(int mode) {
    char buf[64];

    sprintf(buf, "%d", mode);

    if (write_sysfs_file(MIGRATION_MODE_FILE, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "Failed to write %s\n", MIGRATION_MODE_FILE);
        return false;
    }

    return true;
}

// static bool start_tracking(void) {
//     char buf[64];

//     sprintf(buf, "%d", 1);

//     if (write_sysfs_file(TRACKING_STATE_FILE, buf, sizeof(buf)) < 0) {
//         fprintf(stderr, "Failed to write %s\n", TRACKING_STATE_FILE);
//         return false;
//     }

//     return true;
// }

static void init_ram_info(VirtIOMemSplit *ms) {
    MemoryRegion *mr;
    MemoryRegion *sub_mr;

    // Walk over system memory and insert valid GPA ranges into 
    // ms object
    mr = get_system_memory();
    QLIST_INIT(&ms->gpa_ranges);

    qemu_log("System memory subregions:\n");
    QTAILQ_FOREACH(sub_mr, &mr->subregions, subregions_link) {
        if (strcmp(sub_mr->name, "ram-below-4g") == 0 ||
            strcmp(sub_mr->name, "ram-above-4g") == 0) {
            qemu_log("Found %s memory region\n", sub_mr->name);

            hwaddr gpa_start = sub_mr->addr;
            hwaddr gpa_end   = gpa_start + sub_mr->size - 1;
            qemu_log("Subregion gpa range: 0x%lx - 0x%lx\n", gpa_start, gpa_end + 1);

            GPARange *gpa_range = malloc(sizeof(GPARange));
            gpa_range->start = gpa_start;
            gpa_range->size = sub_mr->size;

            QLIST_INSERT_HEAD(&ms->gpa_ranges, gpa_range, next);

            uint8_t *hva = memory_region_get_ram_ptr(sub_mr);
            qemu_log("Subregion hva range: %p - %p\n", hva, hva + sub_mr->size);

            if (ms->hva_ram_start_ptr == NULL || ms->hva_ram_start_ptr > hva) {
                ms->hva_ram_start_ptr = hva;
            }
            ms->hva_ram_size += sub_mr->size;
        }
    }
}

// static bool init_numa_layout(VirtIOMemSplit *ms) {
//     unsigned long long start_addr = 0, end_addr = 0;
//     size_t num_pages, total_bytes;

//     start_addr = get_start_addr();
//     end_addr = get_end_addr();

//     if (start_addr >= end_addr) {
//         fprintf(stderr, "Invalid address range!\n");
//         return false;
//     }

//     num_pages = (end_addr - start_addr) >> PAGE_BITS;
//     total_bytes = num_pages * sizeof(int);

//     ms->numa_layout = malloc(total_bytes);
//     if (!ms->numa_layout) {
//         perror("malloc");
//         return false;
//     }
//     return true;
// }

static void virtio_memsplit_handle_gpa_req(struct VirtIOMemSplitReq *req) 
{
    VirtIOMemSplit *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    int i;
    int fd;
    char f_path[128];
    Error *errp = NULL;

    if (req->elem.out_num > 0) {
        struct VirtIOSendGpaData *buf = req->elem.out_sg[0].iov_base;
        get_gpa_log_file_path(buf->timestamp_ns, f_path);
        fd = open(f_path,
            O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        for (i = 0; i < 128 && buf->pfns[i] > 0; i++) {
            uint64_t gpa = buf->pfns[i] << PAGE_BITS;
            uint64_t hva = (uint64_t) gpa2hva(gpa, 1, &errp);
            if (write(fd, &gpa, sizeof(gpa)) < 0) {
                qemu_log("failed to write GPA\n");
                goto cleanup;
            }
            if (write(fd, &hva, sizeof(hva)) < 0) {
                qemu_log("failed to write HVA\n");
                goto cleanup;
            }
        }
        close(fd);
    }

cleanup:
    virtqueue_push(req->vq, &req->elem, 128 * (sizeof *req));
    virtio_notify(vdev, req->vq);
  
    virtio_memsplit_free_request(req);
}

static void virtio_memsplit_handle_migration_req(struct VirtIOMemSplitReq *req) 
{
    VirtIOMemSplit *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    uint64_t i;
    size_t n_pages = s->hva_ram_size >> PAGE_BITS;

    if (s->pages_left_to_send == 0)
        s->pages_left_to_send = n_pages;
    
    qemu_log("N pages: %lu\n", n_pages);

    if (req->elem.in_num > 0) {
        struct VirtIOReceiveMigrationData *buf = req->elem.in_sg[0].iov_base;
        
        for (i = 0; i < VIRTIO_MEMSPLIT_RECEIVE_MIGRATE_GPA_CAPACITY && s->pages_left_to_send > 0; i++) {
            buf->gpas[i] = s->gpas[s->pages_left_to_send - 1];
            buf->nodes[i] = s->numa_layout[s->pages_left_to_send - 1];
            s->pages_left_to_send--;
        }

        for (; i < VIRTIO_MEMSPLIT_RECEIVE_MIGRATE_GPA_CAPACITY; i++) {
            buf->gpas[i] = 0ULL;
            buf->nodes[i] = -1;
        }
        buf->pages_left_to_send = s->pages_left_to_send;
    }

    virtqueue_push(req->vq, &req->elem, (sizeof *req));
    virtio_notify(vdev, req->vq);
  
    virtio_memsplit_free_request(req);
}

static void virtio_memsplit_handle_gpa(VirtIODevice *vdev, VirtQueue *vq)
{
    struct VirtIOMemSplitReq *req;
    VirtIOMemSplit *ms = (VirtIOMemSplit *)vdev;

    while((req = virtio_memsplit_get_request(ms, vq))) {
        virtio_memsplit_handle_gpa_req(req);
    }
}

static void virtio_memsplit_handle_migration(VirtIODevice *vdev, VirtQueue *vq)
{
    struct VirtIOMemSplitReq *req;
    VirtIOMemSplit *ms = (VirtIOMemSplit *)vdev;

    while((req = virtio_memsplit_get_request(ms, vq))) {
        virtio_memsplit_handle_migration_req(req);
    }
}

static uint64_t virtio_memsplit_get_features(VirtIODevice *vdev, uint64_t features, 
                                        Error **errp) 
{
    qemu_log("virtio memsplit get features\n");
    return features;
}

// static bool build_gfn_map(VirtIOMemSplit *ms) {
//     unsigned long long start_addr = 0, end_addr = 0;
//     size_t num_pages, total_bytes;
//     size_t bytes_read = 0;
//     int fd;

//     fd = open(HVA_TO_GPA_TABLE_FILE, O_RDONLY);
//     if (fd < 0) {
//         perror("open page_placement");
//         return false;
//     }

//     start_addr = get_start_addr();
//     end_addr = get_end_addr();

//     if (start_addr >= end_addr) {
//         fprintf(stderr, "Invalid address range!\n");
//         return false;
//     }

//     num_pages = (end_addr - start_addr) >> PAGE_BITS;
//     total_bytes = num_pages * sizeof(uint64_t);

//     ms->gpas = malloc(total_bytes);

//     while (bytes_read < total_bytes) {
//         ssize_t ret = read(fd, 
//             (char*)ms->gpas + bytes_read, 
//             total_bytes - bytes_read);
//         if (ret < 0) {
//             perror("read");
//             free(ms->gpas);
//             close(fd);
//             return false;
//         }
//         if (ret == 0) {
//             printf("EOF reached: read %zu of %zu bytes\n",
//                 bytes_read, total_bytes);
//             break;
//         }
//         bytes_read += (size_t) ret;
//     }

//     close(fd);

//     return true;
// }

static int virtio_memsplit_update_numa_layout(struct VirtIOMemSplit *ms)
{
    unsigned long long start_addr = 0, end_addr = 0;
    int fd;
    size_t bytes_read = 0;
    size_t num_pages, total_bytes;
    start_addr = get_start_addr();
    end_addr = get_end_addr();

    if (start_addr >= end_addr) {
        fprintf(stderr, "Invalid address range!\n");
        return false;
    }

    num_pages = (end_addr - start_addr) >> PAGE_BITS;
    total_bytes = num_pages * sizeof(int);

    /* Open the binary file. */
    fd = open(PAGE_PLACEMENT, O_RDONLY);
    if (fd < 0) {
        perror("open page_placement");
        return false;
    }

    /* We'll do a single read. If partial, read in a loop. */
    while (bytes_read < total_bytes) {
        ssize_t ret = read(fd,
                           (char *)ms->numa_layout + bytes_read,
                           total_bytes - bytes_read);
        if (ret < 0) {
            perror("read");
            close(fd);
            return false;
        }
        if (ret == 0) {
            /* EOF reached earlier than expected */
            printf("EOF reached: read %zu of %zu bytes\n",
                   bytes_read, total_bytes);
            break;
        }
        bytes_read += (size_t)ret;
    }

    close(fd);

    return true;
}

static void virtio_memsplit_migration_timer_callback(void *opaque)
{
    struct VirtIOMemSplit *ms = opaque;
    int64_t now_ms, next_fire_ms;

    if (!virtio_memsplit_update_numa_layout(ms)) {
        qemu_log("Failed to update NUMA layout\n");
    }

    now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    next_fire_ms = now_ms + 1000; // 1 second from now
    timer_mod(ms->migration_timer, next_fire_ms);

    return;
}

static void virtio_memsplit_realize(DeviceState *dev, Error **errp) 
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOMemSplit *ms = VIRTIO_MEMSPLIT(dev);
    GPARange *gpa_range;
    int ret;

    init_ram_info(ms);

    if (ms->hva_ram_size == 0) {
        error_setg(errp, "Could not find guest RAM region(s)");
        return;
    }

    if (is_tracking() && !stop_tracking()) {
        error_setg(errp, "Could not stop tracking the previous process\n");
        return;
    }

    if (!set_tracked_addr_range((unsigned long long) ms->hva_ram_start_ptr, 
        (unsigned long long) (ms->hva_ram_start_ptr + ms->hva_ram_size))) {
            error_setg(errp, "Could not set tracked address range\n");
            return;
    }

    if (!set_tracked_pid()) {
        error_setg(errp, "Could not set tracked PID\n");
        return;
    }

    if (!set_migration_mode(1)) {  // Manual
        error_setg(errp, "Could not set migration mode\n");
        return;
    }

    // if (!start_tracking()) {
    //     error_setg(errp, "Failed to start tracking\n");
    //     return;
    // }

    // if (!init_numa_layout(ms)) {
    //     error_setg(errp, "Failed to initialize NUMA layout\n");
    //     return;
    // }

    // Test mappings
    qemu_log("gpa sectors:\n");
    QLIST_FOREACH(gpa_range, &ms->gpa_ranges, next) {
        hwaddr gpa_start = gpa_range->start;
        hwaddr gpa_end = gpa_range->start + gpa_range->size - 1;
        qemu_log("GPA: 0x%lx - 0x%lx\n", gpa_start, gpa_end + 1);
        qemu_log("HVA: %p - %p\n", gpa2hva(gpa_start, 1, errp), gpa2hva(gpa_end, 1, errp) + 1);
        if (*errp) {
            error_setg(errp, "Failed to map GPA to HPA");
            return;
        }

        qemu_log("HPA: 0x%lx - 0x%lx\n", gpa2hpa(gpa_start, errp), gpa2hpa(gpa_end, errp) + 1);
        if (*errp) {
            error_setg(errp, "Failed to map GPA to HPA");
            return;
        }
    }

    ret = event_notifier_init(&ms->irqfd, 0);
    if (ret) {
        error_setg(errp, "Failed to initialize event notifier");
        return;
    }

    // if (!build_gfn_map(ms)) {
    //     error_setg(errp, "Could not build HVA -> GPA table\n");
    //     return;
    // }

    virtio_init(vdev, VIRTIO_ID_MEMSPLIT, 0);
    ms->gpa_vq = virtio_add_queue(vdev, QUEUE_SIZE, virtio_memsplit_handle_gpa);
    ms->migration_vq = virtio_add_queue(vdev, QUEUE_SIZE, virtio_memsplit_handle_migration);
    ms->pages_left_to_send = 0;

    ms->migration_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, virtio_memsplit_migration_timer_callback, ms);
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    int64_t first_fire_ms = now_ms + 1000; // 1 second from now
    timer_mod(ms->migration_timer, first_fire_ms);

    qemu_log("virtio memsplit realize\n");
}

static void virtio_memsplit_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    virtio_del_queue(vdev, 0);
    virtio_cleanup(vdev);
    qemu_log("Device unrealized\n");
}

static void virtio_instance_init(Object *obj) 
{
    qemu_log("virtio memsplit instance init\n");
}

static void virtio_memsplit_init(ObjectClass *klass, void *data) 
{
    qemu_log("virtio memsplit init\n");

    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_virtio_memsplit;
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_memsplit_realize;
    vdc->unrealize = virtio_memsplit_unrealize;
    vdc->get_features = virtio_memsplit_get_features;
}

static const TypeInfo virtio_memsplit_info = 
{
    .name = TYPE_VIRTIO_MEMSPLIT,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOMemSplit),
    .instance_init = virtio_instance_init,
    .class_init = virtio_memsplit_init
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_memsplit_info);
}

type_init(virtio_register_types)
