static int checks;
#define CHECK(x) do {assert(x);checks++;}while(0)
static void reply(struct otx*x,long prim,long request,long sequence)
{
 mblk_t*m=allocb(48,1);int n=8;m->b_datap->db_type=M_PCPROTO;
 OB_P32(m->b_wptr,prim);OB_P32(m->b_wptr+4,request);
 if(prim==H_ERROR_ACK){OB_P32(m->b_wptr+8,7);OB_P32(m->b_wptr+12,0);n=16;}
 if(prim==H_DISCON_IND){OB_P32(m->b_wptr+8,sequence);n=12;}
 m->b_wptr+=n;CHECK(otx_upmsg(0,x,m)==0);
}
static void indication(struct otx*x,long sequence,int seed)
{
 mblk_t*m=allocb(40,1);m->b_datap->db_type=M_PROTO;
 OB_P32(m->b_wptr,H_CONN_IND);OB_P32(m->b_wptr+4,16);OB_P32(m->b_wptr+8,24);
 OB_P32(m->b_wptr+12,0);OB_P32(m->b_wptr+16,0);OB_P32(m->b_wptr+20,sequence);
 OB_P16(m->b_wptr+24,2);OB_P16(m->b_wptr+26,1234);OB_P32(m->b_wptr+28,seed);
 m->b_wptr+=40;CHECK(otx_upmsg(0,x,m)==0);
}
int main(void)
{
 struct otx l,a,newa;struct otbs session;mblk_t*m;unsigned char before[16],remote[16];int j;
 memset(&l,0,sizeof l);memset(&a,0,sizeof a);memset(&newa,0,sizeof newa);memset(&session,0,sizeof session);
 session.s_eps=&l;l.x_sess=a.x_sess=newa.x_sess=&session;l.x_snext=&a;l.x_cookie=1;a.x_cookie=2;
 l.x_proto=a.x_proto=newa.x_proto=OTX_TCP;a.x_generation=21;newa.x_generation=22;newa.x_cookie=2;
 l.x_lalen=16;a.x_lalen=a.x_ralen=16;memset(l.x_la,7,16);memset(a.x_la,8,16);memset(a.x_ra,9,16);
 memcpy(before,a.x_ra,16);CHECK(acceptstart(&l,&a,0)==0&&l.x_accept_pending&&l.x_accept_ralen==0);
 reply(&l,H_ERROR_ACK,H_CONN_RES,0);CHECK(!l.x_accept_pending&&!memcmp(a.x_ra,before,16));
 indication(&l,31,0x0a000202);memcpy(remote,l.x_ind[0].a,16);
 CHECK(acceptstart(&l,&a,99)==0&&l.x_accept_ralen==0);reply(&l,H_ERROR_ACK,H_CONN_RES,0);
 CHECK(acceptstart(&l,&a,31)==0);
 CHECK(!memcmp(a.x_ra,before,16)&&a.x_la[0]==8);CHECK(acceptstart(&l,&a,31)==OT_TOUTSTATE);
 reply(&l,H_OK_ACK,H_BIND_REQ,0);CHECK(l.x_accept_pending&&!memcmp(a.x_ra,before,16));
 reply(&l,H_ERROR_ACK,H_CONN_RES,0);CHECK(!l.x_accept_pending&&l.x_ind[0].valid&&!memcmp(a.x_ra,before,16)&&a.x_la[0]==8);
 CHECK(acceptstart(&l,&a,31)==0);reply(&l,H_OK_ACK,H_CONN_RES,0);
 CHECK(!l.x_accept_pending&&!l.x_ind[0].valid&&!memcmp(a.x_ra,remote,16)&&!memcmp(a.x_la,l.x_la,16));
 CHECK(a.x_ralen==16&&a.x_lalen==16);CHECK(acceptstart(&l,&a,31)==0&&l.x_accept_ralen==0);reply(&l,H_ERROR_ACK,H_CONN_RES,0);
 indication(&l,32,0x0a000203);CHECK(acceptstart(&l,&a,32)==0);l.x_snext=0;
 reply(&l,H_OK_ACK,H_CONN_RES,0);CHECK(!l.x_accept_pending&&!l.x_ind[1].valid);
 l.x_snext=&a;indication(&l,33,0x0a000204);CHECK(acceptstart(&l,&a,33)==0);l.x_snext=&newa;
 reply(&l,H_OK_ACK,H_CONN_RES,0);CHECK(newa.x_lalen==0&&newa.x_ralen==0&&!l.x_accept_pending);
 l.x_snext=&a;indication(&l,34,0x0a000205);CHECK(acceptstart(&l,&a,34)==0);
 reply(&l,H_DISCON_IND,0,99);CHECK(l.x_accept_pending);
 reply(&l,H_DISCON_IND,0,34);CHECK(!l.x_accept_pending&&!l.x_ind[3].valid);
 indication(&l,35,0x0a000206);CHECK(acceptstart(&l,&a,35)==0);reply(&l,H_DISCON_IND,0,-1);CHECK(!l.x_accept_pending);for(j=0;j<OTB_NIND;j++)CHECK(!l.x_ind[j].valid);
 for(j=0;j<10;j++)indication(&l,50+j,j);CHECK(acceptstart(&l,&a,50)==0&&l.x_accept_ralen==0);reply(&l,H_OK_ACK,H_CONN_RES,0);CHECK(a.x_ralen==0&&a.x_lalen==16);CHECK(acceptstart(&l,&a,59)==0);
 reply(&l,H_ERROR_ACK,H_CONN_RES,0);CHECK(!l.x_accept_pending);
 CHECK(acceptstart(&l,&a,59)==0);memcpy(before,a.x_ra,16);
 m=allocb(8,1);m->b_datap->db_type=M_PCPROTO;OB_P32(m->b_wptr,H_OK_ACK);OB_P32(m->b_wptr+4,H_CONN_RES);m->b_wptr+=8;
 nomem=1;CHECK(otx_upmsg(0,&l,m)==1&&l.x_accept_pending&&!memcmp(a.x_ra,before,16));nomem=0;
 CHECK(otx_upmsg(0,&l,m)==0&&!l.x_accept_pending&&live==0);
 indication(&l,88,3);CHECK(acceptstart(&l,&a,88)==0);reply(&l,H_OK_ACK,H_UNBIND_REQ,0);
 CHECK(!l.x_accept_pending&&l.x_nind==0&&l.x_lalen==0&&l.x_ralen==0);for(j=0;j<OTB_NIND;j++)CHECK(!l.x_ind[j].valid);
 printf("PASS %d production accept transaction checks\n",checks);return 0;
}
