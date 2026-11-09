/* Private Linux usbfs adapter. No reset/set-configuration, hub or disk I/O.
 * Optional atomic takeover is restricted to accessory interface 0 and usbfs. */
#include <dirent.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifndef USB_EVENT_SOURCE
#define USB_EVENT_SOURCE "physical-usb"
#endif
static int usb_fd=-1,usb_claimed=-1;
static unsigned usb_in,usb_out,usb_bus,usb_dev;
static char usb_port[80],usb_node[96];
static unsigned long usb_rx,usb_tx;
static unsigned usb_aoa_attempts,usb_read_timeouts;
static int usb_takeover_enabled,usb_takeover_attempted,usb_takeover_done;
static const char *usb_last_stage="none",*usb_last_reason="none",*usb_last_detail="none";
static int usb_last_errno,usb_last_rc;
static const char *usb_reason(int e){
 switch(e){case EBUSY:return "busy";case EACCES:case EPERM:return "permissions";
 case ENODEV:case ENOENT:case ESHUTDOWN:return "disconnect";case ETIMEDOUT:return "timeout";
 case ECANCELED:return "cancellation";case EINVAL:case EPROTO:return "invalid-descriptor";default:return "io-error";}
}
static int usb_failure(const char *stage,int rc,int error,const char *detail){
 usb_last_stage=stage;usb_last_rc=rc;usb_last_errno=error;usb_last_reason=usb_reason(error);usb_last_detail=detail;
 printf("USB_FAIL source=%s stage=%s rc=%d errno=%d reason=%s detail=%s port=%s bus=%u dev=%u\n",USB_EVENT_SOURCE,stage,rc,error,usb_last_reason,detail,usb_port,usb_bus,usb_dev);
 errno=error;return 0;
}
static int usb_invalid(const char *field){return usb_failure("descriptor-validation",0,EPROTO,field);}
static unsigned read_number(const char *port,const char *field,unsigned base){
 char path[256],text[40];snprintf(path,sizeof(path),"%s/%s/%s",usb_sysfs_root,port,field);
 FILE *f=fopen(path,"r");if(!f)return 0;int ok=fgets(text,sizeof(text),f)!=NULL;fclose(f);return ok?(unsigned)strtoul(text,NULL,base):0;
}
/* Preserve the initiating error across bounded best-effort owner observations. */
static unsigned long long usb_pid_birth(const char *pid){
 char path[96],b[1024];snprintf(path,sizeof(path),"/proc/%s/stat",pid);FILE*f=fopen(path,"r");if(!f)return 0;
 int ok=fgets(b,sizeof(b),f)!=NULL;fclose(f);char*s=ok?strrchr(b,')'):NULL;if(!s||s[1]!=' ')return 0;s+=2;
 for(unsigned i=0;i<19;i++){s=strchr(s,' ');if(!s)return 0;s++;}return strtoull(s,NULL,10);
}
static void usb_fd_observations(void){
 uint64_t until=now()+200000000ULL;unsigned processes=0,links=0,matches=0;DIR*d=opendir("/proc");if(!d){puts("USB_DIAG proc=unavailable");return;}struct dirent*e;
 while(!cancelled&&now()<until&&processes<1024&&links<8192&&matches<32&&(e=readdir(d))){
  if(!*e->d_name||strspn(e->d_name,"0123456789")!=strlen(e->d_name)||strlen(e->d_name)>10)continue;
  processes++;char pid[16],path[128],target[128],command[96]={0};strcpy(pid,e->d_name);
  unsigned long long birth=usb_pid_birth(pid);if(!birth)continue;
  snprintf(path,sizeof(path),"/proc/%s/fd",pid);DIR*fds=opendir(path);if(!fds)continue;struct dirent*fe;unsigned count=0;
  while(!cancelled&&now()<until&&count++<256&&links<8192&&matches<32&&(fe=readdir(fds))){
   if(strspn(fe->d_name,"0123456789")!=strlen(fe->d_name)||strlen(fe->d_name)>10)continue;
   links++;snprintf(path,sizeof(path),"/proc/%s/fd/%s",pid,fe->d_name);ssize_t n=readlink(path,target,sizeof(target)-1);if(n<0)continue;target[n]=0;
   if(strcmp(target,usb_node))continue;
   /* Only argv[0] basename, never arguments/environ (which may contain secrets). */
   snprintf(path,sizeof(path),"/proc/%s/cmdline",pid);FILE*f=fopen(path,"r");if(f){size_t k=0;int c;while(k<sizeof(command)-1&&(c=fgetc(f))>0)command[k++]=(char)c;fclose(f);}
   char*name=strrchr(command,'/');name=name?name+1:command;
   for(char*s=name;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||strchr("._-",*s)))*s='_';
   if(birth!=usb_pid_birth(pid))continue;matches++;
   printf("USB_DIAG fd_pid=%s starttime=%llu argv0_basename=%s fd=%s node=%s claim_proven=0\n",pid,birth,*name?name:"unavailable",fe->d_name,usb_node);
  }closedir(fds);
 }
 closedir(d);printf("USB_DIAG proc_scanned=%u fd_scanned=%u matches=%u bounded=1 complete=0\n",processes,links,matches);
}
static void usb_diagnose(unsigned config,unsigned iface){
 int saved=errno;struct usbdevfs_getdriver driver;memset(&driver,0,sizeof(driver));driver.interface=iface;
 int rc=ioctl(usb_fd,USBDEVFS_GETDRIVER,&driver),error=rc<0?errno:0;driver.driver[sizeof(driver.driver)-1]=0;
 printf("USB_DIAG source=%s interface=%s:%u.%u getdriver_rc=%d errno=%d driver=%s\n",USB_EVENT_SOURCE,usb_port,config,iface,rc,error,rc<0?"unknown":driver.driver);
 char path[256],link[256];snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s:%u.%u/driver",usb_port,config,iface);
 ssize_t n=readlink(path,link,sizeof(link)-1);error=n<0?errno:0;if(n>=0)link[n]=0;
 printf("USB_DIAG binding=%s rc=%d errno=%d\n",n<0?"unknown":link,(int)n,error);usb_fd_observations();
 errno=saved;
}
static int usb_ctl(unsigned type,unsigned request,unsigned index,void *data,unsigned size){
 if(cancelled){errno=ECANCELED;return -1;}
 struct usbdevfs_ctrltransfer c={.bRequestType=type,.bRequest=request,.wIndex=index,.wLength=size,.timeout=1000,.data=data};
 return ioctl(usb_fd,USBDEVFS_CONTROL,&c);
}
static void usb_close(void){
 int saved=errno,was=usb_claimed>=0,rc=0,error=0;
 if(usb_fd>=0){if(was){rc=ioctl(usb_fd,USBDEVFS_RELEASEINTERFACE,&usb_claimed);error=rc<0?errno:0;}close(usb_fd);}
 usb_fd=usb_claimed=-1;
 printf("USB_CLEANUP source=%s claimed_before=%d release_attempted=%d release_rc=%d release_errno=%d released=%d closed=1 threads=0 rx=%lu tx=%lu\n",USB_EVENT_SOURCE,was,was,rc,error,!was||rc==0||error==ENODEV,usb_rx,usb_tx);errno=saved;
}
static int accessory(unsigned v,unsigned p){return usb_aoa_ids(v,p);}
/* Enumeration picked by the port/any policies (usb_select_discovered); open must find exactly that device. */
static unsigned usb_sel_vid,usb_sel_pid,usb_sel_dev,usb_sel_bus;static int usb_sel_valid;
static int usb_open_port(int reenum){
 if(cancelled)return usb_failure("open",-1,ECANCELED,"cancel-before-open");
 unsigned vid=read_number(usb_port,"idVendor",16),pid=read_number(usb_port,"idProduct",16);
 unsigned bus=read_number(usb_port,"busnum",10),dev=read_number(usb_port,"devnum",10);
 int identity=reenum?accessory(vid,pid):accessory(vid,pid)||
  (usb_policy_discovers()?usb_sel_valid&&vid==usb_sel_vid&&pid==usb_sel_pid&&dev==usb_sel_dev&&bus==usb_sel_bus:usb_phone_matches(vid,pid));
 if(!identity)return usb_failure("device-selection",-1,ENODEV,"identity-mismatch-or-absent");
 if(read_number(usb_port,"bDeviceClass",16)==9)return usb_failure("device-selection",-1,EPERM,"hub-rejected");
 if(!bus||!dev||(reenum&&(bus!=usb_bus||dev==usb_dev)))return usb_failure("enumeration",-1,ENODEV,"bus-or-fresh-devnum");
 snprintf(usb_node,sizeof(usb_node),"/dev/bus/usb/%03u/%03u",bus,dev);
 usb_fd=open(usb_node,O_RDWR|O_CLOEXEC);int error=usb_fd<0?errno:0;
 printf("USB_OPEN source=%s port=%s bus=%u dev=%u node=%s rc=%d errno=%d\n",USB_EVENT_SOURCE,usb_port,bus,dev,usb_node,usb_fd,error);
 if(usb_fd<0)return usb_failure("open",-1,error,"device-node");
 if(cancelled){usb_failure("device-descriptor",-1,ECANCELED,"cancel-before-descriptor");goto fail;}
 unsigned char d[18]={0};struct usbdevfs_ctrltransfer c={.bRequestType=0x80,.bRequest=6,.wValue=0x100,.wLength=18,.timeout=1000,.data=d};int n=ioctl(usb_fd,USBDEVFS_CONTROL,&c);error=n<0?errno:0;
 printf("USB_IOCTL source=%s stage=device-descriptor rc=%d errno=%d\n",USB_EVENT_SOURCE,n,error);
 if(n<0){usb_failure("device-descriptor",n,error,"GET_DESCRIPTOR");goto fail;}
 if(n!=18||d[0]!=18||d[1]!=1){usb_invalid("device.length-or-type");goto fail;}
 if((unsigned)(d[8]|d[9]<<8)!=vid||(unsigned)(d[10]|d[11]<<8)!=pid){usb_invalid("device.fd-vid-pid-mismatch");goto fail;}
 if(read_number(usb_port,"devnum",10)!=dev||read_number(usb_port,"busnum",10)!=bus){usb_failure("enumeration",-1,ENODEV,"changed-after-open");goto fail;}
 usb_bus=bus;usb_dev=dev;printf("USB_DEVICE source=%s port=%s bus=%u dev=%u vid=%04x pid=%04x opened_descriptor_verified=1\n",USB_EVENT_SOURCE,usb_port,bus,dev,vid,pid);return accessory(vid,pid)?2:1;
fail:usb_close();return 0;
}
/* port/any policies: sysfs-only filter (usb_discovery.h). Nothing is sent to any device here; with two or more
 * candidates nothing is sent at all. only_port limits the pass to one port (reconnect, --port). */
static int usb_select_discovered(const char *only_port){
 static struct usb_cand list[USB_DISC_MAX];unsigned picked=0,settling=0,n;uint64_t t=now();const struct usb_cand *one=NULL;
 usb_sel_valid=0;
 if(only_port){n=1;if(usb_disc_eval(only_port,&list[0],t))picked=1;else if(list[0].settling)settling=1;}
 else n=usb_disc_scan(list,USB_DISC_MAX,t,&picked,&settling);
 for(unsigned i=0;i<n;i++){usb_disc_log(USB_EVENT_SOURCE,&list[i]);if(list[i].eligible||list[i].accessory)one=&list[i];}
 printf("USB_SELECTION source=%s policy=%s candidates=%u settling=%u ambiguity_write_attempted=0\n",USB_EVENT_SOURCE,usb_policy_name(),picked,settling);
 if(picked&&settling)return usb_failure("device-selection",0,ENODEV,"wait-for-complete-selection");
 if(picked>1)return usb_failure("device-selection",0,EBUSY,"ambiguous");
 if(picked==1){struct usb_cand verify;
  if(!one || !usb_disc_eval(one->port,&verify,now()) || verify.bus!=one->bus || verify.dev!=one->dev || verify.vid!=one->vid || verify.pid!=one->pid)
   return usb_failure("device-selection",0,ENODEV,"snapshot-changed");}
 if(!picked)return usb_failure("device-selection",0,ENODEV,settling?"settling":"no-device");
 strcpy(usb_port,one->port);usb_sel_vid=one->vid;usb_sel_pid=one->pid;usb_sel_dev=one->dev;usb_sel_bus=one->bus;usb_sel_valid=1;
 return 1;
}
static int usb_select(const char *explicit_port){
 if(explicit_port&&*explicit_port){if(strlen(explicit_port)>=sizeof(usb_port)||strspn(explicit_port,"0123456789-.")!=strlen(explicit_port))return usb_failure("device-selection",0,EINVAL,"invalid-port");
  if(usb_policy_discovers()){char only[sizeof(usb_port)];strcpy(only,explicit_port);return usb_select_discovered(only);}
  if(explicit_port!=usb_port)strcpy(usb_port,explicit_port);return 1;}
 if(usb_policy_discovers())return usb_select_discovered(NULL);
 DIR *dir=opendir("/sys/bus/usb/devices");if(!dir)return usb_failure("device-selection",-1,errno,"sysfs-open");struct dirent *e;unsigned count=0;
 while((e=readdir(dir))){if(strchr(e->d_name,':')||strlen(e->d_name)>=sizeof(usb_port))continue;
  unsigned v=read_number(e->d_name,"idVendor",16),p=read_number(e->d_name,"idProduct",16);
  int match=usb_phone_matches(v,p)||accessory(v,p);
  if(match){count++;strcpy(usb_port,e->d_name);}}
 closedir(dir);printf("USB_SELECTION source=%s candidates=%u ambiguity_write_attempted=0\n",USB_EVENT_SOURCE,count);
 return count==1?1:usb_failure("device-selection",0,count?EBUSY:ENODEV,count?"ambiguous":"no-device");
}
static int usb_config(const unsigned char *c,int n){
 int selected=0,seen=0,endpoints=0;unsigned unique=0;unsigned char interfaces[256]={0};usb_in=usb_out=0;
 if(n<9||c[0]!=9||c[1]!=2)return usb_invalid("config.length-or-type");
 if((unsigned)n!=(unsigned)(c[2]|c[3]<<8))return usb_invalid("config.wTotalLength");
 if(c[5]!=1)return usb_invalid("config.bConfigurationValue-not-1");
 for(int i=9;i<n;){
  if(i+2>n)return usb_invalid("descriptor.trailing-byte");unsigned len=c[i],type=c[i+1];
  if(len<2||i+(int)len>n)return usb_invalid("descriptor.bLength");
  if(type==4){if(len!=9)return usb_invalid("interface.bLength");if(!interfaces[c[i+2]]){interfaces[c[i+2]]=1;unique++;}
   selected=c[i+2]==0&&c[i+3]==0;
   if(selected){if(seen++)return usb_invalid("interface.duplicate-0-alt0");if(c[i+4]!=2||c[i+5]!=0xff)return usb_invalid("interface.accessory-contract");
    printf("USB_INTERFACE source=%s number=0 alt=0 class=%u subclass=%u protocol=%u\n",USB_EVENT_SOURCE,c[i+5],c[i+6],c[i+7]);}}
  if(type==5&&selected){if(len<7||(c[i+3]&3)!=2)return usb_invalid("endpoint.length-or-not-bulk");
   unsigned ep=c[i+2],packet=c[i+4]|c[i+5]<<8;if(!(ep&15)||(ep&0x70)||!packet||packet>1024)return usb_invalid("endpoint.address-or-maxpacket");
   if(ep&0x80){if(usb_in)return usb_invalid("endpoint.duplicate-in");usb_in=ep;}else{if(usb_out)return usb_invalid("endpoint.duplicate-out");usb_out=ep;}endpoints++;}
  i+=len;
 }
 if(unique!=c[4])return usb_invalid("config.bNumInterfaces");
 if(seen!=1||endpoints!=2||!usb_in||!usb_out)return usb_invalid("interface.missing-bulk-pair");
 printf("USB_DESCRIPTOR source=%s config=1 interface=0 alt=0 in=0x%02x out=0x%02x interfaces=%u total=%d\n",USB_EVENT_SOURCE,usb_in,usb_out,unique,n);return 1;
}
/* Supported 148 kernel proc_disconnect_claim, Image VA 0x802cdf94.
 * IF_DRIVER=1 checks the name inside
 * the kernel before release+claim under the USB device lock. No split unbind.
 * The old userspace handle is not ours: releasing our claim does not guarantee
 * its application-level session resumes. Never claim successful handback here. */
static int usb_takeover_interface0(void){
 struct en2_usbfs_disconnect_claim {unsigned interface,flags;char driver[256];};
 _Static_assert(sizeof(struct en2_usbfs_disconnect_claim)==264,"usbfs disconnect-claim ABI");
 const unsigned long request=_IOR('U',27,struct en2_usbfs_disconnect_claim);
 _Static_assert(_IOR('U',27,struct en2_usbfs_disconnect_claim)==0x8108551bUL,"verified ARM ioctl");
 if(!usb_takeover_enabled||usb_takeover_attempted||cancelled)return usb_failure("DISCONNECT_CLAIM",-1,ECANCELED,"not-enabled-or-cancelled");
 usb_takeover_attempted=1;
 if(!accessory(read_number(usb_port,"idVendor",16),read_number(usb_port,"idProduct",16))||
    read_number(usb_port,"busnum",10)!=usb_bus||read_number(usb_port,"devnum",10)!=usb_dev||
    read_number(usb_port,"bConfigurationValue",10)!=1||!usb_in||!usb_out)
  return usb_failure("DISCONNECT_CLAIM",-1,ENODEV,"accessory-identity-changed");
 struct usbdevfs_getdriver driver;memset(&driver,0,sizeof(driver));driver.interface=0;
 int rc=ioctl(usb_fd,USBDEVFS_GETDRIVER,&driver),error=rc<0?errno:0;
 driver.driver[sizeof(driver.driver)-1]=0;
 if(rc<0||strcmp(driver.driver,"usbfs"))return usb_failure("DISCONNECT_CLAIM",-1,error?error:EBUSY,"expected-usbfs-owner");
 struct en2_usbfs_disconnect_claim dc;memset(&dc,0,sizeof(dc));dc.flags=1;strcpy(dc.driver,"usbfs");
 rc=ioctl(usb_fd,request,&dc);error=rc<0?errno:0;
 printf("USB_IOCTL source=%s stage=DISCONNECT_CLAIM interface=0 if_driver=usbfs rc=%d errno=%d reset=0 service_stop=0\n",USB_EVENT_SOURCE,rc,error);
 if(rc<0)return usb_failure("DISCONNECT_CLAIM",rc,error,"atomic-usbfs-only-no-fallback");
 usb_takeover_done=1;return 1;
}
static int usb_connect(const char *port){
 if(cancelled)return usb_failure("connect",-1,ECANCELED,"cancel-before-connect");
 if(!usb_select(port))return 0;int mode=usb_open_port(0);if(!mode)return 0;
 if(mode==1){if(usb_aoa_attempts>=3){usb_failure("AOA-attempt-budget",0,ETIMEDOUT,"max-3");goto fail;}usb_aoa_attempts++;printf("USB_AOA_ATTEMPT source=%s index=%u max=3\n",USB_EVENT_SOURCE,usb_aoa_attempts);
  unsigned char version[2]={0};int n=usb_ctl(0xc0,51,0,version,2),error=n<0?errno:0;unsigned protocol=version[0]|version[1]<<8;
  const char *probe=n<0?(error==EPIPE?"stall":"error"):n!=2?"short":(protocol!=1&&protocol!=2)?"version":"ok";
  printf("USB_AOA_PROBE source=%s port=%s bus=%u dev=%u vid=%04x pid=%04x result=%s version=%u\n",USB_EVENT_SOURCE,usb_port,usb_bus,usb_dev,
   read_number(usb_port,"idVendor",16),read_number(usb_port,"idProduct",16),probe,protocol);
  if(n!=2||(protocol!=1&&protocol!=2)){
   /* Never ask this enumeration again; a replug (new devnum) clears it. */
   if(usb_policy_discovers())usb_disc_mark_not_aoa(usb_port,usb_bus,usb_dev);
   usb_failure("AOA_GET_PROTOCOL",n,n<0?error:EPROTO,"length-or-version");goto fail;}
  printf("USB_AOA source=%s version=%u\n",USB_EVENT_SOURCE,protocol);
  const char *strings[]={"Android","Android Auto","Android Auto","2.0.1","https://example.invalid/entrynav2","WIRED-AA-001"};
  for(unsigned i=0;i<6;i++){n=usb_ctl(0x40,52,i,(void*)strings[i],strlen(strings[i])+1);error=n<0?errno:0;if(n!=(int)strlen(strings[i])+1){printf("USB_AOA_STRING index=%u\n",i);usb_failure("AOA_SEND_STRING",n,n<0?error:EPROTO,"short-or-failed");goto fail;}}
  int r=usb_ctl(0x40,53,0,NULL,0),start_errno=r<0?errno:0;printf("USB_START source=%s result=%d errno=%d\n",USB_EVENT_SOURCE,r,start_errno);close(usb_fd);usb_fd=-1;
  if(r<0&&start_errno!=ENODEV)return usb_failure("AOA_START",r,start_errno,"ioctl");uint64_t until=now()+15000000000ULL;
  do{if(cancelled)return usb_failure("AOA_REENUMERATION",-1,ECANCELED,"cancel");usleep(100000);mode=usb_open_port(1);}while(!mode&&now()<until);
  if(mode!=2)return usb_failure("AOA_REENUMERATION",-1,ETIMEDOUT,"fresh-accessory-deadline");
 }else printf("USB_AOA source=%s already_accessory=1 version=unknown\n",USB_EVENT_SOURCE);
 if(cancelled){usb_failure("config-descriptor",-1,ECANCELED,"cancel-before-descriptor");goto fail;}
 unsigned char config[4096];struct usbdevfs_ctrltransfer c={.bRequestType=0x80,.bRequest=6,.wValue=0x200,.wLength=sizeof(config),.timeout=1000,.data=config};
 int n=ioctl(usb_fd,USBDEVFS_CONTROL,&c),error=n<0?errno:0;
 printf("USB_IOCTL source=%s stage=config-descriptor rc=%d errno=%d\n",USB_EVENT_SOURCE,n,error);
 if(n<0){usb_failure("config-descriptor",n,error,"GET_DESCRIPTOR");goto fail;}
 if(!usb_config(config,n))goto fail;
 unsigned char active=0;n=usb_ctl(0x80,8,0,&active,1);error=n<0?errno:0;
 printf("USB_IOCTL source=%s stage=GET_CONFIGURATION rc=%d errno=%d\n",USB_EVENT_SOURCE,n,error);
 if(n!=1){usb_failure("GET_CONFIGURATION",n,n<0?error:EPROTO,"length-or-ioctl");goto fail;}
 printf("USB_CONFIGURATION source=%s active=%u expected=%u\n",USB_EVENT_SOURCE,active,config[5]);
 if(active!=config[5]){usb_failure("GET_CONFIGURATION",n,EPROTO,"active-mismatch-no-set-configuration");goto fail;}
 if(cancelled){usb_failure("CLAIMINTERFACE",-1,ECANCELED,"cancel-before-claim");goto fail;}
 int iface=0;n=ioctl(usb_fd,USBDEVFS_CLAIMINTERFACE,&iface);error=n<0?errno:0;
 printf("USB_IOCTL source=%s stage=CLAIMINTERFACE interface=0 rc=%d errno=%d\n",USB_EVENT_SOURCE,n,error);
 if(n<0){usb_failure("CLAIMINTERFACE",n,error,"interface-0");usb_diagnose(config[5],0);
  if(error!=EBUSY||!usb_takeover_enabled||!usb_takeover_interface0())goto fail;
 }usb_claimed=0;
 usb_last_stage="BULK_READY";usb_last_errno=0;usb_last_rc=0;usb_last_reason="none";usb_last_detail="interface-0";
 printf("USB_BULK_READY source=%s port=%s bus=%u dev=%u config=1 interface=0 alt=0 in=%u out=%u detach=%d reset=0\n",USB_EVENT_SOURCE,usb_port,usb_bus,usb_dev,usb_in,usb_out,usb_takeover_done);return 1;
fail:usb_close();return 0;
}
static int usb_write(const void *bytes,unsigned size){
 unsigned pos=0;uint64_t until=now()+2000000000ULL;
 while(pos<size){
  if(cancelled)return usb_failure("bulk-out",-1,ECANCELED,"cancel");
  if(now()>until)return usb_failure("bulk-out",-1,ETIMEDOUT,"total-deadline");
  struct usbdevfs_bulktransfer b={.ep=usb_out,.len=size-pos,.timeout=1500,.data=(void*)((const uint8_t*)bytes+pos)};
  int n=ioctl(usb_fd,USBDEVFS_BULK,&b),error=n<0?errno:0;
  if(n<=0||n>(int)(size-pos))return usb_failure("bulk-out",n,n<0?error:EIO,n==0?"zero-progress":"ioctl-or-invalid-length");
  pos+=(unsigned)n;usb_tx+=(unsigned)n;
 }return 1;
}
static int usb_read(void *bytes,unsigned size){
 if(cancelled){usb_failure("bulk-in",-1,ECANCELED,"cancel");return -1;}
 struct usbdevfs_bulktransfer b={.ep=usb_in,.len=size,.timeout=100,.data=bytes};int n=ioctl(usb_fd,USBDEVFS_BULK,&b),error=n<0?errno:0;
 if(n<0&&error==ETIMEDOUT){if(!usb_read_timeouts++)printf("USB_POLL_TIMEOUT source=%s stage=bulk-in errno=%d fatal=0\n",USB_EVENT_SOURCE,error);return 0;}
 if(n<0||n>(int)size){usb_failure("bulk-in",n,n<0?error:EIO,"ioctl-or-invalid-length");return -1;}usb_rx+=(unsigned)n;return n;
}
