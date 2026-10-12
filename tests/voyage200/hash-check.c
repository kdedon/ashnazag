#include <stdio.h>
#include <string.h>
#include "hash.h"
int
main()
{
	unsigned char digest[32]; char output[65]; unsigned int i;
	v_sha256((const unsigned char *)"abc",3UL,digest);
	for(i=0;i<32;i++)sprintf(output+2*i,"%02x",digest[i]);
	if(strcmp(output,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))return 1;
	v_sha256((const unsigned char *)"",0UL,digest);
	for(i=0;i<32;i++)sprintf(output+2*i,"%02x",digest[i]);
	if(strcmp(output,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))return 2;
	if(v_crc((const unsigned char *)"123456789",9UL)!=0xcbf43926UL)return 3;
	puts("PASS SHA256 empty/abc and CRC32 standard vector");return 0;
}
