/* AA protocol subset based on f1x/aasdk 9ee5283 framing/channel source contracts.
 * Copyright (C) 2018 f1x.studio (Michal Szwaj), GPL-3.0-or-later.
 * Bounded framing, channel state, TLS and media message handling. */
#ifndef AA_PROTOCOL_H
#define AA_PROTOCOL_H
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include "aa-phone-version.h"
#define AA_MAX_MESSAGE 262144u
#define AA_PCM_SLOTS 128u
static volatile uint32_t *aa_ram;
static volatile unsigned aa_run=1,aa_reset_request,aa_reset_ack,aa_requested_disconnect;
static unsigned aa_gen,aa_state,aa_open,aa_setup,aa_started,aa_bound;
static unsigned aa_av_session[5],aa_fragment_channel,aa_fragment_flags,aa_total,aa_used;
static unsigned aa_video_count,aa_pcm_peak,aa_wire_peak,aa_str_peak;
static uint64_t aa_str_waits;
static uint64_t aa_video_offset,aa_rx_time,aa_partial_time,aa_pcm_next;
static SSL_CTX *aa_ctx;
static SSL *aa_ssl;
static BIO *aa_rbio,*aa_wbio;
static pthread_t aa_thread;
static pthread_mutex_t aa_audio_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t aa_pcm_outputs;
static void aa_pcm_output_tick(void);
static int aa_controls(void);
#define AA_VIDEO_QUEUE 20u
#define AA_VIDEO_HIGH 16u
struct AaVideo {unsigned generation,id,size;uint64_t pts;uint8_t data[STREAM_MAX_AU];};
static struct AaVideo aa_videos[AA_VIDEO_QUEUE];
static volatile unsigned aa_video_write,aa_video_read,aa_video_eos;
static unsigned aa_video_cursor,aa_video_meta,aa_video_pending_acks,aa_video_peak;
static uint8_t aa_wire[70000],aa_message[AA_MAX_MESSAGE],aa_config[65536];
static unsigned aa_wire_used,aa_config_size;
static char aa_reason[64];
struct AaPcm {unsigned generation;uint64_t first;int16_t samples[PCM_FRAMES*2];};
static struct AaPcm aa_pcm[AA_PCM_SLOTS];
static volatile unsigned aa_pcm_write,aa_pcm_read;
static volatile unsigned aa_focus_request,aa_focus_sent;
static unsigned aa_live(void){return aa_run&&aa_ram[10]&&aa_ram[8]==aa_gen&&aa_requested_disconnect!=aa_gen;}
static int aa_error(const char *reason){snprintf(aa_reason,sizeof(aa_reason),"%s",reason);return 0;}
static void aa_digest(const uint8_t *p,size_t n,char out[65]){unsigned char hash[32];SHA256(p,n,hash);for(int i=0;i<32;i++)snprintf(out+2*i,3,"%02x",hash[i]);}
static int aa_raw_send(const uint8_t *p,unsigned n){return native_write(p,n)?1:aa_error("bulk-out");}
static int aa_frame(unsigned channel,unsigned flags,const uint8_t *data,unsigned n){
 if(!n||n>65535)return aa_error("tx-frame-length");
 uint8_t header[4]={(uint8_t)channel,(uint8_t)flags,(uint8_t)(n>>8),(uint8_t)n};
 return aa_raw_send(header,4)&&aa_raw_send(data,n);
}
static int aa_send(unsigned channel,unsigned id,const uint8_t *body,unsigned n,int encrypted,int control){
 uint8_t plain[8192],cipher[12288];if(n+2>sizeof(plain))return aa_error("tx-message-bound");
 plain[0]=id>>8;plain[1]=id;if(n)memcpy(plain+2,body,n);
 if(!encrypted)return aa_frame(channel,3u|(control?4u:0u),plain,n+2);
 if(SSL_write(aa_ssl,plain,(int)n+2)!=(int)n+2)return aa_error("tls-write");
 int length=BIO_read(aa_wbio,cipher,sizeof(cipher));
 if(length<=0||BIO_ctrl_pending(aa_wbio))return aa_error("tls-write-bound");
 return aa_frame(channel,11u|(control?4u:0u),cipher,(unsigned)length);
}
struct AaPb {uint8_t b[2048];unsigned n;};
static void aa_var(struct AaPb *p,uint64_t v){do{require(p->n<sizeof(p->b),"AA protobuf output bound");unsigned x=v&127;v>>=7;p->b[p->n++]=(uint8_t)(x|(v?128:0));}while(v);}
static void aa_v(struct AaPb *p,unsigned field,uint64_t v){aa_var(p,(uint64_t)field*8);aa_var(p,v);}
static void aa_b(struct AaPb *p,unsigned field,const uint8_t *b,unsigned n){aa_var(p,(uint64_t)field*8+2);aa_var(p,n);require(p->n+n<=sizeof(p->b),"AA protobuf bytes bound");memcpy(p->b+p->n,b,n);p->n+=n;}
static void aa_sub(struct AaPb *p,unsigned field,struct AaPb *s){aa_b(p,field,s->b,s->n);}
#include "aa-input.h"
static int aa_getvar(const uint8_t *p,unsigned n,unsigned *i,uint64_t *v){
 *v=0;for(unsigned shift=0;shift<70;shift+=7){if(*i>=n)return 0;unsigned b=p[(*i)++];if(shift==63&&b>1)return 0;*v|=(uint64_t)(b&127)<<shift;if(!(b&128))return 1;}return 0;
}
static int aa_fields(const uint8_t *p,unsigned n,uint64_t values[16]){
 unsigned i=0;for(int j=0;j<16;j++)values[j]=0;
 while(i<n){uint64_t tag,v;if(!aa_getvar(p,n,&i,&tag)||!(tag>>3))return 0;
  if((tag&7)==0){if(!aa_getvar(p,n,&i,&v))return 0;if((tag>>3)<16)values[tag>>3]=v;}
  else if((tag&7)==2){if(!aa_getvar(p,n,&i,&v)||v>n-i)return 0;i+=(unsigned)v;}
  else if((tag&7)==1){if(n-i<8)return 0;i+=8;}
  else if((tag&7)==5){if(n-i<4)return 0;i+=4;}else return 0;
 }
 return 1;
}
#include "aa-phone-media-setup.h"
static int aa_mic_available(void){return aa_ram[13]==1;}
static int aa_mic_pump(void){return 1;}
static int aa_mic_request(unsigned id,const uint64_t v[16]){
 if(id==0x8000){if(!aa_setup_request_valid(v[1],5,1))return aa_error("mic-config");aa_setup|=32;uint8_t b[]={8,2,16,4,26,1,0};return aa_send(5,0x8003,b,sizeof(b),1,0);}
 if(id==0x8005){struct AaPb reply={0};aa_v(&reply,1,0);aa_v(&reply,2,1);puts("MIC_SKIP reason=no_capture_backend_or_consent capture=0");return aa_send(5,0x8006,reply.b,reply.n,1,0);}
 return aa_error("mic-unavailable");
}
static int aa_sensor_start(uint64_t type){
 struct AaPb response={0},event={0},value={0};
 int supported=type==10||type==13;
 aa_v(&response,1,supported?0:1);
 if(!aa_send(2,0x8002,response.b,response.n,1,0))return 0;
 if(!supported){printf("AA_SENSOR_REJECT generation=%u type=%llu\n",aa_gen,(unsigned long long)type);return 1;}
 /* Static sensor policy, NOT telemetry or a parked-vehicle assertion. */
 aa_v(&value,1,type==13?31:0);aa_sub(&event,(unsigned)type,&value);
 printf("AA_SENSOR generation=%u type=%llu value=%u source=static-policy vehicle_data=0\n",aa_gen,(unsigned long long)type,type==13?31:0);
 return aa_send(2,0x8003,event.b,event.n,1,0);
}
static int aa_service(void){
 struct AaPb response={0},channel={0},av={0},config={0},input_pb={0},codes={0};
 /* Advertise the supported media and sensor channels. */
 if(aa_mic_available()){
  struct AaPb audio={0},source={0},descriptor={0};
  aa_v(&audio,1,16000);aa_v(&audio,2,16);aa_v(&audio,3,1);
  aa_v(&source,1,1);aa_sub(&source,2,&audio);
  aa_v(&descriptor,1,5);aa_sub(&descriptor,5,&source);aa_sub(&response,1,&descriptor);
  puts("AA_MIC_CAPABILITIES channel=5 rate=16000 bits=16 channels=1 backend=disabled-no-consent capture=off");
 }
 if(aa_ram[13]==1){
  struct AaPb sensors={0},sensor={0},descriptor={0};
  aa_v(&sensor,1,10);aa_sub(&sensors,1,&sensor);sensor.n=0;
  aa_v(&sensor,1,13);aa_sub(&sensors,1,&sensor);
  aa_v(&descriptor,1,2);aa_sub(&descriptor,2,&sensors);aa_sub(&response,1,&descriptor);
  printf("AA_SENSOR_CAPABILITIES channel=2 types=10,13 day=0 driving_status=31 backend=static-policy\n");
 }
 aa_v(&config,1,1);aa_v(&config,2,1);aa_v(&config,5,160); /* 480p,30fps */
 aa_v(&av,1,3);aa_sub(&av,4,&config);aa_v(&channel,1,3);aa_sub(&channel,3,&av);aa_sub(&response,1,&channel);
 channel.n=av.n=config.n=0;
 aa_v(&config,1,PCM_RATE);aa_v(&config,2,16);aa_v(&config,3,2);
 aa_v(&av,1,1);aa_v(&av,2,3);aa_sub(&av,3,&config);aa_v(&channel,1,4);aa_sub(&channel,3,&av);aa_sub(&response,1,&channel);
 channel.n=0;aa_input_advertise(&codes);aa_sub(&input_pb,1,&codes);
 aa_v(&channel,1,1);aa_sub(&channel,4,&input_pb);aa_sub(&response,1,&channel);
 const char *name="Wired Android Auto";aa_b(&response,2,(const uint8_t*)name,(unsigned)strlen(name));
 printf("AA_CAPABILITIES generation=%u video_channel=3 resolution_enum=1 fps_enum=1 width=800 height=480 audio_channel=4 rate=48000 bits=16 channels=2 input_channel=1 keys=%s\n",aa_gen,aa_input_keylist());
 return aa_send(0,6,response.b,response.n,1,0);
}
static int aa_str(unsigned kind,const uint8_t *p,unsigned n,uint64_t pts,unsigned id){
 (void)p;(void)n;(void)pts;(void)id;
 if(kind==STREAM_BEGIN){aa_video_count=aa_config_size=0;aa_video_offset=0;aa_video_eos=0;aa_video_pending_acks=0;return video_begin()?1:aa_error("video-decoder-open");}
 if(kind==STREAM_END){video_end();return 1;}return 0;
}
static int aa_video_data(const uint8_t *p,unsigned n,uint64_t us){
 if(!n||n+aa_config_size>STREAM_MAX_AU)return aa_error("video-size");
 int ok=video_packet(p,n,aa_config,aa_config_size,us);aa_config_size=0;aa_video_count++;return ok?1:aa_error("h264-decode-or-timestamp");
}
static int aa_audio_data(const uint8_t *p,unsigned n,uint64_t us){
 if(!n||n%4||n>65536)return aa_error("pcm-size");audio_packets++;audio_bytes+=n;
 (void)p;if(audio_packets<=3)printf("AUDIO_RX packet=%u bytes=%u source_us=%llu route=unverified output_opened=0\n",audio_packets,n,(unsigned long long)us);return 1;
}

static int aa_handshake(void){
 int result=SSL_do_handshake(aa_ssl);int error=SSL_get_error(aa_ssl,result);
 uint8_t buf[8192];int n;
 while((n=BIO_read(aa_wbio,buf,sizeof(buf)))>0)if(!aa_send(0,3,buf,(unsigned)n,0,0))return 0;
 if(result==1){
  if(aa_ram[13]==0&&SSL_get_verify_result(aa_ssl)!=X509_V_OK)return aa_error("tls-peer-verification");
  tls_passes++;aa_state=3;aa_ram[11]=3;uint8_t ok[2]={8,0};
  printf("AA_TLS generation=%u cipher=%s verified=%u version=%s\n",aa_gen,SSL_get_cipher(aa_ssl),aa_ram[13]==0,SSL_get_version(aa_ssl));
  return aa_send(0,4,ok,sizeof(ok),0,0);
 }
 if(error!=SSL_ERROR_WANT_READ&&error!=SSL_ERROR_WANT_WRITE){printf("AA_TLS_ERROR ssl=%d verify=%ld error=%lu\n",error,SSL_get_verify_result(aa_ssl),ERR_get_error());return aa_error("tls-handshake");}
 return 1;
}
static int aa_message_received(unsigned channel,unsigned flags,const uint8_t *p,unsigned n){
 if(n<2)return aa_error("message-too-short");unsigned id=((unsigned)p[0]<<8)|p[1];p+=2;n-=2;
 if(id>1)printf("AA_RX generation=%u channel=%u flags=%u id=%u bytes=%u\n",aa_gen,channel,flags,id,n);
 if(aa_state==1){
  if(channel||flags!=3||id!=2||n!=6)return aa_error("version-response-shape");
  printf("AA_VERSION request_major=1 request_minor=1 peer_major=%u peer_minor=%u status=%u phone_policy=%u\n",aa_version_word(p),aa_version_word(p+2),aa_version_word(p+4),aa_ram[13]);
  if(!aa_version_match(p,n,aa_ram[13]==1))return aa_error("version-mismatch");
  version_passes++;aa_ssl=SSL_new(aa_ctx);if(!aa_ssl)return aa_error("tls-new");
  if(aa_ram[13]==1){
   SSL_set_verify(aa_ssl,SSL_VERIFY_NONE,NULL);
   if(SSL_use_certificate_file(aa_ssl,identity_cert,SSL_FILETYPE_PEM)!=1||
      SSL_use_PrivateKey_file(aa_ssl,identity_key,SSL_FILETYPE_PEM)!=1||SSL_check_private_key(aa_ssl)!=1)return aa_error("phone-identity");
   puts("AA_SECURITY policy=phone peer_authenticated=0 identity=configured-files");
  }else if(aa_ram[13]!=0)return aa_error("unknown-tls-policy");aa_rbio=BIO_new(BIO_s_mem());aa_wbio=BIO_new(BIO_s_mem());
  SSL_set_bio(aa_ssl,aa_rbio,aa_wbio);SSL_set_connect_state(aa_ssl);aa_state=2;return aa_handshake();
 }
 if(aa_state==2){if(channel||flags!=3||id!=3||!n)return aa_error("handshake-shape");if(BIO_write(aa_rbio,p,(int)n)!=(int)n)return aa_error("handshake-bio");return aa_handshake();}
 if(aa_state<3)return aa_error("message-before-auth");
 if(!(flags&8)&&!(channel==0&&(id==11||id==12)))return aa_error("unencrypted-session-message");
 uint64_t v[16];
 if(channel==3&&(id==0||id==1)){
  if(!(aa_started&8))return aa_error("video-before-start");
  if(id==1){if(!n||n>sizeof(aa_config)||aa_video_count)return aa_error("codec-config");memcpy(aa_config,p,n);aa_config_size=n;return 1;}
  if(n<=8)return aa_error("video-timestamp-truncated");uint64_t us=0;for(int i=0;i<8;i++)us=(us<<8)|p[i];
  if(!aa_video_data(p+8,n-8,us))return 0;
 }else if(channel==4&&id==0){
  if(!(aa_started&16)||n<=8)return aa_error("audio-before-start-or-truncated");uint64_t us=0;for(int i=0;i<8;i++)us=(us<<8)|p[i];
  if(!aa_audio_data(p+8,n-8,us))return 0;
 }else{
  if(!aa_fields(p,n,v))return aa_error("protobuf-invalid");
  if(channel==0&&id==5){aa_ram[11]=4;printf("AA_APP_STATE state=negotiating generation=%u\n",aa_gen);if(aa_state!=3)return aa_error("duplicate-discovery");aa_state=4;service_passes++;return aa_service();}
  if(channel==0&&id==11)return aa_send(0,12,p,n,0,0);
  if(channel==0&&id==12){if(!ping_pending||v[1]!=ping_token)return aa_error("ping-correlation");ping_pending=0;ping_responses++;printf("KEEPALIVE_ACK count=%u token=%llu\n",ping_responses,(unsigned long long)v[1]);return 1;}
  if(channel==0&&id==15){if(!aa_controls()||!aa_send(0,16,NULL,0,1,0))return 0;return aa_error("shutdown");}
  if(channel==0&&id==18){uint8_t response[]={8,(uint8_t)(v[1]==4?3:1)};return aa_send(0,19,response,sizeof(response),1,0);}
  if(channel==0)return aa_error("unsupported-message");
  if(aa_state!=4)return aa_error("channel-before-discovery");
  if(id==7&&(channel==1||channel==3||channel==4||(channel==2&&aa_ram[13]==1)||(channel==5&&aa_mic_available()))){
   if(!(flags&4)||v[2]!=channel||(aa_open&(1u<<channel)))return aa_error("channel-open-invalid");
   aa_open|=1u<<channel;uint8_t ok[]={8,0};return aa_send(channel,8,ok,sizeof(ok),1,1);
  }
  if(!(aa_open&(1u<<channel)))return aa_error("unopened-channel");
  if(channel==5&&aa_ram[13]==1)return aa_mic_request(id,v);
  if(channel==2&&aa_ram[13]==1&&id==0x8001)return aa_sensor_start(v[1]);
  if(channel==1&&id==0x8002){aa_bound=1;uint8_t ok[]={8,0};return aa_send(1,0x8003,ok,sizeof(ok),1,0);}
  if((channel==3||channel==4)&&id==0x8000){
   printf("AA_SETUP_REQUEST channel=%u value=%llu phone_policy=%u\n",channel,(unsigned long long)v[1],aa_ram[13]);
   if(!aa_setup_request_valid(v[1],channel,aa_ram[13]==1)||aa_setup&(1u<<channel))return aa_error("unsupported-config");aa_setup|=1u<<channel;
   uint8_t setup[]={8,2,16,4,26,1,0}; /* OK, max_unacked=4, packed config 0 */
   return aa_send(channel,0x8003,setup,sizeof(setup),1,0);
  }
  if((channel==3||channel==4)&&id==0x8001){
   if(!(aa_setup&(1u<<channel))||v[2]||(aa_started&(1u<<channel)))return aa_error("av-start-invalid");
   aa_started|=1u<<channel;aa_av_session[channel]=(unsigned)v[1];
   if(channel==3&&!aa_str(STREAM_BEGIN,NULL,0,0,0))return 0;
   printf("AA_START generation=%u channel=%u session=%u\n",aa_gen,channel,aa_av_session[channel]);return 1;
  }
  if((channel==3||channel==4)&&id==0x8002){
   if(!(aa_started&(1u<<channel)))return aa_error("av-stop-invalid");aa_started&=~(1u<<channel);
   printf("AA_STOP generation=%u channel=%u\n",aa_gen,channel);
   if(channel==3){video_end();aa_video_eos=1;}return 1;
  }
  if(channel==3&&id==0x8007){uint8_t response[]={8,(uint8_t)(v[2]==2?2:1),16,0};return aa_send(3,0x8008,response,sizeof(response),1,0);}
  return aa_error("unsupported-message");
 }
 if(channel==3&&aa_video_write-aa_video_read>=AA_VIDEO_HIGH){aa_video_pending_acks++;return 1;}
 struct AaPb ack={0};aa_v(&ack,1,aa_av_session[channel]);aa_v(&ack,2,1);return aa_send(channel,0x8004,ack.b,ack.n,1,0);
}
static int aa_consume_wire(void){
 while(aa_wire_used>=4){
  unsigned ch=aa_wire[0],flags=aa_wire[1],type=flags&3,n=((unsigned)aa_wire[2]<<8)|aa_wire[3],header=type==1?8:4;
  if((flags&~15u)||!n||n>0x4100u||(ch!=0&&ch!=1&&ch!=3&&ch!=4&&!((ch==2||ch==5)&&aa_ram[13]==1)))return aa_error("frame-header-invalid");
  if(aa_wire_used<header+n)return 1;
  if(type==1){if(aa_used)return aa_error("nested-first");aa_total=((unsigned)aa_wire[4]<<24)|((unsigned)aa_wire[5]<<16)|((unsigned)aa_wire[6]<<8)|aa_wire[7];
   if(aa_total<2||aa_total>AA_MAX_MESSAGE)return aa_error("total-length-invalid");aa_fragment_channel=ch;aa_fragment_flags=flags&12;
  }else if(type!=3&&(!aa_used||ch!=aa_fragment_channel||(flags&12)!=aa_fragment_flags))return aa_error("orphan-or-intertwined-fragment");
  else if(type==3&&aa_used)return aa_error("bulk-inside-fragments");
  uint8_t plain[20000];const uint8_t *payload=aa_wire+header;int size=(int)n;
  if(flags&8){
   if(aa_state<3||BIO_write(aa_rbio,payload,(int)n)!=(int)n)return aa_error("tls-frame-before-auth");
   size=SSL_read(aa_ssl,plain,sizeof(plain));if(size<=0)return aa_error("tls-record-invalid");
   if(SSL_pending(aa_ssl))return aa_error("tls-plaintext-overflow");payload=plain;
  }
  if(aa_used+(unsigned)size>AA_MAX_MESSAGE)return aa_error("message-length-overflow");
  memcpy(aa_message+aa_used,payload,(unsigned)size);aa_used+=(unsigned)size;
  if(type!=3&&aa_used>aa_total)return aa_error("fragment-total-overflow");
  int ok=1;
  if(type==2||type==3){if(type==2&&aa_used!=aa_total)return aa_error("fragment-total-mismatch");ok=aa_message_received(ch,flags,aa_message,aa_used);aa_used=0;}
  memmove(aa_wire,aa_wire+header+n,aa_wire_used-header-n);aa_wire_used-=header+n;
  if(!ok)return 0;aa_partial_time=aa_wire_used||aa_used?now():0;
 }
 return 1;
}
static int aa_controls(void){
 if(aa_state!=4)return 1;
 if(!aa_mic_pump())return 0;
 while(aa_video_pending_acks&&aa_video_write-aa_video_read<AA_VIDEO_HIGH){
  struct AaPb ack={0};aa_v(&ack,1,aa_av_session[3]);aa_v(&ack,2,1);if(!aa_send(3,0x8004,ack.b,ack.n,1,0))return 0;aa_video_pending_acks--;
 }
 if(!aa_input_pump())return 0;
 if(aa_focus_request!=aa_focus_sent&&(aa_open&8)){
  unsigned wanted=aa_focus_request;uint8_t response[]={8,(uint8_t)((wanted&1)?1:2),16,1};
  if(!aa_send(3,0x8008,response,sizeof(response),1,0))return 0;aa_focus_sent=wanted;
 }
 return 1;
}

#endif
