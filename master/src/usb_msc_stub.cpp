#include "tusb.h"
#include <string.h>

// Minimal stub MSC implementation to satisfy TinyUSB/host enumeration.

static const uint32_t kBlockSize  = 512;
static const uint32_t kBlockCount = 8; // 4 KB dummy disk
static uint8_t dummy_disk[kBlockSize * kBlockCount] = {0};

// Sense data (use TinyUSB struct name)
static scsi_sense_fixed_resp_t sense_data;

static void set_sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
    sense_data.response_code = 0x70;
    sense_data.sense_key = key;
    sense_data.add_sense_code = asc;
    sense_data.add_sense_qualifier = ascq;
}

// Inquiry response
extern "C" void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                                   uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    const char vid[] = "AIRWIRE";
    const char pid[] = "MSC STUB";
    const char rev[] = "1.0";
    memcpy(vendor_id,  vid, sizeof(vid));
    memcpy(product_id, pid, sizeof(pid));
    memcpy(product_rev, rev, sizeof(rev));
}

extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

extern "C" void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
    (void)lun;
    *block_count = kBlockCount;
    *block_size  = kBlockSize;
}

extern "C" bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    (void)lun;
    if (lba >= kBlockCount) return -1;
    uint8_t* src = dummy_disk + lba * kBlockSize + offset;
    memcpy(buffer, src, bufsize);
    return (int32_t)bufsize;
}

extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
    (void)lun;
    if (lba >= kBlockCount) return -1;
    uint8_t* dst = dummy_disk + lba * kBlockSize + offset;
    memcpy(dst, buffer, bufsize);
    return (int32_t)bufsize;
}

extern "C" bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

extern "C" void tud_msc_reset_cb(uint8_t lun)
{
    (void)lun;
}

extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
    (void) lun;
    void const* ptr = NULL;
    int32_t len = 0;

    switch ( scsi_cmd[0] )
    {
        case SCSI_CMD_TEST_UNIT_READY:
            set_sense(SCSI_SENSE_NO_SENSE, 0x00, 0x00);
            return 0;

        case SCSI_CMD_READ_CAPACITY_10:
        {
            uint32_t last_lba = kBlockCount - 1;
            uint32_t block_sz = kBlockSize;
            static uint8_t cap[8];
            tu_unaligned_write32(&cap[0], tu_htole32(last_lba));
            tu_unaligned_write32(&cap[4], tu_htole32(block_sz));
            ptr = cap;
            len = sizeof(cap);
            break;
        }

        case SCSI_CMD_READ_FORMAT_CAPACITY:
        {
            static uint8_t fmt_cap[] = {
                0x00, 0x00, 0x00, 0x08,             // capacity list length
                // current capacity descriptor
                (uint8_t)((kBlockCount >> 24) & 0xFF),
                (uint8_t)((kBlockCount >> 16) & 0xFF),
                (uint8_t)((kBlockCount >>  8) & 0xFF),
                (uint8_t)(kBlockCount & 0xFF),
                0x02,                               // descriptor code: formatted media
                (uint8_t)((kBlockSize >> 16) & 0xFF),
                (uint8_t)((kBlockSize >>  8) & 0xFF),
                (uint8_t)(kBlockSize & 0xFF)
            };
            ptr = fmt_cap;
            len = sizeof(fmt_cap);
            break;
        }

        case SCSI_CMD_REQUEST_SENSE:
        {
            len = sizeof(sense_data);
            ptr = &sense_data;
            break;
        }

        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        case SCSI_CMD_START_STOP_UNIT:
        case SCSI_CMD_MODE_SENSE_6:
        case SCSI_CMD_MODE_SENSE_10:
            set_sense(SCSI_SENSE_NO_SENSE, 0x00, 0x00);
            return 0;

        default:
            set_sense(SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
            return -1;
    }

    // Copy response to buffer
    len = (len < bufsize) ? len : bufsize;
    if ( ptr && len )
    {
        memcpy(buffer, ptr, len);
    }
    return len;
}
