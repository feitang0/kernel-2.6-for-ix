/*
 *  linux/drivers/message/fusion/mptscsih.c
 *      For use with LSI PCI chip/adapter(s)
 *      running LSI Fusion MPT (Message Passing Technology) firmware.
 *
 *  Copyright (c) 1999-2008 LSI Corporation
 *  (mailto:DL-MPTFusionLinux@lsi.com)
 *
 */
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; version 2 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    NO WARRANTY
    THE PROGRAM IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OR
    CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED INCLUDING, WITHOUT
    LIMITATION, ANY WARRANTIES OR CONDITIONS OF TITLE, NON-INFRINGEMENT,
    MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Each Recipient is
    solely responsible for determining the appropriateness of using and
    distributing the Program and assumes all risks associated with its
    exercise of rights under this Agreement, including but not limited to
    the risks and costs of program errors, damage to or loss of data,
    programs or equipment, and unavailability or interruption of operations.

    DISCLAIMER OF LIABILITY
    NEITHER RECIPIENT NOR ANY CONTRIBUTORS SHALL HAVE ANY LIABILITY FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING WITHOUT LIMITATION LOST PROFITS), HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
    TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OR DISTRIBUTION OF THE PROGRAM OR THE EXERCISE OF ANY RIGHTS GRANTED
    HEREUNDER, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/kdev_t.h>
#include <linux/blkdev.h>
#include <linux/delay.h>	/* for mdelay */
#include <linux/interrupt.h>	/* needed for in_interrupt() proto */
#include <linux/reboot.h>	/* notifier code */
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/pci.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_dbg.h>

#include "mptbase.h"
#include "mptscsih.h"
#include "lsi/mpi_log_sas.h"

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
#define my_NAME		"Fusion MPT SCSI Host driver"
#define my_VERSION	MPT_LINUX_VERSION_COMMON
#define MYNAM		"mptscsih"

MODULE_AUTHOR(MODULEAUTHOR);
MODULE_DESCRIPTION(my_NAME);
MODULE_LICENSE("GPL");
MODULE_VERSION(my_VERSION);

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
typedef struct _BIG_SENSE_BUF {
	u8		data[MPT_SENSE_BUFFER_ALLOC];
} BIG_SENSE_BUF;


/*
 *  Other private/forward protos...
 */
struct scsi_cmnd * mptscsih_get_scsi_lookup(MPT_ADAPTER *ioc, int i);
static struct scsi_cmnd * mptscsih_getclear_scsi_lookup(MPT_ADAPTER *ioc, int i);
static void	mptscsih_set_scsi_lookup(MPT_ADAPTER *ioc, int i, struct scsi_cmnd *scmd);
static int	SCPNT_TO_LOOKUP_IDX(MPT_ADAPTER *ioc, struct scsi_cmnd *scmd);
int		mptscsih_io_done(MPT_ADAPTER *ioc, MPT_FRAME_HDR *mf, MPT_FRAME_HDR *r);
static void	mptscsih_report_queue_full(struct scsi_cmnd *sc, SCSIIOReply_t *pScsiReply, SCSIIORequest_t *pScsiReq);
int		mptscsih_taskmgmt_complete(MPT_ADAPTER *ioc, MPT_FRAME_HDR *mf, MPT_FRAME_HDR *r);

static int	mptscsih_AddSGE(MPT_ADAPTER *ioc, struct scsi_cmnd *SCpnt,
				 SCSIIORequest_t *pReq, int req_idx);
static void	mptscsih_freeChainBuffers(MPT_ADAPTER *ioc, int req_idx);
static void	mptscsih_copy_sense_data(struct scsi_cmnd *sc, MPT_SCSI_HOST *hd, MPT_FRAME_HDR *mf, SCSIIOReply_t *pScsiReply);

int		mptscsih_ioc_reset(MPT_ADAPTER *ioc, int post_reset);
int		mptscsih_event_process(MPT_ADAPTER *ioc, EventNotificationReply_t *pEvReply);
static void	mptscsih_synchronize_cache(struct scsi_device *sdev, MPT_SCSI_HOST *hd, VirtDevice *vdevice);

void		mptscsih_remove(struct pci_dev *);
void		mptscsih_shutdown(struct pci_dev *);
#ifdef CONFIG_PM
int		mptscsih_suspend(struct pci_dev *pdev, pm_message_t state);
int		mptscsih_resume(struct pci_dev *pdev);
#endif
static int mptscsih_taskmgmt_reply(MPT_ADAPTER *ioc, u8 type,
		SCSITaskMgmtReply_t *pScsiTmReply);
static int mptscsih_get_completion_code(MPT_ADAPTER *ioc, MPT_FRAME_HDR *req,
		MPT_FRAME_HDR *reply);

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_getFreeChainBuffer - Function to get a free chain
 *	from the MPT_SCSI_HOST FreeChainQ.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@req_idx: Index of the SCSI IO request frame. (output)
 *
 *	return SUCCESS or FAILED
 */
static inline int
mptscsih_getFreeChainBuffer(MPT_ADAPTER *ioc, int *retIndex)
{
	MPT_FRAME_HDR *chainBuf;
	unsigned long flags;
	int rc;
	int chain_idx;

	dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT "getFreeChainBuffer called\n",
	    ioc->name));
	spin_lock_irqsave(&ioc->FreeQlock, flags);
	if (!list_empty(&ioc->FreeChainQ)) {
		int offset;

		chainBuf = list_entry(ioc->FreeChainQ.next, MPT_FRAME_HDR,
				u.frame.linkage.list);
		list_del(&chainBuf->u.frame.linkage.list);
		offset = (u8 *)chainBuf - (u8 *)ioc->ChainBuffer;
		chain_idx = offset / ioc->req_sz;
		rc = SUCCESS;
		dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "getFreeChainBuffer chainBuf=%p ChainBuffer=%p offset=%d chain_idx=%d\n",
		    ioc->name, chainBuf, ioc->ChainBuffer, offset, chain_idx));
	} else {
		rc = FAILED;
		chain_idx = MPT_HOST_NO_CHAIN;
		dfailprintk(ioc, printk(MYIOC_s_ERR_FMT "getFreeChainBuffer failed\n",
		    ioc->name));
	}
	spin_unlock_irqrestore(&ioc->FreeQlock, flags);

	*retIndex = chain_idx;
	return rc;
} /* mptscsih_getFreeChainBuffer() */

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_AddSGE - Add a SGE (plus chain buffers) to the
 *	SCSIIORequest_t Message Frame.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@SCpnt: Pointer to scsi_cmnd structure
 *	@pReq: Pointer to SCSIIORequest_t structure
 *
 *	Returns ...
 */
static int
mptscsih_AddSGE(MPT_ADAPTER *ioc, struct scsi_cmnd *SCpnt,
		SCSIIORequest_t *pReq, int req_idx)
{
	char	*psge;
	char	*chainSge;
	struct scatterlist *sg;
	int	 frm_sz;
	int	 sges_left, sg_done;
	int	 chain_idx = MPT_HOST_NO_CHAIN;
	int	 sgeOffset;
	int	 numSgeSlots, numSgeThisFrame;
	u32	 sgflags, sgdir, thisxfer = 0;
	int	 chain_dma_off = 0;
	int	 newIndex;
	int	 ii;
	dma_addr_t v2;
	u32	RequestNB;

#ifdef EEDP_SUPPORT
	if (pReq->Function == MPI_FUNCTION_SCSI_IO_32) {
		SCSIIO32Request_t *mpi_request = (SCSIIO32Request_t *)pReq;

		sgdir = le32_to_cpu(mpi_request->Control)
		    & MPI_SCSIIO_CONTROL_DATADIRECTION_MASK;
		psge = (char *) &mpi_request->SGL;
	} else {
#endif
		sgdir = le32_to_cpu(pReq->Control) & MPI_SCSIIO_CONTROL_DATADIRECTION_MASK;
		psge = (char *) &pReq->SGL;
#ifdef EEDP_SUPPORT
	}
#endif
	if (sgdir == MPI_SCSIIO_CONTROL_WRITE)  {
		sgdir = MPT_TRANSFER_HOST_TO_IOC;
	} else {
		sgdir = MPT_TRANSFER_IOC_TO_HOST;
	}

	frm_sz = ioc->req_sz;


	/* Map the data portion, if any.
	 * sges_left  = 0 if no data transfer.
	 */
	sges_left = scsi_dma_map(SCpnt);
	if (sges_left < 0)
		return FAILED;

	/* Handle the SG case.
	 */
	sg = scsi_sglist(SCpnt);
	sg_done  = 0;
	sgeOffset = psge - (char *) pReq;
	chainSge = NULL;

	/* Prior to entering this loop - the following must be set
	 * current MF:  sgeOffset (bytes)
	 *              chainSge (Null if original MF is not a chain buffer)
	 *              sg_done (num SGE done for this MF)
	 */

nextSGEset:
	numSgeSlots = ((frm_sz - sgeOffset) / ioc->SGE_size);
	numSgeThisFrame = (sges_left < numSgeSlots) ? sges_left : numSgeSlots;

	sgflags = MPT_SGE_FLAGS_SIMPLE_ELEMENT | sgdir;

	/* Get first (num - 1) SG elements
	 * Skip any SG entries with a length of 0
	 * NOTE: at finish, sg and psge pointed to NEXT data/location positions
	 */
	for (ii=0; ii < (numSgeThisFrame-1); ii++) {
		thisxfer = sg_dma_len(sg);
		if (thisxfer == 0) {
			/* Get next SG element from the OS */
			sg = mpt_sg_next(sg);
			sg_done++;
			continue;
		}

		v2 = sg_dma_address(sg);
		ioc->add_sge(psge, sgflags | thisxfer, v2);

		/* Get next SG element from the OS */
		sg = mpt_sg_next(sg);
		psge += ioc->SGE_size;
		sgeOffset += ioc->SGE_size;
		sg_done++;
	}

	if (numSgeThisFrame == sges_left) {
		/* Add last element, end of buffer and end of list flags.
		 */
		sgflags |= MPT_SGE_FLAGS_LAST_ELEMENT |
				MPT_SGE_FLAGS_END_OF_BUFFER |
				MPT_SGE_FLAGS_END_OF_LIST;

		/* Add last SGE and set termination flags.
		 * Note: Last SGE may have a length of 0 - which should be ok.
		 */
		thisxfer = sg_dma_len(sg);

		v2 = sg_dma_address(sg);
		ioc->add_sge(psge, sgflags | thisxfer, v2);
		sgeOffset += ioc->SGE_size;
		sg_done++;

		if (chainSge) {
			/* The current buffer is a chain buffer,
			 * but there is not another one.
			 * Update the chain element
			 * Offset and Length fields.
			 */
			ioc->add_chain((char *)chainSge, 0, sgeOffset,
				ioc->ChainBufferDMA + chain_dma_off);
		} else {
			/* The current buffer is the original MF
			 * and there is no Chain buffer.
			 */
			pReq->ChainOffset = 0;
			RequestNB = (((sgeOffset - 1) >> ioc->NBShiftFactor)  + 1) & 0x03;
			dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			    "Single Buffer RequestNB=%x, sgeOffset=%d\n", ioc->name, RequestNB, sgeOffset));
			ioc->RequestNB[req_idx] = RequestNB;
		}
	} else {
		/* At least one chain buffer is needed.
		 * Complete the first MF
		 *  - last SGE element, set the LastElement bit
		 *  - set ChainOffset (words) for orig MF
		 *             (OR finish previous MF chain buffer)
		 *  - update MFStructPtr ChainIndex
		 *  - Populate chain element
		 * Also
		 * Loop until done.
		 */

		dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT "SG: Chain Required! sg done %d\n",
				ioc->name, sg_done));

		/* Set LAST_ELEMENT flag for last non-chain element
		 * in the buffer. Since psge points at the NEXT
		 * SGE element, go back one SGE element, update the flags
		 * and reset the pointer. (Note: sgflags & thisxfer are already
		 * set properly).
		 */
		if (sg_done) {
			u32 *ptmp = (u32 *) (psge - ioc->SGE_size);
			sgflags = le32_to_cpu(*ptmp);
			sgflags |= MPT_SGE_FLAGS_LAST_ELEMENT;
			*ptmp = cpu_to_le32(sgflags);
		}

		if (chainSge) {
			/* The current buffer is a chain buffer.
			 * chainSge points to the previous Chain Element.
			 * Update its chain element Offset and Length (must
			 * include chain element size) fields.
			 * Old chain element is now complete.
			 */
			u8 nextChain = (u8) (sgeOffset >> 2);
			sgeOffset += ioc->SGE_size;
			ioc->add_chain((char *)chainSge, nextChain, sgeOffset,
					 ioc->ChainBufferDMA + chain_dma_off);
		} else {
			/* The original MF buffer requires a chain buffer -
			 * set the offset.
			 * Last element in this MF is a chain element.
			 */
			pReq->ChainOffset = (u8) (sgeOffset >> 2);
			RequestNB = (((sgeOffset - 1) >> ioc->NBShiftFactor)  + 1) & 0x03;
			dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT "Chain Buffer Needed, RequestNB=%x sgeOffset=%d\n", ioc->name, RequestNB, sgeOffset));
			ioc->RequestNB[req_idx] = RequestNB;
		}

		sges_left -= sg_done;


		/* NOTE: psge points to the beginning of the chain element
		 * in current buffer. Get a chain buffer.
		 */
		if ((mptscsih_getFreeChainBuffer(ioc, &newIndex)) == FAILED) {
			dfailprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			    "getFreeChainBuffer FAILED SCSI cmd=%02x (%p)\n",
			    ioc->name, pReq->CDB[0], SCpnt));
			return FAILED;
		}

		/* Update the tracking arrays.
		 * If chainSge == NULL, update ReqToChain, else ChainToChain
		 */
		if (chainSge) {
			ioc->ChainToChain[chain_idx] = newIndex;
		} else {
			ioc->ReqToChain[req_idx] = newIndex;
		}
		chain_idx = newIndex;
		chain_dma_off = ioc->req_sz * chain_idx;

		/* Populate the chainSGE for the current buffer.
		 * - Set chain buffer pointer to psge and fill
		 *   out the Address and Flags fields.
		 */
		chainSge = (char *) psge;
		dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT "  Current buff @ %p (index 0x%x)",
		    ioc->name, psge, req_idx));

		/* Start the SGE for the next buffer
		 */
		psge = (char *) (ioc->ChainBuffer + chain_dma_off);
		sgeOffset = 0;
		sg_done = 0;

		dsgprintk(ioc, printk(MYIOC_s_DEBUG_FMT "  Chain buff @ %p (index 0x%x)\n",
		    ioc->name, psge, chain_idx));

		/* Start the SGE for the next buffer
		 */

		goto nextSGEset;
	}

	return SUCCESS;
} /* mptscsih_AddSGE() */

static void
mptscsih_issue_sep_command(MPT_ADAPTER *ioc, VirtTarget *vtarget,
    U32 SlotStatus)
{
	MPT_FRAME_HDR *mf;
	SEPRequest_t	 *SEPMsg;

	if (ioc->bus_type != SAS)
		return;

	/* Not supported for hidden raid components
	 */
	if (vtarget->tflags & MPT_TARGET_FLAGS_RAID_COMPONENT)
		return;

	if ((mf = mpt_get_msg_frame(ioc->InternalCtx, ioc)) == NULL) {
		dfailprintk(ioc, printk(MYIOC_s_WARN_FMT "%s: no msg frames!!\n",
		    ioc->name,__func__));
		return;
	}

	SEPMsg = (SEPRequest_t *)mf;
	SEPMsg->Function = MPI_FUNCTION_SCSI_ENCLOSURE_PROCESSOR;
	SEPMsg->Bus = vtarget->channel;
	SEPMsg->TargetID = vtarget->id;
	SEPMsg->Action = MPI_SEP_REQ_ACTION_WRITE_STATUS;
	SEPMsg->SlotStatus = SlotStatus;
	devtverboseprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "Sending SEP cmd=%x channel=%d id=%d\n",
	    ioc->name, SlotStatus, SEPMsg->Bus, SEPMsg->TargetID));
	mpt_put_msg_frame(ioc->DoneCtx, ioc, mf);
}

#ifdef CONFIG_FUSION_LOGGING
/**
 *	mptscsih_info_scsiio - debug print info on reply frame
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@sc: original scsi cmnd pointer
 *	@pScsiReply: Pointer to MPT reply frame
 *
 *	MPT_DEBUG_REPLY needs to be enabled to obtain this info
 *
 *	Refer to lsi/mpi.h.
 **/
static void
mptscsih_info_scsiio(MPT_ADAPTER *ioc, struct scsi_cmnd *sc, SCSIIOReply_t * pScsiReply)
{
	char	*desc = NULL;
	char	*desc1 = NULL;
	u16	ioc_status;
	u8	skey, asc, ascq;

	ioc_status = le16_to_cpu(pScsiReply->IOCStatus) & MPI_IOCSTATUS_MASK;

	switch (ioc_status) {

	case MPI_IOCSTATUS_SUCCESS:
		desc = "success";
		break;
	case MPI_IOCSTATUS_SCSI_INVALID_BUS:
		desc = "invalid bus";
		break;
	case MPI_IOCSTATUS_SCSI_INVALID_TARGETID:
		desc = "invalid target_id";
		break;
	case MPI_IOCSTATUS_SCSI_DEVICE_NOT_THERE:
		desc = "device not there";
		break;
	case MPI_IOCSTATUS_SCSI_DATA_OVERRUN:
		desc = "data overrun";
		break;
	case MPI_IOCSTATUS_SCSI_DATA_UNDERRUN:
		desc = "data underrun";
		break;
	case MPI_IOCSTATUS_SCSI_IO_DATA_ERROR:
		desc = "I/O data error";
		break;
	case MPI_IOCSTATUS_SCSI_PROTOCOL_ERROR:
		desc = "protocol error";
		break;
	case MPI_IOCSTATUS_SCSI_TASK_TERMINATED:
		desc = "task terminated";
		break;
	case MPI_IOCSTATUS_SCSI_RESIDUAL_MISMATCH:
		desc = "residual mismatch";
		break;
	case MPI_IOCSTATUS_SCSI_TASK_MGMT_FAILED:
		desc = "task management failed";
		break;
	case MPI_IOCSTATUS_SCSI_IOC_TERMINATED:
		desc = "IOC terminated";
		break;
	case MPI_IOCSTATUS_SCSI_EXT_TERMINATED:
		desc = "ext terminated";
		break;
	default:
		desc = "";
		break;
	}

	switch (pScsiReply->SCSIStatus)
	{

	case MPI_SCSI_STATUS_SUCCESS:
		desc1 = "success";
		break;
	case MPI_SCSI_STATUS_CHECK_CONDITION:
		desc1 = "check condition";
		break;
	case MPI_SCSI_STATUS_CONDITION_MET:
		desc1 = "condition met";
		break;
	case MPI_SCSI_STATUS_BUSY:
		desc1 = "busy";
		break;
	case MPI_SCSI_STATUS_INTERMEDIATE:
		desc1 = "intermediate";
		break;
	case MPI_SCSI_STATUS_INTERMEDIATE_CONDMET:
		desc1 = "intermediate condmet";
		break;
	case MPI_SCSI_STATUS_RESERVATION_CONFLICT:
		desc1 = "reservation conflict";
		break;
	case MPI_SCSI_STATUS_COMMAND_TERMINATED:
		desc1 = "command terminated";
		break;
	case MPI_SCSI_STATUS_TASK_SET_FULL:
		desc1 = "task set full";
		break;
	case MPI_SCSI_STATUS_ACA_ACTIVE:
		desc1 = "aca active";
		break;
	case MPI_SCSI_STATUS_FCPEXT_DEVICE_LOGGED_OUT:
		desc1 = "fcpext device logged out";
		break;
	case MPI_SCSI_STATUS_FCPEXT_NO_LINK:
		desc1 = "fcpext no link";
		break;
	case MPI_SCSI_STATUS_FCPEXT_UNASSIGNED:
		desc1 = "fcpext unassigned";
		break;
	default:
		desc1 = "";
		break;
	}

	scsi_print_command(sc);
	printk(MYIOC_s_DEBUG_FMT "\tfw_channel = %d, fw_id = %d, lun = %d\n",
	    ioc->name, pScsiReply->Bus, pScsiReply->TargetID, sc->device->lun);
	printk(MYIOC_s_DEBUG_FMT "\trequest_len = %d, underflow = %d, "
	    "resid = %d\n", ioc->name, scsi_bufflen(sc), sc->underflow,
	    scsi_get_resid(sc));
	printk(MYIOC_s_DEBUG_FMT "\ttag = %d, transfer_count = %d, "
	    "sc->result = %08X\n", ioc->name, le16_to_cpu(pScsiReply->TaskTag),
	    le32_to_cpu(pScsiReply->TransferCount), sc->result);

	printk(MYIOC_s_DEBUG_FMT "\tiocstatus = %s (0x%04x), "
	    "scsi_status = %s (0x%02x), scsi_state = (0x%02x)\n",
	    ioc->name, desc, ioc_status, desc1, pScsiReply->SCSIStatus,
	    pScsiReply->SCSIState);

	if (pScsiReply->SCSIState & MPI_SCSI_STATE_AUTOSENSE_VALID) {
		skey = sc->sense_buffer[2] & 0x0F;
		asc = sc->sense_buffer[12];
		ascq = sc->sense_buffer[13];

		printk(MYIOC_s_DEBUG_FMT "\t[sense_key,asc,ascq]: "
		    "[0x%02x,0x%02x,0x%02x]\n", ioc->name, skey, asc, ascq);
	}

	/*
	 *  Look for + dump FCP ResponseInfo[]!
	 */
	if (pScsiReply->SCSIState & MPI_SCSI_STATE_RESPONSE_INFO_VALID &&
	    pScsiReply->ResponseInfo)
		printk(MYIOC_s_DEBUG_FMT "response_info = %08xh\n",
		    ioc->name, le32_to_cpu(pScsiReply->ResponseInfo));
}
#endif

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_io_done - Main SCSI IO callback routine registered to
 *	Fusion MPT (base) driver
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@mf: Pointer to original MPT request frame
 *	@r: Pointer to MPT reply frame (NULL if TurboReply)
 *
 *	This routine is called from mpt.c::mpt_interrupt() at the completion
 *	of any SCSI IO request.
 *	This routine is registered with the Fusion MPT (base) driver at driver
 *	load/init time via the mpt_register() API call.
 *
 *	Returns 1 indicating alloc'd request frame ptr should be freed.
 */
int
mptscsih_io_done(MPT_ADAPTER *ioc, MPT_FRAME_HDR *mf, MPT_FRAME_HDR *mr)
{
	struct scsi_cmnd	*sc;
	MPT_SCSI_HOST	*hd;
	SCSIIORequest_t	*pScsiReq;
	SCSIIOReply_t	*pScsiReply;
	u16		 req_idx, req_idx_MR;
	VirtDevice	 *vdevice;
	VirtTarget	 *vtarget;

	hd = shost_priv(ioc->sh);
	req_idx = le16_to_cpu(mf->u.frame.hwhdr.msgctxu.fld.req_idx);
	req_idx_MR = (mr != NULL) ?
	    le16_to_cpu(mr->u.frame.hwhdr.msgctxu.fld.req_idx) : req_idx;
	if ((req_idx != req_idx_MR) ||
	    (le32_to_cpu(mf->u.frame.linkage.arg1) == 0xdeadbeaf)) {
		printk(MYIOC_s_ERR_FMT "Received a mf that was already freed\n",
		    ioc->name);
		printk (MYIOC_s_ERR_FMT
		    "req_idx=%x req_idx_MR=%x mf=%p mr=%p sc=%p\n",
		    ioc->name, req_idx, req_idx_MR, mf, mr,
		    mptscsih_get_scsi_lookup(ioc, req_idx_MR));
		return 0;
	}

	sc = mptscsih_getclear_scsi_lookup(ioc, req_idx);
	if (sc == NULL) {
		MPIHeader_t *hdr = (MPIHeader_t *)mf;

		/* Remark: writeSDP1 will use the ScsiDoneCtx
		 * If a SCSI I/O cmd, device disabled by OS and
		 * completion done. Cannot touch sc struct. Just free mem.
		 */
		if (hdr->Function == MPI_FUNCTION_SCSI_IO_REQUEST)
			printk(MYIOC_s_ERR_FMT "NULL ScsiCmd ptr!\n",
			ioc->name);

		mptscsih_freeChainBuffers(ioc, req_idx);
		return 1;
	}

	if ((unsigned char *)mf != sc->host_scribble) {
		mptscsih_freeChainBuffers(ioc, req_idx);
		return 1;
	}

	if (ioc->bus_type == SAS) {
		VirtDevice *vdevice = sc->device->hostdata;

		if (!vdevice || !vdevice->vtarget ||
		    vdevice->vtarget->deleted) {
			sc->result = DID_NO_CONNECT << 16;
			goto out;
		}
	}

	sc->host_scribble = NULL;
	sc->result = DID_OK << 16;		/* Set default reply as OK */
	pScsiReq = (SCSIIORequest_t *) mf;
	pScsiReply = (SCSIIOReply_t *) mr;

	if((ioc->facts.MsgVersion >= MPI_VERSION_01_05) && pScsiReply){
		dmfprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			"ScsiDone (mf=%p,mr=%p,sc=%p,idx=%d,task-tag=%d)\n",
			ioc->name, mf, mr, sc, req_idx, pScsiReply->TaskTag));
	}else{
		dmfprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			"ScsiDone (mf=%p,mr=%p,sc=%p,idx=%d)\n",
			ioc->name, mf, mr, sc, req_idx));
	}

	if (pScsiReply == NULL) {
		/* special context reply handling */
		;
	} else {
		u32	 xfer_cnt;
		u32	 difftransfer;
		u16	 status;
		u8	 scsi_state, scsi_status;
		u32	 log_info;

		status = le16_to_cpu(pScsiReply->IOCStatus) & MPI_IOCSTATUS_MASK;
		scsi_state = pScsiReply->SCSIState;
		scsi_status = pScsiReply->SCSIStatus;
		xfer_cnt = le32_to_cpu(pScsiReply->TransferCount);
		scsi_set_resid(sc, scsi_bufflen(sc) - xfer_cnt);
		log_info = le32_to_cpu(pScsiReply->IOCLogInfo);
		vdevice = sc->device->hostdata;

		/*
		 *  if we get a data underrun indication, yet no data was
		 *  transferred and the SCSI status indicates that the
		 *  command was never started, change the data underrun
		 *  to success
		 */
		if (status == MPI_IOCSTATUS_SCSI_DATA_UNDERRUN && xfer_cnt == 0 &&
		    (scsi_status == MPI_SCSI_STATUS_BUSY ||
		     scsi_status == MPI_SCSI_STATUS_RESERVATION_CONFLICT ||
		     scsi_status == MPI_SCSI_STATUS_TASK_SET_FULL)) {
			status = MPI_IOCSTATUS_SUCCESS;
		}

		if (scsi_state & MPI_SCSI_STATE_AUTOSENSE_VALID)
			mptscsih_copy_sense_data(sc, hd, mf, pScsiReply);

		switch(status) {
		case MPI_IOCSTATUS_BUSY:			/* 0x0002 */
		case MPI_IOCSTATUS_INSUFFICIENT_RESOURCES:	/* 0x0006 */
			/* CHECKME!
			 * Maybe: DRIVER_BUSY | SUGGEST_RETRY | DID_SOFT_ERROR (retry)
			 * But not: DID_BUS_BUSY lest one risk
			 * killing interrupt handler:-(
			 */
			sc->result = SAM_STAT_BUSY;
			break;

		case MPI_IOCSTATUS_SCSI_INVALID_BUS:		/* 0x0041 */
		case MPI_IOCSTATUS_SCSI_INVALID_TARGETID:	/* 0x0042 */
			sc->result = DID_BAD_TARGET << 16;
			break;

		case MPI_IOCSTATUS_SCSI_DEVICE_NOT_THERE:	/* 0x0043 */
			/* Spoof to SCSI Selection Timeout! */
			if (ioc->bus_type != FC)
				sc->result = DID_NO_CONNECT << 16;
			/* else fibre, just stall until rescan event */
			else
				sc->result = DID_REQUEUE << 16;

			if (hd->sel_timeout[pScsiReq->TargetID] < 0xFFFF)
				hd->sel_timeout[pScsiReq->TargetID]++;

			if (!vdevice)
				break;
			vtarget = vdevice->vtarget;
			if (vtarget->tflags & MPT_TARGET_FLAGS_LED_ON) {
				mptscsih_issue_sep_command(ioc, vtarget,
				    MPI_SEP_REQ_SLOTSTATUS_UNCONFIGURED);
				vtarget->tflags &= ~MPT_TARGET_FLAGS_LED_ON;
			}
			break;

		case MPI_IOCSTATUS_SCSI_IOC_TERMINATED:		/* 0x004B */
			if ( ioc->bus_type == SAS ) {
				u16 ioc_status = le16_to_cpu(pScsiReply->IOCStatus);
				if (ioc_status & MPI_IOCSTATUS_FLAG_LOG_INFO_AVAILABLE) {
					if ((log_info & SAS_LOGINFO_MASK)
					    == SAS_LOGINFO_NEXUS_LOSS) {
						sc->result = (DID_BUS_BUSY << 16);
						break;
					}
				}
			} else if (ioc->bus_type == FC) {
				/*
				 * The FC IOC may kill a request for variety of
				 * reasons, some of which may be recovered by a
				 * retry, some which are unlikely to be
				 * recovered. Return DID_ERROR instead of
				 * DID_RESET to permit retry of the command,
				 * just not an infinite number of them
				 */
				sc->result = DID_ERROR << 16;
				break;
			}

			/*
			 * Allow non-SAS & non-NEXUS_LOSS to drop into below code
			 */

		case MPI_IOCSTATUS_SCSI_TASK_TERMINATED:	/* 0x0048 */
			/* Linux handles an unsolicited DID_RESET better
			 * than an unsolicited DID_ABORT.
			 */
			sc->result = DID_RESET << 16;

		case MPI_IOCSTATUS_SCSI_EXT_TERMINATED:		/* 0x004C */
			if ( ioc->bus_type == FC )
				sc->result = DID_ERROR << 16;
			else
				sc->result = DID_RESET << 16;
			break;

		case MPI_IOCSTATUS_SCSI_RESIDUAL_MISMATCH:	/* 0x0049 */
			scsi_set_resid(sc, scsi_bufflen(sc) - xfer_cnt);
			if((xfer_cnt==0)||(sc->underflow > xfer_cnt))
				sc->result=DID_SOFT_ERROR << 16;
			else /* Sufficient data transfer occurred */
				sc->result = (DID_OK << 16) | scsi_status;
			dreplyprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			    "RESIDUAL_MISMATCH: result=%x on channel=%d id=%d\n",
			    ioc->name, sc->result, sc->device->channel, sc->device->id));
			break;

		case MPI_IOCSTATUS_SCSI_DATA_UNDERRUN:		/* 0x0045 */
			/*
			 *  Do upfront check for valid SenseData and give it
			 *  precedence!
			 */
			sc->result = (DID_OK << 16) | scsi_status;
			if (!(scsi_state & MPI_SCSI_STATE_AUTOSENSE_VALID)) {

				/*
				 * For an Errata on LSI53C1030
				 * When the length of request data
				 * and transfer data are different
				 * with result of command (READ or VERIFY),
				 * DID_SOFT_ERROR is set.
				 */
				if (ioc->bus_type == SPI) {
					if (pScsiReq->CDB[0] == READ_6  ||
					    pScsiReq->CDB[0] == READ_10 ||
					    pScsiReq->CDB[0] == READ_12 ||
					    pScsiReq->CDB[0] == READ_16 ||
					    pScsiReq->CDB[0] == VERIFY  ||
					    pScsiReq->CDB[0] == VERIFY_16) {
						if (scsi_bufflen(sc) !=
							xfer_cnt) {
							sc->result =
							DID_SOFT_ERROR << 16;
						    printk(KERN_WARNING "Errata"
						    "on LSI53C1030 occurred."
						    "sc->req_bufflen=0x%02x,"
						    "xfer_cnt=0x%02x\n",
						    scsi_bufflen(sc),
						    xfer_cnt);
						}
					}
				}

				if (xfer_cnt < sc->underflow) {
					if (scsi_status == SAM_STAT_BUSY)
						sc->result = SAM_STAT_BUSY;
					else
						sc->result = DID_SOFT_ERROR << 16;
				}
				if (scsi_state & (MPI_SCSI_STATE_AUTOSENSE_FAILED | MPI_SCSI_STATE_NO_SCSI_STATUS)) {
					/* What to do?
					*/
					sc->result = DID_SOFT_ERROR << 16;
				}
				else if (scsi_state & MPI_SCSI_STATE_TERMINATED) {
					/*  Not real sure here either...  */
					sc->result = DID_RESET << 16;
				}
			}


			dreplyprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			    "  sc->underflow={report ERR if < %02xh bytes xfer'd}\n",
			    ioc->name, sc->underflow));
			dreplyprintk(ioc, printk(MYIOC_s_DEBUG_FMT
			    "  ActBytesXferd=%02xh\n", ioc->name, xfer_cnt));

			/* Report Queue Full
			 */
			if (scsi_status == MPI_SCSI_STATUS_TASK_SET_FULL)
				mptscsih_report_queue_full(sc, pScsiReply, pScsiReq);

			break;

		case MPI_IOCSTATUS_SCSI_DATA_OVERRUN:		/* 0x0044 */
			scsi_set_resid(sc, 0);
		case MPI_IOCSTATUS_SCSI_RECOVERED_ERROR:	/* 0x0040 */
		case MPI_IOCSTATUS_SUCCESS:			/* 0x0000 */
			sc->result = (DID_OK << 16) | scsi_status;
			if (scsi_state == 0) {
				;
			} else if (scsi_state &
			    MPI_SCSI_STATE_AUTOSENSE_VALID) {

				/*
				 * For potential trouble on LSI53C1030.
				 * (date:2007.xx.)
				 * It is checked whether the length of
				 * request data is equal to
				 * the length of transfer and residual.
				 * MEDIUM_ERROR is set by incorrect data.
				 */
				if ((ioc->bus_type == SPI) &&
					(sc->sense_buffer[2] & 0x20)) {
					u32	 difftransfer;
					difftransfer =
					sc->sense_buffer[3] << 24 |
					sc->sense_buffer[4] << 16 |
					sc->sense_buffer[5] << 8 |
					sc->sense_buffer[6];
					if (((sc->sense_buffer[3] & 0x80) ==
						0x80) && (scsi_bufflen(sc)
						!= xfer_cnt)) {
						sc->sense_buffer[2] =
						    MEDIUM_ERROR;
						sc->sense_buffer[12] = 0xff;
						sc->sense_buffer[13] = 0xff;
						printk(KERN_WARNING"Errata"
						"on LSI53C1030 occurred."
						"sc->req_bufflen=0x%02x,"
						"xfer_cnt=0x%02x\n" ,
						scsi_bufflen(sc),
						xfer_cnt);
					}
					if (((sc->sense_buffer[3] & 0x80)
						!= 0x80) &&
						(scsi_bufflen(sc) !=
						xfer_cnt + difftransfer)) {
						sc->sense_buffer[2] =
							MEDIUM_ERROR;
						sc->sense_buffer[12] = 0xff;
						sc->sense_buffer[13] = 0xff;
						printk(KERN_WARNING
						"Errata on LSI53C1030 occurred"
						"sc->req_bufflen=0x%02x,"
						" xfer_cnt=0x%02x,"
						"difftransfer=0x%02x\n",
						scsi_bufflen(sc),
						xfer_cnt,
						difftransfer);
					}
				}

				/*
				 * If running against circa 200003dd 909 MPT f/w,
				 * may get this (AUTOSENSE_VALID) for actual TASK_SET_FULL
				 * (QUEUE_FULL) returned from device! --> get 0x0000?128
				 * and with SenseBytes set to 0.
				 */
				if (pScsiReply->SCSIStatus == MPI_SCSI_STATUS_TASK_SET_FULL)
					mptscsih_report_queue_full(sc, pScsiReply, pScsiReq);

			}
			else if (scsi_state &
				 (MPI_SCSI_STATE_AUTOSENSE_FAILED | MPI_SCSI_STATE_NO_SCSI_STATUS)
			   ) {
				/*
				 * What to do?
				 */
				sc->result = DID_SOFT_ERROR << 16;
			}
			else if (scsi_state & MPI_SCSI_STATE_TERMINATED) {
				/*  Not real sure here either...  */
				sc->result = DID_RESET << 16;
			}
			else if (scsi_state & MPI_SCSI_STATE_QUEUE_TAG_REJECTED) {
				/* Device Inq. data indicates that it supports
				 * QTags, but rejects QTag messages.
				 * This command completed OK.
				 *
				 * Not real sure here either so do nothing...  */
			}

			if (sc->result == MPI_SCSI_STATUS_TASK_SET_FULL)
				mptscsih_report_queue_full(sc, pScsiReply, pScsiReq);

			/* Add handling of:
			 * Reservation Conflict, Busy,
			 * Command Terminated, CHECK
			 */
			break;

		case MPI_IOCSTATUS_SCSI_PROTOCOL_ERROR:		/* 0x0047 */
			sc->result = DID_SOFT_ERROR << 16;
			break;
#ifdef EEDP_SUPPORT
		case MPI_IOCSTATUS_EEDP_GUARD_ERROR:
		case MPI_IOCSTATUS_EEDP_REF_TAG_ERROR:
		case MPI_IOCSTATUS_EEDP_APP_TAG_ERROR:
			sc->result = DID_PARITY << 16;
		break;
#endif /* EEDP Support */

		case MPI_IOCSTATUS_INVALID_FUNCTION:		/* 0x0001 */
		case MPI_IOCSTATUS_INVALID_SGL:			/* 0x0003 */
		case MPI_IOCSTATUS_INTERNAL_ERROR:		/* 0x0004 */
		case MPI_IOCSTATUS_RESERVED:			/* 0x0005 */
		case MPI_IOCSTATUS_INVALID_FIELD:		/* 0x0007 */
		case MPI_IOCSTATUS_INVALID_STATE:		/* 0x0008 */
		case MPI_IOCSTATUS_SCSI_IO_DATA_ERROR:		/* 0x0046 */
		case MPI_IOCSTATUS_SCSI_TASK_MGMT_FAILED:	/* 0x004A */
		default:
			/*
			 * What to do?
			 */
			sc->result = DID_SOFT_ERROR << 16;
			break;

		}	/* switch(status) */

#ifdef CONFIG_FUSION_LOGGING
		if (sc->result && (ioc->debug_level & MPT_DEBUG_REPLY))
			mptscsih_info_scsiio(ioc, sc, pScsiReply);
#endif

	} /* end of address reply case */
out:
	/* Unmap the DMA buffers, if any. */
	scsi_dma_unmap(sc);

	sc->scsi_done(sc);		/* Issue the command callback */

	/* Free Chain buffers */
	mptscsih_freeChainBuffers(ioc, req_idx);
	return 1;
}

/*
 *	mptscsih_flush_running_cmds - For each command found, search
 *		Scsi_Host instance taskQ and reply to OS.
 *		Called only if recovering from a FW reload.
 *	@hd: Pointer to a SCSI HOST structure
 *
 *	Returns: None.
 *
 *	Must be called while new I/Os are being queued.
 */
static void
mptscsih_flush_running_cmds(MPT_SCSI_HOST *hd)
{
	MPT_ADAPTER *ioc = hd->ioc;
	struct scsi_cmnd *sc;
	SCSIIORequest_t	*mf = NULL;
	int		 ii;
	int		 channel, id;

	for (ii= 0; ii < ioc->req_depth; ii++) {
		sc = mptscsih_getclear_scsi_lookup(ioc, ii);
		if (!sc)
			continue;
		mf = (SCSIIORequest_t *)MPT_INDEX_2_MFPTR(ioc, ii);
		if (!mf)
			continue;
		channel = mf->Bus;
		id = mf->TargetID;
		mptscsih_freeChainBuffers(ioc, ii);
		mpt_free_msg_frame(ioc, (MPT_FRAME_HDR *)mf);
		if ((unsigned char *)mf != sc->host_scribble)
			continue;
		scsi_dma_unmap(sc);
		sc->result = DID_RESET << 16;
		sc->host_scribble = NULL;
		dtmprintk(ioc, sdev_printk(KERN_INFO, sc->device, MYIOC_s_FMT
		    "completing cmds: fw_channel %d, fw_id %d, sc=%p, mf = %p, "
		    "idx=%x\n", ioc->name, channel, id, sc, mf, ii));
		sc->scsi_done(sc);
	}
}

/*
 *	mptscsih_search_running_cmds - Delete any commands associated
 *		with the specified target and lun. Function called only
 *		when a lun is disable by mid-layer.
 *		Do NOT access the referenced scsi_cmnd structure or
 *		members. Will cause either a paging or NULL ptr error.
 *		(BUT, BUT, BUT, the code does reference it! - mdr)
 *      @hd: Pointer to a SCSI HOST structure
 *	@vdevice: per device private data
 *
 *	Returns: None.
 *
 *	Called from slave_destroy.
 */
static void
mptscsih_search_running_cmds(MPT_SCSI_HOST *hd, VirtDevice *vdevice)
{
	SCSIIORequest_t	*mf = NULL;
	int		 ii;
	struct scsi_cmnd *sc;
	struct scsi_lun  lun;
	MPT_ADAPTER *ioc = hd->ioc;
	unsigned long	flags;

	spin_lock_irqsave(&ioc->scsi_lookup_lock, flags);
	for (ii = 0; ii < ioc->req_depth; ii++) {
		if ((sc = ioc->ScsiLookup[ii]) != NULL) {

			mf = (SCSIIORequest_t *)MPT_INDEX_2_MFPTR(ioc, ii);
			if (mf == NULL)
				continue;
			/* If the device is a hidden raid component, then its
			 * expected that the mf->function will be RAID_SCSI_IO
			 */
			if (vdevice->vtarget->tflags &
			    MPT_TARGET_FLAGS_RAID_COMPONENT && mf->Function !=
			    MPI_FUNCTION_RAID_SCSI_IO_PASSTHROUGH)
				continue;

			int_to_scsilun(vdevice->lun, &lun);
			if ((mf->Bus != vdevice->vtarget->channel) ||
			    (mf->TargetID != vdevice->vtarget->id) ||
			    memcmp(lun.scsi_lun, mf->LUN, 8))
				continue;

			if ((unsigned char *)mf != sc->host_scribble)
				continue;
			ioc->ScsiLookup[ii] = NULL;
			spin_unlock_irqrestore(&ioc->scsi_lookup_lock, flags);
			mptscsih_freeChainBuffers(ioc, ii);
			mpt_free_msg_frame(ioc, (MPT_FRAME_HDR *)mf);
			scsi_dma_unmap(sc);
			sc->host_scribble = NULL;
			sc->result = DID_NO_CONNECT << 16;
			dtmprintk(ioc, sdev_printk(KERN_INFO, sc->device,
			   MYIOC_s_FMT "completing cmds: fw_channel %d, "
			   "fw_id %d, sc=%p, mf = %p, idx=%x\n", ioc->name,
			   vdevice->vtarget->channel, vdevice->vtarget->id,
			   sc, mf, ii));
			sc->scsi_done(sc);
			spin_lock_irqsave(&ioc->scsi_lookup_lock, flags);
		}
	}
	spin_unlock_irqrestore(&ioc->scsi_lookup_lock, flags);
	return;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_report_queue_full - Report QUEUE_FULL status returned
 *	from a SCSI target device.
 *	@sc: Pointer to scsi_cmnd structure
 *	@pScsiReply: Pointer to SCSIIOReply_t
 *	@pScsiReq: Pointer to original SCSI request
 *
 *	This routine periodically reports QUEUE_FULL status returned from a
 *	SCSI target device.  It reports this to the console via kernel
 *	printk() API call, not more than once every 10 seconds.
 */
static void
mptscsih_report_queue_full(struct scsi_cmnd *sc, SCSIIOReply_t *pScsiReply, SCSIIORequest_t *pScsiReq)
{
	long time = jiffies;
	MPT_SCSI_HOST		*hd;
	MPT_ADAPTER	*ioc;

	if (sc->device == NULL)
		return;
	if (sc->device->host == NULL)
		return;
	if ((hd = shost_priv(sc->device->host)) == NULL)
		return;
	ioc = hd->ioc;
	if (time - hd->last_queue_full > 10 * HZ) {
		dprintk(ioc, printk(MYIOC_s_WARN_FMT "Device (%d:%d:%d) reported QUEUE_FULL!\n",
				ioc->name, 0, sc->device->id, sc->device->lun));
		hd->last_queue_full = time;
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_remove - Removed scsi devices
 *	@pdev: Pointer to pci_dev structure
 *
 *
 */
void
mptscsih_remove(struct pci_dev *pdev)
{
	MPT_ADAPTER		*ioc = pci_get_drvdata(pdev);
	struct Scsi_Host	*host = ioc->sh;
	MPT_SCSI_HOST		*hd;
	int sz1;

	if(!host) {
		mpt_detach(pdev);
		return;
	}

	scsi_remove_host(host);

	if((hd = shost_priv(host)) == NULL)
		return;

	mptscsih_shutdown(pdev);

	sz1=0;

	if (ioc->ScsiLookup != NULL) {
		sz1 = ioc->req_depth * sizeof(void *);
		kfree(ioc->ScsiLookup);
		ioc->ScsiLookup = NULL;
	}

	dprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "Free'd ScsiLookup (%d) memory\n",
	    ioc->name, sz1));

	kfree(hd->info_kbuf);

	/* NULL the Scsi_Host pointer
	 */
	ioc->sh = NULL;

	scsi_host_put(host);

	mpt_detach(pdev);

}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_shutdown - reboot notifier
 *
 */
void
mptscsih_shutdown(struct pci_dev *pdev)
{
}

#ifdef CONFIG_PM
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_suspend - Fusion MPT scsi driver suspend routine.
 *
 *
 */
int
mptscsih_suspend(struct pci_dev *pdev, pm_message_t state)
{
	MPT_ADAPTER		*ioc = pci_get_drvdata(pdev);

	scsi_block_requests(ioc->sh);
	flush_scheduled_work();
	mptscsih_shutdown(pdev);
	return mpt_suspend(pdev,state);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_resume - Fusion MPT scsi driver resume routine.
 *
 *
 */
int
mptscsih_resume(struct pci_dev *pdev)
{
	MPT_ADAPTER		*ioc = pci_get_drvdata(pdev);
	int rc;

	rc = mpt_resume(pdev);
	scsi_unblock_requests(ioc->sh);
	return rc;
}

#endif

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_info - Return information about MPT adapter
 *	@SChost: Pointer to Scsi_Host structure
 *
 *	(linux scsi_host_template.info routine)
 *
 *	Returns pointer to buffer where information was written.
 */
const char *
mptscsih_info(struct Scsi_Host *SChost)
{
	MPT_SCSI_HOST *h;
	int size = 0;

	h = shost_priv(SChost);

	if (h) {
		if (h->info_kbuf == NULL)
			if ((h->info_kbuf = kmalloc(0x1000 /* 4Kb */, GFP_KERNEL)) == NULL)
				return h->info_kbuf;
		h->info_kbuf[0] = '\0';

		mpt_print_ioc_summary(h->ioc, h->info_kbuf, &size, 0, 0);
		h->info_kbuf[size-1] = '\0';
	}

	return h->info_kbuf;
}

struct info_str {
	char *buffer;
	int   length;
	int   offset;
	int   pos;
};

static void
mptscsih_copy_mem_info(struct info_str *info, char *data, int len)
{
	if (info->pos + len > info->length)
		len = info->length - info->pos;

	if (info->pos + len < info->offset) {
		info->pos += len;
		return;
	}

	if (info->pos < info->offset) {
		data += (info->offset - info->pos);
		len  -= (info->offset - info->pos);
	}

	if (len > 0) {
		memcpy(info->buffer + info->pos, data, len);
		info->pos += len;
	}
}

static int
mptscsih_copy_info(struct info_str *info, char *fmt, ...)
{
	va_list args;
	char buf[81];
	int len;

	va_start(args, fmt);
	len = vsprintf(buf, fmt, args);
	va_end(args);

	mptscsih_copy_mem_info(info, buf, len);
	return len;
}

static int
mptscsih_host_info(MPT_ADAPTER *ioc, char *pbuf, off_t offset, int len)
{
	struct info_str info;

	info.buffer	= pbuf;
	info.length	= len;
	info.offset	= offset;
	info.pos	= 0;

	mptscsih_copy_info(&info, "%s: %s, ", ioc->name, ioc->prod_name);
	mptscsih_copy_info(&info, "%s%08xh, ", MPT_FW_REV_MAGIC_ID_STRING, ioc->facts.FWVersion.Word);
	mptscsih_copy_info(&info, "Ports=%d, ", ioc->facts.NumberOfPorts);
	mptscsih_copy_info(&info, "MaxQ=%d\n", ioc->req_depth);

	return ((info.pos > info.offset) ? info.pos - info.offset : 0);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_proc_info - Return information about MPT adapter
 *	@host:   scsi host struct
 *	@buffer: if write, user data; if read, buffer for user
 *	@start: returns the buffer address
 *	@offset: if write, 0; if read, the current offset into the buffer from
 *		 the previous read.
 *	@length: if write, return length;
 *	@func:   write = 1; read = 0
 *
 *	(linux scsi_host_template.info routine)
 */
int
mptscsih_proc_info(struct Scsi_Host *host, char *buffer, char **start, off_t offset,
			int length, int func)
{
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER	*ioc = hd->ioc;
	int size = 0;

	if (func) {
		/*
		 * write is not supported
		 */
	} else {
		if (start)
			*start = buffer;

		size = mptscsih_host_info(ioc, buffer, offset, length);
	}

	return size;
}

#ifdef EEDP_SUPPORT
u8 opcode_protection[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, PRO_R, 0, PRO_W, 0, 0, 0, PRO_W, PRO_V,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, PRO_W, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, PRO_R, 0, PRO_W, 0, 0, 0, PRO_W, PRO_V,
	0, 0, 0, PRO_W, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, PRO_R, 0, PRO_W, 0, 0, 0, PRO_W, PRO_V,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/**
 * _scsih_setup_eedp - setup MPI request for EEDP transfer
 * @ioc:
 * @scmd: pointer to scsi command object
 * @mpi_request: pointer to the SCSI_IO reqest message frame
 *
 * Supporting protection 1 only.
 *
 * Returns nothing
 */
static int
_scsih_setup_eedp(MPT_ADAPTER *ioc, struct scsi_cmnd *scmd, SCSIIO32Request_t *mpi_request)
{
	VirtDevice *vdevice = scmd->device->hostdata;
	u16 eedp_flags;
	u8 scsi_opcode;
	int lba_byte;
	u32 *lba32;

	vdevice = scmd->device->hostdata;
	if (!vdevice->eedp_enable)
		return -1;

	/* protection type 1 support only */
	if (vdevice->eedp_type != 0)
		return -1;

	/* check whether scsi opcode supports eedp transfer */
	scsi_opcode = scmd->cmnd[0];
	eedp_flags = opcode_protection[scsi_opcode];
	if (!eedp_flags)
		return -1;

	/*
	 * enable ref/app/guard checking
	 * auto increment ref and app tag
	 */
	mpi_request->EEDPFlags = eedp_flags |
	    MPI_SCSIIO32_EEDPFLAGS_INC_PRI_REFTAG |
	    MPI_SCSIIO32_EEDPFLAGS_T10_CHK_REFTAG |
	    MPI_SCSIIO32_EEDPFLAGS_T10_CHK_LBATAG |
	    MPI_SCSIIO32_EEDPFLAGS_T10_CHK_GUARD;

	/* set block size */
	mpi_request->EEDPBlockSize = vdevice->eedp_block_length;
	mpi_request->EEDPBlockSize += 8;
	memset(mpi_request->CDB.CDB32, 0, 32);

	mpi_request->CDB.EEDP32.PrimaryApplicationTagMask = 0xFFFF;

	/* set reference tag to low 32bit lba */
	lba_byte = (scmd->cmd_len == 16) ? 6 : 2;
	lba32 = (u32 *)&scmd->cmnd[lba_byte];
	mpi_request->CDB.EEDP32.PrimaryReferenceTag = *lba32;

	/* set RDPROTECT, WRPROTECT, VRPROTECT bits to (001b) */
	scmd->cmnd[1] = (scmd->cmnd[1] & 0x1F) | 0x20;

	/* add the rest of the bits */
	mpi_request->Port = 0;
	mpi_request->Flags = MPI_SCSIIO32_FLAGS_FORM_SCSIID;
	mpi_request->DeviceAddress.SCSIID.TargetID = vdevice->vtarget->id;
	mpi_request->DeviceAddress.SCSIID.Bus = vdevice->vtarget->channel;
	mpi_request->ChainOffset = 0;
	mpi_request->Function = MPI_FUNCTION_SCSI_IO_32;
	mpi_request->CDBLength = scmd->cmd_len;
	mpi_request->SenseBufferLength = MPT_SENSE_BUFFER_SIZE;
	mpi_request->MsgFlags = mpt_msg_flags(ioc);
	int_to_scsilun(scmd->device->lun, (struct scsi_lun *)mpi_request->LUN);
	memcpy(mpi_request->CDB.CDB32, scmd->cmnd, scmd->cmd_len);
	mpi_request->SGLOffset0 = offsetof(SCSIIO32Request_t, SGL) / 4;
	mpi_request->SGLOffset1 = 0;
	mpi_request->SGLOffset2 = 0;
	mpi_request->SGLOffset3 = 0;
	return 0;
}
#endif /* EEDP Support */

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
#define ADD_INDEX_LOG(req_ent)	do { } while(0)

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_qcmd - Primary Fusion MPT SCSI initiator IO start routine.
 *	@SCpnt: Pointer to scsi_cmnd structure
 *	@done: Pointer SCSI mid-layer IO completion function
 *
 *	(linux scsi_host_template.queuecommand routine)
 *	This is the primary SCSI IO start routine.  Create a MPI SCSIIORequest
 *	from a linux scsi_cmnd request and send it to the IOC.
 *
 *	Returns 0. (rtn value discarded by linux scsi mid-layer)
 */
int
mptscsih_qcmd(struct scsi_cmnd *SCpnt, void (*done)(struct scsi_cmnd *))
{
	MPT_SCSI_HOST		*hd;
	MPT_FRAME_HDR		*mf;
	SCSIIORequest_t		*pScsiReq;
	VirtDevice		*vdevice = SCpnt->device->hostdata;
	u32	 datalen;
	u32	 scsictl;
	u32	 scsidir;
	u32	 cmd_len;
	int	 my_idx;
	int	 ii;
	MPT_ADAPTER *ioc;

	hd = shost_priv(SCpnt->device->host);
	ioc = hd->ioc;
	SCpnt->scsi_done = done;

	dmfprintk(ioc, printk(MYIOC_s_DEBUG_FMT "qcmd: SCpnt=%p, done()=%p\n",
		ioc->name, SCpnt, done));

	if (ioc->taskmgmt_quiesce_io) {
		dtmprintk(ioc, printk(MYIOC_s_WARN_FMT "qcmd: SCpnt=%p timeout + 60HZ\n",
			ioc->name, SCpnt));
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	/*
	 *  Put together a MPT SCSI request...
	 */
	if ((mf = mpt_get_msg_frame(ioc->DoneCtx, ioc)) == NULL) {
		dprintk(ioc, printk(MYIOC_s_WARN_FMT "QueueCmd, no msg frames!!\n",
				ioc->name));
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	pScsiReq = (SCSIIORequest_t *) mf;

	my_idx = le16_to_cpu(mf->u.frame.hwhdr.msgctxu.fld.req_idx);

	ADD_INDEX_LOG(my_idx);

	/*    TUR's being issued with scsictl=0x02000000 (DATA_IN)!
	 *    Seems we may receive a buffer (datalen>0) even when there
	 *    will be no data transfer!  GRRRRR...
	 */
	if (SCpnt->sc_data_direction == DMA_FROM_DEVICE) {
		datalen = scsi_bufflen(SCpnt);
		scsidir = MPI_SCSIIO_CONTROL_READ;	/* DATA IN  (host<--ioc<--dev) */
	} else if (SCpnt->sc_data_direction == DMA_TO_DEVICE) {
		datalen = scsi_bufflen(SCpnt);
		scsidir = MPI_SCSIIO_CONTROL_WRITE;	/* DATA OUT (host-->ioc-->dev) */
	} else {
		datalen = 0;
		scsidir = MPI_SCSIIO_CONTROL_NODATATRANSFER;
	}

	/* Default to untagged. Once a target structure has been allocated,
	 * use the Inquiry data to determine if device supports tagged.
	 */
	if (vdevice
	    && (vdevice->vtarget->tflags & MPT_TARGET_FLAGS_Q_YES)
	    && (SCpnt->device->tagged_supported)) {
		scsictl = scsidir | MPI_SCSIIO_CONTROL_SIMPLEQ;
	} else {
		scsictl = scsidir | MPI_SCSIIO_CONTROL_UNTAGGED;
	}

	/* Use the above information to set up the message frame
	 */
#ifdef EEDP_SUPPORT
	if (_scsih_setup_eedp(ioc, SCpnt, (SCSIIO32Request_t *)mf) == 0) {
		SCSIIO32Request_t *mpi_request = (SCSIIO32Request_t *)mf;

		/* finish off setting the rest of the SCSIIO32 */
		mpi_request->Control = cpu_to_le32(scsictl);
		mpi_request->DataLength = cpu_to_le32(datalen);

		/* SenseBuffer low address */
		mpi_request->SenseBufferLowAddr =
		    cpu_to_le32(ioc->sense_buf_low_dma
		   + (my_idx * MPT_SENSE_BUFFER_ALLOC));

		/* Now add the SG list
		 * Always have a SGE even if null length.
		 */
		if (datalen == 0) {
			/* Add a NULL SGE */
			ioc->add_sge((char *)&mpi_request->SGL,
			     MPT_SGE_FLAGS_SSIMPLE_READ | 0,
			    (dma_addr_t) -1);
		} else {
			/* Add a 32 or 64 bit SGE */
			if (mptscsih_AddSGE(ioc, SCpnt,
			    pScsiReq, my_idx) != SUCCESS)
				goto fail;
		}
		goto send_mf;
	}
#endif
	pScsiReq->TargetID = (u8) vdevice->vtarget->id;
	pScsiReq->Bus = vdevice->vtarget->channel;
	pScsiReq->ChainOffset = 0;
	if (vdevice->vtarget->tflags &  MPT_TARGET_FLAGS_RAID_COMPONENT)
		pScsiReq->Function = MPI_FUNCTION_RAID_SCSI_IO_PASSTHROUGH;
	else
		pScsiReq->Function = MPI_FUNCTION_SCSI_IO_REQUEST;
	pScsiReq->CDBLength = SCpnt->cmd_len;
	pScsiReq->SenseBufferLength = MPT_SENSE_BUFFER_SIZE;
	pScsiReq->Reserved = 0;
	pScsiReq->MsgFlags = mpt_msg_flags(ioc);
	int_to_scsilun(SCpnt->device->lun, (struct scsi_lun *)pScsiReq->LUN);
	pScsiReq->Control = cpu_to_le32(scsictl);

	/*
	 *  Write SCSI CDB into the message
	 */
	cmd_len = SCpnt->cmd_len;
	for (ii=0; ii < cmd_len; ii++)
		pScsiReq->CDB[ii] = SCpnt->cmnd[ii];

	for (ii=cmd_len; ii < 16; ii++)
		pScsiReq->CDB[ii] = 0;

	/* DataLength */
	pScsiReq->DataLength = cpu_to_le32(datalen);

	/* SenseBuffer low address */
	pScsiReq->SenseBufferLowAddr = cpu_to_le32(ioc->sense_buf_low_dma
					   + (my_idx * MPT_SENSE_BUFFER_ALLOC));

	/* Now add the SG list
	 * Always have a SGE even if null length.
	 */
	if (datalen == 0) {
		/* Add a NULL SGE */
		ioc->add_sge((char *)&pScsiReq->SGL,
			MPT_SGE_FLAGS_SSIMPLE_READ | 0,
			(dma_addr_t) -1);
	} else {
		/* Add a 32 or 64 bit SGE */
		if (mptscsih_AddSGE(ioc, SCpnt, pScsiReq, my_idx) != SUCCESS)
			goto fail;
	}

#ifdef EEDP_SUPPORT
 send_mf:
#endif
	SCpnt->host_scribble = (unsigned char *)mf;
	mptscsih_set_scsi_lookup(ioc, my_idx, SCpnt);

	mpt_put_msg_frame(ioc->DoneCtx, ioc, mf);
	dmfprintk(ioc, printk(MYIOC_s_DEBUG_FMT "Issued SCSI cmd (%p) mf=%p idx=%d\n",
			ioc->name, SCpnt, mf, my_idx));
	DBG_DUMP_REQUEST_FRAME(ioc, (u32 *)mf);
	return 0;

 fail:
	mptscsih_freeChainBuffers(ioc, my_idx);
	mpt_free_msg_frame(ioc, mf);
	return SCSI_MLQUEUE_HOST_BUSY;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_freeChainBuffers - Function to free chain buffers associated
 *	with a SCSI IO request
 *	@hd: Pointer to the MPT_SCSI_HOST instance
 *	@req_idx: Index of the SCSI IO request frame.
 *
 *	Called if SG chain buffer allocation fails and mptscsih callbacks.
 *	No return.
 */
static void
mptscsih_freeChainBuffers(MPT_ADAPTER *ioc, int req_idx)
{
	MPT_FRAME_HDR *chain;
	unsigned long flags;
	int chain_idx;
	int next;

	/* Get the first chain index and reset
	 * tracker state.
	 */
	chain_idx = ioc->ReqToChain[req_idx];
	ioc->ReqToChain[req_idx] = MPT_HOST_NO_CHAIN;

	while (chain_idx != MPT_HOST_NO_CHAIN) {

		/* Save the next chain buffer index */
		next = ioc->ChainToChain[chain_idx];

		/* Free this chain buffer and reset
		 * tracker
		 */
		ioc->ChainToChain[chain_idx] = MPT_HOST_NO_CHAIN;

		chain = (MPT_FRAME_HDR *) (ioc->ChainBuffer
					+ (chain_idx * ioc->req_sz));

		spin_lock_irqsave(&ioc->FreeQlock, flags);
		list_add_tail(&chain->u.frame.linkage.list, &ioc->FreeChainQ);
		spin_unlock_irqrestore(&ioc->FreeQlock, flags);

		dmfprintk(ioc, printk(MYIOC_s_DEBUG_FMT "FreeChainBuffers (index %d)\n",
				ioc->name, chain_idx));

		/* handle next */
		chain_idx = next;
	}
	return;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	Reset Handling
 */

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/

static int
mptscsih_scandv_bus_reset(MPT_ADAPTER *ioc)
{
	MPT_FRAME_HDR	*mf;
	SCSITaskMgmt_t	*pScsiTm;
	SCSITaskMgmtReply_t *pScsiTmReply;
	int		 ii;
	int		 retval;
	unsigned long	 timeout;
	unsigned long	 time_count;
	u16		 iocstatus;

	mutex_lock(&ioc->taskmgmt_cmds.mutex);
	if (mpt_set_taskmgmt_in_progress_flag(ioc) != 0) {
		mutex_unlock(&ioc->taskmgmt_cmds.mutex);
		return -EPERM;
	}

	/* Send request
	 */
	if ((mf = mpt_get_msg_frame(ioc->TaskCtx, ioc)) == NULL) {
		dtmprintk(ioc, printk(MYIOC_s_WARN_FMT "TaskMgmt, no msg frames!!\n",
				ioc->name));
		mpt_clear_taskmgmt_in_progress_flag(ioc);
		retval = -ENOMEM;
		goto out;
	}

	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT "TaskMgmt request (mf=%p)\n",
		ioc->name, mf));

	pScsiTm = (SCSITaskMgmt_t *) mf;
	memset(pScsiTm, 0, sizeof(SCSITaskMgmt_t));
	pScsiTm->Function = MPI_FUNCTION_SCSI_TASK_MGMT;
	pScsiTm->TaskType = MPI_SCSITASKMGMT_TASKTYPE_RESET_BUS;
	pScsiTm->MsgFlags = MPI_SCSITASKMGMT_MSGFLAGS_LIPRESET_RESET_OPTION;
	pScsiTm->TargetID = 0;
	pScsiTm->Bus = 0;
	pScsiTm->ChainOffset = 0;
	pScsiTm->Reserved = 0;
	pScsiTm->Reserved1 = 0;
	pScsiTm->TaskMsgContext = 0;
	for (ii= 0; ii < 8; ii++)
		pScsiTm->LUN[ii] = 0;
	for (ii=0; ii < 7; ii++)
		pScsiTm->Reserved2[ii] = 0;

	switch (ioc->bus_type) {
		case FC:
			timeout = 40;
			break;
		case SAS:
			timeout = 30;
			break;
		case SPI:
		default:
			timeout = 2;
			break;
	}

	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT "TaskMgmt type=%d timeout=%ld\n",
	    ioc->name, MPI_SCSITASKMGMT_TASKTYPE_RESET_BUS, timeout));

	INITIALIZE_MGMT_STATUS(ioc->taskmgmt_cmds.status)
	CLEAR_MGMT_STATUS(ioc->internal_cmds.status)
	retval = 0;
	time_count = jiffies;
	if ((ioc->facts.IOCCapabilities & MPI_IOCFACTS_CAPABILITY_HIGH_PRI_Q) &&
	    (ioc->facts.MsgVersion >= MPI_VERSION_01_05))
		mpt_put_msg_frame_hi_pri(ioc->TaskCtx, ioc, mf);
	else {
		retval = mpt_send_handshake_request(ioc->TaskCtx, ioc,
		    sizeof(SCSITaskMgmt_t), (u32*)pScsiTm, CAN_SLEEP);
		if (retval != 0) {
			dfailprintk(ioc, printk(MYIOC_s_ERR_FMT "TaskMgmt send_handshake FAILED!"
				" (ioc %p, mf %p, rc=%d) \n", ioc->name,
				ioc, mf, retval));
			mpt_clear_taskmgmt_in_progress_flag(ioc);
			goto out;
		}
	}

	/* Now wait for the command to complete */
	ii = wait_for_completion_timeout(&ioc->taskmgmt_cmds.done, timeout*HZ);
	if (!(ioc->taskmgmt_cmds.status & MPT_MGMT_STATUS_COMMAND_GOOD)) {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "TaskMgmt failed\n", ioc->name));
		mpt_free_msg_frame(ioc, mf);
		mpt_clear_taskmgmt_in_progress_flag(ioc);
		retval = -1; /* return failure */
		goto out;
	}

	if (!(ioc->taskmgmt_cmds.status & MPT_MGMT_STATUS_RF_VALID)) {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "TaskMgmt failed\n", ioc->name));
		retval = -1; /* return failure */
		goto out;
	}

	pScsiTmReply = (SCSITaskMgmtReply_t *) ioc->taskmgmt_cmds.reply;
	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "TaskMgmt fw_channel = %d, fw_id = %d, task_type = 0x%02X,\n"
	    "\tiocstatus = 0x%04X, loginfo = 0x%08X, response_code = 0x%02X,\n"
	    "\tterm_cmnds = %d\n", ioc->name, pScsiTmReply->Bus,
	    pScsiTmReply->TargetID, MPI_SCSITASKMGMT_TASKTYPE_RESET_BUS,
	    le16_to_cpu(pScsiTmReply->IOCStatus),
	    le32_to_cpu(pScsiTmReply->IOCLogInfo),
	    pScsiTmReply->ResponseCode,
	    le32_to_cpu(pScsiTmReply->TerminationCount)));

	iocstatus = le16_to_cpu(pScsiTmReply->IOCStatus) & MPI_IOCSTATUS_MASK;

	if (iocstatus == MPI_IOCSTATUS_SCSI_TASK_TERMINATED ||
	   iocstatus == MPI_IOCSTATUS_SCSI_IOC_TERMINATED ||
	   iocstatus == MPI_IOCSTATUS_SUCCESS)
		retval = 0;
	else {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "TaskMgmt failed\n", ioc->name));
		retval = -1; /* return failure */
	}

 out:
	mutex_unlock(&ioc->taskmgmt_cmds.mutex);
	CLEAR_MGMT_STATUS(ioc->taskmgmt_cmds.status)
	return retval;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_IssueTaskMgmt - Generic send Task Management function.
 *	@hd: Pointer to MPT_SCSI_HOST structure
 *	@type: Task Management type
 *	@channel: channel number for task management
 *	@id: Logical Target ID for reset (if appropriate)
 *	@lun: Logical Unit for reset (if appropriate)
 *	@ctx2abort: Context for the task to be aborted (if appropriate)
 *	@timeout: timeout for task management control
 *
 *	Remark: _HardResetHandler can be invoked from an interrupt thread (timer)
 *	or a non-interrupt thread.  In the former, must not call schedule().
 *
 *	Not all fields are meaningfull for all task types.
 *
 *	Returns 0 for SUCCESS, or FAILED.
 *
 **/
int
mptscsih_IssueTaskMgmt(MPT_SCSI_HOST *hd, u8 type, u8 channel, u8 id, int lun, int ctx2abort, ulong timeout)
{
	MPT_FRAME_HDR	*mf;
	SCSITaskMgmt_t	*pScsiTm;
	int		 ii;
	int		 retval;
	MPT_ADAPTER	*ioc = hd->ioc;
	unsigned long	 timeleft;
	u8		 issue_hard_reset;
	u32		 ioc_raw_state;
	unsigned long	 time_count;

	issue_hard_reset = 0;
	ioc_raw_state = mpt_GetIocState(ioc, 0);

	if ((ioc_raw_state & MPI_IOC_STATE_MASK) != MPI_IOC_STATE_OPERATIONAL) {
		printk(MYIOC_s_WARN_FMT
			"TaskMgmt type=%x: IOC Not operational (0x%x)!\n",
			ioc->name, type, ioc_raw_state);
		printk(MYIOC_s_WARN_FMT "Issuing HardReset from %s!!\n",
		    ioc->name, __func__);
		if (mpt_HardResetHandler(ioc, CAN_SLEEP) < 0)
			printk(MYIOC_s_WARN_FMT "TaskMgmt HardReset "
			    "FAILED!!\n", ioc->name);
		return 0;
	}

	if (ioc_raw_state & MPI_DOORBELL_ACTIVE) {
		printk(MYIOC_s_WARN_FMT
			"TaskMgmt type=%x: ioc_state: "
			"DOORBELL_ACTIVE (0x%x)!\n",
			ioc->name, type, ioc_raw_state);
		return FAILED;
	}

	mutex_lock(&ioc->taskmgmt_cmds.mutex);
	if (mpt_set_taskmgmt_in_progress_flag(ioc) != 0) {
		mf = NULL;
		retval = FAILED;
		goto out;
	}

	/* Return Fail to calling function if no message frames available.
	 */
	if ((mf = mpt_get_msg_frame(ioc->TaskCtx, ioc)) == NULL) {
		dfailprintk(ioc, printk(MYIOC_s_ERR_FMT
			"TaskMgmt no msg frames!!\n", ioc->name));
		retval = FAILED;
		mpt_clear_taskmgmt_in_progress_flag(ioc);
		goto out;
	}
	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT "TaskMgmt request (mf=%p)\n",
			ioc->name, mf));

	/* Format the Request
	 */
	pScsiTm = (SCSITaskMgmt_t *) mf;
	pScsiTm->TargetID = id;
	pScsiTm->Bus = channel;
	pScsiTm->ChainOffset = 0;
	pScsiTm->Function = MPI_FUNCTION_SCSI_TASK_MGMT;

	pScsiTm->Reserved = 0;
	pScsiTm->TaskType = type;
	pScsiTm->Reserved1 = 0;
	pScsiTm->MsgFlags = (type == MPI_SCSITASKMGMT_TASKTYPE_RESET_BUS)
		? MPI_SCSITASKMGMT_MSGFLAGS_LIPRESET_RESET_OPTION : 0;

	int_to_scsilun(lun, (struct scsi_lun *)pScsiTm->LUN);

	for (ii=0; ii < 7; ii++)
		pScsiTm->Reserved2[ii] = 0;

	pScsiTm->TaskMsgContext = ctx2abort;

	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT "TaskMgmt: ctx2abort (0x%08x) "
		"task_type = 0x%02X, timeout = %ld\n", ioc->name, ctx2abort,
		type, timeout));

	DBG_DUMP_TM_REQUEST_FRAME(ioc, (u32 *)pScsiTm);

	INITIALIZE_MGMT_STATUS(ioc->taskmgmt_cmds.status)
	time_count = jiffies;
	if ((ioc->facts.IOCCapabilities & MPI_IOCFACTS_CAPABILITY_HIGH_PRI_Q) &&
	    (ioc->facts.MsgVersion >= MPI_VERSION_01_05))
		mpt_put_msg_frame_hi_pri(ioc->TaskCtx, ioc, mf);
	else {
		retval = mpt_send_handshake_request(ioc->TaskCtx, ioc,
			sizeof(SCSITaskMgmt_t), (u32*)pScsiTm, CAN_SLEEP);
		if (retval) {
			dfailprintk(ioc, printk(MYIOC_s_ERR_FMT
				"TaskMgmt handshake FAILED!(mf=%p, rc=%d) \n",
				ioc->name, mf, retval));
			mpt_free_msg_frame(ioc, mf);
			mpt_clear_taskmgmt_in_progress_flag(ioc);
			goto out;
		}
	}

	timeleft = wait_for_completion_timeout(&ioc->taskmgmt_cmds.done,
		timeout*HZ);
	if (!(ioc->taskmgmt_cmds.status & MPT_MGMT_STATUS_COMMAND_GOOD)) {
		retval = FAILED;
		dtmprintk(ioc, printk(MYIOC_s_ERR_FMT
		    "TaskMgmt TIMED OUT!(mf=%p)\n", ioc->name, mf));
		mpt_clear_taskmgmt_in_progress_flag(ioc);
		if (ioc->taskmgmt_cmds.status & MPT_MGMT_STATUS_DID_IOCRESET)
			goto out;
		issue_hard_reset = 1;
		goto out;
	}

	retval = mptscsih_taskmgmt_reply(ioc, type,
	    (SCSITaskMgmtReply_t *) ioc->taskmgmt_cmds.reply);

	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "TaskMgmt completed (%d seconds)\n",
	    ioc->name, jiffies_to_msecs(jiffies - time_count)/1000));

 out:

	CLEAR_MGMT_STATUS(ioc->taskmgmt_cmds.status)
	if (issue_hard_reset) {
		printk(MYIOC_s_WARN_FMT "Issuing Reset from %s!!\n",
			ioc->name, __func__);
		if ((retval = mpt_SoftResetHandler(ioc, CAN_SLEEP)) != 0)
			retval = mpt_HardResetHandler(ioc, CAN_SLEEP);
		mpt_free_msg_frame(ioc, mf);
	}

	retval = (retval == 0) ? 0 : FAILED;
	mutex_unlock(&ioc->taskmgmt_cmds.mutex);
	return retval;
}

static int
mptscsih_get_tm_timeout(MPT_ADAPTER *ioc)
{
	switch (ioc->bus_type) {
	case FC:
		return 40;
	case SAS:
		return 30;
	case SPI:
	default:
		return 10;
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_abort - Abort linux scsi_cmnd routine, new_eh variant
 *	@SCpnt: Pointer to scsi_cmnd structure, IO to be aborted
 *
 *	(linux scsi_host_template.eh_abort_handler routine)
 *
 *	Returns SUCCESS or FAILED.
 **/
int
mptscsih_abort(struct scsi_cmnd * SCpnt)
{
	MPT_SCSI_HOST	*hd;
	MPT_FRAME_HDR	*mf;
	u32		 ctx2abort;
	int		 scpnt_idx;
	int		 retval;
	VirtDevice	 *vdevice;
	ulong		 sn = SCpnt->serial_number;
	MPT_ADAPTER	*ioc;

	/* If we can't locate our host adapter structure, return FAILED status.
	 */
	if ((hd = shost_priv(SCpnt->device->host)) == NULL) {
		SCpnt->result = DID_RESET << 16;
		SCpnt->scsi_done(SCpnt);
		printk(KERN_ERR MYNAM ": task abort: "
		    "can't locate host! (sc=%p)\n", SCpnt);
		return FAILED;
	}

	ioc = hd->ioc;
	printk(MYIOC_s_INFO_FMT "attempting task abort! (sc=%p)\n",
	       ioc->name, SCpnt);
	scsi_print_command(SCpnt);

	vdevice = SCpnt->device->hostdata;
	if (!vdevice || !vdevice->vtarget) {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "task abort: device has been deleted (sc=%p)\n",
		    ioc->name, SCpnt));
		SCpnt->result = DID_NO_CONNECT << 16;
		SCpnt->scsi_done(SCpnt);
		retval = SUCCESS;
		goto out;
	}

	/* Find this command
	 */
	if ((scpnt_idx = SCPNT_TO_LOOKUP_IDX(ioc, SCpnt)) < 0) {
		/* Cmd not found in ScsiLookup.
		 * Do OS callback.
		 */
		SCpnt->result = DID_RESET << 16;
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "task abort: command not in the active list! (sc=%p)\n",
		    ioc->name, SCpnt));
		retval = SUCCESS;
		goto out;
	}

	/* Task aborts are not supported for hidden raid components.
	 */
	if (vdevice->vtarget->tflags & MPT_TARGET_FLAGS_RAID_COMPONENT) {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "task abort: hidden raid component (sc=%p)\n",
		    ioc->name, SCpnt));
		SCpnt->result = DID_RESET << 16;
		retval = FAILED;
		goto out;
	}

	/* Task aborts are not supported for volumes.
	 */
	if (vdevice->vtarget->raidVolume) {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "task abort: raid volume (sc=%p)\n",
		    ioc->name, SCpnt));
		SCpnt->result = DID_RESET << 16;
		retval = FAILED;
		goto out;
	}

	if (mpt_fwfault_debug)
		mpt_halt_firmware(ioc);

	if (ioc->timeouts < -1)
		ioc->timeouts++;


	/* Most important!  Set TaskMsgContext to SCpnt's MsgContext!
	 * (the IO to be ABORT'd)
	 *
	 * NOTE: Since we do not byteswap MsgContext, we do not
	 *	 swap it here either.  It is an opaque cookie to
	 *	 the controller, so it does not matter. -DaveM
	 */
	mf = MPT_INDEX_2_MFPTR(ioc, scpnt_idx);
	ctx2abort = mf->u.frame.hwhdr.msgctxu.MsgContext;
	retval = mptscsih_IssueTaskMgmt(hd,
			 MPI_SCSITASKMGMT_TASKTYPE_ABORT_TASK,
			 vdevice->vtarget->channel,
			 vdevice->vtarget->id, vdevice->lun,
			 ctx2abort, mptscsih_get_tm_timeout(ioc));


	/* check to see whether command actually completed and/or
	 * terminated
	 */
	if (SCPNT_TO_LOOKUP_IDX(ioc, SCpnt) == scpnt_idx &&
	    SCpnt->serial_number == sn) {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "task abort: command still in active list! (sc=%p)\n",
		    ioc->name, SCpnt));
		retval = FAILED;
	} else {
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "task abort: command cleared from active list! (sc=%p)\n",
		    ioc->name, SCpnt));
		retval = SUCCESS;
	}

 out:
	printk(MYIOC_s_INFO_FMT "task abort: %s (sc=%p)\n",
	    ioc->name, ((retval == SUCCESS) ? "SUCCESS" : "FAILED" ), SCpnt);

	return retval;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_dev_reset - Perform a SCSI TARGET_RESET!  new_eh variant
 *	@SCpnt: Pointer to scsi_cmnd structure, IO which reset is due to
 *
 *	(linux scsi_host_template.eh_dev_reset_handler routine)
 *
 *	Returns SUCCESS or FAILED.
 **/
int
mptscsih_dev_reset(struct scsi_cmnd * SCpnt)
{
	MPT_SCSI_HOST	*hd;
	int		 retval;
	VirtDevice	 *vdevice;
	MPT_ADAPTER	*ioc;

	/* If we can't locate our host adapter structure, return FAILED status.
	 */
	if ((hd = shost_priv(SCpnt->device->host)) == NULL){
		printk(KERN_ERR MYNAM ": target reset: "
		   "Can't locate host! (sc=%p)\n", SCpnt);
		return FAILED;
	}

	ioc = hd->ioc;
	printk(MYIOC_s_INFO_FMT "attempting target reset! (sc=%p)\n",
	       ioc->name, SCpnt);
	scsi_print_command(SCpnt);

	vdevice = SCpnt->device->hostdata;
	if (!vdevice || !vdevice->vtarget) {
		retval = SUCCESS;
		goto out;
	}

	/* Target reset to hidden raid component is not supported
	 */
	if (vdevice->vtarget->tflags & MPT_TARGET_FLAGS_RAID_COMPONENT) {
		retval = FAILED;
		goto out;
	}

	retval = mptscsih_IssueTaskMgmt(hd,
				MPI_SCSITASKMGMT_TASKTYPE_TARGET_RESET,
				vdevice->vtarget->channel,
				vdevice->vtarget->id, 0, 0,
				mptscsih_get_tm_timeout(ioc));

 out:
	printk (MYIOC_s_INFO_FMT "target reset: %s (sc=%p)\n",
	    ioc->name, ((retval == 0) ? "SUCCESS" : "FAILED" ), SCpnt);

	if (retval == 0)
		return SUCCESS;
	else
		return FAILED;
}


/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_bus_reset - Perform a SCSI BUS_RESET!	new_eh variant
 *	@SCpnt: Pointer to scsi_cmnd structure, IO which reset is due to
 *
 *	(linux scsi_host_template.eh_bus_reset_handler routine)
 *
 *	Returns SUCCESS or FAILED.
 **/
int
mptscsih_bus_reset(struct scsi_cmnd * SCpnt)
{
	MPT_SCSI_HOST	*hd;
	int		 retval;
	VirtDevice	 *vdevice;
	MPT_ADAPTER	*ioc;

	/* If we can't locate our host adapter structure, return FAILED status.
	 */
	if ((hd = shost_priv(SCpnt->device->host)) == NULL){
		printk(KERN_ERR MYNAM ": bus reset: "
		   "Can't locate host! (sc=%p)\n", SCpnt);
		return FAILED;
	}

	ioc = hd->ioc;
	printk(MYIOC_s_INFO_FMT "attempting bus reset! (sc=%p)\n",
	       ioc->name, SCpnt);
	scsi_print_command(SCpnt);

	if (ioc->timeouts < -1)
		ioc->timeouts++;

	vdevice = SCpnt->device->hostdata;
	if (!vdevice || !vdevice->vtarget)
		return SUCCESS;
	retval = mptscsih_IssueTaskMgmt(hd,
					MPI_SCSITASKMGMT_TASKTYPE_RESET_BUS,
					vdevice->vtarget->channel, 0, 0, 0,
					mptscsih_get_tm_timeout(ioc));

	printk(MYIOC_s_INFO_FMT "bus reset: %s (sc=%p)\n",
	    ioc->name, ((retval == 0) ? "SUCCESS" : "FAILED" ), SCpnt);

	if (retval == 0)
		return SUCCESS;
	else
		return FAILED;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_host_reset - Perform a SCSI host adapter RESET (new_eh variant)
 *	@SCpnt: Pointer to scsi_cmnd structure, IO which reset is due to
 *
 *	(linux scsi_host_template.eh_host_reset_handler routine)
 *
 *	Returns SUCCESS or FAILED.
 */
int
mptscsih_host_reset(struct scsi_cmnd *SCpnt)
{
	MPT_SCSI_HOST *  hd;
	int		status = SUCCESS;
	MPT_ADAPTER	*ioc;
	int		retval;

	/*  If we can't locate the host to reset, then we failed. */
	if ((hd = shost_priv(SCpnt->device->host)) == NULL){
		printk(KERN_ERR MYNAM ": host reset: "
		    "Can't locate host! (sc=%p)\n", SCpnt);
		return FAILED;
	}

	ioc = hd->ioc;
	printk(MYIOC_s_INFO_FMT "attempting host reset! (sc=%p)\n",
	    ioc->name, SCpnt);

	/*  If our attempts to reset the host failed, then return a failed
	 *  status.  The host will be taken off line by the SCSI mid-layer.
	 */
	if ((retval = mpt_SoftResetHandler(ioc, CAN_SLEEP)) != 0)
		retval = mpt_HardResetHandler(ioc, CAN_SLEEP);

	if (retval < 0)
		status = FAILED;
	else
		status = SUCCESS;

	printk(MYIOC_s_INFO_FMT "host reset: %s (sc=%p)\n",
	    ioc->name, ((retval == 0) ? "SUCCESS" : "FAILED" ), SCpnt);

	return status;
}


static int
mptscsih_taskmgmt_reply(MPT_ADAPTER *ioc, u8 type,
	SCSITaskMgmtReply_t *pScsiTmReply)
{
	u16			 iocstatus;
	u32			 termination_count;
	int			 retval;

	if (!(ioc->taskmgmt_cmds.status & MPT_MGMT_STATUS_RF_VALID)) {
		retval = FAILED;
		goto out;
	}

	DBG_DUMP_TM_REPLY_FRAME(ioc, (u32 *)pScsiTmReply);

	iocstatus = le16_to_cpu(pScsiTmReply->IOCStatus) & MPI_IOCSTATUS_MASK;
	termination_count = le32_to_cpu(pScsiTmReply->TerminationCount);

	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "TaskMgmt fw_channel = %d, fw_id = %d, task_type = 0x%02X,\n"
	    "\tiocstatus = 0x%04X, loginfo = 0x%08X, response_code = 0x%02X,\n"
	    "\tterm_cmnds = %d\n", ioc->name, pScsiTmReply->Bus,
	    pScsiTmReply->TargetID, type, le16_to_cpu(pScsiTmReply->IOCStatus),
	    le32_to_cpu(pScsiTmReply->IOCLogInfo), pScsiTmReply->ResponseCode,
	    termination_count));

	if (ioc->facts.MsgVersion >= MPI_VERSION_01_05 &&
	    pScsiTmReply->ResponseCode)
		mptscsih_taskmgmt_response_code(ioc,
		    pScsiTmReply->ResponseCode);

	if (iocstatus == MPI_IOCSTATUS_SUCCESS) {
		retval = 0;
		goto out;
	}

	retval = FAILED;
	if (type == MPI_SCSITASKMGMT_TASKTYPE_ABORT_TASK) {
		if (termination_count == 1)
			retval = 0;
		goto out;
	}

	if (iocstatus == MPI_IOCSTATUS_SCSI_TASK_TERMINATED ||
	   iocstatus == MPI_IOCSTATUS_SCSI_IOC_TERMINATED)
		retval = 0;

 out:
	return retval;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
void
mptscsih_taskmgmt_response_code(MPT_ADAPTER *ioc, u8 response_code)
{
	char *desc;

	switch (response_code) {
	case MPI_SCSITASKMGMT_RSP_TM_COMPLETE:
		desc = "The task completed.";
		break;
	case MPI_SCSITASKMGMT_RSP_INVALID_FRAME:
		desc = "The IOC received an invalid frame status.";
		break;
	case MPI_SCSITASKMGMT_RSP_TM_NOT_SUPPORTED:
		desc = "The task type is not supported.";
		break;
	case MPI_SCSITASKMGMT_RSP_TM_FAILED:
		desc = "The requested task failed.";
		break;
	case MPI_SCSITASKMGMT_RSP_TM_SUCCEEDED:
		desc = "The task completed successfully.";
		break;
	case MPI_SCSITASKMGMT_RSP_TM_INVALID_LUN:
		desc = "The LUN request is invalid.";
		break;
	case MPI_SCSITASKMGMT_RSP_IO_QUEUED_ON_IOC:
		desc = "The task is in the IOC queue and has not been sent to target.";
		break;
	default:
		desc = "unknown";
		break;
	}
	printk(MYIOC_s_INFO_FMT "Response Code(0x%08x): F/W: %s\n",
		ioc->name, response_code, desc);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_taskmgmt_complete - Registered with Fusion MPT base driver
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@mf: Pointer to SCSI task mgmt request frame
 *	@mr: Pointer to SCSI task mgmt reply frame
 *
 *	This routine is called from mptbase.c::mpt_interrupt() at the completion
 *	of any SCSI task management request.
 *	This routine is registered with the MPT (base) driver at driver
 *	load/init time via the mpt_register() API call.
 *
 *	Returns 1 indicating alloc'd request frame ptr should be freed.
 **/
int
mptscsih_taskmgmt_complete(MPT_ADAPTER *ioc, MPT_FRAME_HDR *mf,
	MPT_FRAME_HDR *mr)
{
	dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		"TaskMgmt completed (mf=%p, mr=%p)\n", ioc->name, mf, mr));

	ioc->taskmgmt_cmds.status |= MPT_MGMT_STATUS_COMMAND_GOOD;

	if (!mr)
		goto out;

	ioc->taskmgmt_cmds.status |= MPT_MGMT_STATUS_RF_VALID;
	memcpy(ioc->taskmgmt_cmds.reply, mr,
	    min(MPT_DEFAULT_FRAME_SIZE, 4 * mr->u.reply.MsgLength));
 out:
	if (ioc->taskmgmt_cmds.status & MPT_MGMT_STATUS_PENDING) {
		mpt_clear_taskmgmt_in_progress_flag(ioc);
		ioc->taskmgmt_cmds.status &= ~MPT_MGMT_STATUS_PENDING;
		complete(&ioc->taskmgmt_cmds.done);
		if (ioc->bus_type == SAS)
			ioc->schedule_target_reset(ioc);
		return 1;
	}
	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	This is anyones guess quite frankly.
 */
int
mptscsih_bios_param(struct scsi_device * sdev, struct block_device *bdev,
		sector_t capacity, int geom[])
{
	int		heads;
	int		sectors;
	sector_t	cylinders;
	ulong		dummy;

	heads = 64;
	sectors = 32;

	dummy = heads * sectors;
	cylinders = capacity;
	sector_div(cylinders,dummy);

	/*
	 * Handle extended translation size for logical drives
	 * > 1Gb
	 */
	if ((ulong)capacity >= 0x200000) {
		heads = 255;
		sectors = 63;
		dummy = heads * sectors;
		cylinders = capacity;
		sector_div(cylinders,dummy);
	}

	/* return result */
	geom[0] = heads;
	geom[1] = sectors;
	geom[2] = cylinders;

	return 0;
}

/**
 * Search IOC page 3 to determine if this is hidden physical disk
 *
 */
int
mptscsih_is_phys_disk(MPT_ADAPTER *ioc, u8 channel, u8 id)
{
	struct inactive_raid_component_info *component_info;
	u8 i, j;
	RaidPhysDiskPage1_t *phys_disk;
	int rc = 0;
	int num_paths;

	if (!ioc->raid_data.pIocPg3)
		goto out;
	for (i = 0; i < ioc->raid_data.pIocPg3->NumPhysDisks; i++) {
		if ((id == ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskID) &&
		    (channel == ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskBus)) {
			rc = 1;
			goto out;
		}
	}

	if (ioc->bus_type != SAS)
		goto out;

	/*
	 * Check if dual path
	 */
	for (i = 0; i < ioc->raid_data.pIocPg3->NumPhysDisks; i++) {
		num_paths = mpt_raid_phys_disk_get_num_paths(ioc,
		    ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskNum);
		if (num_paths < 2)
			continue;
		phys_disk = kzalloc(offsetof(RaidPhysDiskPage1_t,Path) +
		   (num_paths * sizeof(RAID_PHYS_DISK1_PATH)), GFP_KERNEL);
		if (!phys_disk)
			continue;
		if ((mpt_raid_phys_disk_pg1(ioc,
		    ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskNum,
		    phys_disk))) {
			kfree(phys_disk);
			continue;
		}
		for (j = 0; j < num_paths; j++) {
			if ((phys_disk->Path[j].Flags &
			    MPI_RAID_PHYSDISK1_FLAG_INVALID))
				continue;
			if ((phys_disk->Path[j].Flags &
			    MPI_RAID_PHYSDISK1_FLAG_BROKEN))
				continue;
			if ((id == phys_disk->Path[j].PhysDiskID) &&
			    (channel == phys_disk->Path[j].PhysDiskBus)) {
				rc = 1;
				kfree(phys_disk);
				goto out;
			}
		}
		kfree(phys_disk);
	}

	/*
	 * Check inactive list for matching phys disks
	 */
	if (list_empty(&ioc->raid_data.inactive_list))
		goto out;

	down(&ioc->raid_data.inactive_list_mutex);
	list_for_each_entry(component_info, &ioc->raid_data.inactive_list,
	    list) {
		if ((component_info->d.PhysDiskID == id) &&
		    (component_info->d.PhysDiskBus == channel))
			rc = 1;
	}
	up(&ioc->raid_data.inactive_list_mutex);

 out:
	return rc;
}
EXPORT_SYMBOL(mptscsih_is_phys_disk);

u8
mptscsih_raid_id_to_num(MPT_ADAPTER *ioc, u8 channel, u8 id)
{
	struct inactive_raid_component_info *component_info;
	int i,j;
	RaidPhysDiskPage1_t *phys_disk;
	int rc = -ENXIO;
	u8 num_paths;

	if (!ioc->raid_data.pIocPg3)
		goto out;
	for (i = 0; i < ioc->raid_data.pIocPg3->NumPhysDisks; i++) {
		if ((id == ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskID) &&
		    (channel == ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskBus)) {
			rc = ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskNum;
			goto out;
		}
	}

	if (ioc->bus_type != SAS)
		goto out;

	/*
	 * Check if dual path
	 */
	for (i = 0; i < ioc->raid_data.pIocPg3->NumPhysDisks; i++) {
		num_paths = mpt_raid_phys_disk_get_num_paths(ioc,
		    ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskNum);
		if (num_paths < 2)
			continue;
		phys_disk = kzalloc(offsetof(RaidPhysDiskPage1_t,Path) +
		   (num_paths * sizeof(RAID_PHYS_DISK1_PATH)), GFP_KERNEL);
		if (!phys_disk)
			continue;
		if ((mpt_raid_phys_disk_pg1(ioc,
		    ioc->raid_data.pIocPg3->PhysDisk[i].PhysDiskNum,
		    phys_disk))) {
			kfree(phys_disk);
			continue;
		}
		for (j = 0; j < num_paths; j++) {
			if ((phys_disk->Path[j].Flags &
			    MPI_RAID_PHYSDISK1_FLAG_INVALID))
				continue;
			if ((phys_disk->Path[j].Flags &
			    MPI_RAID_PHYSDISK1_FLAG_BROKEN))
				continue;
			if ((id == phys_disk->Path[j].PhysDiskID) &&
			    (channel == phys_disk->Path[j].PhysDiskBus)) {
				rc = phys_disk->PhysDiskNum;
				kfree(phys_disk);
				goto out;
			}
		}
		kfree(phys_disk);
	}

	/*
	 * Check inactive list for matching phys disks
	 */
	if (list_empty(&ioc->raid_data.inactive_list))
		goto out;

	down(&ioc->raid_data.inactive_list_mutex);
	list_for_each_entry(component_info, &ioc->raid_data.inactive_list,
	    list) {
		if ((component_info->d.PhysDiskID == id) &&
		    (component_info->d.PhysDiskBus == channel))
			rc = component_info->d.PhysDiskNum;
	}
	up(&ioc->raid_data.inactive_list_mutex);

 out:
	return rc;
}
EXPORT_SYMBOL(mptscsih_raid_id_to_num);

/*
 *	OS entry point to allow for host driver to free allocated memory
 *	Called if no device present or device being unloaded
 */
void
mptscsih_slave_destroy(struct scsi_device *sdev)
{
	struct Scsi_Host	*host = sdev->host;
	MPT_SCSI_HOST		*hd = shost_priv(host);
	VirtTarget		*vtarget;
	VirtDevice		*vdevice;
	struct scsi_target	*starget;

	starget = scsi_target(sdev);
	vtarget = starget->hostdata;
	vtarget->num_luns--;
	vdevice = sdev->hostdata;
	if (!vdevice)
		return;

	mptscsih_search_running_cmds(hd, vdevice);
	mptscsih_synchronize_cache(sdev, hd, vdevice);
	kfree(vdevice);
	sdev->hostdata = NULL;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_change_queue_depth - This function will set a devices queue depth
 *	@sdev: per scsi_device pointer
 *	@qdepth: requested queue depth
 *	@reason: calling context
 *
 *	Adding support for new 'change_queue_depth' api.
*/
int
mptscsih_change_queue_depth(struct scsi_device *sdev, int qdepth, int reason)
{
	MPT_SCSI_HOST		*hd = shost_priv(sdev->host);
	VirtTarget		*vtarget;
	struct scsi_target	*starget;
	int			max_depth;
	int			tagged;
	MPT_ADAPTER		*ioc = hd->ioc;

	starget = scsi_target(sdev);
	vtarget = starget->hostdata;

	if (reason != SCSI_QDEPTH_DEFAULT)
		return -EOPNOTSUPP;

	if (ioc->bus_type == SPI) {
		if (sdev->type == TYPE_DISK &&
		    vtarget->minSyncFactor <= MPT_ULTRA160)
			max_depth = MPT_SCSI_CMD_PER_DEV_HIGH;
		else
			max_depth = MPT_SCSI_CMD_PER_DEV_LOW;
	} else
		max_depth = ioc->sh->can_queue;

	if (!sdev->tagged_supported)
		max_depth = 1;

	if (qdepth > max_depth)
		qdepth = max_depth;
	if (qdepth == 1)
		tagged = 0;
	else
		tagged = MSG_SIMPLE_TAG;

	scsi_adjust_queue_depth(sdev, tagged, qdepth);

	if (sdev->inquiry_len > 7)
		sdev_printk(KERN_INFO, sdev, MYIOC_s_FMT "qdepth=%d, "
		    "tagged=%d, simple=%d, ordered=%d, scsi_level=%d, "
		    "cmd_que=%d\n", ioc->name, sdev->queue_depth,
		    sdev->tagged_supported, sdev->simple_tags,
		    sdev->ordered_tags, sdev->scsi_level,
		    (sdev->inquiry[7] & 2) >> 1);

	return sdev->queue_depth;
}

#ifdef EEDP_SUPPORT
/**
 * _scsih_read_capacity_16 - send READ_CAPACITY_16 to target
 *
 */
static int
_scsih_read_capacity_16(MPT_SCSI_HOST *hd, int id, int channel, u32 lun,
    void *data, u32 length)
{
	INTERNAL_CMD				 iocmd;
	dma_addr_t					 data_dma;
	struct read_cap_parameter	*parameter_data;
	u32							 data_length;
	MPT_ADAPTER					*ioc = hd->ioc;
	int							 rc;
	int							 count;
	u8							 skey;
	u8							 asc;
	u8							 ascq;

	data_length = sizeof(struct read_cap_parameter);
	parameter_data = pci_alloc_consistent(ioc->pcidev,
	   data_length, &data_dma);
	if (!parameter_data) {
		printk(MYIOC_s_ERR_FMT "failure at %s:%d/%s()!\n",
		    ioc->name, __FILE__, __LINE__, __func__);
		return -1;
	}

	iocmd.cmd = SERVICE_ACTION_IN;
	iocmd.data_dma = data_dma;
	iocmd.data = (u8 *)parameter_data;
	iocmd.size = data_length;
	iocmd.channel = channel;
	iocmd.id = id;
	iocmd.lun = lun;

	for (count=0; count < 4; count++) {
		rc = mptscsih_do_cmd(hd, &iocmd);

		if(rc == MPT_SCANDV_GOOD) {
			memcpy(data, parameter_data,
			    min_t(u32, data_length, length));
			break;
		} else if(rc == MPT_SCANDV_BUSY) {
			devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT "%s: "
			    "fw_channel=%d fw_id=%d : device busy\n",
			    ioc->name, __FUNCTION__, channel, id));
			continue;
		} else if(rc == MPT_SCANDV_DID_RESET) {
			devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT "%s: "
			    "fw_channel=%d fw_id=%d : did reset\n",
			    ioc->name, __FUNCTION__, channel, id));
			continue;
		} else if(rc == MPT_SCANDV_SENSE) {
			skey = ioc->internal_cmds.sense[2] & 0x0F;
			asc  = ioc->internal_cmds.sense[12];
			ascq = ioc->internal_cmds.sense[13];
			devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT "%s: "
			    "fw_channel=%d fw_id=%d : [sense_key,arc,ascq]: "
			    "[0x%02x,0x%02x,0x%02x]\n", ioc->name,
			    __FUNCTION__, channel, id, skey, asc, ascq));
			if( skey == UNIT_ATTENTION ||
			    skey == NOT_READY ||
			    skey == ILLEGAL_REQUEST ) {
				continue;
			} else {
				printk(MYIOC_s_ERR_FMT "%s: fw_channel=%d fw_id=%d : "
				    "tur failed due to [sense_key,asc,ascq]: "
				    "[0x%02x,0x%02x,0x%02x]\n", ioc->name,
				    __FUNCTION__, channel, id, skey, asc, ascq);
				break;
			}
		} else if(rc == MPT_SCANDV_SELECTION_TIMEOUT) {
			printk(MYIOC_s_ERR_FMT "%s: fw_channel=%d fw_id=%d: "
			    "read capacity failed due to no device\n", ioc->name,
			    __FUNCTION__, channel, id);
			break;
		} else if(rc == MPT_SCANDV_SOME_ERROR) {
			printk(MYIOC_s_ERR_FMT "%s: fw_channel=%d fw_id=%d: "
			    "read capacity failed due to some error\n", ioc->name,
			    __FUNCTION__, channel, id);
			break;
		} else {
			printk(MYIOC_s_ERR_FMT "%s: fw_channel=%d fw_id=%d: "
			    "read capacity failed due to some error\n", ioc->name,
			    __FUNCTION__, channel, id);
			break;
		}
	}

	if(count > 4 && rc != 0) {
		printk(MYIOC_s_ERR_FMT "%s: fw_channel=%d fw_id=%d: "
		    "read capacity failed to many times\n", ioc->name,
		    __FUNCTION__, channel, id);
	}

	pci_free_consistent(ioc->pcidev, data_length, parameter_data, data_dma);
	return rc;
}
#endif

/*
 *	OS entry point to adjust the queue_depths on a per-device basis.
 *	Called once per device the bus scan. Use it to force the queue_depth
 *	member to 1 if a device does not support Q tags.
 *	Return non-zero if fails.
 */
int
mptscsih_slave_configure(struct scsi_device *sdev)
{
	struct Scsi_Host	*sh = sdev->host;
	VirtTarget		*vtarget;
	VirtDevice		*vdevice;
	struct scsi_target	*starget;
	MPT_SCSI_HOST		*hd = shost_priv(sh);
	MPT_ADAPTER		*ioc = hd->ioc;

	starget = scsi_target(sdev);
	vtarget = starget->hostdata;
	vdevice = sdev->hostdata;

#ifdef EEDP_SUPPORT
	if ((!(vdevice->vtarget->tflags & MPT_TARGET_FLAGS_RAID_COMPONENT)) &&
	    (!(vdevice->vtarget->raidVolume))) {

		struct read_cap_parameter data;
		memset(&data, 0, sizeof(struct read_cap_parameter));

		/*
		 * check PROTECT bit
		 *
		 * NOTE: The crack monkey target mode driver doesn't
		 * set this bit(bug has been reported).
		 * The cm_target command line option is a work around.
		 */
		if (!(sdev->inquiry[5] & 1))
			goto out;

		if ((ioc->bus_type == FC) &&
		    (_scsih_read_capacity_16(hd, vtarget->id,
				vtarget->channel, sdev->lun, &data,
				sizeof(struct read_cap_parameter)) == 0)) {
			vdevice->eedp_enable = data.prot_en;
			vdevice->eedp_type = data.p_type;
			vdevice->eedp_block_length =
			    be32_to_cpu(data.logical_block_length);

			if (!vdevice->eedp_enable)
				goto out;

			sdev_printk(KERN_INFO, sdev, "EEDP enabled: "
			    "protection_type(%d), block_length(%d)\n",
			    vdevice->eedp_type+1,
			    vdevice->eedp_block_length);
		}
	}
 out:
#endif /* EEDP Support */


	dsprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		"device @ %p, channel=%d, id=%d, lun=%d\n",
		ioc->name, sdev, sdev->channel, sdev->id, sdev->lun));
	if (ioc->bus_type == SPI)
		dsprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "sdtr %d wdtr %d ppr %d inq length=%d\n",
		    ioc->name, sdev->sdtr, sdev->wdtr,
		    sdev->ppr, sdev->inquiry_len));

	vdevice->configured_lun = 1;

	if ((ioc->bus_type != SAS) && (sdev->id > sh->max_id)) {
		/* error case, should never happen */
		scsi_adjust_queue_depth(sdev, 0, 1);
		goto slave_configure_exit;
	}
	dsprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		"Queue depth=%d, tflags=%x\n",
		ioc->name, sdev->queue_depth, vtarget->tflags));

	if (ioc->bus_type == SPI)
		dsprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "negoFlags=%x, maxOffset=%x, SyncFactor=%x\n",
		    ioc->name, vtarget->negoFlags, vtarget->maxOffset,
		    vtarget->minSyncFactor));

	mptscsih_change_queue_depth(sdev, ioc->sdev_queue_depth,
				    SCSI_QDEPTH_DEFAULT);

slave_configure_exit:
	dsprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		"tagged %d, simple %d, ordered %d\n",
		ioc->name,sdev->tagged_supported, sdev->simple_tags,
		sdev->ordered_tags));

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *  Private routines...
 */

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/* Utility function to copy sense data from the scsi_cmnd buffer
 * to the FC and SCSI target structures.
 *
 */
static void
mptscsih_copy_sense_data(struct scsi_cmnd *sc, MPT_SCSI_HOST *hd, MPT_FRAME_HDR *mf, SCSIIOReply_t *pScsiReply)
{
	VirtDevice	*vdevice;
	SCSIIORequest_t	*pReq;
	u32		 sense_count = le32_to_cpu(pScsiReply->SenseCount);
	MPT_ADAPTER	*ioc = hd->ioc;

	/* Get target structure
	 */
	pReq = (SCSIIORequest_t *) mf;
	vdevice = sc->device->hostdata;

	if (sense_count) {
		u8 *sense_data;
		int req_index;

		/* Copy the sense received into the scsi command block. */
		req_index = le16_to_cpu(mf->u.frame.hwhdr.msgctxu.fld.req_idx);
		sense_data = ((u8 *)ioc->sense_buf_pool + (req_index * MPT_SENSE_BUFFER_ALLOC));

		if (sense_count > SCSI_SENSE_BUFFERSIZE)
				sense_count = SCSI_SENSE_BUFFERSIZE;

		memcpy(sc->sense_buffer, sense_data, sense_count);
		/* Log SMART data (asc = 0x5D, non-IM case only) if required.
		 */
		if ((ioc->events) && (ioc->eventTypes & (1 << MPI_EVENT_SCSI_DEVICE_STATUS_CHANGE))) {
			if ((sense_data[12] == 0x5D) && (vdevice->vtarget->raidVolume == 0)) {
				int idx;

				idx = ioc->eventContext % MPTCTL_EVENT_LOG_SIZE;
				ioc->events[idx].event = MPI_EVENT_SCSI_DEVICE_STATUS_CHANGE;
				ioc->events[idx].eventContext = ioc->eventContext;

				ioc->events[idx].data[0] = (pReq->LUN[1] << 24) |
					(MPI_EVENT_SCSI_DEV_STAT_RC_SMART_DATA << 16) |
					(sc->device->channel << 8) | sc->device->id;

				ioc->events[idx].data[1] = (sense_data[13] << 8) | sense_data[12];

				ioc->eventContext++;
				if (ioc->pcidev->vendor == PCI_VENDOR_ID_IBM) {
					mptscsih_issue_sep_command(ioc, vdevice->vtarget,
					    MPI_SEP_REQ_SLOTSTATUS_PREDICTED_FAULT);
					vdevice->vtarget->tflags |= MPT_TARGET_FLAGS_LED_ON;
				}
			}
		}
	} else {
		dprintk(ioc, printk(MYIOC_s_DEBUG_FMT "Hmmm... SenseData len=0! (?)\n",
				ioc->name));
	}
}

/**
 * mptscsih_get_scsi_lookup
 *
 * retrieves scmd entry from ScsiLookup[] array list
 *
 * @ioc: Pointer to MPT_ADAPTER structure
 * @i: index into the array
 *
 * Returns the scsi_cmd pointer
 *
 **/
struct scsi_cmnd *
mptscsih_get_scsi_lookup(MPT_ADAPTER *ioc, int i)
{
	unsigned long	flags;
	struct scsi_cmnd *scmd;

	spin_lock_irqsave(&ioc->scsi_lookup_lock, flags);
	scmd = ioc->ScsiLookup[i];
	spin_unlock_irqrestore(&ioc->scsi_lookup_lock, flags);

	return scmd;
}

/**
 * mptscsih_getclear_scsi_lookup
 *
 * retrieves and clears scmd entry from ScsiLookup[] array list
 *
 * @ioc: Pointer to MPT_ADAPTER structure
 * @i: index into the array
 *
 * Returns the scsi_cmd pointer
 *
 **/
static struct scsi_cmnd *
mptscsih_getclear_scsi_lookup(MPT_ADAPTER *ioc, int i)
{
	unsigned long	flags;
	struct scsi_cmnd *scmd;

	spin_lock_irqsave(&ioc->scsi_lookup_lock, flags);
	scmd = ioc->ScsiLookup[i];
	ioc->ScsiLookup[i] = NULL;
	spin_unlock_irqrestore(&ioc->scsi_lookup_lock, flags);

	return scmd;
}

/**
 * mptscsih_set_scsi_lookup
 *
 * writes a scmd entry into the ScsiLookup[] array list
 *
 * @ioc: Pointer to MPT_ADAPTER structure
 * @i: index into the array
 * @scmd: scsi_cmnd pointer
 *
 **/
static void
mptscsih_set_scsi_lookup(MPT_ADAPTER *ioc, int i, struct scsi_cmnd *scmd)
{
	unsigned long	flags;

	spin_lock_irqsave(&ioc->scsi_lookup_lock, flags);
	ioc->ScsiLookup[i] = scmd;
	spin_unlock_irqrestore(&ioc->scsi_lookup_lock, flags);
}

/**
 * SCPNT_TO_LOOKUP_IDX - searches for a given scmd in the ScsiLookup[] array list
 * @ioc: Pointer to MPT_ADAPTER structure
 * @sc: scsi_cmnd pointer
 */
static int
SCPNT_TO_LOOKUP_IDX(MPT_ADAPTER *ioc, struct scsi_cmnd *sc)
{
	unsigned long	flags;
	int i, index=-1;

	spin_lock_irqsave(&ioc->scsi_lookup_lock, flags);
	for (i = 0; i < ioc->req_depth; i++) {
		if (ioc->ScsiLookup[i] == sc) {
			index = i;
			goto out;
		}
	}

 out:
	spin_unlock_irqrestore(&ioc->scsi_lookup_lock, flags);
	return index;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
int
mptscsih_ioc_reset(MPT_ADAPTER *ioc, int reset_phase)
{
	MPT_SCSI_HOST	*hd;

	if ((ioc->sh == NULL) || (ioc->sh->hostdata == NULL))
		return 0;

	hd = shost_priv(ioc->sh);
	switch (reset_phase) {
	case MPT_IOC_SETUP_RESET:
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "%s: MPT_IOC_SETUP_RESET\n", ioc->name, __func__));
		break;
	case MPT_IOC_PRE_RESET:
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "%s: MPT_IOC_PRE_RESET\n", ioc->name, __func__));
		mptscsih_flush_running_cmds(hd);
		break;
	case MPT_IOC_POST_RESET:
		dtmprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "%s: MPT_IOC_POST_RESET\n", ioc->name, __func__));
		if (ioc->internal_cmds.status & MPT_MGMT_STATUS_PENDING) {
			ioc->internal_cmds.status |=
				MPT_MGMT_STATUS_DID_IOCRESET;
			complete(&ioc->internal_cmds.done);
		}
		break;
	default:
		break;
	}
	return 1;		/* currently means nothing really */
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
int
mptscsih_event_process(MPT_ADAPTER *ioc, EventNotificationReply_t *pEvReply)
{
	u8 event = le32_to_cpu(pEvReply->Event) & 0xFF;

	devtverboseprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		"MPT event (=%02Xh) routed to SCSI host driver!\n",
		ioc->name, event));

	if ((event == MPI_EVENT_IOC_BUS_RESET ||
	    event == MPI_EVENT_EXT_BUS_RESET) &&
	    (ioc->bus_type == SPI) && (ioc->soft_resets < -1))
			ioc->soft_resets++;

	return 1;		/* currently means nothing really */
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *  Bus Scan and Domain Validation functionality ...
 */

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptscsih_scandv_complete - Scan and DV callback routine registered
 *	to Fustion MPT (base) driver.
 *
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@mf: Pointer to original MPT request frame
 *	@mr: Pointer to MPT reply frame (NULL if TurboReply)
 *
 *	This routine is called from mpt.c::mpt_interrupt() at the completion
 *	of any SCSI IO request.
 *	This routine is registered with the Fusion MPT (base) driver at driver
 *	load/init time via the mpt_register() API call.
 *
 *	Returns 1 indicating alloc'd request frame ptr should be freed.
 *
 *	Remark: Sets a completion code and (possibly) saves sense data
 *	in the IOC member localReply structure.
 *	Used ONLY for DV and other internal commands.
 */
int
mptscsih_scandv_complete(MPT_ADAPTER *ioc, MPT_FRAME_HDR *req,
				MPT_FRAME_HDR *reply)
{
	SCSIIORequest_t *pReq;
	SCSIIOReply_t	*pReply;
	u8		 cmd;
	u16		 req_idx;
	u8		*sense_data;
	int		 sz;

	ioc->internal_cmds.status |= MPT_MGMT_STATUS_COMMAND_GOOD;
	ioc->internal_cmds.completion_code = MPT_SCANDV_GOOD;
	if (!reply)
		goto out;

	pReply = (SCSIIOReply_t *) reply;
	pReq = (SCSIIORequest_t *) req;
	ioc->internal_cmds.completion_code =
	    mptscsih_get_completion_code(ioc, req, reply);
	ioc->internal_cmds.status |= MPT_MGMT_STATUS_RF_VALID;
	memcpy(ioc->internal_cmds.reply, reply,
	    min(MPT_DEFAULT_FRAME_SIZE, 4 * reply->u.reply.MsgLength));
	cmd = reply->u.hdr.Function;
	if (((cmd == MPI_FUNCTION_SCSI_IO_REQUEST) ||
	    (cmd == MPI_FUNCTION_RAID_SCSI_IO_PASSTHROUGH)) &&
	    (pReply->SCSIState & MPI_SCSI_STATE_AUTOSENSE_VALID)) {
		req_idx = le16_to_cpu(req->u.frame.hwhdr.msgctxu.fld.req_idx);
		sense_data = ((u8 *)ioc->sense_buf_pool +
		    (req_idx * MPT_SENSE_BUFFER_ALLOC));
		sz = min_t(int, pReq->SenseBufferLength,
		    MPT_SENSE_BUFFER_ALLOC);
		memcpy(ioc->internal_cmds.sense, sense_data, sz);
	}
 out:
	if (!(ioc->internal_cmds.status & MPT_MGMT_STATUS_PENDING))
		return 0;
	ioc->internal_cmds.status &= ~MPT_MGMT_STATUS_PENDING;
	complete(&ioc->internal_cmds.done);
	return 1;
}


/**
 *	mptscsih_get_completion_code -
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@reply:
 *	@cmd:
 *
 **/
static int
mptscsih_get_completion_code(MPT_ADAPTER *ioc, MPT_FRAME_HDR *req,
				MPT_FRAME_HDR *reply)
{
	SCSIIOReply_t	*pReply;
	MpiRaidActionReply_t *pr;
	u8		 scsi_status;
	u16		 status;
	int		 completion_code;

	pReply = (SCSIIOReply_t *)reply;
	status = le16_to_cpu(pReply->IOCStatus) & MPI_IOCSTATUS_MASK;
	scsi_status = pReply->SCSIStatus;

	devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "IOCStatus=%04xh, SCSIState=%02xh, SCSIStatus=%02xh,"
	    "IOCLogInfo=%08xh\n", ioc->name, status, pReply->SCSIState,
	    scsi_status, le32_to_cpu(pReply->IOCLogInfo)));

	switch (status) {

	case MPI_IOCSTATUS_SCSI_DEVICE_NOT_THERE:	/* 0x0043 */
		completion_code = MPT_SCANDV_SELECTION_TIMEOUT;
		break;

	case MPI_IOCSTATUS_SCSI_IO_DATA_ERROR:		/* 0x0046 */
	case MPI_IOCSTATUS_SCSI_TASK_TERMINATED:	/* 0x0048 */
	case MPI_IOCSTATUS_SCSI_IOC_TERMINATED:		/* 0x004B */
	case MPI_IOCSTATUS_SCSI_EXT_TERMINATED:		/* 0x004C */
		completion_code = MPT_SCANDV_DID_RESET;
		break;

	case MPI_IOCSTATUS_BUSY:
	case MPI_IOCSTATUS_INSUFFICIENT_RESOURCES:
		completion_code = MPT_SCANDV_BUSY;
		break;

	case MPI_IOCSTATUS_SCSI_DATA_UNDERRUN:		/* 0x0045 */
	case MPI_IOCSTATUS_SCSI_RECOVERED_ERROR:	/* 0x0040 */
	case MPI_IOCSTATUS_SUCCESS:			/* 0x0000 */
		if (pReply->Function == MPI_FUNCTION_CONFIG) {
			completion_code = MPT_SCANDV_GOOD;
		} else if (pReply->Function == MPI_FUNCTION_RAID_ACTION) {
			pr = (MpiRaidActionReply_t *)reply;
			if (le16_to_cpu(pr->ActionStatus) ==
				MPI_RAID_ACTION_ASTATUS_SUCCESS)
				completion_code = MPT_SCANDV_GOOD;
			else
				completion_code = MPT_SCANDV_SOME_ERROR;
		} else if (pReply->SCSIState & MPI_SCSI_STATE_AUTOSENSE_VALID)
			completion_code = MPT_SCANDV_SENSE;
		else if (pReply->SCSIState & MPI_SCSI_STATE_AUTOSENSE_FAILED) {
			if (req->u.scsireq.CDB[0] == INQUIRY)
				completion_code = MPT_SCANDV_ISSUE_SENSE;
			else
				completion_code = MPT_SCANDV_DID_RESET;
		} else if (pReply->SCSIState & MPI_SCSI_STATE_NO_SCSI_STATUS)
			completion_code = MPT_SCANDV_DID_RESET;
		else if (pReply->SCSIState & MPI_SCSI_STATE_TERMINATED)
			completion_code = MPT_SCANDV_DID_RESET;
		else if (scsi_status == MPI_SCSI_STATUS_BUSY)
			completion_code = MPT_SCANDV_BUSY;
		else
			completion_code = MPT_SCANDV_GOOD;
		break;

	case MPI_IOCSTATUS_SCSI_PROTOCOL_ERROR:		/* 0x0047 */
		if (pReply->SCSIState & MPI_SCSI_STATE_TERMINATED)
			completion_code = MPT_SCANDV_DID_RESET;
		else
			completion_code = MPT_SCANDV_SOME_ERROR;
		break;
	default:
		completion_code = MPT_SCANDV_SOME_ERROR;
		break;

	}	/* switch(status) */

	devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "  completionCode set to %08xh\n", ioc->name, completion_code));
	return completion_code;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_do_cmd - Do internal command.
 *	@hd: MPT_SCSI_HOST pointer
 *	@io: INTERNAL_CMD pointer.
 *
 *	Issue the specified internally generated command and do command
 *	specific cleanup. For bus scan / DV only.
 *	NOTES: If command is Inquiry and status is good,
 *	initialize a target structure, save the data
 *
 *	Remark: Single threaded access only.
 *
 *	Return:
 *		< 0 if an illegal command or no resources
 *
 *		   0 if good
 *
 *		 > 0 if command complete but some type of completion error.
 */
int
mptscsih_do_cmd(MPT_SCSI_HOST *hd, INTERNAL_CMD *io)
{
	MPT_FRAME_HDR	*mf;
	SCSIIORequest_t	*pScsiReq;
	int		 my_idx, ii, dir;
	int		 timeout;
	char		 cmdLen;
	char		 CDB[]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	u8		 cmd = io->cmd;
	MPT_ADAPTER *ioc = hd->ioc;
	int		 ret = 0;
	unsigned long	 timeleft;
	unsigned long	 flags;

	/* don't send internal command during diag reset */
	spin_lock_irqsave(&ioc->taskmgmt_lock, flags);
	if (ioc->ioc_reset_in_progress) {
		spin_unlock_irqrestore(&ioc->taskmgmt_lock, flags);
		dfailprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "%s: busy with host reset\n", ioc->name, __FUNCTION__));
		return MPT_SCANDV_BUSY;
	}
	spin_unlock_irqrestore(&ioc->taskmgmt_lock, flags);

	mutex_lock(&ioc->internal_cmds.mutex);

	/* Set command specific information
	 */
	switch (cmd) {
	case INQUIRY:
		cmdLen = 6;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
		CDB[4] = io->size;
		timeout = 10;
		break;

	case TEST_UNIT_READY:
		cmdLen = 6;
		dir = MPI_SCSIIO_CONTROL_READ;
		timeout = 10;
		break;

	case START_STOP:
		cmdLen = 6;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
		CDB[4] = 1;	/*Spin up the disk */
		timeout = 15;
		break;

	case REQUEST_SENSE:
		cmdLen = 6;
		CDB[0] = cmd;
		CDB[4] = io->size;
		dir = MPI_SCSIIO_CONTROL_READ;
		timeout = 10;
		break;

	case READ_BUFFER:
		cmdLen = 10;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
		if (io->flags & MPT_ICFLAG_ECHO) {
			CDB[1] = 0x0A;
		} else {
			CDB[1] = 0x02;
		}

		if (io->flags & MPT_ICFLAG_BUF_CAP) {
			CDB[1] |= 0x01;
		}
		CDB[6] = (io->size >> 16) & 0xFF;
		CDB[7] = (io->size >>  8) & 0xFF;
		CDB[8] = io->size & 0xFF;
		timeout = 10;
		break;

	case WRITE_BUFFER:
		cmdLen = 10;
		dir = MPI_SCSIIO_CONTROL_WRITE;
		CDB[0] = cmd;
		if (io->flags & MPT_ICFLAG_ECHO) {
			CDB[1] = 0x0A;
		} else {
			CDB[1] = 0x02;
		}
		CDB[6] = (io->size >> 16) & 0xFF;
		CDB[7] = (io->size >>  8) & 0xFF;
		CDB[8] = io->size & 0xFF;
		timeout = 10;
		break;

	case RESERVE:
		cmdLen = 6;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
		timeout = 10;
		break;

	case RELEASE:
		cmdLen = 6;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
		timeout = 10;
		break;

	case SYNCHRONIZE_CACHE:
		cmdLen = 10;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
//		CDB[1] = 0x02;	/* set immediate bit */
		timeout = 10;
		break;

	case REPORT_LUNS:
		cmdLen = 12;
		dir = MPI_SCSIIO_CONTROL_READ;
		CDB[0] = cmd;
		CDB[6] = (io->size >> 24) & 0xFF;
		CDB[7] = (io->size >> 16) & 0xFF;
		CDB[8] = (io->size >>  8) & 0xFF;
		CDB[9] = io->size & 0xFF;
		timeout = 10;
		break;

	case TRANSPORT_LAYER_RETRIES:
		CDB[0] = cmd;
		CDB[1] = 0x01;
		cmdLen = 6;
		dir = MPI_SCSIIO_CONTROL_READ;
		timeout = 10;
		break;
#ifdef EEDP_SUPPORT
	case SERVICE_ACTION_IN:
		CDB[0] = cmd;
		CDB[1] = 0x10;
		CDB[13] = io->size & 0xFF;
		dir = MPI_SCSIIO_CONTROL_READ;
		timeout = 10;
		cmdLen = 16;
		break;
#endif

	default:
		/* Error Case */
		ret = -EFAULT;
		goto out;
	}

	/* Get and Populate a free Frame
	 * MsgContext set in mpt_get_msg_frame call
	 */
	if ((mf = mpt_get_msg_frame(ioc->InternalCtx, ioc)) == NULL) {
		dfailprintk(ioc, printk(MYIOC_s_WARN_FMT "%s: No msg frames!\n",
		    ioc->name, __func__));
		ret = MPT_SCANDV_BUSY;
		goto out;
	}

	pScsiReq = (SCSIIORequest_t *) mf;

	/* Get the request index */
	my_idx = le16_to_cpu(mf->u.frame.hwhdr.msgctxu.fld.req_idx);
	ADD_INDEX_LOG(my_idx); /* for debug */

	if (io->flags & MPT_ICFLAG_PHYS_DISK) {
		pScsiReq->TargetID = io->physDiskNum;
		pScsiReq->Bus = 0;
		pScsiReq->ChainOffset = 0;
		pScsiReq->Function = MPI_FUNCTION_RAID_SCSI_IO_PASSTHROUGH;
	} else {
		pScsiReq->TargetID = io->id;
		pScsiReq->Bus = io->channel;
		pScsiReq->ChainOffset = 0;
		pScsiReq->Function = MPI_FUNCTION_SCSI_IO_REQUEST;
	}

	pScsiReq->CDBLength = cmdLen;
	pScsiReq->SenseBufferLength = MPT_SENSE_BUFFER_SIZE;

	pScsiReq->Reserved = 0;

	pScsiReq->MsgFlags = mpt_msg_flags(ioc);
	/* MsgContext set in mpt_get_msg_fram call  */

	int_to_scsilun(io->lun, (struct scsi_lun *)pScsiReq->LUN);

	if (io->flags & MPT_ICFLAG_TAGGED_CMD)
		pScsiReq->Control = cpu_to_le32(dir | MPI_SCSIIO_CONTROL_SIMPLEQ);
	else
		pScsiReq->Control = cpu_to_le32(dir | MPI_SCSIIO_CONTROL_UNTAGGED);

	if (cmd == REQUEST_SENSE) {
		pScsiReq->Control = cpu_to_le32(dir | MPI_SCSIIO_CONTROL_UNTAGGED);
		devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "%s: Untagged! 0x%02x\n", ioc->name, __func__, cmd));
	}

	for (ii = 0; ii < 16; ii++)
		pScsiReq->CDB[ii] = CDB[ii];

	pScsiReq->DataLength = cpu_to_le32(io->size);
	pScsiReq->SenseBufferLowAddr = cpu_to_le32(ioc->sense_buf_low_dma
					   + (my_idx * MPT_SENSE_BUFFER_ALLOC));

	devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT
	    "%s: Sending Command 0x%02x for fw_channel=%d fw_id=%d lun=%d\n",
	    ioc->name, __func__, cmd, io->channel, io->id, io->lun));

	if (dir == MPI_SCSIIO_CONTROL_READ)
		ioc->add_sge((char *) &pScsiReq->SGL,
		    MPT_SGE_FLAGS_SSIMPLE_READ | io->size, io->data_dma);
	else
		ioc->add_sge((char *) &pScsiReq->SGL,
		    MPT_SGE_FLAGS_SSIMPLE_WRITE | io->size, io->data_dma);

	INITIALIZE_MGMT_STATUS(ioc->internal_cmds.status)
	mpt_put_msg_frame(ioc->InternalCtx, ioc, mf);
	timeleft = wait_for_completion_timeout(&ioc->internal_cmds.done,
	    timeout*HZ);
	if (!(ioc->internal_cmds.status & MPT_MGMT_STATUS_COMMAND_GOOD)) {
		ret = MPT_SCANDV_DID_RESET;
		dfailprintk(ioc, printk(MYIOC_s_DEBUG_FMT
		    "%s: TIMED OUT for cmd=0x%02x\n", ioc->name, __func__,
		    cmd));
		if (ioc->internal_cmds.status & MPT_MGMT_STATUS_DID_IOCRESET) {
			mpt_free_msg_frame(ioc, mf);
			goto out;
		}
		if (!timeleft) {
			if (!mptscsih_scandv_bus_reset(ioc))
				goto out;
			printk(MYIOC_s_WARN_FMT "Issuing Reset from %s!!\n",
			    ioc->name, __func__);
			if (mpt_SoftResetHandler(ioc, CAN_SLEEP) != 0)
				mpt_HardResetHandler(ioc, CAN_SLEEP);
			mpt_free_msg_frame(ioc, mf);
		}
		goto out;
	}

	ret = ioc->internal_cmds.completion_code;
	devtprintk(ioc, printk(MYIOC_s_DEBUG_FMT "%s: success, rc=0x%02x\n",
			ioc->name, __func__, ret));

 out:
	CLEAR_MGMT_STATUS(ioc->internal_cmds.status)
	mutex_unlock(&ioc->internal_cmds.mutex);
	return ret;
}

int
mptscsih_quiesce_raid(MPT_SCSI_HOST *hd, int quiesce, u8 channel, u8 id)
{
	MPT_ADAPTER		*ioc = hd->ioc;
	MpiRaidActionRequest_t	*pReq;
	MPT_FRAME_HDR		*mf;
	int			ret;
	unsigned long		timeleft;

	mutex_lock(&ioc->internal_cmds.mutex);

	/* Get and Populate a free Frame
	 */
	if ((mf = mpt_get_msg_frame(ioc->InternalCtx, ioc)) == NULL) {
		dfailprintk(hd->ioc, printk(MYIOC_s_WARN_FMT "%s: no msg frames!\n",
			ioc->name, __FUNCTION__));
		ret = -EAGAIN;
		goto out;
	}
	pReq = (MpiRaidActionRequest_t *)mf;
	if (quiesce)
		pReq->Action = MPI_RAID_ACTION_QUIESCE_PHYS_IO;
	else
		pReq->Action = MPI_RAID_ACTION_ENABLE_PHYS_IO;
	pReq->Reserved1 = 0;
	pReq->ChainOffset = 0;
	pReq->Function = MPI_FUNCTION_RAID_ACTION;
	pReq->VolumeID = id;
	pReq->VolumeBus = channel;
	pReq->PhysDiskNum = 0;
	pReq->MsgFlags = 0;
	pReq->Reserved2 = 0;
	pReq->ActionDataWord = 0; /* Reserved for this action */

	ioc->add_sge((char *)&pReq->ActionDataSGE,
		MPT_SGE_FLAGS_SSIMPLE_READ | 0, (dma_addr_t) -1);

	ddvprintk(ioc, printk(MYIOC_s_DEBUG_FMT "RAID Volume action=%x channel=%d id=%d\n",
			ioc->name, pReq->Action, channel, id));

	INITIALIZE_MGMT_STATUS(ioc->internal_cmds.status)
	mpt_put_msg_frame(ioc->InternalCtx, ioc, mf);
	timeleft = wait_for_completion_timeout(&ioc->internal_cmds.done, 10*HZ);
	if (!(ioc->internal_cmds.status & MPT_MGMT_STATUS_COMMAND_GOOD)) {
		ret = -ETIME;
		dfailprintk(ioc, printk(MYIOC_s_DEBUG_FMT "%s: TIMED OUT!\n",
		    ioc->name, __FUNCTION__));
		if (ioc->internal_cmds.status & MPT_MGMT_STATUS_DID_IOCRESET)
			goto out;
		if (!timeleft) {
			printk(MYIOC_s_WARN_FMT "Issuing Reset from %s!!\n",
			    ioc->name, __FUNCTION__);
			if (mpt_SoftResetHandler(ioc, CAN_SLEEP) != 0)
				mpt_HardResetHandler(ioc, CAN_SLEEP);
			mpt_free_msg_frame(ioc, mf);
		}
		goto out;
	}

	ret = ioc->internal_cmds.completion_code;

 out:
	CLEAR_MGMT_STATUS(ioc->internal_cmds.status)
	mutex_unlock(&ioc->internal_cmds.mutex);
	return ret;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mptscsih_synchronize_cache - Send SYNCHRONIZE_CACHE to all disks.
 *	@hd: Pointer to a SCSI HOST structure
 *	@vdevice: virtual target device
 *
 *	Uses the ISR, but with special processing.
 *	MUST be single-threaded.
 *
 */
static void
mptscsih_synchronize_cache(struct scsi_device *sdev, MPT_SCSI_HOST *hd, VirtDevice *vdevice)
{
	INTERNAL_CMD		 iocmd;
	MPT_ADAPTER *ioc = hd->ioc;

	/* Ignore hidden raid components, this is handled when the command
	 * is sent to the volume
	 */
	if (vdevice->vtarget->tflags & MPT_TARGET_FLAGS_RAID_COMPONENT)
		return;

	if (vdevice->vtarget->type != TYPE_DISK || vdevice->vtarget->deleted ||
	    !vdevice->configured_lun)
		return;

	memset(&iocmd, 0, sizeof(INTERNAL_CMD));
	iocmd.cmd = SYNCHRONIZE_CACHE;
	iocmd.physDiskNum = -1;
	iocmd.data = NULL;
	iocmd.data_dma = -1;
	iocmd.channel = vdevice->vtarget->channel;
	iocmd.id = vdevice->vtarget->id;
	iocmd.lun = vdevice->lun;

	sdev_printk(KERN_INFO, sdev, MYIOC_s_FMT "SYNCHRONIZE_CACHE: fw_channel %d,"
	    " fw_id %d\n", ioc->name, vdevice->vtarget->channel, vdevice->vtarget->id);
	mptscsih_do_cmd(hd, &iocmd);
}

/*
 * shost attributes
 */
static ssize_t
mptscsih_fault_show(struct device *dev, struct device_attribute *attr,
   char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%d\n", ioc->is_fault);
}
static ssize_t
mptscsih_fault_store(struct device *dev, struct device_attribute *attr,
    const char *buf, size_t count)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;
	int val = 0;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	ioc->is_fault = val;
	return strlen(buf);

}

struct DIAG_BUFFER_START {
	u32 Size;
	u32 DiagVersion;
	u8 BufferType;
	u8 Reserved[3];
	u32 Reserved1;
	u32 Reserved2;
	u32 Reserved3;
};

static ssize_t
mptscsih_ring_buffer_size_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;
	u32 size = 0;
	struct DIAG_BUFFER_START *request_data;

	ioc->ring_buffer_sz = 0;
	if (!ioc->DiagBuffer[0])
		return 0;

	request_data = (struct DIAG_BUFFER_START *)ioc->DiagBuffer[0];
	if ((le32_to_cpu(request_data->DiagVersion) == 0x00000000 ||
	    le32_to_cpu(request_data->DiagVersion) == 0x01000000) &&
	    le32_to_cpu(request_data->Reserved3) == 0x4742444c) {
		size = le32_to_cpu(request_data->Size);
		ioc->ring_buffer_sz = size;
	}

	return snprintf(buf, PAGE_SIZE, "%d\n", size);
}

static ssize_t
mptscsih_ring_buffer_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;
	void *request_data;
	u32 size;

	if (!ioc->DiagBuffer[0])
		return 0;

	if (ioc->ring_buffer_offset > ioc->ring_buffer_sz)
		return 0;

	size = ioc->ring_buffer_sz - ioc->ring_buffer_offset;
	size = (size > PAGE_SIZE) ? PAGE_SIZE : size;
	request_data = ioc->DiagBuffer[0] + ioc->ring_buffer_offset;
	memcpy(buf, request_data, size);
	return size;
}

static ssize_t
mptscsih_ring_buffer_store(struct device *dev, struct device_attribute *attr,
    const char *buf, size_t count)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;
	int val = 0;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	ioc->ring_buffer_offset = val;
	return strlen(buf);
}

static ssize_t
mptscsih_version_fw_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02d.%02d.%02d.%02d\n",
	    (ioc->facts.FWVersion.Word & 0xFF000000) >> 24,
	    (ioc->facts.FWVersion.Word & 0x00FF0000) >> 16,
	    (ioc->facts.FWVersion.Word & 0x0000FF00) >> 8,
	    ioc->facts.FWVersion.Word & 0x000000FF);
}

static ssize_t
mptscsih_version_bios_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02d.%02d.%02d.%02d\n",
	    (ioc->biosVersion & 0xFF000000) >> 24,
	    (ioc->biosVersion & 0x00FF0000) >> 16,
	    (ioc->biosVersion & 0x0000FF00) >> 8,
	    ioc->biosVersion & 0x000000FF);
}

static ssize_t
mptscsih_version_mpi_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	if (ioc->facts.MsgVersion >= MPI_VERSION_01_05)
		return snprintf(buf, PAGE_SIZE, "%03x.%02x\n",
		    ioc->facts.MsgVersion, ioc->facts.HeaderVersion >> 8);
	else
		return snprintf(buf, PAGE_SIZE, "%03x\n",
		    ioc->facts.MsgVersion);
}

static ssize_t
mptscsih_version_product_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%s\n", ioc->prod_name);
}

static ssize_t
mptscsih_version_nvdata_persistent_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02xh\n",
	    ioc->nvdata_version_persistent);
}

static ssize_t
mptscsih_version_nvdata_default_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02xh\n",ioc->nvdata_version_default);
}

static ssize_t
mptscsih_board_name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%s\n", ioc->board_name);
}

static ssize_t
mptscsih_board_assembly_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%s\n", ioc->board_assembly);
}

static ssize_t
mptscsih_board_tracer_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%s\n", ioc->board_tracer);
}

static ssize_t
mptscsih_io_delay_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02d\n", ioc->io_missing_delay);
}

static ssize_t
mptscsih_device_delay_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02d\n", ioc->device_missing_delay);
}

static ssize_t
mptscsih_debug_level_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%08xh\n", ioc->debug_level);
}

static ssize_t
mptscsih_debug_level_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;
	int val = 0;

	if (sscanf(buf, "%x", &val) != 1)
		return -EINVAL;

	ioc->debug_level = val;
	printk(MYIOC_s_INFO_FMT "debug_level=%08xh\n", ioc->name,
	    ioc->debug_level);
	return strlen(buf);
}

static ssize_t
mptscsih_disable_hotplug_remove_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;

	return snprintf(buf, PAGE_SIZE, "%02xh\n", ioc->disable_hotplug_remove);
}
static ssize_t
mptscsih_disable_hotplug_remove_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct Scsi_Host *host = class_to_shost(dev);
	MPT_SCSI_HOST	*hd = shost_priv(host);
	MPT_ADAPTER *ioc = hd->ioc;
	int val = 0;

	if (sscanf(buf, "%x", &val) != 1)
		return -EINVAL;

	ioc->disable_hotplug_remove = val;
	if (ioc->disable_hotplug_remove)
		printk(MYIOC_s_INFO_FMT "disabling hotplug remove\n",
		    ioc->name);
	else
		printk(MYIOC_s_INFO_FMT "eanbling hotplug remove\n", ioc->name);
	return strlen(buf);
}

static DEVICE_ATTR(fault, S_IRUGO | S_IWUSR,
    mptscsih_fault_show, mptscsih_fault_store);
static DEVICE_ATTR(ring_buffer_size, S_IRUGO,
    mptscsih_ring_buffer_size_show, NULL);
static DEVICE_ATTR(ring_buffer, S_IRUGO | S_IWUSR,
    mptscsih_ring_buffer_show, mptscsih_ring_buffer_store);
static DEVICE_ATTR(version_fw, S_IRUGO, mptscsih_version_fw_show, NULL);
static DEVICE_ATTR(version_bios, S_IRUGO,
    mptscsih_version_bios_show, NULL);
static DEVICE_ATTR(version_mpi, S_IRUGO, mptscsih_version_mpi_show, NULL);
static DEVICE_ATTR(version_product, S_IRUGO,
    mptscsih_version_product_show, NULL);
static DEVICE_ATTR(version_nvdata_persistent, S_IRUGO,
    mptscsih_version_nvdata_persistent_show, NULL);
static DEVICE_ATTR(version_nvdata_default, S_IRUGO,
    mptscsih_version_nvdata_default_show, NULL);
static DEVICE_ATTR(board_name, S_IRUGO, mptscsih_board_name_show, NULL);
static DEVICE_ATTR(board_assembly, S_IRUGO,
    mptscsih_board_assembly_show, NULL);
static DEVICE_ATTR(board_tracer, S_IRUGO,
    mptscsih_board_tracer_show, NULL);
static DEVICE_ATTR(io_delay, S_IRUGO,
    mptscsih_io_delay_show, NULL);
static DEVICE_ATTR(device_delay, S_IRUGO,
    mptscsih_device_delay_show, NULL);
static DEVICE_ATTR(debug_level, S_IRUGO | S_IWUSR,
    mptscsih_debug_level_show, mptscsih_debug_level_store);
static DEVICE_ATTR(disable_hotplug_remove, S_IRUGO | S_IWUSR,
    mptscsih_disable_hotplug_remove_show, mptscsih_disable_hotplug_remove_store);

struct device_attribute *mptscsih_host_attrs[] = {
	&dev_attr_fault,
	&dev_attr_ring_buffer_size,
	&dev_attr_ring_buffer,
	&dev_attr_version_fw,
	&dev_attr_version_bios,
	&dev_attr_version_mpi,
	&dev_attr_version_product,
	&dev_attr_version_nvdata_persistent,
	&dev_attr_version_nvdata_default,
	&dev_attr_board_name,
	&dev_attr_board_assembly,
	&dev_attr_board_tracer,
	&dev_attr_io_delay,
	&dev_attr_device_delay,
	&dev_attr_debug_level,
	&dev_attr_disable_hotplug_remove,
	NULL,
};

EXPORT_SYMBOL(mptscsih_host_attrs);

EXPORT_SYMBOL(mptscsih_remove);
EXPORT_SYMBOL(mptscsih_shutdown);
#ifdef CONFIG_PM
EXPORT_SYMBOL(mptscsih_suspend);
EXPORT_SYMBOL(mptscsih_resume);
#endif
EXPORT_SYMBOL(mptscsih_proc_info);
EXPORT_SYMBOL(mptscsih_info);
EXPORT_SYMBOL(mptscsih_qcmd);
EXPORT_SYMBOL(mptscsih_slave_destroy);
EXPORT_SYMBOL(mptscsih_slave_configure);
EXPORT_SYMBOL(mptscsih_abort);
EXPORT_SYMBOL(mptscsih_dev_reset);
EXPORT_SYMBOL(mptscsih_bus_reset);
EXPORT_SYMBOL(mptscsih_host_reset);
EXPORT_SYMBOL(mptscsih_bios_param);
EXPORT_SYMBOL(mptscsih_io_done);
EXPORT_SYMBOL(mptscsih_taskmgmt_complete);
EXPORT_SYMBOL(mptscsih_scandv_complete);
EXPORT_SYMBOL(mptscsih_event_process);
EXPORT_SYMBOL(mptscsih_ioc_reset);
EXPORT_SYMBOL(mptscsih_change_queue_depth);
EXPORT_SYMBOL(mptscsih_IssueTaskMgmt);
EXPORT_SYMBOL(mptscsih_do_cmd);
EXPORT_SYMBOL(mptscsih_quiesce_raid);
EXPORT_SYMBOL(mptscsih_get_scsi_lookup);
EXPORT_SYMBOL(mptscsih_taskmgmt_response_code);
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
