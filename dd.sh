MNT=/mnt/agg_gc

start_time=$(date +%s.%N)
for i in $(seq -w 1 10); do
    mkdir -p $MNT/dir_$i
    for j in $(seq -w 1 100); do
        dd if=/dev/zero of=$MNT/dir_$i/file_$j bs=64k count=1024 status=progress
    done
done
end_time=$(date +%s.%N)

wall_clock_time=$(echo "$end_time - $start_time" | bc)
echo "Total time: $wall_clock_time seconds"

# MNT=/mnt/medfs

# start_time=$(date +%s.%N)
# for i in $(seq -w 1 10); do
#     for j in $(seq -w 1 100); do
#         dd if=$MNT/dir_$i/file_$j of=/dev/null bs=1M count=1024 status=progress
#     done
# done
# end_time=$(date +%s.%N)

# wall_clock_time=$(echo "$end_time - $start_time" | bc)
#echo "Total time: $wall_clock_time seconds"
