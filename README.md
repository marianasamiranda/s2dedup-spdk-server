# S2Dedup's Server

This repository is part of the S2Dedup project. Please refer to [S2Dedup repository](https://github.com/mmm97/S2Dedup) for further information or you may read the paper published in SYSTOR'21:

- "S2Dedup: SGX-enabled Secure Deduplication"

S2Dedup's server implementation is based on the Storage Performance Development Kit ([SPDK](http://www.spdk.io)), which provides a set of tools and libraries for writing high performance, scalable, user-mode storage applications. Please refer to ([SPDK-Getting_Started](https://spdk.io/doc/getting_started.html)) for installation instructions.

Our secure deduplication engine is implemented as an SPDK
virtual block device. It intercepts incoming block I/O requests,
performs secure deduplication, with a fixed block-size specified
by users, and only then forwards the requests to the
NVMe block device or another virtual processing layer, depending
on the targeted SPDK deployment. These requests
will eventually reach the NVMe driver and storage device
unless intermediate processing eliminates this need (e.g.,
duplicate writes). Moreover, SPDK also provides a set of
storage protocols that can be stacked over the block device
abstraction layer. Of these, our work uses an iSCSI (Internet
Small Computer System Interface) target implementation that
enables clients to access the storage server remotely.

S2Dedup leverages Intel Software Guard Extensions (Intel SGX) to achieve a secure trusted execution environment to perform critical operations. To evaluate the impact of introducing security into the system, it was also implemented a version that does not perform encryption. Additionally, to assess the impact of storing data persistently, two alternatives were considered for the deduplication engine implementation, one that stores the index and metadata components in memory, by using GLib, and another that does it persistently, by resorting to the LevelDB key-value store.

Therefore, S2Dedup introduces 4 new virtual block devices:
- [non_persistent_dedup](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup) - In-memory implementation without security.
- [non_persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup_sgx) -  In-memory implementation with security.
- [persistent_dedup](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/persistent_dedup) -  Persistent implementation without security.
- [persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/persistent_dedup_sgx) - Persistent implementation with security.

## Secure Schemes
S2Dedup supports different secure schemes, each one offering more robust security guarantees:

- Plain - Basic secure scheme. When receiving a request, the block device computes its hash and consults the deduplication engine to verify if the block is new or duplicated. If new, it is reencrypted with the server's encryption key.
- Epoch - Similar to the Plain scheme, however, differentiates by performing deduplication in epochs. That is, depending on the criteria for epoch duration (e.g., a pre-defined number of operations or time period), a new random key used to compute the block's hash is generated. Thus, duplicate blocks stored in different epochs are considered to
be different blocks for the deduplication engine.
- Estimated - Based on '[Balancing Storage Efficiency and Data Confidentiality with Tunable Encrypted Deduplication](https://doi.org/10.1145/3342195.3387531)'. This scheme defines an upper bound for the number of duplicate copies that each stored chunk may have. This solution relies on Count-Min Sketch algorithm, which uses a probabilistic data structure (matrix) to deduce the
approximate number of copies (counters) per stored block. 
- Exact - Combines the security
advantages of the two schemes: the Epoch and Estimated. The epoch-based approach establishes a temporal boundary for duplicate detection, while the frequency-based approach allows masking
the number of duplicates found within an epoch. Additionally, this scheme instead of relying on an estimated counter, keeps an in-memory hash table at the secure enclave that maps
blocks’ hash sums to their exact number of copies (counters). By keeping the exact number of copies per block, we avoid the deduplication loss effects of using estimated counters. 

Note: The secure schemes are achieved through Intel SGX. Thus, to use this schemes, it is important to first install and compile the [S2Dedup SGX repository](https://github.com/mmm97/s2dedup-sgx), and, lastly, copy the signed enclave shared object (*Enclave.signed.so*) to the respective block device directory (e.g., module/bdev/non_persistent_dedup_sgx).

## Running the S2Dedup server
Before starting the S2Dedup server is important to:
- follow [SPDK installation instructions](https://spdk.io/doc/getting_started.html)
- follow [Intel SGX installation instructions](https://github.com/intel/linux-sgx) and [S2Dedup SGX repository instructions](https://github.com/mmm97/s2dedup-sgx), if deploying a secure block device
- install the *GLib* library if using the in memory alternative, or the *LevelDB* key-value store in case of the persistent approach.
- define in the CONFIG file the virtual block device to use (e.g., CONFIG_NON_PERSISTENT_DEDUPAS=y)

Afterwards, one can proceed to compile and start the server:
~~~{.sh}
./configure
make
sudo app/iscsi_tgt/iscsi_tgt -m [0,1,2]
~~~

Then, it is necessary to attach the SPDK to the NVMe controller.

~~~{.sh}
sudo scripts/rpc.py construct_nvme_bdev -b Nvme0 -t PCIe -a 0000:03:00.0
~~~

Afterwards, it is possible to construct the deduplication block device. For each of the modules, it can be executed as follows. Please pay attention to the new flags introduced by S2Dedup:  
~~~
-l : block size (e.g. 4096)
-s : security level (0: Plain, 1: Epoch, 2: Estimated, 3: Exact)
-t : number of operations to change of epoch or upper bound for the number of duplicate copies
~~~

- [non_persistent_dedup](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup) - In-memory implementation without security. 
~~~{.sh}
sudo ./scripts/rpc.py bdev_non_persistent_dedupas_create -b Nvme0n1p1 -p DedupasPA -l 4096
~~~

- [non_persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup_sgx) -  In-memory implementation with security.
~~~{.sh}
sudo ./scripts/rpc.py bdev_non_persistent_dedupas_sgx_create -b Nvme0n1p1 -p DedupasPA -l 4096 -s 1 -t 4000000 
~~~

- [persistent_dedup](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/persistent_dedup) -  Persistent implementation without security.
~~~{.sh}
sudo ./scripts/rpc.py bdev_persistent_dedupas_create  -b Nvme0n1p1 -p DedupasPA -l 4096
~~~

- [persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/persistent_dedup_sgx) - Persistent implementation with security.

~~~{.sh}
sudo ./scripts/rpc.py bdev_persistent_dedupas_sgx_create -b Nvme0n1p1 -p DedupasPA -l 4096 -s 3 -t 5
~~~

Lastly, we can create the iSCSI target to be accessed by the client:
~~~{.sh}
sudo ./scripts/rpc.py iscsi_create_portal_group 1 192.168.112.129:3260

sudo ./scripts/rpc.py iscsi_create_initiator_group 2 'ANY' 192.168.112.130/24

sudo ./scripts/rpc.py iscsi_create_target_node Target Target_alias DedupasPA:0 1:2 128 -d
~~~

Examples are available at [init_iscsi_non_persistent_dedupas.sh]( https://github.com/mmm97/s2dedup-spdk-server/blob/master/init_iscsi_non_persistent_dedupas.sh), [init_iscsi_non_persistent_dedupas_sgx.sh]( https://github.com/mmm97/s2dedup-spdk-server/blob/master/init_iscsi_non_persistent_dedupas_sgx.sh), [init_iscsi_persistent_dedupas.sh]( https://github.com/mmm97/s2dedup-spdk-server/blob/master/init_iscsi_persistent_dedupas.sh) and [init_iscsi_persistent_dedupas_sgx.sh]( https://github.com/mmm97/s2dedup-spdk-server/blob/master/init_iscsi_persistent_dedupas_sgx.sh). 

Note: When SPDK connects to the NVMe disk, it firsts unbinds the kernel driver from the device and then rebinds the driver to a “dummy” driver so that the operating system won’t automatically try to re-bind the default driver. This makes the device no longer accessible outside of the SPDK environment, meaning that it is not possible to simply partition a disk and dedicate a part to run SPDK over it and the other to store the deduplication related metadata. Nevertheless, SPDK allows for the creation of partitions within its environment, so it is possible to mount the deduplication virtual block device over part of the disk and run a Linux NBD (Network Block Device) over the other, which is also provided by the SPDK, allowing for the data to be stored in the same disk as the deduplication related metadata. This is why [init_iscsi_persistent_dedupas.sh]( https://github.com/mmm97/s2dedup-spdk-server/blob/master/init_iscsi_persistent_dedupas.sh) and [init_iscsi_persistent_dedupas_sgx.sh]( https://github.com/mmm97/s2dedup-spdk-server/blob/master/init_iscsi_persistent_dedupas_sgx.sh) require an extra step to create the /dev/nbd0 and mount it to the metadata directory.

~~~{.sh}
sudo umount /dev/nbd0
sudo scripts/rpc.py nbd_start_disk Nvme0n1p2 /dev/nbd0
sudo mount -o dax /dev/nbd0 module/bdev/persistent_dedup/dbs
~~~

## Contacts
For more information please contact: 

- Mariana Miranda - mariana.m.miranda at inesctec.pt
- João Paulo - joao.t.paulo at inesctec.pt
- Tânia Esteves - tania.c.araujo at inestec.pt
- Bernardo Portela - b.portela at fct.unl.pt
