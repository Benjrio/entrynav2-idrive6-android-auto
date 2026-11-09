/* Validate the codec enum advertised for the selected media channel.
 * The compatibility dialect uses config index zero. */
#ifndef AA_PHONE_MEDIA_SETUP_H
#define AA_PHONE_MEDIA_SETUP_H
#include <stdint.h>
static int aa_setup_request_valid(uint64_t value,unsigned channel,int phone){
 if(channel!=3&&channel!=4&&channel!=5)return 0;
 return value==(phone?(channel==3?3u:1u):0u);
}
#endif
