# Ex mode edits a file; floats and 64-bit numbers come back right; every
# file type loads without an error; help opens; a terminal session saves
printf 'one\ntwo\n' > v.txt
vim -es -c '%s/two/TWO/' -c wq v.txt
printf 'one\nTWO\n' | cmp - v.txt && echo vim=0 || { echo vim=1; od -c v.txt; }
cat > v.vim <<'EOV'
call writefile([string(sqrt(16.0) * 1.5), string(str2float('2.5')), 1000000 * 3000000, printf('%x', 0x123456789)], 'v.out')
syntax on
filetype plugin indent on
let e = []
for f in glob('$VIMRUNTIME/syntax/*.vim', 0, 1)->map('fnamemodify(v:val, ":t:r")')
  if f !~# '^\(syn\|nosyntax\|manual\)'
    enew!
    try | exe 'setf' f | catch | call add(e, f .. ': ' .. v:exception) | endtry
  endif
endfor
call writefile(e, 'ft.out')
help options
call writefile([expand('%:t')], 'h.out')
qa!
EOV
vim -es -N -u /opt/amix/share/vim/vimrc -S v.vim
printf '6.0\n2.5\n3000000000000\n123456789\n' | cmp - v.out && echo vimrt=0 || { echo vimrt=1; cat v.out; }
[ -f ft.out ] && [ ! -s ft.out ] && echo vimft=0 || { echo vimft=1; cat ft.out; }
echo options.txt | cmp - h.out && echo vimdoc=0 || { echo vimdoc=1; cat h.out; }
TERM=vt100 /mnt/ptyrun '~~~~ihello\033:wq\r~~' vim v2.txt > vt.out
echo hello | cmp - v2.txt && echo vimtty=0 || { echo vimtty=1; od -c vt.out | tail; }
