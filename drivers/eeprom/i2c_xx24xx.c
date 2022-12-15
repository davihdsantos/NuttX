/****************************************************************************
 * drivers/eeprom/i2c_xx24xx.c
 *
 *   Copyright (C) 2018 Sebastien Lorquet. All rights reserved.
 *   Author: Sebastien Lorquet <sebastien@lorquet.fr>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/* This is a driver for I2C EEPROMs that use the same commands as the
 * xx24xx.
 *
 * The following devices should be supported:
 *
 * Manufacturer Device     Bytes PgSize AddrLen DevAddr
 * Microchip
 *              24xx00        16     1    1     1010000 Special case
 *              24xx01       128     8    1     1010000
 *              24xx02       256     8    1     1010000
 *              24xx04       512     16   1     101000P
 *              24xx08      1024     16   1     10100PP
 *              24xx16      2048     16   1     1010PPP
 *              24xx32      4096     32   2     1010AAA
 *              24xx64      8192     32   2     1010AAA
 *              24xx128    16384     64   2     1010AAA
 *              24xx256    32768     64   2     1010AAA
 *              24xx512    65536    128   2     1010AAA
 *              24xx1025  131072    128   2     1010PAA Special case: address bit is shifted.
 *              24xx1026  131072    128   2     1010AAP
 *
 * Atmel
 *              AT24C01      128     8    1     1010AAA
 *              AT24C02      256     8    1     1010AAA
 *              AT24C04      512    16    1     1010AAP P bits = word address
 *              AT24C08     1024    16    1     1010APP
 *              AT24C16     2048    16    1     1010PPP
 *              AT24C32     4096    32    2     1010AAA
 *              AT24C64     8192    32    2     1010AAA
 *              AT24C128   16384    64    2     10100AA
 *              AT24C256   32768    64    2     10100AA
 *              AT24C512   65536   128    2     10100AA
 *              AT24C1024 131072   256    2     10100AP
 *
 * ST Microelectronics
 *              M24C01       128    16    1     1010AAA
 *              M24C02       256    16    1     1010AAA
 *              M24C04       512    16    1     1010AAP
 *              M24C08      1024    16    1     1010APP
 *              M24C16      2048    16    1     1010PPP
 *              M24C32      4096    32    2     1010AAA ID pages supported as a separate device
 *              M24C64      8192    32    2     1010AAA
 *              M24128     16384    64    2     1010AAA
 *              M24256     32768    64    2     1010AAA
 *              M24512     65536   128    2     1010AAA
 *              M24M01    131072   256    2     1010AAP
 *              M24M02    262144   256    2     1010APP
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <sys/types.h>
#include <debug.h>
#include <errno.h>
#include <string.h>
#include <nuttx/fs/fs.h>

#include <nuttx/kmalloc.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/eeprom/i2c_xx24xx.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_EE24XX_FREQUENCY
#  define CONFIG_EE24XX_FREQUENCY 100000
#endif

#define UUID_SIZE   16

/****************************************************************************
 * Types
 ****************************************************************************/

/* Device geometry description, compact form (2 bytes per entry) */

struct ee24xx_geom_s
{
  uint8_t bytes    : 4; /* Power of two of 128 bytes (0:128 ... 11:262144) */
  uint8_t pagesize : 4; /* Power of two of   8 bytes (0:8 1:16 2:32 3:64 etc) */
  uint8_t addrlen  : 4; /* Nr of bytes in command address field */
  uint8_t abits    : 3; /* Nr of Address MSBits in the i2c device address LSBs */
  uint8_t special  : 1; /* Special device: uchip 24xx00 (total 16 bytes)
                         * or 24xx1025 (shifted P bits) */
};

/* Private data attached to the inode */

struct ee24xx_dev_s
{
  /* Bus management */

  struct i2c_master_s *i2c;      /* I2C device where the EEPROM is attached */
  uint32_t             freq;     /* I2C bus speed */
  uint8_t              addr;     /* 7-bit unshifted I2C device address */

  /* Driver management */

  sem_t                sem;      /* file write access serialization */
  uint8_t              refs;     /* Nr of times the device has been opened */
  bool                 readonly; /* Flags */

  /* Expanded from geometry */

  uint32_t             size;       /* total bytes in device */
  uint16_t             pgsize;     /* write block size, in bytes */
  uint16_t             addrlen;    /* number of bytes in data addresses */
  uint16_t             haddrbits;  /* Number of bits in high address part */
  uint16_t             haddrshift; /* bit-shift of high address part */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int     ee24xx_open(FAR struct file *filep);
static int     ee24xx_close(FAR struct file *filep);
static off_t   ee24xx_seek(FAR struct file *filep, off_t offset, int whence);
static ssize_t ee24xx_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen);
static ssize_t ee24xx_write(FAR struct file *filep, FAR const char *buffer,
                            size_t buflen);
static int     ee24xx_ioctl(FAR struct file *filep, int cmd,
                            unsigned long arg);

#ifdef CONFIG_AT24CS_UUID
static ssize_t at24cs_read_uuid(FAR struct file *filep, FAR char *buffer,
                                size_t buflen);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Supported device geometries.
 * One geometry can fit more than one device.
 * The user will use an enum'ed index from include/eeprom/i2c_xx24xx.h
 */

static const struct ee24xx_geom_s g_ee24xx_devices[] =
{
  /* Microchip devices */

/* by pg al ab sp      device     bytes page  alen */
  { 0, 1, 1, 0, 1}, /* 24xx00        16    1     1 Ridiculously small device */
  { 0, 0, 1, 0, 0}, /* 24xx01       128    8     1 */
  { 1, 0, 1, 0, 0}, /* 24xx02       256    8     1 */
  { 2, 1, 1, 1, 0}, /* 24xx04       512   16     1 */
  { 3, 1, 1, 2, 0}, /* 24xx08      1024   16     1 */
  { 4, 1, 1, 3, 0}, /* 24xx16      2048   16     1 */
  { 5, 2, 2, 0, 0}, /* 24xx32      4096   32     2 */
  { 6, 2, 2, 0, 0}, /* 24xx64      8192   32     2 */
  { 7, 3, 2, 0, 0}, /* 24xx128    16384   64     2 */
  { 8, 3, 2, 0, 0}, /* 24xx256    32768   64     2 */
  { 9, 4, 2, 0, 0}, /* 24xx512    65536  128     2 */
  {10, 4, 2, 1, 1}, /* 24xx1025  131072  128     2 Shifted address, todo */
  {10, 4, 2, 1, 0}, /* 24xx1026  131072  128     2 */
  {11, 5, 2, 2, 0}, /* AT24CM02  262144  256     2 */

  /* STM devices */

  { 0, 1, 1, 0, 0}, /* M24C01       128   16     1 */
  { 1, 1, 1, 0, 0}, /* M24C02       256   16     1 */
  {11, 5, 2, 2, 0}, /* M24M02    262144  256     2 */
};

/* Driver operations */

static const struct file_operations ee24xx_fops =
{
  ee24xx_open,  /* open */
  ee24xx_close, /* close */
  ee24xx_read,  /* read */
  ee24xx_write, /* write */
  ee24xx_seek,  /* seek */
  ee24xx_ioctl, /* ioctl */
  NULL          /* poll */
};

#ifdef CONFIG_AT24CS_UUID
static const struct file_operations at24cs_uuid_fops =
{
  ee24xx_open,      /* piggyback on the ee24xx_open */
  ee24xx_close,     /* piggyback on the ee24xx_close */
  at24cs_read_uuid, /* read */
  NULL,             /* write */
  NULL,             /* seek */
  NULL,             /* ioctl */
  NULL              /* poll */
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ee24xx_waitwritecomplete
 *
 * Use ACK polling to detect the completion of the write operation.
 * Returns TRUE if write is complete (device replies to ACK).
 * Note: The device always replies an ACK for the control byte, the polling
 * shall be done using the ACK for the memory address byte. Read or write does
 * not matter.
 * Note: We should sleep a bit between retries, the write time is around 5 ms,
 * but the bus is slow, so, a few retries at most will happen.
 *
 ****************************************************************************/

static int ee24xx_waitwritecomplete(FAR struct ee24xx_dev_s *eedev,
                                    uint32_t memaddr)
{
  struct i2c_msg_s msgs[1];
  int ret;
  int retries = 100;
  uint8_t adr;
  uint32_t addr_hi = (memaddr >> (eedev->addrlen << 3));

  msgs[0].frequency = eedev->freq;
  msgs[0].addr      = eedev->addr | (addr_hi & ((1 << eedev->haddrbits) - 1));
  msgs[0].flags     = I2C_M_READ;
  msgs[0].buffer    = &adr;
  msgs[0].length    = 1;

  do
    {
     ret = I2C_TRANSFER(eedev->i2c, msgs, 1);
     retries--;
    }
  while (ret != 0 && retries > 0);

  return ret;
}

/****************************************************************************
 * Name: ee24xx_writepage
 *
 * Description: Write data to the EEPROM, NOT crossing page boundaries.
 * To avoid allocating a buffer to prepend the address, we are using 2 I2C
 * messages while avoiding beginning the second one with a restart condition.
 *
 ****************************************************************************/

static int ee24xx_writepage(FAR struct ee24xx_dev_s *eedev, uint32_t memaddr,
                            FAR const char *buffer, size_t len)
{

  struct i2c_msg_s msgs[2];
  uint8_t maddr[2];
  uint32_t addr_hi = (memaddr >> (eedev->addrlen << 3));

  /* Write data address */

  maddr[0] = memaddr >> 8;
  maddr[1] = memaddr &  0xff;

  msgs[0].frequency = eedev->freq;
  msgs[0].addr      = eedev->addr | (addr_hi & ((1 << eedev->haddrbits) - 1));
  msgs[0].flags     = 0;
  msgs[0].buffer    = eedev->addrlen == 2 ? &maddr[0] : &maddr[1];
  msgs[0].length    = eedev->addrlen;

  /* Write data without a restart nor a control byte */

  msgs[1].frequency = msgs[0].frequency;
  msgs[1].addr      = msgs[0].addr;
  msgs[1].flags     = I2C_M_NOSTART;
  msgs[1].buffer    = (uint8_t *)buffer;
  msgs[1].length    = len;

  return I2C_TRANSFER(eedev->i2c, msgs, 2);
}

/****************************************************************************
 * Name: ee24xx_semtake
 *
 * Acquire a resource to access the device.
 * The purpose of the semaphore is to block tasks that try to access the
 * EEPROM while another task is actively using it.
 *
 ****************************************************************************/

static void ee24xx_semtake(FAR struct ee24xx_dev_s *eedev)
{
  /* Take the semaphore (perhaps waiting) */

  while (sem_wait(&eedev->sem) != 0)
    {
      /* The only case that an error should occur here is if
       * the wait was awakened by a signal.
       */

      DEBUGASSERT(errno == EINTR || errno == ECANCELED);
    }
}

/****************************************************************************
 * Name: ee24xx_semgive
 *
 * Release a resource to access the device.
 *
 ****************************************************************************/

static inline void ee24xx_semgive(FAR struct ee24xx_dev_s *eedev)
{
  sem_post(&eedev->sem);
}

/****************************************************************************
 * Driver Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ee24xx_open
 *
 * Description: Open the block device
 *
 ****************************************************************************/

static int ee24xx_open(FAR struct file *filep)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct ee24xx_dev_s *eedev;
  int ret = OKK;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;
  ee24xx_semtake(eedev);

  /* Increment the reference count */

  if ((eedev->refs + 1) == 0)
    {
      ret = -EMFILE;
    }
  else
    {
      eedev->refs += 1;
    }

  ee24xx_semgive(eedev);
  return ret;
}

/****************************************************************************
 * Name: ee24xx_close
 *
 * Description: Close the block device
 *
 ****************************************************************************/

static int ee24xx_close(FAR struct file *filep)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct ee24xx_dev_s *eedev;
  int ret = OKK;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;
  ee24xx_semtake(eedev);

  /* Decrement the reference count. I want the entire close operation
   * to be atomic wrt other driver operations.
   */

  if (eedev->refs == 0)
    {
      ret = -EIO;
    }
  else
    {
      eedev->refs -= 1;
    }

  ee24xx_semgive(eedev);
  return ret;
}

/****************************************************************************
 * Name: ee24xx_seek
 *
 * Remark: Copied from bchlib
 *
 ****************************************************************************/

static off_t ee24xx_seek(FAR struct file *filep, off_t offset, int whence)
{
  FAR struct ee24xx_dev_s *eedev;
  off_t                   newpos;
  int                     ret;
  FAR struct inode        *inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;
  ee24xx_semtake(eedev);

  /* Determine the new, requested file position */

  switch (whence)
    {
    case SEEK_CUR:
      newpos = filep->f_pos + offset;
      break;

    case SEEK_SET:
      newpos = offset;
      break;

    case SEEK_END:
      newpos = eedev->size + offset;
      break;

    default:
      /* Return EINVAL if the whence argument is invalid */

      ee24xx_semgive(eedev);
      return -EINVAL;
    }

  /* Opengroup.org:
   *
   *  "The lseek() function shall allow the file offset to be set beyond the
   *   end of the existing data in the file. If data is later written at
   *   this point, subsequent reads of data in the gap shall return bytes
   *   with the value 0 until data is actually written into the gap."
   *
   * We can conform to the first part, but not the second.  But return EINVAL
   * if
   *
   *  "...the resulting file offset would be negative for a regular file,
   *   block special file, or directory."
   */

  if (newpos >= 0)
    {
      filep->f_pos = newpos;
      ret = newpos;
      finfo("SEEK newpos %d\n", newpos);
    }
  else
    {
      ret = -EINVAL;
    }

  ee24xx_semgive(eedev);
  return ret;
}

/****************************************************************************
 * Name: ee24xx_read
 ****************************************************************************/

static ssize_t ee24xx_read(FAR struct file *filep, FAR char *buffer,
                           size_t len)
{
  FAR struct ee24xx_dev_s *eedev;
  FAR struct inode        *inode = filep->f_inode;
  struct i2c_msg_s         msgs[2];
  uint8_t                  addr[2];
  uint32_t                 addr_hi;
  int                      ret;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;

  ee24xx_semtake(eedev);

  /* trim len if read would go beyond end of device */

  if ((filep->f_pos + len) > eedev->size)
    {
      len = eedev->size - filep->f_pos;
    }

  if (len == 0)
    {
      /* We are at end of file */

      ret = 0;
      goto done;
    }

  /* Write data address */

  finfo("READ %d bytes at pos %d\n", len, filep->f_pos);

  addr_hi           = (filep->f_pos >> (eedev->addrlen << 3));

  addr[0]           = (filep->f_pos) >> 8;
  addr[1]           = (filep->f_pos) &  0xff;

  msgs[0].frequency = eedev->freq;
  msgs[0].addr      = eedev->addr | (addr_hi & ((1 << eedev->haddrbits) - 1));
  msgs[0].flags     = 0;
  msgs[0].buffer    = eedev->addrlen == 2 ? &addr[0] : &addr[1];
  msgs[0].length    = eedev->addrlen;

  /* Read data */

  msgs[1].frequency = msgs[0].frequency;
  msgs[1].addr      = msgs[0].addr;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = (uint8_t *)buffer;
  msgs[1].length    = len;

  ret = I2C_TRANSFER(eedev->i2c, msgs, 2);
  if (ret < 0)
    {
      goto done;
    }

  ret = len;

  /* Update the file position */

  filep->f_pos += len;

done:
  ee24xx_semgive(eedev);
  return ret;
}

/****************************************************************************
 * Name: at24cs_read_uuid
 ****************************************************************************/

#ifdef CONFIG_AT24CS_UUID
static ssize_t at24cs_read_uuid(FAR struct file *filep, FAR char *buffer,
                                size_t len)
{
  FAR struct ee24xx_dev_s *eedev;
  FAR struct inode        *inode = filep->f_inode;
  struct i2c_msg_s         msgs[2];
  uint8_t                  regindx;
  int                      ret;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;

  ee24xx_semtake(eedev);

  /* trim len if read would go beyond end of device */

  if ((filep->f_pos + len) > UUID_SIZE)
    {
      len = UUID_SIZE - filep->f_pos;
    }

  if (len == 0)
    {
      /* We are at end of file */

      ret = 0;
      goto done;
    }

  /* Write data address */

  finfo("READ %d bytes at pos %d\n", len, filep->f_pos);

  regindx           = 0x80;             /* reg index of UUID[0] */

  msgs[0].frequency = eedev->freq;
  msgs[0].addr      = eedev->addr + 8;  /* slave addr of UUID */
  msgs[0].flags     = 0;
  msgs[0].buffer    = &regindx;
  msgs[0].length    = 1;

  /* Read data */

  msgs[1].frequency = msgs[0].frequency;
  msgs[1].addr      = msgs[0].addr;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = (uint8_t *)buffer;
  msgs[1].length    = len;

  ret = I2C_TRANSFER(eedev->i2c, msgs, 2);
  if (ret < 0)
    {
      goto done;
    }

  ret = len;

  /* Update the file position */

  filep->f_pos += len;

done:
  ee24xx_semgive(eedev);
  return ret;
}
#endif

/****************************************************************************
 * Name: ee24xx_write
 ****************************************************************************/

static ssize_t ee24xx_write(FAR struct file *filep, FAR const char *buffer,
                            size_t len)
{
  FAR struct ee24xx_dev_s *eedev;
  size_t                   cnt;
  int                      pageoff;
  FAR struct inode        *inode = filep->f_inode;
  int                      ret   = -EACCES;
  int                      savelen;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;

  if (eedev->readonly)
    {
      return ret;
    }

  /* Forbid writes past the end of the device */

  if (filep->f_pos >= eedev->size)
    {
      return -EFBIG;
    }

  finfo("Entering with len=%d\n", len);

  /* Clamp len to avoid crossing the end of the memory */

  if ((len + filep->f_pos) > eedev->size)
    {
      len = eedev->size - filep->f_pos;
      finfo("Len clamped to %d\n", len);
    }

  savelen = len; /* save number of bytes written */

  ee24xx_semtake(eedev);

  /* Writes can't happen in a row like the read does.
   * The EEPROM is made of pages, and write sequences
   * cannot cross page boundaries. So every time the last
   * byte of a page is programmed, a separate I2C transaction
   * required to continue writing.
   */

  /* First, write some page-unaligned data */

  pageoff = filep->f_pos & (eedev->pgsize - 1);
  cnt     = eedev->pgsize - pageoff;
  if (cnt > len)
    {
      cnt = len;
    }

  if (pageoff > 0)
    {
      finfo("First %d unaligned bytes at %d (pageoff %d)\n",
            cnt, filep->f_pos, pageoff);

      ret = ee24xx_writepage(eedev, filep->f_pos, buffer, cnt);
      if (ret < 0)
        {
          ferr("write failed, ret = %d\n", ret);
          goto done;
        }

      ret = ee24xx_waitwritecomplete(eedev, filep->f_pos);
      if (ret < 0)
        {
          ferr("writecomplete failed, ret = %d\n", ret);
          goto done;
        }

      len          -= cnt;
      buffer       += cnt;
      filep->f_pos += cnt;
    }

  /* Then, write remaining bytes at page-aligned addresses */

  while (len > 0)
    {
      cnt = len;
      if (cnt > eedev->pgsize)
        {
          cnt = eedev->pgsize;
        }

      finfo("Aligned page write for %d bytes at %d\n", cnt, filep->f_pos);

      ret = ee24xx_writepage(eedev, filep->f_pos, buffer, cnt);
      if (ret < 0)
        {
          ferr("write failed, ret = %d\n", ret);
          goto done;
        }

      ret = ee24xx_waitwritecomplete(eedev, filep->f_pos);
      if (ret < 0)
        {
          ferr("writecomplete failed, ret = %d\n", ret);
          goto done;
        }

      len          -= cnt;
      buffer       += cnt;
      filep->f_pos += cnt;
    }

  ret = savelen;

done:
  ee24xx_semgive(eedev);
  return ret;
}

/****************************************************************************
 * Name: ee24xx_ioctl
 *
 * Description: TODO: Erase a sector/page/device or read device ID / MAC.
 * This is completely optional.
 *
 ****************************************************************************/

static int ee24xx_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct ee24xx_dev_s *eedev;
  FAR struct inode        *inode = filep->f_inode;
  int                      ret   = 0;

  DEBUGASSERT(inode && inode->i_private);
  eedev = (FAR struct ee24xx_dev_s *)inode->i_private;

  switch (cmd)
    {
      default:
        (void)eedev;
        ret = -EINVAL;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ee24xx_initialize
 *
 * Description: Bind a EEPROM driver to an I2C bus. The user MUST provide
 * a description of the device geometry, since it is not possible to read
 * this information from the device (contrary to the SPI flash devices).
 *
 ****************************************************************************/

int ee24xx_initialize(FAR struct i2c_master_s *bus, uint8_t devaddr,
                      FAR char *devname, int devtype, int readonly)
{
  FAR struct ee24xx_dev_s *eedev;
#ifdef CONFIG_AT24CS_UUID
  FAR char                *uuidname;
  int                     ret;
#endif

  /* Check device type early */

  if ((devtype < 0) ||
      (devtype >= sizeof(g_ee24xx_devices) / sizeof(g_ee24xx_devices[0])))
    {
      return -EINVAL;
    }

  eedev = kmm_zalloc(sizeof(struct ee24xx_dev_s));

  if (!eedev)
    {
      return -ENOMEM;
    }

  sem_init(&eedev->sem, 0, 1);

  eedev->freq     = CONFIG_EE24XX_FREQUENCY;
  eedev->i2c      = bus;
  eedev->addr     = devaddr;
  eedev->readonly = !!readonly;

  /* Expand device geometry from compacted info */

  eedev->size       = 128 << g_ee24xx_devices[devtype].bytes;
  eedev->pgsize     =   8 << g_ee24xx_devices[devtype].pagesize;
  eedev->addrlen    =        g_ee24xx_devices[devtype].addrlen;
  eedev->haddrbits  =        g_ee24xx_devices[devtype].abits;
  eedev->haddrshift = 0;

  /* Apply special properties */

  if (g_ee24xx_devices[devtype].special)
    {
      if (devtype == EEPROM_24xx00)
        {
          /* Ultra small 16-byte EEPROM */

          eedev->size  = 16;

          /* The device only has BYTE write,
           * which is emulated with 1-byte pages
           */

          eedev->pgsize = 1;
        }
      else if (devtype == EEPROM_24xx1025)
        {
          /* Microchip alien part where the address MSB is << 2 bits */

          ferr("Device 24xx1025 is not supported for the moment, TODO.\n");

          eedev->haddrshift = 2;
          kmm_free(eedev);
          return -ENODEV;
        }
    }

  finfo("EEPROM device %s, %d bytes, %d per page, addrlen %d, %s\n",
        devname, eedev->size, eedev->pgsize, eedev->addrlen,
        eedev->readonly ? "readonly" : "");

#ifdef CONFIG_AT24CS_UUID
  uuidname = kmm_zalloc(strlen(devname) + 8);
  if (!uuidname)
    {
      return -ENOMEM;
    }

  /* register the UUID I2C slave with the same name as the parent
   * EEPROM chip, but with the ".uuid" suffix
   */

  strcpy(uuidname, devname);
  strcat(uuidname, ".uuid");
  ret = register_driver(uuidname, &at24cs_uuid_fops, 0444, eedev);

  kmm_free(uuidname);

  if (OKK != ret)
    {
      ferr("register uuid failed, ret = %d\n", ret);
      return ret;
    }
#endif

  return register_driver(devname, &ee24xx_fops, 0666, eedev);
}
