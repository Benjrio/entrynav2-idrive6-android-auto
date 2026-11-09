/* Version response fields: saved aasdk ControlServiceChannel.cpp and
 * VersionResponseStatusEnum.proto, GPL-3.0-or-later, f1x.studio (2018).
 * Supported response status: MATCH=0.
 * Accepting that observed reply continues the existing 1.1 service subset;
 * this does not implement new 1.7 services or accept unknown future minors. */
#ifndef AA_PHONE_VERSION_H
#define AA_PHONE_VERSION_H
static unsigned aa_version_word(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
static int aa_version_match(const unsigned char *p,unsigned n,int phone){
 if(n!=6||aa_version_word(p)!=1||aa_version_word(p+4)!=0)return 0;
 unsigned minor=aa_version_word(p+2);
 return minor==1||(phone&&minor==7);
}
#endif
