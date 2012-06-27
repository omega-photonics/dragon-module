// not changeable - hardcoded in FPGA
#define DRAGON_PACKET_SIZE_DWORDS  32
#define DRAGON_PACKET_SIZE_BYTES  (DRAGON_PACKET_SIZE_DWORDS*4)
#define DRAGON_BUFFER_SIZE	(4096 * 1024)
#define DRAGON_PACKET_COUNT (DRAGON_BUFFER_SIZE/DRAGON_PACKET_SIZE_BYTES)

// can be changed: up to 512
#define DRAGON_BUFFER_COUNT 10


#define DRAGON_DEFAULT_FRAME_LENGTH 49140
#define DRAGON_DEFAULT_FRAMES_PER_BUFFER 60
#define DRAGON_DEFAULT_HALF_SHIFT 0
#define DRAGON_DEFAULT_CHANNEL_AUTO 0
#define DRAGON_DEFAULT_CHANNEL 0
#define DRAGON_DEFAULT_SYNC_OFFSET 0
#define DRAGON_DEFAULT_SYNC_WIDTH 50

enum
{
    DRAGON_START,
    DRAGON_QUEUE_BUFFER,
    DRAGON_SET_DAC,
    DRAGON_REQUEST_BUFFER_NUMBER,
    DRAGON_SET_FRAME_LENGTH,            //in ticks, 90 to 49140, must be multiple of 90 or will be rounded up
    DRAGON_SET_FRAME_PER_BUFFER_COUNT,  //count of frames in one buffer, 1 to 32768, (FRAME_LENGTH*FRAME_COUNT) must be less or equal 32768*90
    DRAGON_SET_HALF_SHIFT,     //0 or 1 - shifts sync pulse 1/2tick forward
    DRAGON_SET_CHANNEL_AUTO,   //1: auto, 0: using next parameter
    DRAGON_SET_CHANNEL,        //select adc - 0 or 1; works only if AUTO=0
    DRAGON_SET_SYNC_OFFSET,    //ticks, 0 to 511
    DRAGON_SET_SYNC_WIDTH,     //ticks, 0 to 127
    DRAGON_SET_SWITCH_PERIOD   //frames, 1 to 2^24, must be multiple of FRAME_PER_BUFFER_COUNT or will be rounded up
};

