mkdir z; cp d /etc/passwd z/
zip -r -v t.zip z && mv z z0 && unzip -tq t.zip && unzip -q t.zip && cmp z/d z0/d &&
	cmp z/passwd /etc/passwd && unzip -v t.zip && zipinfo t.zip && zip -d t.zip z/passwd &&
	unzip -l t.zip && echo zip=0 || echo zip=1
cat d | zip > s.zip && funzip s.zip | cmp - d && echo funzip=0 || echo funzip=1
rm -rf z z0 t.zip s.zip
