/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>       // file_operations
#include <linux/slab.h>		// kmalloc()

#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Matthew Skogen");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    // Aesd char device
    struct aesd_dev *dev;

    PDEBUG("open");

    // Store device info in file private data field
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    // Nothing to release
    return 0;
}

/* 
 * Return partial or full content of recent 10 write commands in order received,
 * for any read attempt. Use filp to determine where to start read and count
 * specifies the number of bytes to return.
 */
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry, *end_entry;
    size_t end_entry_off = 0;
    uint8_t index;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    // If the userspace buffer is NULL we can't do anything useful
    if (buf == NULL) {
        retval = -EFAULT;
        goto closeout;
    }

    // Check if we need to read all or some of buffer
    end_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->aesd_cb,
            count, &end_entry_off);

    if (end_entry == NULL) {
        // to many bytes, we will only read what's available (i.e the who buffer)
        AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->aesd_cb, index) {
            if ((entry->buffptr != NULL) && (entry->size > 0)) {
                if (copy_to_user(buf, entry->buffptr, entry->size)) {
                    retval = -EFAULT;
                    goto closeout;
                }
                retval += entry->size;
            }
        }
    } else {
        // Only read up to entry returned and bytes returned in that offset
        AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->aesd_cb, index) {
            if ((entry->buffptr != NULL) && (entry->size > 0)) {
                if (entry == end_entry) {
                    if (copy_to_user(buf, entry->buffptr, end_entry_off)) {
                        retval = -EFAULT;
                        goto closeout;
                    }
                    retval += end_entry_off;
                    goto closeout;
                } else {
                    if (copy_to_user(buf, entry->buffptr, entry->size)) {
                        retval = -EFAULT;
                        goto closeout;
                    }
                    retval += entry->size;
                }
            }
        }
    }

    closeout:
        return retval;
}

/* 
 * Allocate memory (kmalloc) for each write command and save command in 
 * allocated memory each write command is \n character terminated and any none 
 * \n terminated command will remain and be appended to by future writes only
 * keep track of most recent 10 commands, overwrites should free memory before
 * overwritting command. 
 */
ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *p_entry, tmp_entry;
    char* tmp_buffer = NULL;
    size_t tmp_size = count;
    uint8_t in_pos = 0;
    memset(&tmp_entry, 0, sizeof(struct aesd_buffer_entry));

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    // If the userspace buffer is NULL we can't do anything useful
    if (buf == NULL) {
        retval = -EFAULT;
        goto closeout;
    }

    // lock and allow only one write file at a time, do interruptible lock so
    // we can kill process if needed
    if (mutex_lock_interruptible(&dev->mx_lock)) {
        return -ERESTARTSYS;
    }

    // find current write position, free memory if we are full before creating
    // new node
    in_pos = dev->aesd_cb.in_offs;
    p_entry = &dev->aesd_cb.entry[in_pos];
    if (dev->partial) {
        // Writing to an untermintated command string.
        // Update size for buffer and allocate for new memory
        tmp_size += p_entry->size;
        tmp_buffer = kmalloc(tmp_size, GFP_KERNEL);
        if (!tmp_buffer) {
            goto closeout;
        }

        // Make sure memory is zero'd for new write
        memset(tmp_buffer, 0, tmp_size);

        if (p_entry->buffptr != NULL) {
            // Copy memory from current partial string and free old memory
            memcpy(tmp_buffer, p_entry->buffptr, p_entry->size);
            kfree(p_entry->buffptr);
        }

        // Write new buffer from userspace
        if(copy_from_user(&tmp_buffer[p_entry->size], buf, count)) {
            retval = -EFAULT;
            goto closeout;
        }

        // Update entry with new buffer
        p_entry->buffptr = tmp_buffer;
        p_entry->size = tmp_size;
    } else {
        // Writing a new command, check if we need to free memory
        if (dev->aesd_cb.full) {
            if (p_entry->buffptr != NULL) {
                kfree(p_entry->buffptr);
            }
            p_entry->size = 0;
        }

        tmp_buffer = kmalloc(tmp_size, GFP_KERNEL);
        if (!tmp_buffer) {
            goto closeout;
        }

        // Make sure memory is zero'd for new write
        memset(tmp_buffer, 0, tmp_size);

        // Write new buffer from userspace
        if(copy_from_user(tmp_buffer, buf, count)) {
            retval = -EFAULT;
            goto closeout;
        }

        tmp_entry.buffptr = tmp_buffer;
        tmp_entry.size = tmp_size;

        // Add new entry to circular buffer
        aesd_circular_buffer_add_entry(&dev->aesd_cb, &tmp_entry);
    }

    // See if we need to set/reset partial flag
    // Might need to analyze if the write command has multiple commands in it
    if (strchr(tmp_buffer, (int)'\n')) {
        dev->partial = false;
    } else {
        dev->partial = true;
    }

    // If we made it here bytes were succesfully written
    retval = count;

    closeout:
        mutex_unlock(&dev->mx_lock);
        return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    // Initialize the AESD specific portion of the device
    // Zero out circular buffer, temporary buffer entry, and initialize mutex
    aesd_circular_buffer_init(&aesd_device.aesd_cb);
    aesd_device.partial = false;
    mutex_init(&aesd_device.mx_lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    uint8_t index;
    struct aesd_circular_buffer *buffer = &aesd_device.aesd_cb;
    struct aesd_buffer_entry *entry;

    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // Deallocate all the buffer entries in the circular buffer and destroy mutex
    AESD_CIRCULAR_BUFFER_FOREACH(entry, buffer, index) {
        if ((entry->size > 0) && (entry->buffptr != NULL)) {
            kfree(entry->buffptr);
        }
    }
    mutex_destroy(&aesd_device.mx_lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
