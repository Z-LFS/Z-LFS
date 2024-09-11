# Z-LFS: A Zoned Namespace-tailored Log-structured File System for Commodity ZNS SSDs

## How to build & install
* Install and boot provided kernel

* Build Z-LFS kernel module
```bash
cd ./linux-5.17.4
make fs/f2fs/f2fs.ko
```

* Build mkfs tool
```bash
cd ./f2fs-tools-1.15.0/
./autogen.sh
./configure
make
```

## How to mkfs
```bash
sudo f2fs-tools-1.15.0/mkfs/mkfs.f2fs -m /dev/ZNS
```

## How to mount
```bash
sudo insmod linux-5.17.4/fs/f2fs/f2fs.ko
sudo mount /dev/ZNS /mnt/ZNS
```
