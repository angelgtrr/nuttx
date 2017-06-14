/****************************************************************************
 * wireless/ieee802154/mac802154_device.c
 *
 *   Copyright (C) 2017 Verge Inc. All rights reserved.
 *   Author: Anthony Merlino <anthony@vergeaero.com>
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>

#include <nuttx/mm/iob.h>

#include <nuttx/wireless/ieee802154/ieee802154_ioctl.h>
#include <nuttx/wireless/ieee802154/ieee802154_mac.h>

#include "mac802154.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Device naming ************************************************************/

#define DEVNAME_FMT    "/dev/ieee%d"
#define DEVNAME_FMTLEN (9 + 3 + 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes the state of one open mac driver instance */

struct mac802154dev_open_s
{
  /* Supports a singly linked list */

  FAR struct mac802154dev_open_s *md_flink;

  /* The following will be true if we are closing */

  volatile bool md_closing;
};

struct mac802154dev_callback_s
{
  /* This holds the information visible to the MAC layer */

  struct mac802154_maccb_s mc_cb;     /* Interface understood by the MAC layer */
  FAR struct mac802154_chardevice_s *mc_priv;    /* Our priv data */
};

struct mac802154_chardevice_s
{
  MACHANDLE md_mac;                     /* Saved binding to the mac layer */
  struct mac802154dev_callback_s md_cb; /* Callback information */
  sem_t md_exclsem;                     /* Exclusive device access */

  /* Hold a list of events */

  bool enableevents : 1;                /* Are events enabled? */
  bool geteventpending : 1;             /* Is there a get event using the semaphore? */
  sem_t geteventsem;                    /* Signaling semaphore for waiting get event */
  FAR struct ieee802154_notif_s *event_head;
  FAR struct ieee802154_notif_s *event_tail;

  /* The following is a singly linked list of open references to the
   * MAC device.
   */

  FAR struct mac802154dev_open_s *md_open;

  /* Hold a list of transactions as a "readahead" buffer */

  bool readpending;                     /* Is there a read using the semaphore? */
  sem_t readsem;                        /* Signaling semaphore for waiting read */
  sq_queue_t dataind_queue; 

#ifndef CONFIG_DISABLE_SIGNALS
  /* MAC Service notification information */

  bool notify_registered;
  struct mac802154dev_notify_s md_notify;
  pid_t md_notify_pid;

#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

 /* Semaphore helpers */

static inline int mac802154dev_takesem(sem_t *sem);
#define mac802154dev_givesem(s) sem_post(s);

static inline void mac802154dev_pushevent(FAR struct mac802154_chardevice_s *dev,
                                FAR struct ieee802154_notif_s *notif);
static inline FAR struct ieee802154_notif_s *
  mac802154dev_popevent(FAR struct mac802154_chardevice_s *dev);

static void mac802154dev_notify(FAR const struct mac802154_maccb_s *maccb,
                                FAR struct ieee802154_notif_s *notif);

static void mac802154dev_rxframe(FAR const struct mac802154_maccb_s *maccb,
                                 FAR struct ieee802154_data_ind_s *ind);

static int  mac802154dev_open(FAR struct file *filep);
static int  mac802154dev_close(FAR struct file *filep);
static ssize_t mac802154dev_read(FAR struct file *filep, FAR char *buffer,
              size_t len);
static ssize_t mac802154dev_write(FAR struct file *filep,
              FAR const char *buffer, size_t len);
static int  mac802154dev_ioctl(FAR struct file *filep, int cmd,
              unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations mac802154dev_fops =
{
  mac802154dev_open , /* open */
  mac802154dev_close, /* close */
  mac802154dev_read , /* read */
  mac802154dev_write, /* write */
  NULL,               /* seek */
  mac802154dev_ioctl  /* ioctl */
#ifndef CONFIG_DISABLE_POLL
  , NULL              /* poll */
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , NULL               /* unlink */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mac802154dev_semtake
 *
 * Description:
 *   Acquire the semaphore used for access serialization.
 *
 ****************************************************************************/

static inline int mac802154dev_takesem(sem_t *sem)
{
  /* Take a count from the semaphore, possibly waiting */

  if (sem_wait(sem) < 0)
    {
      /* EINTR is the only error that we expect */

      int errcode = get_errno();
      DEBUGASSERT(errcode == EINTR);
      return -errcode;
    }

  return OK;
}

/****************************************************************************
 * Name: mac802154dev_pushevent
 *
 * Description:
 *   Push event onto the event queue
 *
 * Assumptions:
 *   Called with the char device struct locked.
 *
 ****************************************************************************/

static inline void mac802154dev_pushevent(FAR struct mac802154_chardevice_s *dev,
                                FAR struct ieee802154_notif_s *notif)
{
  notif->flink = NULL;
  if (!dev->event_head)
    {
      dev->event_head = notif;
      dev->event_tail = notif;
    }
  else
    {
      dev->event_tail->flink = notif;
      dev->event_tail        = notif;
    }
}

/****************************************************************************
 * Name: mac802154dev_popevent
 *
 * Description:
 *   Pop an event off of the event queue
 *
 * Assumptions:
 *   Called with the char device struct locked.
 *
 ****************************************************************************/

static inline FAR struct ieee802154_notif_s *
  mac802154dev_popevent(FAR struct mac802154_chardevice_s *dev)
{
  FAR struct ieee802154_notif_s *notif = dev->event_head;

  if (notif)
    {
      dev->event_head = notif->flink;
      if (!dev->event_head)
        {
          dev->event_head = NULL;
        }
      
      notif->flink = NULL;
    }

  return notif;
}

/****************************************************************************
 * Name: mac802154dev_open
 *
 * Description:
 *   Open the 802.15.4 MAC character device.
 *
 ****************************************************************************/

static int mac802154dev_open(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct mac802154_chardevice_s *dev;
  FAR struct mac802154dev_open_s *opriv;
  int ret;

  DEBUGASSERT(filep != NULL && filep->f_inode != NULL);
  inode = filep->f_inode;

  dev   = inode->i_private;
  DEBUGASSERT(dev != NULL);

  /* Get exclusive access to the MAC driver data structure */

  ret = mac802154dev_takesem(&dev->md_exclsem);
  if (ret < 0)
    {
      wlerr("ERROR: mac802154dev_takesem failed: %d\n", ret);
      return ret;
    }

  /* Allocate a new open struct */

  opriv = (FAR struct mac802154dev_open_s *)
    kmm_zalloc(sizeof(struct mac802154dev_open_s));

  if (!opriv)
    {
      wlerr("ERROR: Failed to allocate new open struct\n");
      ret = -ENOMEM;
      goto errout_with_sem;
    }

  /* Attach the open struct to the device */

  opriv->md_flink = dev->md_open;
  dev->md_open = opriv;

  /* Attach the open struct to the file structure */

  filep->f_priv = (FAR void *)opriv;
  ret = OK;

errout_with_sem:
  mac802154dev_givesem(&dev->md_exclsem);
  return ret;
}

/****************************************************************************
 * Name: mac802154dev_close
 *
 * Description:
 *   Close the 802.15.4 MAC character device.
 *
 ****************************************************************************/

static int mac802154dev_close(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct mac802154_chardevice_s *dev;
  FAR struct mac802154dev_open_s *opriv;
  FAR struct mac802154dev_open_s *curr;
  FAR struct mac802154dev_open_s *prev;
  irqstate_t flags;
  bool closing;
  int ret;

  DEBUGASSERT(filep && filep->f_priv && filep->f_inode);
  opriv = filep->f_priv;
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  dev = (FAR struct mac802154_chardevice_s *)inode->i_private;

  /* Handle an improbable race conditions with the following atomic test
   * and set.
   *
   * This is actually a pretty feeble attempt to handle this.  The
   * improbable race condition occurs if two different threads try to
   * close the driver at the same time.  The rule:  don't do
   * that!  It is feeble because we do not really enforce stale pointer
   * detection anyway.
   */

  flags = enter_critical_section();
  closing = opriv->md_closing;
  opriv->md_closing = true;
  leave_critical_section(flags);

  if (closing)
    {
      /* Another thread is doing the close */

      return OK;
    }

  /* Get exclusive access to the driver structure */

  ret = mac802154dev_takesem(&dev->md_exclsem);
  if (ret < 0)
    {
      wlerr("ERROR: mac802154_takesem failed: %d\n", ret);
      return ret;
    }

  /* Find the open structure in the list of open structures for the device */

  for (prev = NULL, curr = dev->md_open;
       curr && curr != opriv;
       prev = curr, curr = curr->md_flink);

  DEBUGASSERT(curr);
  if (!curr)
    {
      wlerr("ERROR: Failed to find open entry\n");
      ret = -ENOENT;
      goto errout_with_exclsem;
    }

  /* Remove the structure from the device */

  if (prev)
    {
      prev->md_flink = opriv->md_flink;
    }
  else
    {
      dev->md_open = opriv->md_flink;
    }

  /* And free the open structure */

  kmm_free(opriv);

  /* If there are now no open instances of the driver and a signal handler is
   * not registered, purge the list of events.
   */
  
  if (dev->md_open)
    {
      FAR struct ieee802154_notif_s *notif;

      while (dev->event_head != NULL)
        {
          notif = mac802154dev_popevent(dev);
          DEBUGASSERT(notif != NULL);
          mac802154_notif_free(dev->md_mac, notif);
        }
    }

  ret = OK;

errout_with_exclsem:
  mac802154dev_givesem(&dev->md_exclsem);
  return ret;
}

/****************************************************************************
 * Name: mac802154dev_read
 *
 * Description:
 *   Return the last received packet.
 *
 ****************************************************************************/

static ssize_t mac802154dev_read(FAR struct file *filep, FAR char *buffer,
                                 size_t len)
{
  FAR struct inode *inode;
  FAR struct mac802154_chardevice_s *dev;
  FAR struct mac802154dev_rxframe_s *rx;
  FAR struct ieee802154_data_ind_s *ind;
  int ret;

  DEBUGASSERT(filep && filep->f_inode);
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  dev = (FAR struct mac802154_chardevice_s *)inode->i_private;

  /* Check to make sure the buffer is the right size for the struct */

  if (len != sizeof(struct mac802154dev_rxframe_s))
    {
      wlerr("ERROR: buffer isn't a mac802154dev_rxframe_s: %lu\n",
            (unsigned long)len);
      return -EINVAL;
    }

  DEBUGASSERT(buffer != NULL);
  rx = (FAR struct mac802154dev_rxframe_s *)buffer;

  while (1)
    {
      /* Get exclusive access to the driver structure */

      ret = mac802154dev_takesem(&dev->md_exclsem);
      if (ret < 0)
        {
          wlerr("ERROR: mac802154dev_takesem failed: %d\n", ret);
          return ret;
        }

      /* Try popping a data indication off the list */

      ind = (FAR struct ieee802154_data_ind_s *)sq_remfirst(&dev->dataind_queue);
      
      /* If the indication is not null, we can exit the loop and copy the data */

      if (ind != NULL)
        {
          mac802154dev_givesem(&dev->md_exclsem);
          break;
        }

      /* If this is a non-blocking call, or if there is another read operation 
       * already pending, don't block. This driver returns EAGAIN even when 
       * configured as non-blocking if another read operation is already pending
       * This situation should be rare. It will only occur when there are 2 calls
       * to read from separate threads and there was no data in the rx list.
       */

      if ((filep->f_oflags & O_NONBLOCK) || dev->readpending)
        {
          mac802154dev_givesem(&dev->md_exclsem);
          return -EAGAIN;
        }

      dev->readpending = true;
      mac802154dev_givesem(&dev->md_exclsem);

      /* Wait to be signaled when a frame is added to the list */
      
      if (sem_wait(&dev->readsem) < 0)
        {
          DEBUGASSERT(errno == EINTR);
          dev->readpending = false;
          return -EINTR;
        }

      /* Let the loop wrap back around, we will then pop a indication and this
       * time, it should have a data indication
       */
    }
          
 rx->length = (ind->frame->io_len - ind->frame->io_offset);

 /* Copy the data from the IOB to the user supplied struct */

 memcpy(&rx->payload[0], &ind->frame->io_data[ind->frame->io_offset],
        rx->length); 

 memcpy(&rx->meta, ind, sizeof(struct ieee802154_data_ind_s));

 /* Zero out the forward link and IOB reference */
 
 rx->meta.flink = NULL;
 rx->meta.frame = NULL;

 /* Deallocate the data indication */

 ieee802154_ind_free(ind);

 return OK;
}

/****************************************************************************
 * Name: mac802154dev_write
 *
 * Description:
 *   Send a packet over the IEEE802.15.4 network.
 *
 ****************************************************************************/

static ssize_t mac802154dev_write(FAR struct file *filep,
                                  FAR const char *buffer, size_t len)
{
  FAR struct inode *inode;
  FAR struct mac802154_chardevice_s *dev;
  FAR struct mac802154dev_txframe_s *tx;
  FAR struct iob_s *iob;
  int ret;

  DEBUGASSERT(filep && filep->f_inode);
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  dev  = (FAR struct mac802154_chardevice_s *)inode->i_private;

  /* Check if the struct is the correct size */

  if (len != sizeof(struct mac802154dev_txframe_s))
    {
      wlerr("ERROR: buffer isn't a mac802154dev_txframe_s: %lu\n",
            (unsigned long)len);

      return -EINVAL;
    }

  DEBUGASSERT(buffer != NULL);
  tx = (FAR struct mac802154dev_txframe_s *)buffer;

  /* Allocate an IOB to put the frame in */

  iob = iob_alloc(false);
  DEBUGASSERT(iob != NULL);

  iob->io_flink  = NULL;
  iob->io_len    = 0;
  iob->io_offset = 0;
  iob->io_pktlen = 0;

  /* Get the MAC header length */

  ret = mac802154_get_mhrlen(dev->md_mac, &tx->meta);
  if (ret < 0)
    {
      wlerr("ERROR: TX meta-data is invalid");
      return ret;
    }

  iob->io_offset = ret;
  iob->io_len = iob->io_offset;

  memcpy(&iob->io_data[iob->io_offset], tx->payload, tx->length);

  iob->io_len += tx->length;

  /* Pass the request to the MAC layer */

  ret = mac802154_req_data(dev->md_mac, &tx->meta, iob);
  if (ret < 0)
    {
      iob_free(iob);
      wlerr("ERROR: req_data failed %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: mac802154dev_ioctl
 *
 * Description:
 *   Control the MAC layer via IOCTL commands.
 *
 ****************************************************************************/

static int mac802154dev_ioctl(FAR struct file *filep, int cmd,
                              unsigned long arg)
{
  FAR struct inode *inode;
  FAR struct mac802154_chardevice_s *dev;
  int ret;

  DEBUGASSERT(filep != NULL && filep->f_priv != NULL &&
              filep->f_inode != NULL);
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  dev = (FAR struct mac802154_chardevice_s *)inode->i_private;

  /* Get exclusive access to the driver structure */

  ret = mac802154dev_takesem(&dev->md_exclsem);
  if (ret < 0)
    {
      wlerr("ERROR: mac802154dev_takesem failed: %d\n", ret);
      return ret;
    }

  /* Handle the ioctl command */

  switch (cmd)
    {
#ifndef CONFIG_DISABLE_SIGNALS
      /* Command:     MAC802154IOC_MLME_REGISTER, MAC802154IOC_MCPS_REGISTER
       * Description: Register to receive a signal whenever there is a
       *              event primitive sent from the MAC layer.
       * Argument:    A read-only pointer to an instance of struct
       *              mac802154dev_notify_s
       * Return:      Zero (OK) on success.  Minus one will be returned on
       *              failure with the errno value set appropriately.
       */

      case MAC802154IOC_NOTIFY_REGISTER:
        {
          FAR struct mac802154dev_notify_s *notify =
            (FAR struct mac802154dev_notify_s *)((uintptr_t)arg);

          if (notify)
            {
              /* Save the notification events */

              dev->md_notify.mn_signo      = notify->mn_signo;
              dev->md_notify_pid           = getpid();
              dev->notify_registered = true;

              ret = OK;
            }
          else
            {
              ret = -EINVAL;
            }
        }
        break;
#endif

      case MAC802154IOC_GET_EVENT:
        {
          FAR struct ieee802154_notif_s *usr_notif =
            (FAR struct ieee802154_notif_s *)((uintptr_t)arg);
          FAR struct ieee802154_notif_s *notif;

          while (1)
            {
              /* Try popping an event off the queue */

              notif = mac802154dev_popevent(dev);

              /* If there was an event to pop off, copy it into the user data and
               * free it from the MAC layer's memory.
               */

              if (notif != NULL)
                {
                  memcpy(usr_notif, notif, sizeof(struct ieee802154_notif_s));

                  /* Free the notification */

                  mac802154_notif_free(dev->md_mac, notif);
                  ret = OK;
                  break;
                }
              
              /* If this is a non-blocking call, or if there is another getevent
               * operation already pending, don't block. This driver returns
               * EAGAIN even when configured as non-blocking if another getevent
               * operation is already pending This situation should be rare.
               * It will only occur when there are 2 calls from separate threads
               * and there was no events in the queue.
               */

              if ((filep->f_oflags & O_NONBLOCK) || dev->geteventpending)
                {
                  ret = -EAGAIN;
                  break;
                }

              dev->geteventpending = true;
              mac802154dev_givesem(&dev->md_exclsem);

              /* Wait to be signaled when an event is queued */

              if (sem_wait(&dev->geteventsem) < 0)
                {
                  DEBUGASSERT(errno == EINTR);
                  dev->geteventpending = false;
                  return -EINTR;
                }

              /* Get exclusive access again, then loop back around and try and
               * pop an event off the queue
               */

                ret = mac802154dev_takesem(&dev->md_exclsem);
                if (ret < 0)
                  {
                    wlerr("ERROR: mac802154dev_takesem failed: %d\n", ret);
                    return ret;
                  }
            }
        }
        break;
      
      case MAC802154IOC_ENABLE_EVENTS:
        {
          dev->enableevents = (bool)arg;
          ret = OK;
        }
        break;

      default:
        {
          /* Forward any unrecognized commands to the MAC layer */
          
          ret = mac802154_ioctl(dev->md_mac, cmd, arg);
        }
        break;
    }

  mac802154dev_givesem(&dev->md_exclsem);
  return ret;
}

static void mac802154dev_notify(FAR const struct mac802154_maccb_s *maccb,
                                FAR struct ieee802154_notif_s *notif)
{
  FAR struct mac802154dev_callback_s *cb =
    (FAR struct mac802154dev_callback_s *)maccb;
  FAR struct mac802154_chardevice_s *dev;

  DEBUGASSERT(cb != NULL && cb->mc_priv != NULL);
  dev = cb->mc_priv;

  /* Get exclusive access to the driver structure.  We don't care about any
   * signals so if we see one, just go back to trying to get access again */

  while (mac802154dev_takesem(&dev->md_exclsem) != 0);

  /* If there is a registered notification receiver, queue the event and signal
   * the receiver. Events should be popped from the queue from the application
   * at a reasonable rate in order for the MAC layer to be able to allocate new
   * notifications.
   */

  if (dev->enableevents && (dev->md_open != NULL || dev->notify_registered)) 
    {
      mac802154dev_pushevent(dev, notif);

      /* Check if there is a read waiting for data */

      if (dev->geteventpending)
        {
          /* Wake the thread waiting for the data transmission */

          dev->geteventpending = false;
          sem_post(&dev->geteventsem);
        }

#ifndef CONFIG_DISABLE_SIGNALS 
      if (dev->notify_registered)
        {

#ifdef CONFIG_CAN_PASS_STRUCTS
          union sigval value;
          value.sival_int = (int)notif->notiftype;
          (void)sigqueue(dev->md_notify_pid, dev->md_notify.mn_signo, value);
#else
          (void)sigqueue(dev->md_notify_pid, dev->md_notify.mn_signo,
                         (FAR void *)notif->notiftype);
#endif
        }
#endif 
    }
  else
    {
      /* Just free the event if the driver is closed and there isn't a registered
       * signal number.
       */

      mac802154_notif_free(dev->md_mac, notif);
    }

  /* Release the driver */

  mac802154dev_givesem(&dev->md_exclsem);
}

static void mac802154dev_rxframe(FAR const struct mac802154_maccb_s *maccb,
                                 FAR struct ieee802154_data_ind_s *ind)
{
  FAR struct mac802154dev_callback_s *cb =
    (FAR struct mac802154dev_callback_s *)maccb;
  FAR struct mac802154_chardevice_s *dev;

  DEBUGASSERT(cb != NULL && cb->mc_priv != NULL);
  dev = cb->mc_priv;

  /* Get exclusive access to the driver structure.  We don't care about any
   * signals so if we see one, just go back to trying to get access again */

  while (mac802154dev_takesem(&dev->md_exclsem) != 0);

  /* Push the indication onto the list */

  sq_addlast((FAR sq_entry_t *)ind, &dev->dataind_queue);

  /* Check if there is a read waiting for data */

  if (dev->readpending)
    {
      /* Wake the thread waiting for the data transmission */

      dev->readpending = false;
      sem_post(&dev->readsem);
    }
 
  /* Release the driver */

  mac802154dev_givesem(&dev->md_exclsem);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mac802154dev_register
 *
 * Description:
 *   Register a character driver to access the IEEE 802.15.4 MAC layer from
 *   user-space
 *
 * Input Parameters:
 *   mac - Pointer to the mac layer struct to be registerd.
 *   minor - The device minor number.  The IEEE802.15.4 MAC character device
 *     will be registered as /dev/ieeeN where N is the minor number
 *
 * Returned Values:
 *   Zero (OK) is returned on success.  Otherwise a negated errno value is
 *   returned to indicate the nature of the failure.
 *
 ****************************************************************************/

int mac802154dev_register(MACHANDLE mac, int minor)
{
  FAR struct mac802154_chardevice_s *dev;
  FAR struct mac802154_maccb_s *maccb;
  char devname[DEVNAME_FMTLEN];
  int ret;

  dev = kmm_zalloc(sizeof(struct mac802154_chardevice_s));
  if (!dev)
    {
      wlerr("ERROR: Failed to allocate device structure\n");
      return -ENOMEM;
    }

  /* Initialize the new mac driver instance */

  dev->md_mac = mac;
  sem_init(&dev->md_exclsem, 0, 1); /* Allow the device to be opened once
                                     * before blocking */
                                     
  sem_init(&dev->readsem, 0, 0);
  sem_setprotocol(&dev->readsem, SEM_PRIO_NONE);
  dev->readpending = false;

  sq_init(&dev->dataind_queue);

  dev->geteventpending = false;
  sem_init(&dev->geteventsem, 0, 0);
  sem_setprotocol(&dev->geteventsem, SEM_PRIO_NONE);

  dev->event_head = NULL;
  dev->event_tail = NULL;

  dev->enableevents = true;
  dev->notify_registered = false;

  /* Initialize the MAC callbacks */

  dev->md_cb.mc_priv  = dev;

  maccb           = &dev->md_cb.mc_cb;
  maccb->notify   = mac802154dev_notify;
  maccb->rxframe  = mac802154dev_rxframe;

  /* Bind the callback structure */

  ret = mac802154_bind(mac, maccb);
  if (ret < 0)
    {
      nerr("ERROR: Failed to bind the MAC callbacks: %d\n", ret);

      /* Free memory and return the error */
      kmm_free(dev);
      return ret;
    }

  /* Create the character device name */

  snprintf(devname, DEVNAME_FMTLEN, DEVNAME_FMT, minor);

  /* Register the mac character driver */

  ret = register_driver(devname, &mac802154dev_fops, 0666, dev);
  if (ret < 0)
    {
      wlerr("ERROR: register_driver failed: %d\n", ret);
      goto errout_with_priv;
    }

  return OK;

errout_with_priv:
  sem_destroy(&dev->md_exclsem);
  kmm_free(dev);
  return ret;
}
