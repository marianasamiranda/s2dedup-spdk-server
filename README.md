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

S2Dedup leverages Intel Software Guard Extensions to enable cross-user privacy-preserving deduplication at third-party storage infrastructures. 
To evaluate the impact of introducing security into the system, it was also implemented a version that does not perform encryption. Also, to access the performance's impact of storing the data persistently, two alternatives were considered for the deduplication engine implementation, one that stores the index and metadata components in memory, by using GLib, and another that does it persistently, by resorting to the LevelDB key-value store.

Therefore, S2Dedup introduces 4 new modules:
- [non_persistent_dedup](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup) - In-memory implementation without security.
- [non_persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup_sgx) -  In-memory implementation with security.
- [persistent_dedup](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/persistent_dedup) -  Persistent implementation without security.
- [persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup) - Persistent implementation with security.

## Secure Schemes
S2Dedup supports different secure schemes, each one offering more robust security guarantees:

- Plain - Basic secure scheme. When receiving a request, the block device computes its hash and consults the deduplication engine to verify if the block is new or duplicated. If new, it is reencrypted with the server's encryption key.
- Epoch - Similar to the Plain scheme, however, differentiates by performing deduplication in epochs. That is, depending on the criteria for epoch duration (e.g., a pre-defined number of operations or time period), a new random hash key used to compute the block's hash is generated. Thus, duplicate blocks stored in different epochs are considered to
be different blocks for the deduplication engine.
- Estimated - Based on '[Balancing Storage Efficiency and Data Confidentiality with Tunable Encrypted Deduplication](https://doi.org/10.1145/3342195.3387531)'. This scheme defines an upper bound for the number of duplicate copies that each stored chunk may have. This solution relies on Count-Min Sketch algorithm, which uses a probabilistic data structure (matrix) to deduce the
approximate number of copies (counters) per stored block. 
- Exact - Combines the security
advantages of the two schemes: the Epoch and Estimated. The epoch-based approach establishes a temporal boundary for duplicate detection, while the frequency-based approach allows masking
the number of duplicates found within an epoch. Additionally, this scheme instead of relying on an estimated counter, this approach keeps an in-memory hash table at the secure enclave that maps
blocks’ hash sums to their exact number of copies (counters). By keeping the exact number of copies per block, we avoid the deduplication loss effects of using estimated counters. 

## Running the S2Dedup server
After following the [SPDK installation instructions](https://spdk.io/doc/getting_started.html), one can proceed to start the server:

~~~{.sh}
./configure
make
sudo app/iscsi_tgt/iscsi_tgt -m [0,1,2]
~~~

Then, it is necessary to attach the SPDK to the NVMe controller.

~~~{.sh}
sudo scripts/rpc.py construct_nvme_bdev -b Nvme0 -t PCIe -a 0000:03:00.0
~~~

Afterwards, it is possible to construct the deduplication block device. For each of the modules, it can be executed as follows. Please pay attention to the following flags:  
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

- [persistent_dedup_sgx](https://github.com/mmm97/s2dedup-spdk-server/tree/master/module/bdev/non_persistent_dedup) - Persistent implementation with security.

~~~{.sh}
sudo ./scripts/rpc.py bdev_persistent_dedupas_sgx_create -b Nvme0n1p1 -p DedupasPA -l 4096 -s 3 -t 5
~~~

Lastly, create the iscai target to be accessed by the client.
~~~{.sh}
sudo ./scripts/rpc.py iscsi_create_portal_group 1 192.168.112.129:3260

sudo ./scripts/rpc.py iscsi_create_initiator_group 2 'ANY' 192.168.112.130/24

sudo ./scripts/rpc.py iscsi_create_target_node Target Target_alias DedupasPA:0 1:2 128 -d
~~~
