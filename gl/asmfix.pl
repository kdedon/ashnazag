# asmfix.pl -- fixups to gcc's assembly for GL and Quake code (perl -0777 -pi).
#
# FScc, which the 68060 traps, becomes a branch around sf:
#
#	fslt %d0	->	st %d0; fblt .LFSn; sf %d0; .LFSn:
#
# A -fPIC function that needs the GOT only for constants reload put in
# memory loads %a5 without saving it.  With a frame pointer, %a5 is pushed
# before the frame and popped after unlk, and the arguments move up by 4;
# without one this exits 3, for a build with one.
BEGIN { %cc = map { $_ => 1 } qw(f t eq ne neq gt ngt ge nge lt nlt le nle gl ngl
    gle ngle ogt oge olt ole ogl or un ueq ugt uge ult ule sf st seq sne sneq) }
$n = 0;
s{^(\s*)fs([a-z]+)(\s+)(%d[0-7])[ \t]*$}{
	$cc{$2} ? ($n++, "$1st$3$4\n$1fb$2$3.LFS$n\n$1sf$3$4\n.LFS$n:")[1] : $&
}meg;
$gots = () = /^\s*lea \(%pc, _GLOBAL_OFFSET_TABLE_\@GOTPC\), %a5$/mg;
$seen = 0;
s{^([\w.\$]+:\n)((?:\s*(?:link|pea|fmovm|move?\.l|movm\.l|add|sub|lea)\S*\s[^\n]*\n)*?)
  (\s*lea\ \(%pc,\ _GLOBAL_OFFSET_TABLE_\@GOTPC\),\ %a5\n)((?:.*\n)*?)(\.Lfe\d+:)}{
	my ($l, $pro, $got, $body, $end) = ($1, $2, $3, $4, $5);
	$seen++;
	my $saved = $pro =~ /^\s*mov\.l %a5,-\(%sp\)$/m;
	$saved ||= hex($1) & 4 while $pro =~ /^\s*movm\.l &0x([0-9a-f]+),-\(%sp\)$/mg;
	unless ($saved) {
		exit 3 unless $pro =~ s/^(\s*)(link|pea \(%fp\))/$1mov.l %a5,-(%sp)\n$1$2/m;
		$body =~ s/(?<![\w-])(\d+)\(%fp([,)])/($1 + 4) . "(%fp$2"/ge;
		my $r = () = $body =~ /\brts\b/g;
		my $u = $body =~ s/^(\s*)unlk %fp\n(\s*rts\b)/$1unlk %fp\n$1mov.l (%sp)+,%a5\n$2/mg;
		die "asmfix: $l: $r returns, $u after unlk\n" if $r != $u;
	}
	"$l$pro$got$body$end"
}mexg;
die "asmfix: $seen of $gots GOT loads after a known prologue\n" if $seen != $gots;
