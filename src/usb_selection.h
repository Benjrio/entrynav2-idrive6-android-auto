/* Explicit phone selection. Read once from the private runtime directory.
 * No wildcard vendor probing; AOA negotiation still validates the protocol.
 */
#pragma once
static unsigned usb_expected_vid, usb_expected_pid;
static char usb_configured_port[80];
static int usb_phone_config(void) {
    int fd=open("usb-device.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return 0;
    struct stat st;char b[128]={0};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>120){close(fd);return 0;}
    ssize_t n=read(fd,b,sizeof(b)-1);close(fd);if(n!=st.st_size)return 0;
    char extra,vid[8],pid[8];
    /* Auto discovers only devices already in standard AOA accessory mode. */
    if(!strcmp(b,"auto\n")||!strcmp(b,"auto"))return 1;
    if(sscanf(b,"%79s %7s %7s %c",usb_configured_port,vid,pid,&extra)!=3)return 0;
    if(!strchr(usb_configured_port,'-')||strspn(usb_configured_port,"0123456789-.")!=strlen(usb_configured_port))return 0;
    if(strlen(vid)!=4||strlen(pid)!=4||strspn(vid,"0123456789abcdefABCDEF")!=4||strspn(pid,"0123456789abcdefABCDEF")!=4)return 0;
    usb_expected_vid=strtoul(vid,NULL,16);usb_expected_pid=strtoul(pid,NULL,16);
    return usb_expected_vid!=0;
}
static int usb_phone_matches(unsigned vid,unsigned pid) {
    return usb_expected_vid!=0 && vid==usb_expected_vid && pid==usb_expected_pid;
}
