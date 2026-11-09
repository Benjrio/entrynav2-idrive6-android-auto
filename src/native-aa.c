/* USB Android Auto receiver with bounded session lifetime and video decode. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <gnu/libc-version.h>
#include <libavcodec/avcodec.h>
#include <libavutil/sha.h>
#include <openssl/ssl.h>
static volatile sig_atomic_t cancelled;
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void require(int ok,const char *label){if(!ok){printf("FATAL stage=%s errno=%d\n",label,errno);exit(2);}}
static void stop(int s){(void)s;cancelled=1;}
static unsigned audio_packets;static uint64_t audio_bytes;
static const char *identity_cert="identity/phone.crt",*identity_key="identity/phone.key";
static const char *out_dir="result";
static unsigned ping_pending,ping_responses,ping_requests;
static unsigned version_passes,tls_passes,service_passes;
static uint64_t ping_token,ping_deadline,ping_next;
static AVCodecContext *decoder;
static AVCodecParserContext *parser;
static AVFrame *picture;
static AVPacket *packet;
static unsigned video_epoch,video_rx,decoded_count,stream_frames,video_packets,video_failures;
static uint64_t video_origin,video_last,first_frame;
static int video_active,hide_video;
static uint32_t local_control[16];
#include "stream-wire.h"
#define PCM_FRAMES 480u
#define PCM_RATE 48000u
#include "usb_selection.h"
#include "usb-native.h"
#include "frame_output.h"
static void video_end(void){
 graphics_reset();
 if(parser)av_parser_close(parser);parser=NULL;avcodec_free_context(&decoder);video_active=0;
 printf("VIDEO_END epoch=%u stream_frames=%u decoder_parser_released=1\n",video_epoch,stream_frames);
}
static int video_begin(void){
 video_end();video_epoch++;stream_frames=video_packets=0;video_origin=video_last=0;video_active=1;
 const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);decoder=avcodec_alloc_context3(codec);
 if(!decoder)return 0;decoder->thread_count=1;decoder->max_pixels=1280*720;decoder->pkt_timebase=(AVRational){1,1000000};
 if(avcodec_open2(decoder,codec,NULL)<0)return 0;parser=av_parser_init(AV_CODEC_ID_H264);if(!parser)return 0;
 /* Each call below is ONE completely reassembled AA video media message.
  * USB reads and TLS fragments never reach this parser directly. */
 parser->flags|=PARSER_FLAG_COMPLETE_FRAMES;
 printf("VIDEO_BEGIN epoch=%u config_count_pts_eos_reset=1 complete_message_AU=1\n",video_epoch);return 1;
}
static int video_packet(const uint8_t *p,unsigned n,const uint8_t *config,unsigned config_size,uint64_t us){
 if(!video_active||n+config_size>STREAM_MAX_AU)return 0;
 if(!video_packets){video_origin=us;printf("VIDEO_ORIGIN epoch=%u source_us=%llu\n",video_epoch,(unsigned long long)us);}
 if(us<video_origin||(video_packets&&us<video_last))return 0;video_last=us;video_rx++;video_packets++;
 av_packet_unref(packet);if(av_new_packet(packet,(int)(n+config_size))<0)return 0;
 memcpy(packet->data,config,config_size);memcpy(packet->data+config_size,p,n);
 uint8_t *parsed=NULL;int size=0;int used=av_parser_parse2(parser,decoder,&parsed,&size,packet->data,packet->size,(int64_t)(us-video_origin),(int64_t)(us-video_origin),-1);
 if(used!=packet->size||size!=packet->size){printf("VIDEO_FAIL parser_consumed=%d packet=%d emitted=%d\n",used,packet->size,size);return 0;}
 packet->pts=packet->dts=(int64_t)(us-video_origin);int r=avcodec_send_packet(decoder,packet);if(r<0){printf("VIDEO_FAIL decoder_send=%d\n",r);return 0;}
 for(;;){r=avcodec_receive_frame(decoder,picture);if(r==AVERROR(EAGAIN)||r==AVERROR_EOF)break;if(r<0)return 0;
  if(picture->width<=0||picture->height<=0||picture->width>1280||picture->height>720||picture->format!=AV_PIX_FMT_YUV420P)return 0;
  decoded_count++;stream_frames++;if(!first_frame)first_frame=now();
  if(stream_frames<=2)printf("VIDEO_DECODE epoch=%u count=%u pts=%lld layout=%dx%d format=%d range=%d matrix=%d transfer=%d monotonic_ns=%llu\n",video_epoch,decoded_count,(long long)picture->pts,picture->width,picture->height,picture->format,picture->color_range,picture->colorspace,picture->color_trc,(unsigned long long)now());
  if(graphics_ok&&!hide_video)graphics_draw(picture,out_dir);
  av_frame_unref(picture);
 }
 return 1;
}
static int native_write(const uint8_t *p,unsigned n){return usb_write(p,n);}
static int native_read(uint8_t *p,unsigned capacity){return usb_read(p,capacity);}
#include "aa-protocol.h"
static const char *transport_name(void){
 return "physical-usb";
}
static void aa_pcm_output_tick(void){}
static int keepalive(void){
 if(aa_state<3)return 1;uint64_t t=now();if(ping_pending&&t>ping_deadline)return aa_error("ping-deadline");
 if(!ping_pending&&t>=ping_next){ping_token=t/1000;struct AaPb p={0};aa_v(&p,1,ping_token);if(!aa_send(0,11,p.b,p.n,0,0))return 0;ping_pending=1;ping_requests++;ping_deadline=t+5000000000ULL;ping_next=t+5000000000ULL;printf("KEEPALIVE_REQUEST count=%u token=%llu\n",ping_requests,(unsigned long long)ping_token);}
 return 1;
}
static void summary(unsigned connects,const char *reason){
 printf("STAGE_COUNTS version=%u TLS=%u services=%u video_RX=%u decode=%u present=%u\n",version_passes,tls_passes,service_passes,video_rx,decoded_count,present_count);
 char path[512];snprintf(path,sizeof(path),"%s/summary.json",out_dir);FILE*f=fopen(path,"w");if(!f)return;
 fprintf(f,"{\n  \"transport\": \"%s\",\n",transport_name());
 fprintf(f,"  \"schema\": 1, \"physical_acceptance\": false, \"physical_pixels_verified\": false,\n  \"connections\": %u, \"video_rx\": %u, \"decoded\": %u, \"present_submitted\": %u,\n  \"video_epochs\": %u, \"keepalive_requests\": %u, \"keepalive_responses\": %u,\n  \"audio_packets\": %u, \"audio_bytes\": %llu, \"audio_output\": \"UNVERIFIED_ROUTE_NOT_OPENED\",\n  \"idrive\": \"UNVERIFIED_NATIVE_INPUT_BACKEND\", \"microphone\": \"SKIP_NO_CAPTURE_BACKEND_OR_CONSENT\",\n  \"graphics_cleanup\": \"%s\", \"end_reason\": \"%s\"\n}\n",connects,video_rx,decoded_count,present_count,video_epoch,ping_requests,ping_responses,audio_packets,(unsigned long long)audio_bytes,graphics_blocked?"UNCONFIRMED":"OWN_REMOVED",reason);fclose(f);
 snprintf(path,sizeof(path),"%s/usb-transport.json",out_dir);f=fopen(path,"w");if(!f)return;
 fprintf(f,"{\"source\":\"%s\",\"last_stage\":\"%s\",\"last_reason\":\"%s\",\"last_detail\":\"%s\",\"rc\":%d,\"errno\":%d,\"port\":\"%s\",\"bus\":%u,\"devnum\":%u,\"in\":%u,\"out\":%u,\"read_poll_timeouts\":%u,\"physical_acceptance\":false}\n","physical-usb",usb_last_stage,usb_last_reason,usb_last_detail,usb_last_rc,usb_last_errno,usb_port,usb_bus,usb_dev,usb_in,usb_out,usb_read_timeouts);fclose(f);
}
int main(int argc,char **argv){
 const char *port=NULL;unsigned total_seconds=1200,max_attempts=2;int preflight=0;
 /* This executable belongs to a window producer, never to the HMI process.
  * Parent death releases USB even while waiting for phone enumeration. */
 pid_t owner=getppid();
 if(owner<=1||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=owner)return 65;
 for(int i=1;i<argc;i++){
  if(!strcmp(argv[i],"--out")&&i+1<argc)out_dir=argv[++i];
  else if(!strcmp(argv[i],"--port")&&i+1<argc)port=argv[++i];
  else if(!strcmp(argv[i],"--usb-takeover-usbfs"))usb_takeover_enabled=1;
  else if(!strcmp(argv[i],"--single-attempt"))max_attempts=1;
  else if(!strcmp(argv[i],"--seconds")&&i+1<argc){
   const char *value=argv[++i];char *end=NULL;errno=0;unsigned long seconds=strtoul(value,&end,10);
   if(!*value||strspn(value,"0123456789")!=strlen(value)||errno||!end||*end||seconds<1||seconds>1200)return 64;
   total_seconds=(unsigned)seconds;
  }
  else if(!strcmp(argv[i],"--preflight"))preflight=1;
  else return 64;
 }
 if(!usb_phone_config()){puts("USB_CONFIG_ERROR expected_auto_or_port_vid_pid=1");return 64;}
 if(*usb_configured_port){if(port&&strcmp(port,usb_configured_port))return 64;port=usb_configured_port;}
 setbuf(stdout,NULL);signal(SIGINT,stop);signal(SIGTERM,stop);signal(SIGHUP,stop);signal(SIGPIPE,SIG_IGN);mkdir(out_dir,0700);
 printf("AA_HU_BEGIN abi=ARMv7-hardfloat glibc=%s AA_TLS_decode_on_HU=1 transport=%s\n",gnu_get_libc_version(),transport_name());
 printf("AUDIO_CHECK route=unverified pcm_not_opened=1 output=disabled\nIDRIVE_CHECK native_backend=unverified no_blind_input_grab=1\nMIC_CHECK capture=disabled\n");
 if(preflight){printf("PREFLIGHT_LOADER_OK avcodec=%u openssl=%s\n",avcodec_version(),SSLeay_version(SSLEAY_VERSION));return 0;}
 graphics_open();
 require(graphics_ok,"own-window-inherited-frame-buffer");
 packet=av_packet_alloc();picture=av_frame_alloc();require(packet&&picture,"decoder-packet-frame");
 SSL_library_init();SSL_load_error_strings();aa_ctx=SSL_CTX_new(TLSv1_2_client_method());require(aa_ctx!=NULL,"TLS-context");
 require(SSL_CTX_use_certificate_file(aa_ctx,identity_cert,SSL_FILETYPE_PEM)==1&&SSL_CTX_use_PrivateKey_file(aa_ctx,identity_key,SSL_FILETYPE_PEM)==1&&SSL_CTX_check_private_key(aa_ctx)==1,"identity");
 require(SSL_CTX_set_cipher_list(aa_ctx,"ECDHE-RSA-AES128-GCM-SHA256:AES128-GCM-SHA256")==1,"cipher-list");
 aa_ram=local_control;aa_ram[13]=1;aa_ram[10]=1;
 unsigned connects=0;const char *end_reason="complete";char end_detail[256]={0};uint64_t end=now()+(uint64_t)total_seconds*1000000000ULL;
 for(unsigned attempt=0;attempt<max_attempts&&!cancelled&&now()<end;attempt++){
  printf("AA_ATTEMPT index=%u max=%u\n",attempt+1,max_attempts);
  if(attempt){
   puts("ACTION_RECONNECT: Unplug/replug ONLY the phone HU USB cable once NOW; waiting up to 60 seconds for fresh enumeration.");}
  uint64_t wait_end=now()+(attempt?60000000000ULL:20000000000ULL);int connected=0;
  unsigned previous_dev=usb_dev;int saw_disconnect=0;
  do{if(attempt){
    unsigned current=read_number(usb_port,"devnum",10);if(!current)saw_disconnect=1;
    if(current&&(saw_disconnect||current!=previous_dev))connected=usb_connect(usb_port);
   }else connected=usb_connect(port);
   /* Give the owner time to insert a cable. Do not retry busy/ambiguous
    * interfaces or repeatedly send AOA requests after negotiation failure. */
   if(connected)break;
   if(!attempt&&(strcmp(usb_last_stage,"device-selection")||usb_last_errno!=ENODEV))break;
   usleep(500000);
  }while(!cancelled&&now()<wait_end&&now()<end);
  if(!connected){
   if(attempt&&!cancelled)usb_failure("reconnect",-1,ETIMEDOUT,"fresh-enumeration-deadline");
   end_reason=usb_last_reason;break;
  }connects++;end_reason="complete";
  aa_gen=attempt+1;aa_ram[8]=aa_gen;aa_state=1;aa_reason[0]=0;aa_open=aa_setup=aa_started=aa_bound=aa_wire_used=aa_used=aa_config_size=aa_video_count=0;
  aa_focus_request=1;aa_focus_sent=0;aa_video_write=aa_video_read=aa_video_eos=aa_video_pending_acks=0;
  ping_pending=0;ping_next=now()+5000000000ULL;uint64_t started=now(),last_rx=started;first_frame=0;hide_video=0;graphics_hide(0);
  const uint8_t version[]={0,1,0,1};int ok=aa_send(0,1,version,sizeof(version),0,0);
  while(ok&&!cancelled&&now()<end){
   uint8_t raw[16384];int n=native_read(raw,sizeof(raw));if(n<0){
    aa_error("bulk-in-or-disconnect");
    break;
   }
   if(n){if(aa_wire_used+(unsigned)n>sizeof(aa_wire)){aa_error("wire-bound");break;}memcpy(aa_wire+aa_wire_used,raw,n);aa_wire_used+=(unsigned)n;last_rx=now();ok=aa_consume_wire();}
   if(ok)ok=aa_controls()&&keepalive();
   uint64_t t=now();if(aa_state<3&&t-started>8000000000ULL){aa_error("version-or-TLS-deadline");break;}
   if(aa_state==3&&t-started>20000000000ULL){aa_error("services-deadline");break;}
   if((aa_used||aa_wire_used)&&t-last_rx>2000000000ULL){aa_error("partial-message-deadline");break;}
   if(!first_frame&&t-started>35000000000ULL){aa_error("video-start-deadline");break;}
   graphics_pump();
  }
  if(!cancelled&&!aa_reason[0]&&aa_state==4){aa_focus_request=(aa_focus_request|1u)+1u;aa_controls();}
  if(aa_reason[0]){printf("AA_SESSION_END generation=%u reason=%s\n",aa_gen,aa_reason);snprintf(end_detail,sizeof(end_detail),"%s",aa_reason);end_reason=end_detail;}else puts("AA_SESSION_END reason=session-complete");
  graphics_hide(1);video_end();if(aa_ssl)SSL_free(aa_ssl);aa_ssl=NULL;aa_state=0;
  usb_close();
 }
 if(cancelled)end_reason="clean-cancel";
 graphics_close();video_end();av_packet_free(&packet);av_frame_free(&picture);SSL_CTX_free(aa_ctx);
 summary(connects,end_reason);
 printf("AA_HU_END transport=%s transport_closed=1 TLS_closed=1 decoder_closed=1 threads=0\n",transport_name());
 return cancelled?0:(!strcmp(end_reason,"complete")&&tls_passes&&decoded_count&&present_count?0:1);
}
