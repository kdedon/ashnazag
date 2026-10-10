#include <assert.h>
static int checks;
#define CHECK(x) do { assert(x); checks++; } while (0)
int main(void)
{
 struct fbinfo fi; char b[SP_LEN]; int l,e;
 memset(&fi,0,sizeof fi); fi.fi_depth=8;fi.fi_width=640;fi.fi_height=480;fi.fi_rowbytes=640;
 uv.v_on=1;uv.v_base=UV_SLOTVA;uv.v_len=640*480;
 uv_mkdecl(&fi);
 CHECK(uv_rsrc(UV_SLOT,UV_NET)<0);
 memset(b,0,sizeof b);CHECK(uv_next(b,0)==0 && G8(b+SP_ID)==1);
 CHECK(uv_next(b,0)==0 && G8(b+SP_ID)==UV_MODE);
 CHECK(uv_next(b,0)==SM_NOMORE);
 CHECK(uv_tgt(-1)==-1 && uv_tgt(UV_DECLSIZE-2)==-1);
 uinter_netport=1;uv_mkdecl(&fi);
 l=uv_rsrc(UV_SLOT,UV_NET);CHECK(l==0xd0);
 CHECK(uv_rsrc(UV_SLOT+1,UV_NET)<0);
 memset(b,0,sizeof b);P16(b+SP_CAT,4);P16(b+SP_CAT+2,1);P8(b+SP_TBMASK,3);
 CHECK(uv_next(b,1)==0 && G8(b+SP_ID)==UV_NET);
 CHECK(G16(b+SP_CAT)==4 && G16(b+SP_CAT+2)==1 && G16(b+SP_CAT+4)==1);
 CHECK(G8(b+SP_HWDEV)==2);
 CHECK(uv_next(b,1)==SM_NOMORE);
 memset(b,0,sizeof b);P8(b+SP_SLOT,UV_SLOT);P8(b+SP_ID,UV_NET);P32(b+SP_RESULT,0x1000);
 CHECK(uv_slot(0x19,b)==0 && output[0]==5 && memcmp(output+1,".ENET",5)==0);
 e=uv_tgt(uv_find(l,4));CHECK(e==0x110);
 P32(b+SP_PTR,UV_DECLVA+e);P8(b+SP_ID,2);P32(b+SP_RESULT,0x1000);
 CHECK(uv_slot(0x05,b)==0 && G32(b+SP_SIZE)==sizeof uv_enet);
 CHECK(memcmp(output,uv_enet,sizeof uv_enet)==0);
 P8(b+SP_ID,3);CHECK(uv_slot(0x05,b)==SM_BADREF);
 P8(b+SP_SLOT,UV_SLOT);P8(b+SP_ID,UV_NET);CHECK(uv_slot(0x2d,b)==SM_GETDRVR);
 memset(b,0,sizeof b);CHECK(uv_next(b,0)==0);CHECK(uv_next(b,0)==0);CHECK(uv_next(b,0)==0 && G8(b+SP_ID)==UV_NET);CHECK(uv_next(b,0)==SM_NOMORE);
 P8(b+SP_SLOT,UV_SLOT);P8(b+SP_ID,UV_MODE);CHECK(uv_slot(0x16,b)==0 && G16(b+SP_CAT)==3);
 printf("PASS %d production SlotManager checks\n",checks);return 0;
}
