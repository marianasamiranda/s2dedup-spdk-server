#!/bin/bash

sudo scripts/rpc.py construct_nvme_bdev -b Nvme0 -t PCIe -a 0000:03:00.0

sudo ./scripts/rpc.py bdev_non_persistent_dedupas_create -b Nvme0n1p1 -p DedupasPA -l 4096

sleep 1

sudo ./scripts/rpc.py iscsi_create_portal_group 1 192.168.112.129:3260

sudo ./scripts/rpc.py iscsi_create_initiator_group 2 'ANY' 192.168.112.130/24

sudo ./scripts/rpc.py iscsi_create_target_node Target Target_alias DedupasPA:0 1:2 128 -d





