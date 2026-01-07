cd ~/Z-LFS/linux-5.17.4

# 修复属主，确保你这个用户能操作
# sudo chown -R $(whoami):$(whoami) /home/zlfs/Z-LFS/f2fs-tools-1.15.0
# sudo chown -R $(whoami):$(whoami) /home/zlfs/Z-LFS/linux-5.17.4

make M=fs/f2fs clean

make M=fs/f2fs

sudo modprobe zstd_compress
sudo modprobe lz4_compress
sudo modprobe lz4hc_compress

sudo rmmod f2fs
sudo insmod fs/f2fs/f2fs.ko
# sudo rmmod zlfs
# sudo insmod fs/f2fs/zlfs.ko

cd ../f2fs-tools-1.15.0/
make clean && ./autogen.sh && ./configure && make -j$(nproc)

DEV=nvme3n2
MNT=/mnt/ZNS
cd -
sudo nvme zns reset-zone /dev/$DEV -a
# sudo nvme zns reset-zone /dev/nvme3n2 -a
sudo nvme zns reset-zone /dev/nvme3n3 -a
sleep 5
#  cat /sys/block/nvme3n6/queue/scheduler
#  cat /sys/block/nvme3n5/queue/scheduler
echo "mq-deadline" | sudo tee /sys/block/$DEV/queue/scheduler
# echo "mq-deadline" | sudo tee /sys/block/nvme3n6/queue/scheduler
# echo "mq-deadline" | sudo tee /sys/block/nvme3n5/queue/scheduler
sudo ../f2fs-tools-1.15.0/mkfs/mkfs.f2fs -d 1 -f -m /dev/$DEV
# sudo ../f2fs-tools-1.15.0/mkfs/mkfs.f2fs -d 1 -f -m /dev/nvme3n6
# sudo ../f2fs-tools-1.15.0/mkfs/mkfs.f2fs -d 1 -f -m /dev/nvme3n5

# 挂载zlfs
echo "请执行挂载命令: sudo mount /dev/$DEV $MNT"
# sudo mount /dev/nvme3n2 /mnt/ZNS

# sudo dmesg -T | tail -n 300 | sudo tee ~/Z-LFS/dmesgLog/dmesg-zlfs.log1207-1
# sudo dmesg -T | tail -n 300 > /home/zlfs/Z-LFS/dmesgLog/dmesg-zlfs.log1207-1
# sudo dmesg -T | tail -n 300 > /home/ttt/dmesgLog/dmesg-zlfs.log1207-1
# sudo tail -n 200 /var/log/syslog > /home/zlfs/Z-LFS/dmesgLog/syslog-zlfs.log1207-1
# sudo tail -n 200 /var/log/syslog > /home/zlfs/dmesgLog/syslog-zlfs.log1207-1

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
# echo 1 | sudo tee /proc/sys/vm/drop_caches
free -h

# sudo cat /var/log/syslog | grep 18396 > /home/zlfs/Z-LFS/dmesgLog/syslog.log1207-1

# sudo iostat -dxm 1 /dev/nvme3n2 
# sudo chown $USER:$USER /mnt/zns

# sudo blkzone report /dev/nvme3n1  | nl > /home/zlfs/Z-LFS/ReportZNS/nvme3n1-1207-1.log
# sudo blkzone report /dev/nvme3n2  | nl > /home/zlfs/Z-LFS/ReportZNS/nvme3n2-1207-1.log
# sudo blkzone report /dev/nvme3n3  | nl > /home/zlfs/Z-LFS/ReportZNS/nvme3n3-1207-1.log
    #    Report output:

    #         start      Zone start sector
    #         len        Zone length in number of sectors
    #         cap        Zone capacity in number of sectors
    #         wptr       Zone write pointer position
    #         reset      Reset write pointer recommended
    #         non-seq    Non-sequential write resources active
    #         cond       Zone condition
    #         type       Zone type
# 提高日志级别
# echo 8 | sudo tee /proc/sys/kernel/printk

# 注意事项：
# 1.是否开启mq-deadline
# 2.检查所有盘的active zone之和是否超过limit
    # zlfs@zns2:~/Z-LFS/f2fs-tools-1.15.0$ ./report_all_zns.sh
# 3.设备是否reset

# sudo iostat -dxm 1 /dev/nvme3n2
# sudo blktrace -d /dev/nvme3n3 -o - | blkparse -i -
# sudo blktrace -d /dev/nvme3n3 -o trace
# dumpe2fs /dev/nvme3n2
# sudo dd if=/dev/nvme3n2 of=read_block.bin bs=4096 count=1 skip=18874880
# hexdump -C -n 4096 read_block.bin
# smem -r -k -t -P "" | grep filebench
# 输出的是  PID User     Command                         Swap      USS      PSS      RSS 
# split -b 500M blkparse.txt blkparse_part_
# dumpe2fs /dev/nvme3n2


# grep menuentry /boot/grub/grub.cfg
# sudo vim /etc/default/grub
# #更改
# GRUB_DEFAULT="1>2" #1代表进入高级选项，>2代表选择第几个内核(从0开始)
# #保存后
# sudo update-grub

# echo 1 | sudo tee /proc/sys/vm/drop_caches

# 限制memory cgroup使用
# cat /sys/fs/cgroup/cgroup.subtree_control
# sudo mkdir -p /sys/fs/cgroup/memoryLimit
# echo "10G" | sudo tee /sys/fs/cgroup/memoryLimit/memory.max
# cat /sys/fs/cgroup/memoryLimit/memory.max

# echo $$ | sudo tee /sys/fs/cgroup/memoryLimit/cgroup.procs
# cat /proc/$$/cgroup
# ps -ef | grep filebench
# cat /proc/100004/cgroup

# filebench编译
# sudo ./configure
# sudo make
# sudo make install