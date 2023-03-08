/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to check if current in_off is at end of entry array
 * @param offs_idx an index in the buffer entry list
 * @return true if entry is the last entry in the entry array, false if not
 */
static bool end_entry(struct aesd_circular_buffer *buffer, uint8_t offs_idx)
{
    int buffer_size = (sizeof(buffer->entry)) / sizeof(struct aesd_buffer_entry);
    
    if (offs_idx == (buffer_size - 1)) {
        return true;
    }
    return false;
}

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    size_t cur_offset = 0;
    size_t cur_entry_offset = 0;
    size_t entry_idx = 0;
    struct aesd_buffer_entry* p_entry = NULL;

    // If buffer is NULL, return NULL
    if (buffer == NULL) {
        return NULL;
    }

    entry_idx = buffer->out_offs;
    p_entry = &(buffer->entry[entry_idx]);

    // Ensure first entry isn't a null pointer
    if (p_entry == NULL) {
        return NULL;
    }

    // Walk through buffer until offset is reached
    while (cur_offset < char_offset) {
       
        if (p_entry->size == 0 || (p_entry->buffptr == NULL)) {
            // If size of entry is zero or buffer pointer is NULL, return NULL
            return NULL;
        } else if (cur_entry_offset == (p_entry->size - 1)) {
            // If we are at the end of the current entry, go to next entry or 
            // if we have wrapped all the way back around return NULL
            if (end_entry(buffer, entry_idx)) {
                // Wrap back to beginning entry
                entry_idx = 0;
            } else {
                entry_idx++;
            }

            p_entry = &(buffer->entry[entry_idx]);

            // Ensure first entry isn't a null pointer
            if (p_entry == NULL) {
                return NULL;
            }

            // If we are back to the beginning, return NULL, end of buffer
            if (entry_idx == buffer->out_offs) {
                return NULL;
            }
            
            // Reset current entry offset for next entry buffer
            cur_entry_offset = 0;
        } else {
            // Not at end of entry yet, increment offset
            cur_entry_offset++;
        }

        cur_offset++;
    }

    // Successfully found entry
    if (cur_offset == char_offset) {
        // Verify entry isn't empty
        if ((p_entry->size != 0) && (p_entry->buffptr != NULL)) {
            *entry_offset_byte_rtn = cur_entry_offset;
            return p_entry;
        }
    }

    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    // No matter what we will write to current input offset
    buffer->entry[buffer->in_offs] = *add_entry;

    // Move input offset to next pointer, check if at end of entry array
    if (end_entry(buffer, buffer->in_offs)) {
        buffer->in_offs = 0;
    } else {
        buffer->in_offs++;
    }

    if (buffer->full) {
        // If buffer was already full, make sure output offset moves with input
        // offset
        buffer->out_offs = buffer->in_offs;
    } else {
        // Check to see if buffer is now full for the first time
        if (buffer->in_offs == buffer->out_offs) {
            buffer->full = true;
        } else {
            // Buffer shouldn't be full, make sure flag isn't set to true
            buffer->full = false;
        }
    }

    return;
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
    buffer->full = false;
    buffer->in_offs = 0;
    buffer->out_offs = 0;
}
