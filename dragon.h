#ifndef DRAGON_DEFINITIONS_HEADER
#define DRAGON_DEFINITIONS_HEADER


// not changeable - hardcoded in FPGA
#define DRAGON_PACKET_SIZE_DWORDS  32
#define DRAGON_PACKET_SIZE_BYTES  (DRAGON_PACKET_SIZE_DWORDS*4)

#define DRAGON_DATA_PER_PACKET 120
#define DRAGON_MIN_FRAME_LENGTH DRAGON_DATA_PER_PACKET
#define DRAGON_MAX_FRAME_LENGTH 65520
#define DRAGON_MAX_FRAMES_PER_BUFFER 32768
#define DRAGON_MAX_DATA_IN_BUFFER (32760*120)


// maximum buffers count in FIFO
#define DRAGON_MAX_BUFFER_COUNT 512


typedef struct dragon_params
{
    uint32_t frame_length;  // in ticks, 120 to 65520,
                            // must be multiple of 120 or will be rounded up
    uint32_t frames_per_buffer; // count of frames in one buffer, 1 to 32768,
                                // (frame_length*frames_per_buffer) must be less or equal 65520*60
    uint32_t switch_period;  // frames, 1 to 2^24, must be multiple of frames_per_buffer
                             // or will be rounded up
    uint32_t switch_auto;    // 1 - auto (switch_period), 0 - manual
    uint32_t switch_state;   // can be set 0 or 1 if switch_auto==0
    uint32_t half_shift;     // 0 or 1 - shifts sync pulse 1/2tick forward
    uint32_t channel_auto;   // 1: auto, 0: manual channel selection
    uint32_t channel;        // select adc - 0 or 1; works only if channel_auto = 0
    uint32_t sync_offset;    // ticks, 0 to 511
    uint32_t sync_width;     // ticks, 0 to 127
    uint32_t dac_data;       // four bytes for 4 adjustment DACs on dragon board
    uint32_t adc_type;       // 0 for 8-bit, 1 for 12-bit
    uint32_t board_type;         // 0 for red KNJN, 1 for new green
} dragon_params;

typedef struct dragon_buffer
{
    size_t idx;
    void*  ptr;
    size_t len;
    off_t  offset;
} dragon_buffer;


#define DRAGON_SET_ACTIVITY         _IOW( 'D', 0, int)
#define DRAGON_SET_DAC              _IOW( 'D', 1, int)
#define DRAGON_QUERY_PARAMS         _IOWR('D', 2, dragon_params*)
#define DRAGON_SET_PARAMS           _IOWR('D', 3, dragon_params*)
#define DRAGON_REQUEST_BUFFERS      _IOWR('D', 4, size_t*)
#define DRAGON_RELEASE_BUFFERS      _IOWR('D', 5, void*)
#define DRAGON_QUERY_BUFFER         _IOWR('D', 6, dragon_buffer*)
#define DRAGON_QBUF                 _IOWR('D', 7, dragon_buffer*)
#define DRAGON_DQBUF                _IOWR('D', 8, dragon_buffer*)
#define DRAGON_GET_ID               _IOWR('D', 9, uint32_t*)

#endif //DRAGON_DEFINITIONS_HEADER
