#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/cleancache.h>
#include <linux/pfn.h>
#include <asm/uaccess.h>
#include <asm/pgalloc.h>

#include "dragon.h"

MODULE_LICENSE("Dual BSD/GPL");

#define DRAGON_VID      0x10EE
#define DRAGON_DID      0x0007
#define DRAGON_MAXNUM_DEVS 256

#define DRAGON_DEFAULT_FRAME_LENGTH 65520
#define DRAGON_DEFAULT_FRAMES_PER_BUFFER 60
//#define DRAGON_DEFAULT_SWITCH_PERIOD (1 << 24)
//#define DRAGON_DEFAULT_SWITCH_AUTO 1
//#define DRAGON_DEFAULT_SWITCH_STATE 0
#define DRAGON_DEFAULT_PULSE_MASK 0x80000000
#define DRAGON_DEFAULT_HALF_SHIFT 0
#define DRAGON_DEFAULT_CHANNEL_AUTO 0
#define DRAGON_DEFAULT_CHANNEL 0
#define DRAGON_DEFAULT_SYNC_OFFSET 0
#define DRAGON_DEFAULT_SYNC_WIDTH 50
#define DRAGON_BUFFER_ORDER 10
#define DRAGON_DEFAULT_DAC_DATA 0xFFFFFFFF
#define DRAGON_DEFAULT_ADC_TYPE 0
#define DRAGON_DEFAULT_BOARD_TYPE 0

static const char DRV_NAME[] = "dragon";
static struct class *dragon_class;
static dev_t dragon_dev_number;
DEFINE_SPINLOCK(dev_number_lock);


static const struct pci_device_id dragon_ids[] = {
    { PCI_DEVICE(DRAGON_VID, DRAGON_DID) },
    { 0 },
};

MODULE_DEVICE_TABLE(pci, dragon_ids);

typedef struct dragon_buffer_opaque
{
    dragon_buffer buf;
    dma_addr_t dma_handle;
    struct list_head qlist;
    struct list_head dqlist;
    atomic_t owned_by_cpu;
} dragon_buffer_opaque;

typedef struct dragon_private
{
    struct pci_dev *pci_dev;
    struct cdev cdev;
    dev_t cdev_no;
    char dev_name[10];
    void __iomem *io_buffer;
    atomic_t dev_available;
    atomic_t queue_length;
    dragon_params params;
    dragon_buffer_opaque *buffers;
    size_t buf_count;
    struct list_head *qlist_head;
    struct list_head *dqlist_head;
    spinlock_t lists_lock;
    spinlock_t page_table_lock;
    wait_queue_head_t wait;
    int activity;
    spinlock_t activity_lock;
} dragon_private;


static void dragon_params_set_defaults(dragon_params* params)
{
    params->frame_length      = DRAGON_DEFAULT_FRAME_LENGTH;
    params->frames_per_buffer = DRAGON_DEFAULT_FRAMES_PER_BUFFER;
    //params->switch_period     = DRAGON_DEFAULT_SWITCH_PERIOD;
    //params->switch_auto       = DRAGON_DEFAULT_SWITCH_AUTO;
    //params->switch_state      = DRAGON_DEFAULT_SWITCH_STATE;
    params->pulse_mask        = DRAGON_DEFAULT_PULSE_MASK;
    params->half_shift        = DRAGON_DEFAULT_HALF_SHIFT;
    params->channel_auto      = DRAGON_DEFAULT_CHANNEL_AUTO;
    params->channel           = DRAGON_DEFAULT_CHANNEL;
    params->sync_offset       = DRAGON_DEFAULT_SYNC_OFFSET;
    params->sync_width        = DRAGON_DEFAULT_SYNC_WIDTH;
    params->dac_data          = DRAGON_DEFAULT_DAC_DATA;
    //params->adc_type          = DRAGON_DEFAULT_ADC_TYPE;
    //params->board_type        = DRAGON_DEFAULT_BOARD_TYPE;
}

static int dragon_check_params(dragon_params* params)
{
    if (!params)
        return -EINVAL;

    if (DRAGON_MIN_FRAME_LENGTH <= params->frame_length && params->frame_length <= DRAGON_MAX_FRAME_LENGTH)
    {
        params->frame_length =
            ((params->frame_length - 1)/DRAGON_DATA_PER_PACKET + 1)*DRAGON_DATA_PER_PACKET; //round up
    }
    else
    {
        printk(KERN_INFO "Bad dragon frame_length value\n");
        return -EINVAL;
    }

    if (!params->frames_per_buffer ||
        params->frames_per_buffer*params->frame_length > DRAGON_MAX_FRAMES_PER_BUFFER*DRAGON_DATA_PER_PACKET)
    {
        printk(KERN_INFO "Bad dragon frames_per_buffer value\n");
        return -EINVAL;
    }

    /*
    if (1 <= params->switch_period && params->switch_period <= (1 << 24))
    {
        if (params->switch_period < params->frames_per_buffer)
            params->switch_period = params->frames_per_buffer;

        params->switch_period =
            ( (params->switch_period - params->frames_per_buffer) / params->frames_per_buffer + 1)
                *params->frames_per_buffer; //round up
    }
    else
    {
        printk(KERN_INFO "Bad dragon switch_period value\n");
        return -EINVAL;
    }
    */

    params->half_shift &= 1;
    params->channel_auto &= 1;
    params->channel &= 1;
    //params->switch_auto &= 1;
    //params->switch_state &= 1;

    if (params->sync_width > 127)
    {
        printk(KERN_INFO "Bad dragon sync_width value\n");
        return -EINVAL;
    }


    if (params->sync_offset > 511)
    {
        printk(KERN_INFO "Bad dragon sync_offset value\n");
        return -EINVAL;
    }

    return 0;
}

static inline void dragon_write_reg32(dragon_private* private,
                                      uint32_t dw_offset, uint32_t val)
{
    iowrite32(val, private->io_buffer + ((dw_offset) << 2));
    mmiowb();
}

static inline uint32_t dragon_read_reg32(dragon_private* private,
                                      uint32_t dw_offset)
{
    return ioread32(private->io_buffer + ((dw_offset) << 2));
}

static long dragon_write_params(dragon_private* private,
                                dragon_params* params)
{
#ifdef VAL_CHANGED
#undef VAL_CHANGED
#endif
#define VAL_CHANGED(name)                                               \
    ((params) ? ( (params->name != private->params.name) ?              \
                  (private->params.name = params->name, 1) : 0   ) : 1)
#ifdef VAL
#undef VAL
#endif
#define VAL(name) private->params.name

    long err = 0;

    spin_lock(&private->activity_lock);
    if (private->activity)
    {
        printk(KERN_INFO "Couldn't set params while in active mode\n");
        err = -EAGAIN;
        goto unlock;
    }

    if (VAL_CHANGED(frame_length))
    {
        dragon_write_reg32(private, 7, VAL(frame_length)/8 - 1);
    }

    if (VAL_CHANGED(frames_per_buffer))
    {
        dragon_write_reg32(private, 6,
                           VAL(frames_per_buffer)*VAL(frame_length)/DRAGON_DATA_PER_PACKET - 1);
    }

    /*
    if (  VAL_CHANGED(switch_period)  |
          VAL_CHANGED(switch_auto)    |
          VAL_CHANGED(switch_state)   |
          VAL_CHANGED(adc_type)       |
          VAL_CHANGED(board_type))
    {
        dragon_write_reg32(private, 5,
                           (VAL(switch_period - 1))         |
                           (VAL(switch_auto) << 24)   |
                           (VAL(switch_state) << 25) |
                           (VAL(adc_type) << 28) |
                           (VAL(board_type) << 29)
                                ); //|(1<<26)); //testmode
    }
    */
    if (  VAL_CHANGED(pulse_mask) )
    {
        dragon_write_reg32(private, 5, VAL(pulse_mask));
    }

    if (  VAL_CHANGED(half_shift)   |
          VAL_CHANGED(channel_auto) |
          VAL_CHANGED(channel)      |
          VAL_CHANGED(sync_width)   |
          VAL_CHANGED(sync_offset)  )
    {
        dragon_write_reg32( private, 4,
                            (VAL(sync_width))        |
                            (VAL(channel) << 7)      |
                            (VAL(channel_auto) << 8) |
                            (VAL(half_shift) << 9)   |
                            (VAL(sync_offset) << 10) );
    }

    if (VAL_CHANGED(dac_data))
    {
        dragon_write_reg32(private, 3, VAL(dac_data));
    }

unlock:
    spin_unlock(&private->activity_lock);
    return err;


#undef  VAL
#undef  VAL_CHANGED
}

static long dragon_set_activity(dragon_private *private, int arg)
{
    if (arg)
    {
        dragon_write_reg32(private, 1, 1); // start DMA writing
        spin_lock(&private->activity_lock);
        private->activity = 1;
        spin_unlock(&private->activity_lock);
    }
    else
    {
        spin_lock(&private->activity_lock);
        private->activity = 0;
        spin_unlock(&private->activity_lock);

        //Wait for completeness
        while (atomic_read(&private->queue_length) > 0)
        {
            DEFINE_WAIT(wait_for_completeness);
            prepare_to_wait(&private->wait, &wait_for_completeness,
                            TASK_INTERRUPTIBLE);
            if (atomic_read(&private->queue_length) > 0)
                schedule();
            finish_wait(&private->wait, &wait_for_completeness);
        }

        dragon_write_reg32(private, 1, 0); // disable DMA writing
        dragon_write_reg32(private, 0, 1); // assert reset signal in FPGA: stop FIFOs, reset counters
        msleep(100);
        dragon_write_reg32(private, 0, 0); // deassert reset
    }

    return 0;
}

static void dragon_lock_pages(dragon_private* private,
                              void* va, size_t size)
{
    int i;
    struct page *pg = virt_to_page(va);

    spin_lock(&private->page_table_lock);
    for ( i = 0; i < PAGE_ALIGN(size) >> PAGE_SHIFT; ++i )
    {
        SetPageReserved(&pg[i]);
    }
    spin_unlock(&private->page_table_lock);
}

static void dragon_unlock_pages(dragon_private* private,
                                void* va, size_t size)
{
    int i;
    struct page *pg = virt_to_page(va);

    spin_lock(&private->page_table_lock);
    for ( i = 0; i < PAGE_ALIGN(size) >> PAGE_SHIFT; ++i )
    {
        ClearPageReserved(&pg[i]);
    }
    spin_unlock(&private->page_table_lock);

}

static long dragon_release_buffers(dragon_private* private)
{
    int i;
    long err = 0;

    spin_lock(&private->activity_lock);
    if (private->activity)
    {
        printk(KERN_INFO "Couldn't release buffers while in active mode\n");
        err = -EAGAIN;
        goto unlock;
    }

    if (private->buffers)
    {
        for (i = 0; i < private->buf_count; i++)
       {
            pci_unmap_single(private->pci_dev, private->buffers[i].dma_handle,
                             private->buffers[i].buf.len, PCI_DMA_FROMDEVICE);

            //Unlock memory pages
            dragon_unlock_pages(private,
                                private->buffers[i].buf.ptr,
                                private->buffers[i].buf.len);

            free_pages((unsigned long)private->buffers[i].buf.ptr,
                       DRAGON_BUFFER_ORDER);
        }

        vfree(private->buffers);

        private->buffers = 0;
        private->buf_count = 0;
    }

    private->qlist_head = 0;
    private->dqlist_head = 0;

unlock:
    spin_unlock(&private->activity_lock);
    return err;
}

static long dragon_request_buffers(dragon_private* private, size_t *count)
{
    size_t i, idx = 0;
    long err = 0;
    dragon_buffer_opaque *buffers;
    size_t buffer_size =
        (private->params.frame_length/DRAGON_DATA_PER_PACKET)*DRAGON_PACKET_SIZE_BYTES*
         private->params.frames_per_buffer;

    spin_lock(&private->activity_lock);
    if (private->activity)
    {
        printk(KERN_INFO "Couldn't request buffers while in active mode\n");
        err = -EAGAIN;
        goto unlock;
    }

    if (!buffer_size)
    {
        printk(KERN_INFO "Zero buffer size\n");
        err = -EINVAL;
        goto unlock;
    }

    if (*count > DRAGON_MAX_BUFFER_COUNT)
    {
        printk(KERN_INFO "Too much number of requested buffers\n");
        *count = 0;
        err = -EINVAL;
        goto unlock;
    }

    if(get_order(buffer_size) > DRAGON_BUFFER_ORDER)
    {
        printk(KERN_INFO "dragon buffer size is too big\n");
        err = -EINVAL;
        goto unlock;
    }


    if (private->buf_count >= *count)
    {
        // buffers already available
        *count = private->buf_count;
        goto unlock;
    }

    buffers = vmalloc_32(*count*sizeof(dragon_buffer_opaque));
    if (!buffers)
    {
        printk(KERN_INFO "dragon buffers array allocation failed\n");
        err = -ENOMEM;
        goto unlock;
    }
    memset(buffers, 0, *count*sizeof(dragon_buffer_opaque));

    if (private->buffers)
    {
        memcpy(buffers, private->buffers,
               private->buf_count*sizeof(dragon_buffer));
        vfree(private->buffers);
        private->buffers = 0;
        idx = private->buf_count;
    }

    for (i = idx; i < *count; i++)
    {
        if ( !(buffers[i].buf.ptr = (void*)
               __get_free_pages(GFP_DMA32, DRAGON_BUFFER_ORDER)) )
        {
            break;
        }
        buffers[i].buf.len = (1 << DRAGON_BUFFER_ORDER) << PAGE_SHIFT;

        if (!(buffers[i].dma_handle = pci_map_single(private->pci_dev,
                                                     buffers[i].buf.ptr,
                                                     buffers[i].buf.len,
                                                     PCI_DMA_FROMDEVICE)))
        {
            free_pages((unsigned long)buffers[i].buf.ptr, DRAGON_BUFFER_ORDER);
            break;
        }

        buffers[i].buf.offset = buffers[i].dma_handle;
        buffers[i].buf.idx = i;
        atomic_set(&buffers[i].owned_by_cpu, 0);

        INIT_LIST_HEAD(&buffers[i].qlist);
        INIT_LIST_HEAD(&buffers[i].dqlist);

        //Lock memory pages
        dragon_lock_pages(private,
                          buffers[i].buf.ptr,
                          buffers[i].buf.len);
    }

    if (!i)
    {
        vfree(buffers);
        printk(KERN_INFO "dragon couldn't allocate or map buffer\n");
        err = -ENOMEM;
        goto unlock;
    }

    private->buffers = buffers;
    private->buf_count = *count = i;

unlock:
    spin_unlock(&private->activity_lock);
    return err;
}

static long dragon_query_buffer(dragon_private *private, dragon_buffer *buffer)
{
    if (!buffer || buffer->idx >= private->buf_count)
    {
         return -EINVAL;
    }

    *buffer = private->buffers[buffer->idx].buf;

    return 0;
}

static long dragon_qbuf(dragon_private *private, dragon_buffer *buffer)
{
    unsigned long irq_flags;
    dragon_buffer_opaque *opaque;
    uint32_t addr_read;
    long err = 0;
    size_t buffer_size =
        (private->params.frame_length/DRAGON_DATA_PER_PACKET)*DRAGON_PACKET_SIZE_BYTES*
        private->params.frames_per_buffer;

    spin_lock(&private->activity_lock);
    if (!private->activity)
    {
        printk(KERN_INFO "Couldn't queue buffer while in non-active mode\n");
        err = -EAGAIN;
        goto unlock;
    }

    if (!buffer || buffer->idx >= private->buf_count)
    {
        err = -EINVAL;
        goto unlock;
    }

    opaque = &private->buffers[buffer->idx];

    if (opaque->buf.len < buffer_size)
    {
        printk(KERN_INFO "Couldn't queue small size buffer\n");
        err = -EAGAIN;
        goto unlock;
    }

    spin_lock_irqsave(&private->lists_lock, irq_flags);

    if (private->qlist_head)
    {
        list_add_tail(&opaque->qlist, private->qlist_head);
    }
    else
    {
        private->qlist_head = &opaque->qlist;
    }

    spin_unlock_irqrestore(&private->lists_lock, irq_flags);

    if (atomic_cmpxchg(&opaque->owned_by_cpu, 1, 0))
    {
        pci_dma_sync_single_for_device(private->pci_dev,
                                       opaque->dma_handle,
                                       opaque->buf.len,
                                       PCI_DMA_FROMDEVICE);
    }

    atomic_inc(&private->queue_length);
    dragon_write_reg32(private, 2, opaque->dma_handle);
    addr_read = dragon_read_reg32(private, 2);

unlock:
    spin_unlock(&private->activity_lock);
    return err;
}

static long dragon_dqbuf(dragon_private *private, dragon_buffer *buffer)
{
    unsigned long irq_flags;
    struct list_head *dqlist_next = 0;
    dragon_buffer_opaque *opaque = 0;
    long err = 0;
    int32_t addr_read;


    spin_lock_irqsave(&private->lists_lock, irq_flags);
    if (!private->dqlist_head)
    {
        err = -EAGAIN;
    }
    else
    {
        if (!list_empty(private->dqlist_head))
        {
            dqlist_next = private->dqlist_head->next;
        }

        opaque = list_entry(private->dqlist_head, dragon_buffer_opaque, dqlist);
        *buffer = opaque->buf;

        list_del_init(private->dqlist_head);
        private->dqlist_head = dqlist_next;
    }
    spin_unlock_irqrestore(&private->lists_lock, irq_flags);

    if (!err && !atomic_cmpxchg(&opaque->owned_by_cpu, 0, 1))
    {
        addr_read = dragon_read_reg32(private, 2);
        if (addr_read != (int32_t)opaque->dma_handle)
        {
            printk(KERN_INFO "Buffers queue is broken:\n");
            printk(KERN_INFO "\t opaque->dma_handle = %08x, addr_read = %08x\n",
                   (int)opaque->dma_handle, addr_read);
        }

        pci_dma_sync_single_for_cpu(private->pci_dev,
                                    addr_read,
                                    opaque->buf.len,
                                    PCI_DMA_FROMDEVICE);
    }

    return err;
}

static void dragon_switch_one_buffer(dragon_private *private)
{
    unsigned long irq_flags;
    struct list_head *qlist_next = 0;
    dragon_buffer_opaque *opaque;

    spin_lock_irqsave(&private->lists_lock, irq_flags);
    if (!private->qlist_head)
    {
        printk(KERN_INFO "Buffers queue is empty\n");
    }
    else
    {
        if (!list_empty(private->qlist_head))
        {
            qlist_next = private->qlist_head->next;
        }
        list_del_init(private->qlist_head);

        opaque = list_entry(private->qlist_head, dragon_buffer_opaque, qlist);
        if (private->dqlist_head)
        {
            list_add_tail(&opaque->dqlist, private->dqlist_head);
        }
        else
        {
            private->dqlist_head = &opaque->dqlist;
        }

        private->qlist_head = qlist_next;
    }
    spin_unlock_irqrestore(&private->lists_lock, irq_flags);
}

static long dragon_ioctl(struct file *file,
                        unsigned int cmd, unsigned long arg)
{
    int err = 0;
    void* parg = (void*)arg;
    dragon_private* private = file->private_data;

    if (!private)
    {
        printk(KERN_INFO "private is empty\n");
        return -1;
    }

    switch (cmd)
    {
    case DRAGON_SET_ACTIVITY:
        err = dragon_set_activity(private, arg);
        break;

    case DRAGON_SET_DAC:
        dragon_write_reg32(private, 3, arg);
        break;

    case DRAGON_QUERY_PARAMS:
        if (parg)
            *(dragon_params*)parg = private->params;
        break;

    case DRAGON_SET_PARAMS:
        if (dragon_check_params(parg))
            return -EINVAL;
        err = dragon_write_params(private, parg);
        break;

    case DRAGON_REQUEST_BUFFERS:
        err = dragon_request_buffers(private, parg);
        break;

    case DRAGON_RELEASE_BUFFERS:
        err = dragon_release_buffers(private);
        break;

    case DRAGON_QUERY_BUFFER:
        err = dragon_query_buffer(private, parg);
        break;

    case DRAGON_QBUF:
        err = dragon_qbuf(private, parg);
        break;

    case DRAGON_DQBUF:
        err = dragon_dqbuf(private, parg);
        break;
    case DRAGON_GET_ID:
        if(parg)
            *(uint32_t*)parg = dragon_read_reg32(private, 8);
        break;

    default: err = -EINVAL;
    }

    return err;
}


static irqreturn_t dragon_irq_handler(int irq, void *data)
{
    dragon_private *private = data;

    if (!private || private->pci_dev->irq != irq)
    {
        return IRQ_NONE;
    }

    dragon_switch_one_buffer(private);
    wake_up_interruptible(&private->wait);
    atomic_dec(&private->queue_length);

    return IRQ_HANDLED;
}

static int dragon_open(struct inode *inode, struct file *file)
{
    dragon_private* private;

    if (!inode || !file)
    {
        printk(KERN_INFO "dragon open error: inode or file is zero\n");
        return -1;
    }
    private = container_of(inode->i_cdev, dragon_private, cdev);

    if (!private)
    {
        printk(KERN_INFO "dragon open error: private data pointer is zero\n");
        return -1;
    }

    file->private_data = private;

    if ( !atomic_dec_and_test(&private->dev_available) )
    {
        atomic_inc(&private->dev_available);
        printk(KERN_INFO "device %d is busy\n", MINOR(private->cdev_no));
        return -EBUSY;
    }

    // Disable device activity just in case
    dragon_set_activity(private, 0);

    //Init wait queue head and spin locks
    init_waitqueue_head(&private->wait);
    spin_lock_init(&private->lists_lock);
    spin_lock_init(&private->page_table_lock);
    spin_lock_init(&private->activity_lock);
    atomic_set(&private->queue_length, 0);

    //Init IRQ
    if ( request_irq(private->pci_dev->irq, dragon_irq_handler, 0,
                     private->dev_name, private) )
    {
        printk(KERN_INFO "request_irq() failed\n");
        return -1;
    }

    //Write default params to device
    dragon_params_set_defaults(&private->params);
    dragon_check_params(&private->params);
    dragon_write_params(private, NULL);

    printk(KERN_INFO "successfully open dragon device %d\n", MINOR(private->cdev_no));

    return 0;
}

static int dragon_release(struct inode *inode, struct file *file)
{
    dragon_private* private;

    if (!file)
    {
        printk(KERN_INFO "dragon release error: file is zero\n");
        return -1;
    }

    private = file->private_data;
    if (!private)
    {
        printk(KERN_INFO "dragon release error: private data pointer is zero\n");
        return -1;
    }

    dragon_set_activity(private, 0);

    free_irq(private->pci_dev->irq, private);

    dragon_release_buffers(private);

    file->private_data = 0;

    atomic_inc(&private->dev_available);

    printk(KERN_INFO "release dragon device %d\n", MINOR(private->cdev_no));

    return 0;
}

static unsigned int dragon_poll(struct file *file, struct poll_table_struct *poll_table)
{
    unsigned long irq_flags;
    dragon_private *private = file->private_data;
    unsigned int ret = 0;

    spin_lock_irqsave(&private->lists_lock, irq_flags);
    if (private->dqlist_head)
    {
        ret = POLLIN | POLLRDNORM;
    }
    spin_unlock_irqrestore(&private->lists_lock, irq_flags);

    if (!ret)
    {
        poll_wait(file, &private->wait, poll_table);

        spin_lock_irqsave(&private->lists_lock, irq_flags);
        if (private->dqlist_head)
        {
            ret = POLLIN | POLLRDNORM;
        }
        spin_unlock_irqrestore(&private->lists_lock, irq_flags);
    }

    return ret;
}

static int dragon_mmap(struct file *file, struct vm_area_struct *vma)
{
    vma->vm_flags |= VM_IO;

    if ( io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
                            vma->vm_end - vma->vm_start,
                            vma->vm_page_prot) )
        return -EAGAIN;

    return 0;
}

static const struct file_operations dragon_fops = {
    .owner            =  THIS_MODULE,
    .open             =  dragon_open,
    .release          =  dragon_release,
    .poll             =  dragon_poll,
    .mmap             =  dragon_mmap,
    .unlocked_ioctl   =  dragon_ioctl,
};

static int probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    dev_t cdev_no;
    struct dragon_private *private = 0;
    unsigned long mmio_length;

    private = vmalloc_32(sizeof(struct dragon_private));
    if (!private)
    {
        pci_set_drvdata(dev, 0);
        printk(KERN_INFO "vmalloc_32() failed\n");
        goto err_alloc;
    }
    memset(private, 0, sizeof(struct dragon_private));

    private->pci_dev = dev;
    pci_set_drvdata(dev, private);

    //FIXME: Reimplement with atomic counter (CAS operation)
    spin_lock(&dev_number_lock);
    cdev_no = dragon_dev_number++;
    spin_unlock(&dev_number_lock);

    private->cdev_no = cdev_no;

    cdev_init(&private->cdev, &dragon_fops);
    private->cdev.owner = THIS_MODULE;

    if ( cdev_add(&private->cdev, private->cdev_no, 1) )
    {
        printk(KERN_INFO "cdev_add() failed\n");
        goto err_cdev_add;
    }

    snprintf(private->dev_name, sizeof(private->dev_name),
             "dragon%d", MINOR(private->cdev_no));

    if ( IS_ERR(device_create(dragon_class, NULL, private->cdev_no, NULL,
                              private->dev_name)) )
    {
        printk(KERN_INFO "device_create() failed\n");
        goto err_device_create;
    }

    if ( pci_enable_device(private->pci_dev) )
    {
        printk(KERN_INFO "pci_enable_device() failed\n");
        goto err_pci_enable_device;
    }

    if ( pci_enable_msi(private->pci_dev) )
    {
        printk(KERN_INFO "pci_enable_msi() failed\n");
        goto err_pci_enable_msi;
    }

    pci_set_master(private->pci_dev);

    if ( pci_set_dma_mask(dev, DMA_BIT_MASK(64)) )
    {
        printk(KERN_INFO "pci_set_dma_mask() 64-bit failed\n");
        goto err_pci_set_dma_mask;
    }

    //Request region for BAR0
    if ( pci_request_region(private->pci_dev, 0, private->dev_name) )
    {
        printk(KERN_INFO "pci_request_region() failed\n");
        goto err_pci_request_region;
    }
    mmio_length = pci_resource_len(private->pci_dev, 0);
    if ( !mmio_length ||
         !(private->io_buffer =
           pci_iomap(private->pci_dev, 0, mmio_length)) )
    {
        printk(KERN_INFO "pci_iomap mmio_length = %ld failed\n", mmio_length);
        goto err_pci_iomap;
    }

    //set device availability flag
    atomic_set(&private->dev_available, 1);

    printk(KERN_INFO "probe dragon device %d complete\n", MINOR(private->cdev_no));

    return 0;

err_pci_iomap:
    pci_release_region(private->pci_dev, 0);
err_pci_request_region:
err_pci_set_dma_mask:
    pci_clear_master(private->pci_dev);
    pci_disable_msi(private->pci_dev);
err_pci_enable_msi:
    pci_disable_device(private->pci_dev);
err_pci_enable_device:
    device_destroy(dragon_class, private->cdev_no);
err_device_create:
    cdev_del(&private->cdev);
err_cdev_add:
    vfree(private);
err_alloc:
    pci_set_drvdata(dev, 0);
    return -1;
}

static void remove(struct pci_dev *dev)
{
    unsigned cdev_no;
    struct dragon_private *private = pci_get_drvdata(dev);

    if (!private) return;

    cdev_no = MINOR(private->cdev_no);

    pci_release_region(dev, 0);

    pci_disable_msi(dev);

    pci_clear_master(dev);

    pci_disable_device(dev);

    device_destroy(dragon_class, private->cdev_no);

    cdev_del(&private->cdev);

    vfree(private);

    printk(KERN_INFO "remove dragon device %d complete\n", cdev_no);
}

static struct pci_driver dragon_driver = {
    .name = (char*)DRV_NAME,
    .id_table = dragon_ids,
    .probe = probe,
    .remove = remove,
    /* resume, suspend are optional */
};


static int dragon_init(void)
{
    printk(KERN_INFO "dragon module init\n");

    /* Request dynamic allocation of a device major number */
    if (alloc_chrdev_region(&dragon_dev_number, 0,
                            DRAGON_MAXNUM_DEVS, DRV_NAME) < 0) {
        printk(KERN_INFO "can't register device\n");
        return -1;
    }

    if( !(dragon_class = class_create(THIS_MODULE, DRV_NAME)) )
    {
        printk(KERN_INFO "dragon class creation failed\n");
        return -1;
    }

    return pci_register_driver(&dragon_driver);
}

static void dragon_exit(void)
{
    pci_unregister_driver(&dragon_driver);

    class_destroy(dragon_class);

    unregister_chrdev_region(MAJOR(dragon_dev_number), DRAGON_MAXNUM_DEVS);

    printk(KERN_INFO "dragon module exit\n");
}

module_init(dragon_init);
module_exit(dragon_exit);
