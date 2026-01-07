# sudo umount /mnt/ZNS
make clean && ./autogen.sh && ./configure && make -j$(nproc)

echo "mq-deadline" | sudo tee /sys/block/nvme3n2/queue/scheduler
sudo nvme zns reset-zone /dev/nvme3n2 -a
sudo nvme zns reset-zone /dev/nvme3n3 -a
sudo nvme zns reset-zone /dev/nvme3n4 -a
sudo nvme zns reset-zone /dev/nvme3n5 -a
sudo nvme zns reset-zone /dev/nvme3n6 -a

sleep 5

sudo ./mkfs/mkfs.f2fs -m -f /dev/nvme3n2
# sudo dmesg -T | tail -n 300 >> ~/Z-LFS/dmesg.log2

# sudo mount /dev/nvme3n2 /mnt/ZNS
# df -h /mnt/ZNS

# sudo umount /mnt/ZNS

# sudo blkzone report /dev/nvme3n6 | nl -v 0 -w 15 | sed 's/^[[:space:]]*//' > ~/Z-LFS/blkzoneRpt/nvme3n6.log