# ^K L goes to a line through the integer prompt; ^K X saves and exits
/mnt/ptyrun '~~~~one\rtwo\r\013L1\r~X\013X~~' joe j.txt > joe.out
printf 'Xone\ntwo\n' > j0
cmp j.txt j0 && echo joe=0 || { echo joe=1; od -c j.txt; }
/mnt/ptyrun '~~~~pico\r\030~y~\r~~' jpico p.txt > /dev/null
echo pico | cmp - p.txt && echo jpico=0 || { echo jpico=1; od -c p.txt; }
