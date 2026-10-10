/* Exercise the host-backed File Manager from a classic Mac application. */
extern long auxwrite(), auxsysr(), hfs(), create(), openrf(), fread(), fwrite(), fclose();
extern long getnext(), getprocess();
extern long videolist(), videomain(), videonext();
extern long getinfo(), setinfo(), renamefile(), removefile(), volinfo();
extern long aecall(), cfmcall(), dragcall(), threadcall(), threaddispose(), aliascall();
extern void threadentry(), exitapp(), disposehandle();
#define W(p,n) (*(short *)((p)+(n)))
#define L(p,n) (*(long *)((p)+(n)))

static void zero(p,n) char *p; int n; { while(n--) *p++=0; }
static void out(s) char *s; { int n=0; long fd; while(s[n]) n++; auxwrite(1L,s,(long)n);
  fd=auxsysr(5L,(long)"/tmp/mac81probe.out",0x109L,0666L);
  if(fd>=0) { auxsysr(4L,fd,(long)s,(long)n); auxsysr(6L,fd,0L,0L); } }
static int check(name,ok) char *name; int ok;
{ out(ok ? "mac81probe P " : "mac81probe F "); out(name); out("\n"); return ok; }
static void pb(p,name,dir) char *p,*name; long dir;
{ zero(p,160); L(p,18)=(long)name; L(p,48)=dir; }
static int forkio(name,dir,rf) char *name; long dir; int rf;
{
    char p[160], buf[12]; short ref; int i,ok=0; long e;
    pb(p,name,dir); p[27]=3;
    e=rf ? openrf(p) : hfs(p,0x1aL);
    if(e) return 0;
    ref=W(p,24); zero(p,160); W(p,24)=ref;
    L(p,32)=(long)"fork bytes"; L(p,36)=10; W(p,44)=1;
    if(fwrite(p)==0 && L(p,40)==10) {
        zero(p,160); W(p,24)=ref;
        if(fclose(p)) return 0;
        pb(p,name,dir); p[27]=1;
        if(rf ? openrf(p) : hfs(p,0x1aL)) return 0;
        ref=W(p,24); zero(p,160); W(p,24)=ref; W(p,44)=1;
        zero(buf,12); L(p,32)=(long)buf; L(p,36)=10; L(p,46)=0;
        if(fread(p)==0 && L(p,40)==10) {
            for(i=0;i<10 && buf[i]=="fork bytes"[i];i++);
            ok=i==10;
        }
    }
    zero(p,160); W(p,24)=ref; return fclose(p)==0 && ok;
}
static void number(v) unsigned long v;
{ char b[12],s[12]; int n=0,i=0;
  do { b[n++]='0'+v%10; } while((v/=10)!=0);
  while(n) s[i++]=b[--n];
  s[i]=0; out(s); }
static void finder()
{
    char psn[8],info[60],name[64]; int i,found=0;
    zero(psn,8);
    for(i=0;i<64 && getnext(psn)==0;i++) {
        zero(info,60); L(info,0)=60; L(info,4)=(long)name;
        if(getprocess(psn,info)==0 && L(info,20)==0x4d414353L) {
            out("mac81probe I finder_psn ");
            number((unsigned long)L(psn,0)); out(" ");
            number((unsigned long)L(psn,4)); out("\n"); found++;
        }
    }
    check("finder_process",found==1);
}
static void pstr(p,s) char *p,*s;
{ int n=0; while(s[n]) { p[n+1]=s[n]; n++; } p[0]=n; }
static int forkread(name,dir,rf) char *name; long dir; int rf;
{
    char p[160],buf[12]; short ref; long e; int i,ok;
    pb(p,name,dir); p[27]=1;
    if(rf ? openrf(p) : hfs(p,0x1aL)) return 0;
    ref=W(p,24); zero(p,160); W(p,24)=ref; W(p,44)=1;
    zero(buf,12); L(p,32)=(long)buf; L(p,36)=10; L(p,46)=0;
    e=fread(p);
    for(i=0;i<10 && buf[i]=="fork bytes"[i];i++);
    ok=e==0 && L(p,40)==10 && i==10;
    zero(p,160); W(p,24)=ref;
    return fclose(p)==0 && ok;
}
static void diagnostic();
static void videodevices()
{
    long h,main,seen[8]; int i,j;
    main=videomain(); diagnostic("video_main",main);
    h=videolist(); diagnostic("video_list",h);
    for(i=0;i<8 && h;i++) {
        char *g,*pm;
        for(j=0;j<i && seen[j]!=h;j++);
        if(j<i) { diagnostic("video_cycle",h); break; }
        seen[i]=h; diagnostic("video_handle",h);
        g=*(char **)h;
        if(!g) { diagnostic("video_purged",h); break; }
        diagnostic("video_refnum",W(g,0));
        diagnostic("video_type",W(g,4));
        diagnostic("video_flags",(unsigned short)W(g,20));
        diagnostic("video_mode",L(g,42));
        diagnostic("video_rect_top",W(g,34));
        diagnostic("video_rect_left",W(g,36));
        diagnostic("video_rect_bottom",W(g,38));
        diagnostic("video_rect_right",W(g,40));
        pm=L(g,22) ? *(char **)L(g,22) : 0;
        if(pm) {
            diagnostic("video_pixels",W(pm,32));
            diagnostic("video_rowbytes",(unsigned short)W(pm,4));
            diagnostic("video_base",L(pm,0));
        }
        h=videonext(h);
    }
    diagnostic("video_count",i);
    diagnostic("video_remaining",h);
}
static void persistentids(name,dir) char *name; long dir;
{
    char p[160],result[64],next[64]; long id,e; short vref; int i;
    pb(p,0L,0L); if(volinfo(p)) return; vref=W(p,22);
    pb(p,name,dir); W(p,22)=vref;
    e=hfs(p,0x14L); id=L(p,54);
    diagnostic("persistent_create_error",e);
    diagnostic("persistent_created_id",id);
    check("persistent_create",(e==0 || e==-1301) && id!=0);
    if((e!=0 && e!=-1301) || !id) return;
    pb(p,result,0L); W(p,22)=vref; L(p,54)=id; zero(result,64);
    e=hfs(p,0x16L);
    diagnostic("persistent_before_error",e);
    diagnostic("persistent_before_parent",L(p,48));
    diagnostic("persistent_before_length",(unsigned char)result[0]);
    for(i=0;i<=name[0] && result[i]==name[i];i++);
    check("persistent_before_rename",e==0 && W(p,22)==vref && L(p,48)==dir && i>name[0]);
    pstr(next,"FileIDProbe"); pb(p,name,dir); W(p,22)=vref; L(p,28)=(long)next;
    e=renamefile(p); diagnostic("persistent_rename_error",e);
    check("persistent_rename",e==0);
    if(e) return;
    for(i=0;i<=next[0];i++) name[i]=next[i];
    pb(p,result,0L); W(p,22)=vref; L(p,54)=id; zero(result,64);
    e=hfs(p,0x16L);
    diagnostic("persistent_after_error",e);
    diagnostic("persistent_after_parent",L(p,48));
    diagnostic("persistent_after_length",(unsigned char)result[0]);
    for(i=0;i<=name[0] && result[i]==name[i];i++);
    check("persistent_after_rename",e==0 && W(p,22)==vref && L(p,48)==dir && i>name[0]);
    check("data_fork_after_rename",forkread(name,dir,0));
    check("resource_fork_after_rename",forkread(name,dir,1));
}
static int specsame(a,b) char *a,*b;
{
    int i,n=(unsigned char)a[6];
    if(W(a,0)!=W(b,0) || L(a,2)!=L(b,2) || n!=(unsigned char)b[6]) return 0;
    for(i=0;i<n;i++) if(a[7+i]!=b[7+i]) return 0;
    return 1;
}
static void specpaths(file,folder,dir) char *file,*folder; long dir;
{
    char p[160],v[160],base[70],spec[70],path[256],vol[64];
    long e; short ref; int i,n;
    pb(v,vol,0L); zero(vol,64); if(volinfo(v)) return; ref=W(v,22);
    pb(p,file,dir); W(p,22)=ref; L(p,28)=(long)base;
    if(hfs(p,0x1bL)) return;
    path[0]=file[0]+1; path[1]=':';
    for(i=1;i<=file[0];i++) path[i+1]=file[i];
    {
        short wd; long opened, resolved=-1, closed=-1;
        pb(p,0L,dir); W(p,22)=ref; L(p,28)=0x4d383150L;
        opened=hfs(p,1L); wd=W(p,22);
        diagnostic("spec_wd_open_error",opened); diagnostic("spec_wd_ref",wd);
        if(!opened) {
            pb(p,path,0L); W(p,22)=wd; L(p,28)=(long)spec;
            resolved=hfs(p,0x1bL); diagnostic("spec_wd_resolve_error",resolved);
            pb(p,0L,0L); W(p,22)=wd; closed=hfs(p,2L);
            diagnostic("spec_wd_close_error",closed);
        }
        check("spec_working_directory",opened==0 && resolved==0 && closed==0 && specsame(base,spec));
    }
    pb(p,path,dir); W(p,22)=ref; L(p,28)=(long)spec;
    e=hfs(p,0x1bL); diagnostic("spec_relative_error",e);
    check("spec_relative_path",e==0 && specsame(base,spec));
    n=1; path[n++]=':'; path[n++]=':';
    for(i=1;i<=folder[0];i++) path[n++]=folder[i];
    path[n++]=':'; for(i=1;i<=file[0];i++) path[n++]=file[i];
    path[0]=n-1;
    pb(p,path,dir); W(p,22)=ref; L(p,28)=(long)spec;
    e=hfs(p,0x1bL); diagnostic("spec_parent_error",e);
    check("spec_parent_path",e==0 && specsame(base,spec));
    n=1; for(i=1;i<=vol[0];i++) path[n++]=vol[i]; path[n++]=':';
    for(i=1;i<=folder[0];i++) path[n++]=folder[i];
    path[n++]=':';
    for(i=1;i<=file[0];i++) path[n++]=file[i];
    path[0]=n-1;
    pb(p,path,0L); W(p,22)=ref; L(p,28)=(long)spec;
    e=hfs(p,0x1bL); diagnostic("spec_absolute_error",e);
    check("spec_absolute_path",e==0 && specsame(base,spec));
    pstr(path,":AbsentSpecProbe"); pb(p,path,dir); W(p,22)=ref; L(p,28)=(long)spec;
    e=hfs(p,0x1bL); diagnostic("spec_missing_error",e);
    check("spec_missing_parent",e==-43 && W(spec,0)==ref && L(spec,2)==dir && spec[6]==15);
    pstr(path,":AbsentSpecProbe:leaf"); zero(spec,70); spec[0]=0x55;
    pb(p,path,dir); W(p,22)=ref; L(p,28)=(long)spec;
    e=hfs(p,0x1bL); diagnostic("spec_intermediate_error",e);
    check("spec_missing_intermediate",e==-43 && spec[0]==0x55);
}

static void aliases(name,dir) char *name; long dir;
{
    char p[160],spec[70],target[70],newname[64],changed=0;
    long a[4],alias=0,fileid=0; int i,renamed=0;
    pb(p,name,dir); if(hfs(p,9L)==0) fileid=L(p,48);
    pb(p,0L,0L); volinfo(p);
    pb(p,name,dir); L(p,28)=(long)spec;
    /* FSSpec requires the volume's negative reference, not the default. */
    {
        char v[160],attrs[20]; long e; pb(v,0L,0L);
        if(volinfo(v)==0) {
            W(p,22)=W(v,22); L(v,18)=0;
            zero(attrs,20); L(v,32)=(long)attrs; L(v,36)=20;
            e=hfs(v,0x30L); diagnostic("volume_parameters_error",e);
            diagnostic("volume_parameters_version",W(attrs,0));
            diagnostic("volume_parameters_attributes",L(attrs,2));
            diagnostic("volume_parameters_length",L(v,40));
        }
    }
    if(check("file_spec",hfs(p,0x1bL)==0)) {
        a[0]=0; a[1]=(long)spec; a[2]=(long)&alias;
        { long e=aliascall(a,3L,2L); diagnostic("alias_create_error",e);
          check("alias_create",e==0 && alias); }
    }
    if(alias) {
        long e;
        zero(target,70); changed=0;
        a[0]=0; a[1]=alias; a[2]=(long)target; a[3]=(long)&changed;
        e=aliascall(a,4L,3L);
        diagnostic("alias_before_rename_error",e);
        diagnostic("alias_before_rename_parent",L(target,2));
        diagnostic("alias_before_rename_length",(unsigned char)target[6]);
    }
    pstr(newname,"RenamedProbe");
    pb(p,name,dir); L(p,28)=(long)newname;
    { long e=renamefile(p); diagnostic("rename_error",e);
      renamed=e==0; } check("rename_file",renamed);
    if(renamed) {
        for(i=0;i<=newname[0];i++) name[i]=newname[i];
        pb(p,name,dir);
        check("file_id_after_rename",hfs(p,9L)==0 && fileid && L(p,48)==fileid);
        if(alias) {
            a[0]=0; a[1]=alias; a[2]=(long)target; a[3]=(long)&changed;
            { long e;
              zero(target,70); e=aliascall(a,4L,3L);
              diagnostic("alias_resolve_error",e);
              diagnostic("alias_target_vref",W(target,0));
              diagnostic("alias_target_parent",L(target,2));
              diagnostic("alias_target_name_length",(unsigned char)target[6]);
              diagnostic("alias_changed",(unsigned char)changed);
              if((unsigned char)target[6]<=63) {
                  char text[64]; int j;
                  for(j=0;j<(unsigned char)target[6];j++) text[j]=target[7+j];
                  text[j]=0; out("mac81probe I alias_target_name "); out(text); out("\n");
              }
              check("alias_after_rename",e==0 &&
                  W(target,0)==W(spec,0) && L(target,2)==dir && target[6]==name[0]); }
        }
    }
    if(alias) disposehandle(alias);
    if(renamed) persistentids(name,dir);
    pstr(newname,"MissingProbe"); pb(p,newname,dir);
    check("missing_file",hfs(p,9L)==-43);
}
static void diagnostic(name,e) char *name; long e;
{
    out("mac81probe I "); out(name); out(" ");
    if(e<0) out("-");
    number(e<0 ? (unsigned long)-e : (unsigned long)e); out("\n");
}
static void aelong(a,n,v) unsigned short *a; int *n; unsigned long v;
{ a[(*n)++]=v; a[(*n)++]=v>>16; }
static long aedesc(type,data,size,desc) long type,size; char *data,*desc;
{
    unsigned short a[8]; int n=0;
    aelong(a,&n,type); aelong(a,&n,(long)data);
    aelong(a,&n,size); aelong(a,&n,(long)desc);
    return aecall(a,(long)n,0x0825L);
}
static long aeparam(event,key,desc) char *event,*desc; long key;
{
    unsigned short a[6]; int n=0;
    aelong(a,&n,(long)event); aelong(a,&n,key); aelong(a,&n,(long)desc);
    return aecall(a,(long)n,0x0610L);
}
static void aefree(desc) char *desc;
{
    unsigned short a[2]; int n=0;
    if(!L(desc,4)) return;
    aelong(a,&n,(long)desc); aecall(a,(long)n,0x0204L);
}
static void findercopy(name,dir) char *name; long dir;
{
    char p[160],v[160],path[64],src[70],dst[70];
    char address[8],event[8],reply[8],source[8],destination[8];
    unsigned short a[16]; long signature=0x4d414353L,target=0,e=-1;
    long error=0,type=0,size=0; short vref=0; int n=0,created=0,ok=0;
    zero(address,8); zero(event,8); zero(reply,8); zero(source,8); zero(destination,8);
    zero(src,70); zero(dst,70);
    pstr(path,":macsys:Mac81FinderCopy");
    pb(v,0L,0L); if(volinfo(v)==0) vref=W(v,22);
    pb(p,path,2L); W(p,22)=vref; e=hfs(p,6L);
    diagnostic("finder_copy_folder_error",e);
    created=e==0; if(created) target=L(p,48);
    check("finder_copy_folder",created && target!=0);
    pb(p,name,dir); W(p,22)=vref; L(p,28)=(long)src;
    e=hfs(p,0x1bL); check("finder_copy_source_spec",e==0);
    if(!e && created) {
        pb(p,0L,target); W(p,22)=vref; L(p,28)=(long)dst;
        e=hfs(p,0x1bL);
    } else e=-1;
    diagnostic("finder_copy_destination_error",e);
    if(!e) {
        diagnostic("finder_copy_destination_vref",W(dst,0));
        diagnostic("finder_copy_destination_parent",L(dst,2));
        diagnostic("finder_copy_destination_name_length",(unsigned char)dst[6]);
    }
    check("finder_copy_destination_spec",e==0 && W(dst,0)==vref && dst[6]==15);
    if(!e) e=aedesc(0x7369676eL,(char *)&signature,4L,address);
    if(!e) {
        aelong(a,&n,0x636f7265L); aelong(a,&n,0x636c6f6eL);
        aelong(a,&n,(long)address); a[n++]=0xffff;
        aelong(a,&n,0L); aelong(a,&n,(long)event);
        e=aecall(a,(long)n,0x0b14L);
    }
    if(!e) e=aedesc(0x66737320L,src,70L,source);
    if(!e) e=aedesc(0x66737320L,dst,70L,destination);
    if(!e) e=aeparam(event,0x2d2d2d2dL,source);
    if(!e) e=aeparam(event,0x696e7368L,destination);
    diagnostic("finder_copy_event_error",e);
    if(!e) {
        n=0; aelong(a,&n,(long)event); aelong(a,&n,(long)reply);
        aelong(a,&n,19L); a[n++]=0; aelong(a,&n,600L);
        aelong(a,&n,0L); aelong(a,&n,0L);
        e=aecall(a,(long)n,0x0d17L);
        diagnostic("finder_copy_send_error",e);
        if(!e) {
            n=0; aelong(a,&n,(long)reply); aelong(a,&n,0x6572726eL);
            aelong(a,&n,0x6c6f6e67L); aelong(a,&n,(long)&type);
            aelong(a,&n,(long)&error); aelong(a,&n,4L); aelong(a,&n,(long)&size);
            e=aecall(a,(long)n,0x0e11L);
            diagnostic("finder_copy_reply_status",e);
            diagnostic("finder_copy_reply_error",error);
            ok=(e==-1701 || (e==0 && type==0x6c6f6e67L && size==4)) && !error;
        }
    }
    check("finder_copy_reply",ok);
    check("finder_copy_data",ok && forkread(name,target,0));
    check("finder_copy_resource",ok && forkread(name,target,1));
    pb(p,name,target);
    check("finder_copy_metadata",ok && getinfo(p)==0 && L(p,32)==0x54455854L &&
        L(p,36)==0x6d383170L && W(p,40)==0x0100 &&
        L(p,72)==0xe0000000L && L(p,76)==0xe0000100L);
    check("finder_copy_source_intact",forkread(name,dir,0) && forkread(name,dir,1));
    aefree(destination); aefree(source); aefree(reply); aefree(event); aefree(address);
    if(created) {
        pb(p,name,target); removefile(p);
        pb(p,path,2L); W(p,22)=vref; e=removefile(p);
    } else e=-1;
    diagnostic("finder_copy_cleanup_error",e);
    check("finder_copy_cleanup",e==0);
}
static void fragments(owned)
long *owned;
{
    char lib[64],sym[64],bad[64],err[256],cls;
    char *libs[]={"InterfaceLib","ThreadsLib","DragLib","AppearanceLib"};
    char *syms[]={"Gestalt","GetCurrentThread","NewDrag","RegisterAppearanceClient"};
    long a[6],conn,extra,addr,mainaddr,e; int i;
    pstr(bad,"MissingMac81Symbol");
    for(i=0;i<4;i++) {
        conn=addr=mainaddr=0; pstr(lib,libs[i]); pstr(sym,syms[i]);
        a[0]=(long)lib; a[1]=0x6d36386bL; a[2]=1;
        a[3]=(long)&conn; a[4]=(long)&mainaddr; a[5]=(long)err;
        e=cfmcall(a,6L,1L);
        diagnostic("connection_id",conn);
        out("mac81probe I library "); out(libs[i]); out(" error ");
        if(e<0) out("-");
        number(e<0 ? (unsigned long)-e : (unsigned long)e); out("\n");
        if(check("cfm_load",e==0 && conn)) {
            owned[i]=conn;
            a[0]=conn; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
            e=cfmcall(a,4L,5L);
            diagnostic("symbol_error",e); diagnostic("symbol_address",addr);
            diagnostic("symbol_class",(unsigned char)cls);
            check("cfm_symbol",e==0 && addr);
            a[1]=(long)bad; addr=0;
            check("cfm_missing_symbol",cfmcall(a,4L,5L)<0 && !addr);
            extra=0; a[0]=(long)lib; a[1]=0x6d36386bL; a[2]=1;
            a[3]=(long)&extra; a[4]=(long)&mainaddr; a[5]=(long)err;
            e=cfmcall(a,6L,1L);
            check("cfm_repeat_load",e==0 && extra);
            if(extra) { a[0]=(long)&extra; check("cfm_close",cfmcall(a,1L,4L)==0); }
            /* Keep the primary connection through manager use. */
        }
    }
    pstr(lib,"MissingMac81Library"); conn=0;
    a[0]=(long)lib; a[1]=0x6d36386bL; a[2]=1;
    a[3]=(long)&conn; a[4]=(long)&mainaddr; a[5]=(long)err;
    check("cfm_missing_library",cfmcall(a,6L,1L)<0 && !conn);
}
static void drags()
{
    long a[6],drag=0,size=12; short count=0; char data[12]; int i;
    a[0]=(long)&drag;
    if(!check("drag_create",dragcall(a,1L,5L)==0 && drag)) return;
    a[0]=drag; a[1]=1; a[2]=0x54455854L;
    a[3]=(long)"fork bytes"; a[4]=10; a[5]=0;
    check("drag_add",dragcall(a,6L,7L)==0);
    a[1]=(long)&count;
    check("drag_count",dragcall(a,2L,14L)==0 && count==1);
    zero(data,12); a[1]=1; a[2]=0x54455854L;
    a[3]=(long)data; a[4]=(long)&size; a[5]=0;
    i=dragcall(a,6L,20L)==0 && size==10;
    check("drag_data",i && data[0]=='f' && data[9]=='s');
    check("drag_dispose",dragcall(a,1L,6L)==0);
}
static void threads()
{
    long a[7],id=0,current=0,again=0,result=0,e;
    volatile long flag=0; int i;
    a[0]=(long)&current;
    check("thread_current",threadcall(a,1L,0x0206L)==0 && current);
    a[0]=1; a[1]=(long)threadentry; a[2]=(long)&flag;
    a[3]=0; a[4]=1; a[5]=(long)&result; a[6]=(long)&id;
    e=threadcall(a,7L,0x0e03L);
    diagnostic("new_thread_error",e);
    if(check("thread_suspended",e==0 && id && !flag)) {
        check("thread_dispose",threaddispose(id)==0);
    }
    a[0]=1; a[1]=(long)threadentry; a[2]=(long)&flag;
    a[3]=0; a[4]=0; a[5]=(long)&result; a[6]=(long)&id;
    e=threadcall(a,7L,0x0e03L);
    diagnostic("new_thread_error",e);
    if(check("thread_create",e==0 && id)) {
        a[0]=0;
        for(i=0;i<32 && !flag;i++) threadcall(a,1L,0x0205L);
        check("thread_callback",flag==0x81cafe && result==(long)&flag);
    }
    a[0]=(long)&again;
    check("thread_return",threadcall(a,1L,0x0206L)==0 && again==current);
}
static void mounted()
{
    char p[160],folder[64],file[64]; long dir=0;
    pstr(folder,":macsys:Mac81MountProbe"); pstr(file,"MountFile");
    pb(p,folder,2L);
    { long e=hfs(p,6L); diagnostic("mount_dircreate_error",e);
      if(check("host_mount_folder",e==0)) dir=L(p,48); }
    if(!dir) return;
    pb(p,file,dir);
    if(check("host_mount_file",create(p)==0)) {
        check("host_mount_data",forkio(file,dir,0));
        check("host_mount_resource",forkio(file,dir,1));
        pb(p,file,dir);
        check("host_mount_catalog",hfs(p,9L)==0 && L(p,100)==dir);
        pb(p,file,dir); check("host_mount_unlink",removefile(p)==0);
    }
    pb(p,folder,2L); check("host_mount_rmdir",removefile(p)==0);
}
void start()
{
    char p[160], name[64], file[64]={9,'F','o','r','k','P','r','o','b','e'};
    char folder[]={10,'M','a','c','8','1','P','r','o','b','e'};
    long dir=0,e,fd,owned[4]; int i,vols=0;
    fd=auxsysr(5L,(long)"/tmp/mac81probe.out",0x309L,0666L);
    if(fd>=0) auxsysr(6L,fd,0L,0L);
    for(i=0;i<4;i++) owned[i]=0;
    out("mac81probe I started 1\n");
    diagnostic("unix_pid",auxsysr(20L,0L,0L,0L));
    pb(p,0L,0L); diagnostic("volinfo_before_managers",volinfo(p));
    diagnostic("vref_before_managers",W(p,22));
    finder();
    videodevices();
    out("mac81probe I phase cfm\n"); fragments(owned);
    out("mac81probe I phase drag\n"); drags();
    out("mac81probe I phase thread\n"); threads();
    out("mac81probe I phase filemanager\n");
    pb(p,0L,0L); diagnostic("volinfo_after_managers",volinfo(p));
    diagnostic("vref_after_managers",W(p,22));
    pb(p,folder,2L); e=hfs(p,6L); diagnostic("dircreate_error",e);
    if(check("create_folder",e==0)) dir=L(p,48);
    if(dir) {
        pb(p,folder,2L); W(p,28)=0;
        check("folder_id",hfs(p,9L)==0 && L(p,48)==dir);
        pb(p,file,dir);
        if(check("create_file",create(p)==0)) {
            check("data_fork",forkio(file,dir,0));
            check("resource_fork",forkio(file,dir,1));
            pb(p,file,dir);
            if(check("get_metadata",getinfo(p)==0)) {
                diagnostic("metadata_return_id",L(p,48));
                diagnostic("metadata_parent_id",dir);
                L(p,32)=0x54455854L; L(p,36)=0x6d383170L;
                W(p,40)=0x0100; L(p,72)=0xe0000000L; L(p,76)=0xe0000100L;
                L(p,48)=dir;
                e=setinfo(p); diagnostic("set_metadata_error",e);
                check("set_metadata",e==0);
                pb(p,file,dir);
                check("metadata_roundtrip",getinfo(p)==0 &&
                    L(p,32)==0x54455854L && L(p,36)==0x6d383170L &&
                    W(p,40)==0x0100 && L(p,72)==0xe0000000L && L(p,76)==0xe0000100L);
            }
            specpaths(file,folder,dir);
            findercopy(file,dir);
            aliases(file,dir);
            pb(p,file,dir); e=removefile(p); diagnostic("remove_file_error",e);
            check("remove_file",e==0);
        }
        pb(p,folder,2L); e=removefile(p); diagnostic("remove_folder_error",e);
        check("remove_folder",e==0);
    }
    for(i=1;i<=16;i++) {
        zero(name,64); pb(p,name,0L); W(p,28)=i; e=volinfo(p);
        diagnostic("enumerate_error",e); diagnostic("enumerate_vref",W(p,22));
        if(e) break;
        if(W(p,22)<0 && name[0]>0 && name[0]<=63) vols++;
    }
    check("volume_enumeration",vols>=1);
    mounted();
    for(i=3;i>=0;i--) {
        long a[1]; a[0]=(long)&owned[i];
        check("cfm_close_primary",owned[i] && cfmcall(a,1L,4L)==0 && !owned[i]);
    }
    out("mac81probe done\n"); exitapp();
}
