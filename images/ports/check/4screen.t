# a session detached with ^A d, then reattached and closed
/mnt/ptyrun '~~~~\r~~echo one > s1\r~~\001d~~' screen > sc1.out
screen -ls | tee ls.out; grep Detached ls.out > /dev/null && echo detach=0 || echo detach=1
/mnt/ptyrun '~~~~echo two > s2\r~~exit\r~~~' screen -r > sc2.out
printf 'one\ntwo\n' > s0; cat s1 s2 | cmp - s0 && echo screen=0 || { echo screen=1; cat sc1.out sc2.out | od -c | tail; }
screen -ls
