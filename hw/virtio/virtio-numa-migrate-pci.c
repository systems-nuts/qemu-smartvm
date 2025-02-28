 #include "qemu/osdep.h"
 #include "hw/pci/pci.h"
 #include "hw/qdev-properties.h"
 #include "hw/virtio/virtio.h"
 #include "hw/virtio/virtio-bus.h"
 #include "hw/virtio/virtio-pci.h"
 #include "hw/virtio/virtio-numa-migrate.h"
 #include "qapi/error.h"
 #include "qemu/module.h"
 #include "qom/object.h"
 
 typedef struct VirtIONUMAMigratePCI VirtIONUMAMigratePCI;
 
 /*
  * virtio-numa-migrate-pci: This extends VirtioPCIProxy.
  */
 #define TYPE_VIRTIO_NUMA_MIGRATE_PCI "virtio-numa-migrate-pci"
 DECLARE_INSTANCE_CHECKER(VirtIONUMAMigratePCI, VIRTIO_NUMA_MIGRATE_PCI,
                          TYPE_VIRTIO_NUMA_MIGRATE_PCI)
 
 struct VirtIONUMAMigratePCI {
     VirtIOPCIProxy parent_obj;
     VirtioNUMAMigrate vdev;
 };
 
 static Property virtio_numa_migrate_pci_properties[] = {
     DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags,
                     VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
     DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 1),
     DEFINE_PROP_END_OF_LIST(),
 };
 
 static void virtio_numa_migrate_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
 {
     VirtIONUMAMigratePCI *nmigrate = VIRTIO_NUMA_MIGRATE_PCI(vpci_dev);
     DeviceState *vdev = DEVICE(&nmigrate->vdev);
 
     virtio_pci_force_virtio_1(vpci_dev);
     if (!qdev_realize(vdev, BUS(&vpci_dev->bus), errp)) {
         return;
     }
 }
 
 static void virtio_numa_migrate_pci_class_init(ObjectClass *klass, void *data)
 {
     DeviceClass *dc = DEVICE_CLASS(klass);
     VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
     PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
 
     k->realize = virtio_numa_migrate_pci_realize;
     set_bit(DEVICE_CATEGORY_MISC, dc->categories);
     device_class_set_props(dc, virtio_numa_migrate_pci_properties);
     pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
     pcidev_k->class_id = PCI_CLASS_OTHERS;
 }
 
 static void virtio_numa_migrate_initfn(Object *obj)
 {
     VirtIONUMAMigratePCI *dev = VIRTIO_NUMA_MIGRATE_PCI(obj);
 
     virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                 TYPE_VIRTIO_NUMA_MIGRATE);
 }
 
 static const VirtioPCIDeviceTypeInfo virtio_numa_migrate_pci_info = {
     .generic_name  = TYPE_VIRTIO_NUMA_MIGRATE_PCI,
     .instance_size = sizeof(VirtIONUMAMigratePCI),
     .instance_init = virtio_numa_migrate_initfn,
     .class_init    = virtio_numa_migrate_pci_class_init,
 };
 
 static void virtio_numa_migrate_pci_register_types(void)
 {
     virtio_pci_types_register(&virtio_numa_migrate_pci_info);
 }
 type_init(virtio_numa_migrate_pci_register_types)
