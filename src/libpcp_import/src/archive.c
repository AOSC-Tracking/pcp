/*
 * Copyright (c) 2017-2022 Red Hat.
 * Copyright (c) 2010 Ken McDonell.  All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#include "pmapi.h"
#include "libpcp.h"
#include "import.h"
#include "private.h"

static __pmTimestamp	stamp;

static int
check_context_start(pmi_context *current)
{
    const char	*host;
    char	myname[MAXHOSTNAMELEN];
    __pmLogCtl	*lcp;
    __pmArchCtl	*acp;
    int		sts;

    if (current->state != CONTEXT_START)
	return 0; /* ok */

    if (current->hostname == NULL) {
	(void)gethostname(myname, MAXHOSTNAMELEN);
	myname[MAXHOSTNAMELEN-1] = '\0';
	host = myname;
    }
    else
	host = current->hostname;

    acp = &current->archctl;
    acp->ac_log = &current->logctl;
    lcp = &current->logctl;

    /* open a possibly-existing-possibly-not archive, create it if not */
    if ((current->flags & PMI_FLAG_APPEND)) {
	if (__pmLogFindOpen(acp, current->archive) == 0) {
	    /* file exists so we're going to use it */
	    acp->ac_curvol = -1;
	    if ((sts = __pmLogChangeVol(acp, lcp->maxvol)) < 0)
		return PM_ERR_LOGFILE;
	    if ((sts = __pmLogLoadMeta(acp)) < 0)
		return PM_ERR_LOGFILE;
	    if ((sts = __pmLogLoadIndex(lcp)) < 0)
		return PM_ERR_LOGFILE;
	    if (acp->ac_mfp)
		__pmFseek(acp->ac_mfp, 0, SEEK_END);
	    if (lcp->mdfp)
		__pmFseek(lcp->mdfp, 0, SEEK_END);
	    if (lcp->tifp)
		__pmFseek(lcp->tifp, 0, SEEK_END);
	    if (lcp->label.zoneinfo)
		pmNewZone(lcp->label.zoneinfo);
	    else
		pmNewZone(lcp->label.timezone);
	    current->state = CONTEXT_ACTIVE;
	    lcp->state = PM_LOG_STATE_INIT;
	    return 1; /* ok */
	}
    }

    /* clear append flag as archive must be created */
    current->flags &= ~PMI_FLAG_APPEND;

    sts = __pmLogCreate(host, current->archive, current->version, acp);
    if (sts < 0)
	return sts;

    if (current->timezone != NULL) {
	free(lcp->label.timezone);
	lcp->label.timezone = strdup(current->timezone);
	free(lcp->label.zoneinfo);
	lcp->label.zoneinfo = NULL;
    }
    pmNewZone(lcp->label.timezone);
    current->state = CONTEXT_ACTIVE;

    /*
     * Do the archive label records (it is too late when __pmLogPutResult
     * or __pmLogPutResult2 is called as we've already output some
     * metadata) ... this code is stolen from logputresult() in
     * libpcp
     */
    lcp->label.start.sec = stamp.sec;
    lcp->label.start.nsec = stamp.nsec;
    lcp->label.vol = PM_LOG_VOL_TI;
    __pmLogWriteLabel(lcp->tifp, &lcp->label);
    lcp->label.vol = PM_LOG_VOL_META;
    __pmLogWriteLabel(lcp->mdfp, &lcp->label);
    lcp->label.vol = 0;
    __pmLogWriteLabel(acp->ac_mfp, &lcp->label);
    lcp->state = PM_LOG_STATE_INIT;

    return 0; /* ok */
}

static int
compare_instances(__pmArchCtl *acp, pmi_indom *inp)
{
    char	**namelist;
    int		*instlist;
    int		i, j, n, sts;

    /* when appending, see if this indom is on-disk already */
    sts = n = __pmLogGetInDom(acp, inp->indom, &stamp, &instlist, &namelist);
    if (sts < 0)
	return sts;
    if (n != inp->ninstance)
	return -ENOENT;

    for (i = 0; i < inp->ninstance; i++) {
	for (j = 0; j < n; j++) {
	    if (instlist[i] != inp->inst[j])
		continue;
	    if (strcmp(namelist[i], inp->name[j]) == 0)
		break;
	}
	if (j == n) /* no match found */
	    return -ENOENT;
    }

    return 0;	/* completely matched all indom elements */
}

static int
check_indom(pmi_context *current, pmInDom indom, int *needti)
{
    int			i;
    int			sts = 0;
    int			type;
    __pmArchCtl		*acp = &current->archctl;
    __pmLogInDom	lid;

    type = current->version == PM_LOG_VERS03 ? TYPE_INDOM : TYPE_INDOM_V2;

    for (i = 0; i < current->nindom; i++) {
	if (indom == current->indom[i].indom) {
	    if (current->indom[i].meta_done == 0) {
		if ((current->flags & PMI_FLAG_APPEND) &&
		    (compare_instances(acp, &current->indom[i])) == 0) {
		    current->indom[i].meta_done = 1;
		    break;
		}

		lid.stamp = stamp;
		lid.indom = current->indom[i].indom;
		lid.numinst = current->indom[i].ninstance;
		lid.instlist = current->indom[i].inst;
		lid.namelist = current->indom[i].name;
		lid.alloc = 0;
		if ((sts = __pmLogPutInDom(acp, type, &lid)) < 0)
		    return sts;
		current->indom[i].meta_done = 1;
		*needti = 1;
		break;
	    }
	}
    }

    return sts;
}

int
compare_descs(pmDesc *a, pmDesc *b)
{
    if (a->type != b->type)
	return PM_ERR_LOGCHANGETYPE;
    if (a->indom != b->indom)
	return PM_ERR_LOGCHANGEINDOM;
    if (a->sem != b->sem)
	return PM_ERR_LOGCHANGESEM;
    if (memcmp(&a->units, &b->units, sizeof(pmUnits)) != 0)
	return PM_ERR_LOGCHANGEUNITS;
    return 0;
}

static int
check_metric(pmi_context *current, pmID pmid, int *needti)
{
    int		m;
    int		sts = 0;
    __pmArchCtl	*acp = &current->archctl;

    for (m = 0; m < current->nmetric; m++) {
	if (pmid != current->metric[m].pmid)
	    continue;
	if (current->metric[m].meta_done == 0) {
	    char	**namelist = &current->metric[m].name;
	    pmDesc	chk, *desc = &current->metric[m].desc;
	    int		desc_done = 0;

	    if ((current->flags & PMI_FLAG_APPEND)) {
		if (__pmLogLookupDesc(acp, pmid, &chk) == 0) {
		    if ((sts = compare_descs(desc, &chk)) < 0)
			return sts;
		    desc_done = 1;
		}
	    }

	    if (desc_done)
		current->metric[m].meta_done = 1;
	    else if ((sts = __pmLogPutDesc(acp, desc, 1, namelist)) < 0)
		return sts;
	    else {
		current->metric[m].meta_done = 1;
		*needti = 1;
	    }
	}
	if (current->metric[m].desc.indom != PM_INDOM_NULL) {
	    if ((sts = check_indom(current, current->metric[m].desc.indom, needti)) < 0)
		return sts;
	}
	break;
    }

    return sts;
}

static int
newvolume(pmi_context *current)
{
    __pmFILE		*newfp;
    __pmArchCtl		*acp = &current->archctl;
    __pmLogCtl		*lcp = &current->logctl;
    int			nextvol = acp->ac_curvol + 1;

    if ((newfp = __pmLogNewFile(current->archive, nextvol)) == NULL) {
	return PM_ERR_LOGFILE;
    }

    if (pmDebugOptions.log)
	fprintf(stderr, "logimport: '%s' new log volume %d\n", current->archive, nextvol);

    __pmFclose(acp->ac_mfp);
    acp->ac_mfp = newfp;
    lcp->label.vol = acp->ac_curvol = nextvol;
    __pmLogWriteLabel(acp->ac_mfp, &lcp->label);
    __pmFflush(acp->ac_mfp);
    return 0;
}

static off_t	flushsize = 100000;

int
_pmi_put_result(pmi_context *current, __pmResult *result)
{
    int		sts;
    __pmPDU	*pb;
    __pmArchCtl	*acp = &current->archctl;
    __pmLogCtl	*lcp = &current->logctl;
    int		k;
    int		needti;
    char	*p;
    static __uint64_t	max_logsz = 0;
    unsigned long off;
    off_t	old_meta_offset;

    /*
     * some front-end tools use lazy discovery of instances and/or process
     * data in non-deterministic order ... it is simpler for everyone if
     * we sort the values into ascending instance order.
     */
    __pmSortInstances(result);

    stamp = result->timestamp;	/* struct assignment */

    /* One time processing for the start of the context. */
    sts = check_context_start(current);
    if (sts < 0)
	return sts;

    old_meta_offset = __pmFtell(lcp->mdfp);

    __pmOverrideLastFd(__pmFileno(acp->ac_mfp));
    sts = __pmEncodeResult(acp->ac_log, result, &pb);
    if (sts < 0)
	return sts;

    needti = 0;
    for (k = 0; k < result->numpmid; k++) {
	sts = check_metric(current, result->vset[k]->pmid, &needti);
	if (sts < 0) {
	    __pmUnpinPDUBuf(pb);
	    return sts;
	}
    }

    if (max_logsz == 0) {
	if ((p = getenv("PCP_LOGIMPORT_MAXLOGSZ")) != NULL)
	    max_logsz = strtoull(p, NULL, 10);
	else if (current->version >= PM_LOG_VERS03)
	    max_logsz = LONGLONG_MAX;
	else  /* PM_LOG_VERS02 */
	    max_logsz = 0x7fffffff;
    }

    off = __pmFtell(acp->ac_mfp) + ((__pmPDUHdr *)pb)->len - sizeof(__pmPDUHdr) + 2*sizeof(int);
    if (off >= max_logsz) {
    	sts = newvolume(current);
	if (sts < 0) {
	    __pmUnpinPDUBuf(pb);
	    return sts;
	}
	flushsize = 100000;
	needti = 1;
    }

    if (needti || __pmFtell(acp->ac_mfp) + ((__pmPDUHdr *)pb)->len - sizeof(__pmPDUHdr) + 2*sizeof(int) > flushsize) {
	/*
	 * need new temporal index entry ... seek pointers need to be
	 * _before_ this pmResult and associated metadata (if any)
	 */
	off_t	new_meta_offset;
	__pmFflush(lcp->mdfp);
	new_meta_offset = __pmFtell(lcp->mdfp);;
	__pmFseek(lcp->mdfp, old_meta_offset, SEEK_SET);
	 __pmLogPutIndex(acp, &stamp);
	/* and restore metadata seek pointer */
	__pmFseek(lcp->mdfp, new_meta_offset, SEEK_SET);
	flushsize = __pmFtell(acp->ac_mfp) + 100000;
    }

    sts = current->version >= PM_LOG_VERS03 ?
	    __pmLogPutResult3(acp, pb) : __pmLogPutResult2(acp, pb);

    __pmUnpinPDUBuf(pb);

    if (sts < 0)
	return sts;
    return 0;
}

int
_pmi_put_mark(pmi_context *current, __pmTimestamp *last_stamp)
{
    __pmArchCtl		*acp = &current->archctl;
    __pmTimestamp	msec = { 0, 1000000 };  /* 1msec */
    int			sts;

    /* One time processing for the start of the context. */
    sts = check_context_start(current);
    if (sts < 0)
	return sts;

    return __pmLogWriteMark(acp, last_stamp, &msec);
}

int
_pmi_put_text(pmi_context *current)
{
    __pmArchCtl	*acp = &current->archctl;
    pmi_text	*tp;
    char	*bp;
    int		needti;
    int		sts;
    int		t;

    /* last_stamp has been set by the caller. */
    stamp = current->last_stamp;

    /* One time processing for the start of the context. */
    sts = check_context_start(current);
    if (sts < 0)
	return sts;

    __pmOverrideLastFd(__pmFileno(acp->ac_mfp));

    needti = 0;
    for (t = 0; t < current->ntext; t++) {
	tp = &current->text[t];
	if (tp->meta_done)
	    continue; /* Already written */

	if ((current->flags & PMI_FLAG_APPEND) &&
	    (__pmLogLookupText(acp, tp->id, tp->type, &bp)) == 0)
	    continue;	/* Previously written to archive */

	if ((tp->type & PM_TEXT_PMID)) {
	    /*
	     * This text is for a metric. Make sure that the metric desc
	     * has been written.
	     */
	    sts = check_metric(current, tp->id, &needti);
	    if (sts < 0)
		return sts;
	}
	else if ((tp->type & PM_TEXT_INDOM)) {
	    /*
	     * This text is for an indom. Make sure that the indom
	     * has been written.
	     */
	    sts = check_indom(current, tp->id, &needti);
	    if (sts < 0)
		return sts;
	}

	/*
	 * Now write out the text record.
	 * libpcp, via __pmLogPutText(), makes a copy of the storage pointed
	 * to by buffer.  (final 1 parameter below == 'cached')
	 */
	if ((sts = __pmLogPutText(acp, tp->id, tp->type, tp->content, 1)) < 0)
	    return sts;

	tp->meta_done = 1;
    }

    if (needti)
	__pmLogPutIndex(acp, &stamp);

    return 0;
}

int
_pmi_put_label(pmi_context *current)
{
    int		sts;
    __pmArchCtl	*acp = &current->archctl;
    pmLabelSet	*lsp;
    pmi_label	*lp;
    int		l;
    int		needti;

    /* last_stamp has been set by the caller. */
    stamp = current->last_stamp;

    /* One time processing for the start of the context. */
    sts = check_context_start(current);
    if (sts < 0)
	return sts;

    __pmOverrideLastFd(__pmFileno(acp->ac_mfp));

    needti = 0;
    for (l = 0; l < current->nlabel; l++) {
	lp = &current->label[l];

	if ((current->flags & PMI_FLAG_APPEND) &&
	    (__pmLogLookupLabel(acp, lp->type, lp->id, &lsp, NULL)) > 0)
	    continue;	/* Previously written to archive */

	if (lp->type == PM_LABEL_ITEM) {
	    /*
	     * This label is for a metric. Make sure that the metric desc
	     * has been written.
	     */
	    sts = check_metric(current, lp->id, &needti);
	    if (sts < 0)
		return sts;
	}
	else if (lp->type == PM_LABEL_INDOM || lp->type == PM_LABEL_INSTANCES) {
	    /*
	     * This label is for an indom. Make sure that the indom
	     * has been written.
	     */
	    sts = check_indom(current, lp->id, &needti);
	    if (sts < 0)
		return sts;
	}

	/*
	 * Now write out the label record.
	 * libpcp, via __pmLogPutLabels(), assumes control of the
	 * storage pointed to by lp->labelset.
	 */
	if ((sts = __pmLogPutLabels(acp, lp->type, lp->id,
				   1, lp->labelset, &stamp)) < 0)
	    return sts;

	lp->labelset = NULL;
    }

    /* We no longer need the accumulated list of labelsets. */
    free(current->label);
    current->nlabel = 0;
    current->label = NULL;

    if (needti)
	__pmLogPutIndex(acp, &stamp);

    return 0;
}

int
_pmi_end(pmi_context *current)
{
    /* Final temporal index update to finish the archive
     * ... same logic here as in run_done() for pmlogger
     */
    __pmLogPutIndex(&current->archctl, &stamp);

    __pmLogClose(&current->archctl);

    current->state = CONTEXT_END;
    return 0;
}
