#!/bin/bash

sudo scripts/rpc.py construct_nvme_bdev -b Nvme0 -t PCIe -a 0000:03:00.0

sudo umount /dev/nbd0

sudo scripts/rpc.py nbd_start_disk Nvme0n1p2 /dev/nbd0

#sudo mkfs.ext4 /dev/nbd0
#sudo mkfs.ext4 /dev/nbd0

sudo mount -o dax /dev/nbd0 module/bdev/persistent_dedup_sgx/dbs

sudo ./scripts/rpc.py bdev_persistent_dedupas_sgx_create -b Nvme0n1p1 -p DedupasPA -l 4096 -s 3 -t 5

sleep 200

echo "Time elapsed\n"

sudo ./scripts/rpc.py iscsi_create_portal_group 1 192.168.112.129:3260

sudo ./scripts/rpc.py iscsi_create_initiator_group 2 'ANY' 192.168.112.130/24

sudo ./scripts/rpc.py iscsi_create_target_node Target Target_alias DedupasPA:0 1:2 128 -d

