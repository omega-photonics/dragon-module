// not changeable - hardcoded in FPGA
#define DRAGON_PACKET_SIZE_DWORDS  32
#define DRAGON_PACKET_SIZE_BYTES  (DRAGON_PACKET_SIZE_DWORDS*4)
#define DRAGON_BUFFER_SIZE	(4096 * 1024)
#define DRAGON_PACKET_COUNT (DRAGON_BUFFER_SIZE/DRAGON_PACKET_SIZE_BYTES)

// can be changed: up to 512
#define DRAGON_BUFFER_COUNT 10


enum
{
    DRAGON_START,
    DRAGON_QUEUE_BUFFER,
    DRAGON_SET_DAC,
    DRAGON_SET_REG1,
    DRAGON_SET_REG2,
    DRAGON_REQUEST_BUFFER_NUMBER
};

