/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "index-storage.h"

int index_expunge_seek_first(IndexMailbox *ibox, unsigned int *seq,
			     MailIndexRecord **rec)
{
	MailIndexHeader *hdr;

	i_assert(ibox->index->lock_type == MAIL_LOCK_EXCLUSIVE);

	hdr = ibox->index->get_header(ibox->index);
	if (hdr->deleted_messages_count == 0) {
		/* no deleted messages */
		*seq = 0;
		*rec = NULL;
		return TRUE;
	}

	/* find mails with DELETED flag and expunge them */
	if (hdr->first_deleted_uid_lowwater > 1) {
		*rec = hdr->first_deleted_uid_lowwater >= hdr->next_uid ? NULL :
			ibox->index->lookup_uid_range(ibox->index,
						hdr->first_deleted_uid_lowwater,
						hdr->next_uid-1, seq);
		if (*rec == NULL) {
			mail_storage_set_critical(ibox->box.storage,
				"index header's deleted_messages_count (%u) "
				"or first_deleted_uid_lowwater (%u) "
				"is invalid.", hdr->deleted_messages_count,
				hdr->first_deleted_uid_lowwater);

			/* fsck should be enough to fix it */
			ibox->index->set_flags |= MAIL_INDEX_FLAG_FSCK;
			return FALSE;
		}
	} else {
		*rec = ibox->index->lookup(ibox->index, 1);
		*seq = 1;
	}

	return TRUE;
}

int index_expunge_mail(IndexMailbox *ibox, MailIndexRecord *rec,
		       unsigned int seq, int notify)
{
	if (!ibox->index->expunge(ibox->index, rec, seq, FALSE))
		return FALSE;

	if (seq <= ibox->synced_messages_count) {
		if (notify) {
			ibox->sync_callbacks.expunge(&ibox->box, seq,
						     ibox->sync_context);
		}
		ibox->synced_messages_count--;
	}

	return TRUE;
}

int index_storage_expunge(Mailbox *box, int notify)
{
	IndexMailbox *ibox = (IndexMailbox *) box;
	int failed;

	if (box->readonly) {
		mail_storage_set_error(box->storage, "Mailbox is read-only");
		return FALSE;
	}

	if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_EXCLUSIVE))
		return mail_storage_set_index_error(ibox);

	if (!index_storage_sync_and_lock(ibox, FALSE, MAIL_LOCK_EXCLUSIVE))
		return FALSE;

	/* modifylog must be marked synced before expunging
	   anything new */
	if (!index_storage_sync_modifylog(ibox, TRUE))
		failed = TRUE;
	else
		failed = !ibox->expunge_locked(ibox, notify);

	if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_UNLOCK))
		return mail_storage_set_index_error(ibox);

	return !failed;
}
