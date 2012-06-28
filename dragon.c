#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/uaccess.h>
#include "dragon.h"

MODULE_LICENSE("Dual BSD/GPL");

#define PCI_VENDOR_ID_XILINX      0x10ee
#define PCI_DEVICE_ID_XILINX_PCIE 0x0007

#define DRAGON_REGISTER_SIZE      (4*8)    // There are eight registers, and each is 4 bytes wide.
#define HAVE_REGION               0x01     // I/O Memory region
#define HAVE_IRQ                  0x02     // Interupt
#define HAVE_KREG                 0x04     // Kernel registration

int             gDrvrMajor = 241;           // Major number not dynamic.
unsigned int    gStatFlags = 0x00;          // Status flags used for cleanup.
unsigned long   gBaseHdwr;                  // Base register address (Hardware address)
unsigned long   gBaseLen;                   // Base register address Length
void           *gBaseVirt = NULL;           // Base register address (Virtual address, for I/O).
char            gDrvrName[]= "dragon";      // Name of driver in proc.
struct pci_dev *gDev = NULL;                // PCI device structure.
int             gIrq;                       // IRQ assigned by PCI system.

char           *gWriteBuffers[DRAGON_BUFFER_COUNT];            // Pointers to dword aligned DMA buffer.
dma_addr_t      gWriteHWAddr[DRAGON_BUFFER_COUNT];             // Pointers to DMA buffers hardware addresses

int BufferCount=0;
int RequestedBufferNumber=0;

uint16_t FrameLength=DRAGON_DEFAULT_FRAME_LENGTH;
uint16_t FramesPerBuffer=DRAGON_DEFAULT_FRAMES_PER_BUFFER;
bool HalfShiftEnabled=DRAGON_DEFAULT_HALF_SHIFT;
bool AutoChannel=DRAGON_DEFAULT_CHANNEL_AUTO;
bool ActiveChannel=DRAGON_DEFAULT_CHANNEL;
uint16_t SyncOffset=DRAGON_DEFAULT_SYNC_OFFSET;
uint16_t SyncWidth=DRAGON_DEFAULT_SYNC_WIDTH;

void XPCIe_WriteReg (u32 dw_offset, u32 val)
{
    writel(val, (gBaseVirt + (4 * dw_offset)));
}

static irqreturn_t XPCIe_IRQHandler(int irq, void *data)
{
    printk(KERN_INFO "%s IRQ: %d", gDrvrName, irq);
    return IRQ_HANDLED;
}

static int XPCIe_MMap(struct file *filp, struct vm_area_struct *vma)
{
    printk(KERN_INFO"%s: mmap buffer %d\n", gDrvrName, RequestedBufferNumber);
    if (remap_pfn_range(vma, vma->vm_start, gWriteHWAddr[RequestedBufferNumber] >> PAGE_SHIFT, vma->vm_end - vma->vm_start, vma->vm_page_prot))
        return -EAGAIN;
    return 0;
}

long XPCIe_Ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    switch (cmd)
    {
    case DRAGON_START:
        if(arg>0)
        {
            XPCIe_WriteReg(0, 0); // activate
            XPCIe_WriteReg(1, 1); // re-enable
        }
        else
        {
            XPCIe_WriteReg(1, 0); // disable
            XPCIe_WriteReg(0, 1); // reset
        }
        break;
    case DRAGON_QUEUE_BUFFER:
        if(arg<DRAGON_BUFFER_COUNT) XPCIe_WriteReg(2, gWriteHWAddr[arg]);
        else ret=-1;
        break;
    case DRAGON_SET_DAC:
        XPCIe_WriteReg(3, arg);
        break;
    case DRAGON_SET_FRAME_LENGTH:
        if(arg>=90 && arg<=49140)
        {
            if(arg%90!=0) arg=(arg+90)%90; // round up
            FrameLength=arg;
            XPCIe_WriteReg(7, (FrameLength/6)-1);
            return arg;
        }
        else ret=-1;
        break;
    case DRAGON_SET_FRAME_PER_BUFFER_COUNT:
        if(arg>=1 && arg*FrameLength<=32768*90)
        {
            FramesPerBuffer=arg;
            XPCIe_WriteReg(6, (arg*FrameLength/90));
        }
        else ret=-1;
        break;
    case DRAGON_SET_SWITCH_PERIOD:
        XPCIe_WriteReg(5, arg);
        break;
    case DRAGON_SET_HALF_SHIFT:
        HalfShiftEnabled=arg&1;
        XPCIe_WriteReg(4, SyncWidth|(ActiveChannel<<7)|(AutoChannel<<8)|(HalfShiftEnabled<<9)|(SyncOffset<<10));
        break;
    case DRAGON_SET_CHANNEL_AUTO:
        AutoChannel=arg&1;
        XPCIe_WriteReg(4, SyncWidth|(ActiveChannel<<7)|(AutoChannel<<8)|(HalfShiftEnabled<<9)|(SyncOffset<<10));
        break;
    case DRAGON_SET_CHANNEL:
        ActiveChannel=arg&1;
        XPCIe_WriteReg(4, SyncWidth|(ActiveChannel<<7)|(AutoChannel<<8)|(HalfShiftEnabled<<9)|(SyncOffset<<10));
        break;
    case DRAGON_SET_SYNC_WIDTH:
        SyncWidth=arg&127;
        XPCIe_WriteReg(4, SyncWidth|(ActiveChannel<<7)|(AutoChannel<<8)|(HalfShiftEnabled<<9)|(SyncOffset<<10));
        break;
    case DRAGON_SET_SYNC_OFFSET:
        SyncOffset=arg&511;
        XPCIe_WriteReg(4, SyncWidth|(ActiveChannel<<7)|(AutoChannel<<8)|(HalfShiftEnabled<<9)|(SyncOffset<<10));
        break;
    case DRAGON_REQUEST_BUFFER_NUMBER:
        if(arg<DRAGON_BUFFER_COUNT) RequestedBufferNumber=arg;
        else ret=-1;
        break;
    default:
        break;
    }

    return ret;
}


struct file_operations XPCIe_Intf =
{
    unlocked_ioctl:      XPCIe_Ioctl,
    mmap:       XPCIe_MMap
};


static int XPCIe_init(void)
{
    gDev = pci_get_device (PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_XILINX_PCIE, gDev);
    if (NULL == gDev)
    {
        printk(KERN_WARNING"%s: Init: Hardware not found.\n", gDrvrName);
        return -1;
    }

    pci_enable_msi_block(gDev, 1);
    if (0 > pci_enable_device(gDev))
    {
        printk(KERN_WARNING"%s: Device not enabled.\n", gDrvrName);
        return -1;
    }
    else printk(KERN_WARNING"%s: Device enabled.\n", gDrvrName);
    pci_set_master(gDev);
    printk(KERN_WARNING"%s: MSI enabled.\n", gDrvrName);
    if(0!=request_irq(gDev->irq, XPCIe_IRQHandler, 0, gDrvrName, NULL))
    {
        printk(KERN_WARNING"%s: Can not request IRQ.\n", gDrvrName);
        return -1;
    }
    printk(KERN_WARNING"%s: IRQ requested.\n", gDrvrName);
    gStatFlags = gStatFlags | HAVE_IRQ;
    printk(KERN_WARNING"%s: Set bus master.\n", gDrvrName);
    gIrq = gDev->irq;


    gBaseHdwr = pci_resource_start (gDev, 0);
    if (0 > gBaseHdwr)
    {
        printk(KERN_WARNING"%s: Base Address not set.\n", gDrvrName);
        return -1;
    }
    gBaseLen = pci_resource_len (gDev, 0);
    gBaseVirt = ioremap(gBaseHdwr, gBaseLen);
    if (!gBaseVirt)
    {
        printk(KERN_WARNING"%s: Could not remap memory.\n", gDrvrName);
        return -1;
    }
    if (0 > check_mem_region(gBaseHdwr, DRAGON_REGISTER_SIZE))
    {
        printk(KERN_WARNING"%s: Error: Memory region is in use!\n", gDrvrName);
        return -1;
    }
    request_mem_region(gBaseHdwr, DRAGON_REGISTER_SIZE, "Dragon_Drv");
    gStatFlags = gStatFlags | HAVE_REGION;
    printk(KERN_INFO"%s: Init done\n",gDrvrName);
    for(BufferCount=0; BufferCount<DRAGON_BUFFER_COUNT; BufferCount++)
    {
        gWriteBuffers[BufferCount] = pci_alloc_consistent(gDev, DRAGON_BUFFER_SIZE, &gWriteHWAddr[BufferCount]);
        if (NULL == gWriteBuffers[BufferCount])
        {
            printk(KERN_CRIT"%s: Unable to allocate gBuffer %d.\n",gDrvrName, BufferCount);
            return -1;
        }
    }
    if (0 > register_chrdev(gDrvrMajor, gDrvrName, &XPCIe_Intf))
    {
        printk(KERN_WARNING"%s: Can not register module\n", gDrvrName);
        return -1;
    }
    printk(KERN_INFO"%s: Module registered\n", gDrvrName);
    gStatFlags = gStatFlags | HAVE_KREG;
    printk("%s driver loaded\n", gDrvrName);

    XPCIe_WriteReg(7, (FrameLength/6)-1);       // set frame length
    XPCIe_WriteReg(6, (FramesPerBuffer*FrameLength/90)); //set frames per buffer
    XPCIe_WriteReg(4, SyncWidth|(ActiveChannel<<7)|(AutoChannel<<8)|(HalfShiftEnabled<<9)|(SyncOffset<<10)); //misc settings
    XPCIe_WriteReg(1, 0); // disable
    XPCIe_WriteReg(0, 1); // reset
    XPCIe_WriteReg(0, 0); // activate
    XPCIe_WriteReg(1, 1); // re-enable
    XPCIe_WriteReg(2, gWriteHWAddr[0]);      //write 1st buffer

    return 0;
}


static void XPCIe_exit(void)
{
    if (gStatFlags & HAVE_REGION)
        (void) release_mem_region(gBaseHdwr, DRAGON_REGISTER_SIZE);
    if (gStatFlags & HAVE_IRQ)
        (void) free_irq(gIrq, gDev);
    for(BufferCount--; BufferCount>=0; BufferCount--)
    {
        pci_free_consistent(gDev, DRAGON_BUFFER_SIZE, gWriteBuffers[BufferCount], gWriteHWAddr[BufferCount]);
//        if (NULL != gWriteBuffers[BufferCount])
//            (void) kfree(gWriteBuffers[BufferCount]);
        gWriteBuffers[BufferCount] = NULL;
    }
    if (gBaseVirt != NULL) iounmap(gBaseVirt);
    gBaseVirt = NULL;
    if (gStatFlags & HAVE_KREG) unregister_chrdev(gDrvrMajor, gDrvrName);
    gStatFlags = 0;
    printk(KERN_ALERT"%s driver is unloaded\n", gDrvrName);
}

module_init(XPCIe_init);
module_exit(XPCIe_exit);
