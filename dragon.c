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
#include <asm/uaccess.h>


#include "dragon.h"

MODULE_LICENSE("Dual BSD/GPL");

#define DRAGON_VID      0x10EE
#define DRAGON_DID      0x0007
#define DRAGON_MAXNUM_DEVS 256

#define DRAGON_DEFAULT_FRAME_LENGTH 49140
#define DRAGON_DEFAULT_FRAMES_PER_BUFFER 60
#define DRAGON_DEFAULT_SWITCH_PERIOD (1 << 24)
#define DRAGON_DEFAULT_HALF_SHIFT 0
#define DRAGON_DEFAULT_CHANNEL_AUTO 0
#define DRAGON_DEFAULT_CHANNEL 0
#define DRAGON_DEFAULT_SYNC_OFFSET 0
#define DRAGON_DEFAULT_SYNC_WIDTH 50
#define DRAGON_BUFFER_ORDER 10

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
    dragon_params params;
    dragon_buffer_opaque *buffers;
    size_t buf_count;
    struct list_head *qlist_head;
    struct list_head *dqlist_head;
    spinlock_t lists_lock;
    spinlock_t page_table_lock;
    wait_queue_head_t wait;
} dragon_private;


static void dragon_params_set_defaults(dragon_params* params)
{
    params->frame_length      = DRAGON_DEFAULT_FRAME_LENGTH;
    params->frames_per_buffer = DRAGON_DEFAULT_FRAMES_PER_BUFFER;
    params->switch_period     = DRAGON_DEFAULT_SWITCH_PERIOD;
    params->half_shift        = DRAGON_DEFAULT_HALF_SHIFT;
    params->channel_auto      = DRAGON_DEFAULT_CHANNEL_AUTO;
    params->channel           = DRAGON_DEFAULT_CHANNEL;
    params->sync_offset       = DRAGON_DEFAULT_SYNC_OFFSET;
    params->sync_width        = DRAGON_DEFAULT_SYNC_WIDTH;
}

static int dragon_check_params(dragon_params* params)
{
    if (!params)
        return -EINVAL;

    if (90 <= params->frame_length && params->frame_length <= 49140)
    {
        params->frame_length =
            ((params->frame_length - 1)/90 + 1)*90; //round up
    }
    else
    {
        printk(KERN_INFO "Bad dragon frame_length value\n");
        return -EINVAL;
    }

    if (!params->frames_per_buffer ||
        params->frames_per_buffer*params->frame_length > 32768*90)
    {
        printk(KERN_INFO "Bad dragon frames_per_buffer value\n");
        return -EINVAL;
    }

    if (1 <= params->switch_period && params->switch_period <= (1 << 24))
    {
        params->switch_period =
            ( (params->switch_period - params->frames_per_buffer) /
              params->frames_per_buffer + 1)*params->frames_per_buffer; //round up
    }
    else
    {
        printk(KERN_INFO "Bad dragon switch_period value\n");
        return -EINVAL;
    }

    params->half_shift &= 1;
    params->channel_auto &= 1;
    params->channel &= 1;

    if (params->sync_offset > 511)
    {
        printk(KERN_INFO "Bad dragon sync_offset value\n");
        return -EINVAL;
    }

    if (params->sync_offset > 127)
    {
        printk(KERN_INFO "Bad dragon sync_offset value\n");
        return -EINVAL;
    }

    return 0;
}

static inline void dragon_write_reg32(dragon_private* private,
                                      uint32_t dw_offset, uint32_t val)
{
    writel(val, private->io_buffer + ((dw_offset) << 2));
}

static void dragon_write_params(dragon_private* private,
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

    if (VAL_CHANGED(frame_length))
    {
        dragon_write_reg32(private, 7, VAL(frame_length)/6 - 1);
    }

    if (VAL_CHANGED(frames_per_buffer))
    {
        dragon_write_reg32(private, 6,
                           VAL(frames_per_buffer)*VAL(frame_length)/90);
    }

    if (VAL_CHANGED(switch_period))
    {
        dragon_write_reg32(private, 5, VAL(switch_period));
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

#undef  VAL
#undef  VAL_CHANGED
}

static long dragon_set_activity(dragon_private *private, int arg)
{
    if (arg)
    {
        dragon_write_reg32(private, 0, 0); // activate
        dragon_write_reg32(private, 1, 1); // re-enable
    }
    else
    {
        dragon_write_reg32(private, 1, 0); // disable
        dragon_write_reg32(private, 0, 1); // reset
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

static void dragon_release_buffers(dragon_private* private)
{
    int i;

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
}

static long dragon_request_buffers(dragon_private* private, size_t *count)
{
    size_t i, idx = 0;
    dragon_buffer_opaque *buffers;
    size_t buffer_size =
        private->params.frame_length * private->params.frames_per_buffer;

    if (!buffer_size || *count > DRAGON_MAX_BUFFER_COUNT)
    {
        *count = 0;
        return -EINVAL;
    }

    if(get_order(buffer_size) > DRAGON_BUFFER_ORDER)
    {
        printk(KERN_INFO "dragon buffer size is too big\n");
        return -EINVAL;
    }


    if (private->buf_count >= *count)
    {
        // buffers already available
        *count = private->buf_count;
        return 0;
    }

    buffers = vzalloc(*count*sizeof(dragon_buffer_opaque));
    if (!buffers)
    {
        printk(KERN_INFO "dragon buffers array allocation failed\n");
        return -ENOMEM;
    }

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
        return -ENOMEM;
    }

    private->buffers = buffers;
    private->buf_count = *count = i;

    return 0;
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
    dragon_buffer_opaque *opaque;
    unsigned long irq_flags;

    if (!buffer || buffer->idx >= private->buf_count)
    {
        return -EINVAL;
    }

    opaque = &private->buffers[buffer->idx];

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

    dragon_write_reg32(private, 2, opaque->dma_handle);

    return 0;
}

static long dragon_dqbuf(dragon_private *private, dragon_buffer *buffer)
{

    struct list_head *dqlist_next = 0;
    dragon_buffer_opaque *opaque;
    long err = 0;
    unsigned long irq_flags;


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
        pci_dma_sync_single_for_cpu(private->pci_dev,
                                    opaque->dma_handle,
                                    opaque->buf.len,
                                    PCI_DMA_FROMDEVICE);
    }

    return err;
}

static void dragon_switch_one_buffer(dragon_private *private)
{
    struct list_head *qlist_next = 0;
    dragon_buffer_opaque *opaque;
    unsigned long irq_flags;

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
        dragon_write_params(private, parg);
        break;

    case DRAGON_REQUEST_BUFFERS:
        err = dragon_request_buffers(private, parg);
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

    return IRQ_HANDLED;
}

static int dragon_open(struct inode *inode, struct file *file)
{
    unsigned long mmio_length;
    dragon_private* private = container_of(inode->i_cdev, dragon_private, cdev);

    if (!private)
        return -1;

    file->private_data = private;

    printk(KERN_INFO "trying to open dragon device %d\n", MINOR(private->cdev_no));

    if ( !atomic_dec_and_test(&private->dev_available) )
    {
        atomic_inc(&private->dev_available);
        printk(KERN_INFO "device %d is busy\n", MINOR(private->cdev_no));
        return -EBUSY;
    }

    if ( pci_enable_device(private->pci_dev) )
    {
        printk(KERN_INFO "pci_enable_device() failed\n");
        goto err_pci_enable_device;
    }

    pci_set_master(private->pci_dev);

    //Request region for BAR0
    if ( pci_request_region(private->pci_dev, 0, private->dev_name) )
    {
        printk(KERN_INFO "pci_request_region() failed\n");
        goto err_pci_request_region;
    }
    mmio_length = pci_resource_len(private->pci_dev, 0);
    private->io_buffer = pci_iomap(private->pci_dev, 0, mmio_length);

    //Init IRQ
    if ( request_irq(private->pci_dev->irq, dragon_irq_handler, 0,
                     private->dev_name, private) )
    {
        printk(KERN_INFO "request_irq() failed\n");
        goto err_request_irq;
    }

    //Init wait queue head
    init_waitqueue_head(&private->wait);

    //Write default params to device
    dragon_params_set_defaults(&private->params);
    dragon_write_params(private, NULL);

    printk(KERN_INFO "successfully open dragon device %d\n", MINOR(private->cdev_no));

    return 0;


err_request_irq:
    pci_iounmap(private->pci_dev, private->io_buffer);
    pci_release_region(private->pci_dev, 0);
err_pci_request_region:
    pci_clear_master(private->pci_dev);
    pci_disable_device(private->pci_dev);
err_pci_enable_device:
    atomic_inc(&private->dev_available);

    return -1;
}

static int dragon_release(struct inode *inode, struct file *file)
{
    dragon_private* private = container_of(inode->i_cdev, dragon_private, cdev);
    if (!private)
        return -1;

    printk(KERN_INFO "release dragon device %d\n", MINOR(private->cdev_no));

    dragon_set_activity(private, 0);

    free_irq(private->pci_dev->irq, private);

    pci_iounmap(private->pci_dev, private->io_buffer);

    pci_release_region(private->pci_dev, 0);

    pci_clear_master(private->pci_dev);

    pci_disable_device(private->pci_dev);

    dragon_release_buffers(private);

    atomic_inc(&private->dev_available);

    return 0;
}

static unsigned int dragon_poll(struct file *file, struct poll_table_struct *poll_table)
{
    dragon_private *private = file->private_data;
    unsigned long irq_flags;
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
    if ( remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
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

static int __devinit probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    dev_t cdev_no;
    struct dragon_private *private = 0;

    private = kzalloc(sizeof(struct dragon_private), GFP_KERNEL);
    if (!private)
    {
        pci_set_drvdata(dev, 0);
        printk(KERN_INFO "kzalloc() failed\n");
        goto err_kzalloc;
    }
    spin_lock_init(&private->lists_lock);
    spin_lock_init(&private->page_table_lock);
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

    if ( pci_enable_msi(dev) )
    {
        printk(KERN_INFO "pci_enable_msi() failed\n");
        goto err_pci_enable_msi;
    }

    if ( pci_set_dma_mask(dev, DMA_BIT_MASK(64)) )
    {
        printk(KERN_INFO "pci_set_dma_mask() 64-bit failed\n");
        goto err_pci_set_dma_mask;
    }

    //set device availability flag
    atomic_set(&private->dev_available, 1);

    printk(KERN_INFO "probe dragon device %d complete\n", MINOR(private->cdev_no));

    return 0;

err_pci_set_dma_mask:
    pci_disable_msi(dev);
err_pci_enable_msi:
    device_destroy(dragon_class, private->cdev_no);
err_device_create:
    cdev_del(&private->cdev);
err_cdev_add:
    kfree(private);
err_kzalloc:
    return -1;
}

static void __devexit remove(struct pci_dev *dev)
{
    struct dragon_private *private = pci_get_drvdata(dev);

    if (!private) return;

    pci_disable_msi(dev);

    device_destroy(dragon_class, private->cdev_no);

    cdev_del(&private->cdev);

    printk(KERN_INFO "remove dragon device %d complete\n", MINOR(private->cdev_no));

    kfree(private);
}

static struct pci_driver dragon_driver = {
    .name = (char*)DRV_NAME,
    .id_table = dragon_ids,
    .probe = probe,
    .remove = __devexit_p(remove),
    /* resume, suspend are optional */
};


static int __init dragon_init(void)
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

static void __exit dragon_exit(void)
{
    pci_unregister_driver(&dragon_driver);

    class_destroy(dragon_class);

    unregister_chrdev_region(MAJOR(dragon_dev_number), DRAGON_MAXNUM_DEVS);

    printk(KERN_INFO "dragon module exit\n");
}

module_init(dragon_init);
module_exit(dragon_exit);
