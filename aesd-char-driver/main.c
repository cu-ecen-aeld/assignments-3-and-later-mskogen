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
#include "aesd_ioctl.h"

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
 * for any read attempt. Use f_pos to determine where to start read and count
 * specifies the number of bytes to return.
 */
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *start_entry;
    size_t start_entry_off = 0, read_length = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    // If the userspace buffer is NULL we can't do anything useful
    if (buf == NULL) {
        retval = -EFAULT;
        goto closeout;
    }

    // Check if start position is valid
    start_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->aesd_cb,
            *f_pos, &start_entry_off);

    if (start_entry == NULL) {
        PDEBUG("Nothing to read");
        retval = 0;
        goto closeout;
    } 
    
    // Truncate read if there are more bytes in command than requested to read
    PDEBUG("Found a starting entry reading from there");
    read_length = start_entry->size - start_entry_off;
    if (read_length > count) {
        read_length = count;
    }

    PDEBUG("buffer at start_entry is %s",  &(start_entry->buffptr[start_entry_off]));
    if (copy_to_user(buf, &(start_entry->buffptr[start_entry_off]), read_length)) {
        retval = -EFAULT;
        goto closeout;
    }

    // Update return value and file position for next read
    PDEBUG("Successfully read %ld bytes!", read_length);
    retval = read_length;
    *f_pos = *f_pos + read_length;

    closeout:
        PDEBUG("Read is returning value %ld", retval);
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
    struct aesd_buffer_entry new_entry;
    uint8_t in_pos = 0;
    size_t bytes_missing = 0;

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

    // Check if there is a temporary entry that hasn't been added to buffer
    if (dev->tmp_size > 0) {
        // Writing to an untermintated command string.
        // Update size for buffer and allocate for new memory
        dev->tmp_buf = krealloc(dev->tmp_buf, dev->tmp_size + count, GFP_KERNEL);
        if (!dev->tmp_buf) {
            retval = -ENOMEM;
            goto closeout;
        }

        // Make sure memory is zero'd for new write
        memset(&dev->tmp_buf[dev->tmp_size], 0, count);

        // Write new buffer from userspace, save any bytes missed for return val
        bytes_missing = copy_from_user(&dev->tmp_buf[dev->tmp_size], buf, count);

        // Set retval for number of bytes actually copied and update size
        // of the tmp_entry
        retval = count - bytes_missing;
        dev->tmp_size += retval;

    } else {
        // Writing a new command
        // Allocate memory for new write
        dev->tmp_buf = kmalloc(count, GFP_KERNEL);
        if (!dev->tmp_buf) {
            retval = -ENOMEM;
            goto closeout;
        }

        // Make sure memory is zero'd for new write
        memset(dev->tmp_buf, 0, count);

        // Write new buffer from userspace
        // Write new buffer from userspace, save any bytes missed for return val
        bytes_missing = copy_from_user(dev->tmp_buf, buf, count);

        retval = count - bytes_missing;
        dev->tmp_size = retval;
    }

    // See if we need to set/reset partial flag
    // Might need to analyze if the write command has multiple commands in it
    if (memchr(dev->tmp_buf, '\n', dev->tmp_size)) {
        // Found newline. Adding new entry...
        // Check if current entry will be overwritten and must be freed first
        if (dev->aesd_cb.full) {
            in_pos = dev->aesd_cb.in_offs;
            if (dev->aesd_cb.entry[in_pos].buffptr != NULL) {
                kfree(dev->aesd_cb.entry[in_pos].buffptr);
            }
            dev->aesd_cb.entry[in_pos].size = 0;
        }

        // Add new entry to circular buffer
        new_entry.buffptr = dev->tmp_buf;
        new_entry.size = dev->tmp_size;
        aesd_circular_buffer_add_entry(&dev->aesd_cb, &new_entry);
        PDEBUG("Added entry of %zu bytes '%s' to buffer", new_entry.size, new_entry.buffptr);

        // Don't free memory of buffer pointer because it gets freed later by
        // the aesd_cleanup_module function or before being overwritten.
        // Instead set pointer to NULL to start a new temporary entry
        dev->tmp_buf = NULL;
        dev->tmp_size = 0;
    } else {
        PDEBUG("Partial write to tmp entry, now %zu bytes", dev->tmp_size);
        PDEBUG("Tmp entry buf is '%s'", dev->tmp_buf);
    }

    // Update device buffer size for successful writes
    dev->buf_size += retval;

    closeout:
        mutex_unlock(&dev->mx_lock);
        return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos, errors = 0;

    // lock device to prevent write from altering filp or writing before llseek
    // finishes.
    if (mutex_lock_interruptible(&dev->mx_lock)) {
        return -ERESTARTSYS;
    }

    // Determine new position offset value to update file pointer too
    switch(whence) {
        case SEEK_SET:
            // Beginning of device
            newpos = off;
            break;
        case SEEK_CUR:
            // Specific location on device
            newpos = filp->f_pos + off;
            break;
        case SEEK_END:
            // End of device
            newpos = dev->buf_size + off;
            break;
        default:
            // Invalid, shouldn't ever hit this
            errors++;
            newpos = -EINVAL;
    }

    // Check validity of new position, can't be negative or larger than the size
    // of the buffer
    if (newpos < 0) {
        errors++;
        newpos = -EINVAL;
    }

    if (newpos > dev->buf_size) {
        errors++;
        newpos = -EINVAL;
    }

    // Set new file position if there aren't any errors
    if (errors == 0) {
        filp->f_pos = newpos;
    }

    // Unlock mutex now that writes to the device should be valid
    mutex_unlock(&dev->mx_lock);

    return newpos;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seek_cmd;
    struct aesd_buffer_entry *entry;
    loff_t loc_off = 0;
    uint32_t index;
    long retval = 0;
    bool mx_locked = false;

    PDEBUG("aesd_ioctl call with cmd '%u'\n", cmd);

    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:

            // Fetch seek command from user space
            if (copy_from_user(&seek_cmd, (const void __user *)arg, sizeof(seek_cmd)) != 0) {
                PDEBUG("Failed to get seek command from userspace\n");
                retval = -EINVAL;
                break;
            }

            // Seek to command specified by write_cmd to offset specified
            // by write_cmd_offset. If either of the two values is out of range
            // then return -EINVAL. returns zero for successful seek.
            if ((seek_cmd.write_cmd < 0) ||
               (seek_cmd.write_cmd > AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED))
            {
                PDEBUG("write_cmd value invalid: %u\n", seek_cmd.write_cmd);
                retval = -EINVAL;
                break;
            }

            // lock device to prevent write from altering filp or writing to
            // the circular buffer before seekto ioctl completes
            if (mutex_lock_interruptible(&dev->mx_lock)) {
                retval = -ERESTARTSYS;
                break;
            } else {
                mx_locked = true;
            }

            for (index = 0; index < seek_cmd.write_cmd; index++) {
                entry = &dev->aesd_cb.entry[index];
                if ((entry->buffptr != NULL) && (entry->size > 0)) {
                    loc_off += entry->size;
                } else {
                    // buffer isn't large enough for specified write_cmd
                    PDEBUG("write_cmd '%u' doesn't exist in current list\n", seek_cmd.write_cmd);
                    retval = -EINVAL;
                    break;
                }
            }

            entry = &dev->aesd_cb.entry[seek_cmd.write_cmd];
            if ((entry->buffptr != NULL) && (entry->size > 0)) {
                if (seek_cmd.write_cmd_offset > entry->size) {
                    // write_cmd is not big enough
                    PDEBUG("write_cmd_offset %u doesn't fit for entry size %lu\n", seek_cmd.write_cmd_offset, entry->size);
                    retval = -EINVAL;
                    break;                
                }
                loc_off += seek_cmd.write_cmd_offset;
            } else {
                // buffer isn't large enough for specified write_cmd
                PDEBUG("Entry is wrong\n");
                retval = -EINVAL;
                break;
            }

            // Update file pointer position and return zero for success
            filp->f_pos = loc_off;

            break;
        default:
            // Invalid/Unknown ioctl
            PDEBUG("unknown ioctl value: %u\n", cmd);
            retval = -EINVAL;
    }

    if (mx_locked) {
        mutex_unlock(&dev->mx_lock);
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
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
    aesd_device.buf_size = 0;
    aesd_device.tmp_buf = NULL;
    aesd_device.tmp_size = 0;
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
            entry->size = 0;
        }
    }
    mutex_destroy(&aesd_device.mx_lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
