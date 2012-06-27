// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync/glue/generic_change_processor.h"

#include "base/location.h"
#include "base/string_number_conversions.h"
#include "base/utf_string_conversions.h"
#include "content/public/browser/browser_thread.h"
#include "sync/api/sync_change.h"
#include "sync/api/sync_error.h"
#include "sync/api/syncable_service.h"
#include "sync/internal_api/public/base_node.h"
#include "sync/internal_api/public/change_record.h"
#include "sync/internal_api/public/read_node.h"
#include "sync/internal_api/public/read_transaction.h"
#include "sync/internal_api/public/util/unrecoverable_error_handler.h"
#include "sync/internal_api/public/write_node.h"
#include "sync/internal_api/public/write_transaction.h"
#include "sync/syncable/entry.h"  // TODO(tim): Bug 123674.

using content::BrowserThread;

namespace browser_sync {

GenericChangeProcessor::GenericChangeProcessor(
    DataTypeErrorHandler* error_handler,
    const base::WeakPtr<csync::SyncableService>& local_service,
    csync::UserShare* user_share)
    : ChangeProcessor(error_handler),
      local_service_(local_service),
      share_handle_(user_share) {
  DCHECK(CalledOnValidThread());
}

GenericChangeProcessor::~GenericChangeProcessor() {
  DCHECK(CalledOnValidThread());
}

void GenericChangeProcessor::ApplyChangesFromSyncModel(
    const csync::BaseTransaction* trans,
    const csync::ImmutableChangeRecordList& changes) {
  DCHECK(CalledOnValidThread());
  DCHECK(running());
  DCHECK(syncer_changes_.empty());
  for (csync::ChangeRecordList::const_iterator it =
           changes.Get().begin(); it != changes.Get().end(); ++it) {
    if (it->action == csync::ChangeRecord::ACTION_DELETE) {
      syncer_changes_.push_back(
          csync::SyncChange(csync::SyncChange::ACTION_DELETE,
                     csync::SyncData::CreateRemoteData(it->id, it->specifics)));
    } else {
      csync::SyncChange::SyncChangeType action =
          (it->action == csync::ChangeRecord::ACTION_ADD) ?
          csync::SyncChange::ACTION_ADD : csync::SyncChange::ACTION_UPDATE;
      // Need to load specifics from node.
      csync::ReadNode read_node(trans);
      if (read_node.InitByIdLookup(it->id) != csync::BaseNode::INIT_OK) {
        error_handler()->OnSingleDatatypeUnrecoverableError(
            FROM_HERE,
            "Failed to look up data for received change with id " +
                base::Int64ToString(it->id));
        return;
      }
      syncer_changes_.push_back(
          csync::SyncChange(action,
                     csync::SyncData::CreateRemoteData(
                         it->id, read_node.GetEntitySpecifics())));
    }
  }
}

void GenericChangeProcessor::CommitChangesFromSyncModel() {
  DCHECK(CalledOnValidThread());
  if (!running())
    return;
  if (syncer_changes_.empty())
    return;
  if (!local_service_) {
    syncable::ModelType type = syncer_changes_[0].sync_data().GetDataType();
    csync::SyncError error(FROM_HERE, "Local service destroyed.", type);
    error_handler()->OnSingleDatatypeUnrecoverableError(error.location(),
                                                        error.message());
    return;
  }
  csync::SyncError error = local_service_->ProcessSyncChanges(FROM_HERE,
                                                       syncer_changes_);
  syncer_changes_.clear();
  if (error.IsSet()) {
    error_handler()->OnSingleDatatypeUnrecoverableError(
        error.location(), error.message());
  }
}

csync::SyncError GenericChangeProcessor::GetSyncDataForType(
    syncable::ModelType type,
    csync::SyncDataList* current_sync_data) {
  DCHECK(CalledOnValidThread());
  std::string type_name = syncable::ModelTypeToString(type);
  csync::ReadTransaction trans(FROM_HERE, share_handle());
  csync::ReadNode root(&trans);
  if (root.InitByTagLookup(syncable::ModelTypeToRootTag(type)) !=
          csync::BaseNode::INIT_OK) {
    csync::SyncError error(FROM_HERE,
                    "Server did not create the top-level " + type_name +
                    " node. We might be running against an out-of-date server.",
                    type);
    return error;
  }

  // TODO(akalin): We'll have to do a tree traversal for bookmarks.
  DCHECK_NE(type, syncable::BOOKMARKS);

  int64 sync_child_id = root.GetFirstChildId();
  while (sync_child_id != csync::kInvalidId) {
    csync::ReadNode sync_child_node(&trans);
    if (sync_child_node.InitByIdLookup(sync_child_id) !=
            csync::BaseNode::INIT_OK) {
      csync::SyncError error(FROM_HERE,
                      "Failed to fetch child node for type " + type_name + ".",
                       type);
      return error;
    }
    current_sync_data->push_back(csync::SyncData::CreateRemoteData(
        sync_child_node.GetId(), sync_child_node.GetEntitySpecifics()));
    sync_child_id = sync_child_node.GetSuccessorId();
  }
  return csync::SyncError();
}

namespace {

// TODO(isherman): Investigating http://crbug.com/121592
// WARNING: this code is sensitive to compiler optimizations. Be careful
// modifying any code around an OnSingleDatatypeUnrecoverableError call, else
// the compiler attempts to merge it with other calls, losing useful information
// in breakpad uploads.
csync::SyncError LogLookupFailure(
    csync::BaseNode::InitByLookupResult lookup_result,
    const tracked_objects::Location& from_here,
    const std::string& error_prefix,
    syncable::ModelType type,
    DataTypeErrorHandler* error_handler) {
  switch (lookup_result) {
    case csync::BaseNode::INIT_FAILED_ENTRY_NOT_GOOD: {
      csync::SyncError error;
      error.Reset(from_here,
                  error_prefix +
                      "could not find entry matching the lookup criteria.",
                  type);
      error_handler->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                        error.message());
      LOG(ERROR) << "Delete: Bad entry.";
      return error;
    }
    case csync::BaseNode::INIT_FAILED_ENTRY_IS_DEL: {
      csync::SyncError error;
      error.Reset(from_here, error_prefix + "entry is already deleted.", type);
      error_handler->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                        error.message());
      LOG(ERROR) << "Delete: Deleted entry.";
      return error;
    }
    case csync::BaseNode::INIT_FAILED_DECRYPT_IF_NECESSARY: {
      csync::SyncError error;
      error.Reset(from_here, error_prefix + "unable to decrypt", type);
      error_handler->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                        error.message());
      LOG(ERROR) << "Delete: Undecryptable entry.";
      return error;
    }
    case csync::BaseNode::INIT_FAILED_PRECONDITION: {
      csync::SyncError error;
      error.Reset(from_here,
                  error_prefix + "a precondition was not met for calling init.",
                  type);
      error_handler->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                        error.message());
      LOG(ERROR) << "Delete: Failed precondition.";
      return error;
    }
    default: {
      csync::SyncError error;
      // Should have listed all the possible error cases above.
      error.Reset(from_here, error_prefix + "unknown error", type);
      error_handler->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                        error.message());
      LOG(ERROR) << "Delete: Unknown error.";
      return error;
    }
  }
}

csync::SyncError AttemptDelete(const csync::SyncChange& change,
                        syncable::ModelType type,
                        const std::string& type_str,
                        csync::WriteNode* node,
                        DataTypeErrorHandler* error_handler) {
  DCHECK_EQ(change.change_type(), csync::SyncChange::ACTION_DELETE);
  if (change.sync_data().IsLocal()) {
    const std::string& tag = change.sync_data().GetTag();
    if (tag.empty()) {
      csync::SyncError error(
          FROM_HERE,
          "Failed to delete " + type_str + " node. Local data, empty tag.",
          type);
      error_handler->OnSingleDatatypeUnrecoverableError(error.location(),
                                                        error.message());
      NOTREACHED();
      return error;
    }

    csync::BaseNode::InitByLookupResult result =
        node->InitByClientTagLookup(change.sync_data().GetDataType(), tag);
    if (result != csync::BaseNode::INIT_OK) {
      return LogLookupFailure(
          result, FROM_HERE,
          "Failed to delete " + type_str + " node. Local data, ",
          type, error_handler);
    }
  } else {
    csync::BaseNode::InitByLookupResult result =
        node->InitByIdLookup(change.sync_data().GetRemoteId());
    if (result != csync::BaseNode::INIT_OK) {
      return LogLookupFailure(
          result, FROM_HERE,
          "Failed to delete " + type_str + " node. Non-local data, ",
          type, error_handler);
    }
  }
  node->Remove();
  return csync::SyncError();
}

}  // namespace

// WARNING: this code is sensitive to compiler optimizations. Be careful
// modifying any code around an OnSingleDatatypeUnrecoverableError call, else
// the compiler attempts to merge it with other calls, losing useful information
// in breakpad uploads.
csync::SyncError GenericChangeProcessor::ProcessSyncChanges(
    const tracked_objects::Location& from_here,
    const csync::SyncChangeList& list_of_changes) {
  DCHECK(CalledOnValidThread());
  csync::WriteTransaction trans(from_here, share_handle());

  for (csync::SyncChangeList::const_iterator iter = list_of_changes.begin();
       iter != list_of_changes.end();
       ++iter) {
    const csync::SyncChange& change = *iter;
    DCHECK_NE(change.sync_data().GetDataType(), syncable::UNSPECIFIED);
    syncable::ModelType type = change.sync_data().GetDataType();
    std::string type_str = syncable::ModelTypeToString(type);
    csync::WriteNode sync_node(&trans);
    if (change.change_type() == csync::SyncChange::ACTION_DELETE) {
      csync::SyncError error = AttemptDelete(change, type, type_str, &sync_node,
                                      error_handler());
      if (error.IsSet()) {
        NOTREACHED();
        return error;
      }
    } else if (change.change_type() == csync::SyncChange::ACTION_ADD) {
      // TODO(sync): Handle other types of creation (custom parents, folders,
      // etc.).
      csync::ReadNode root_node(&trans);
      if (root_node.InitByTagLookup(
              syncable::ModelTypeToRootTag(change.sync_data().GetDataType())) !=
                  csync::BaseNode::INIT_OK) {
        csync::SyncError error(FROM_HERE,
                        "Failed to look up root node for type " + type_str,
                        type);
        error_handler()->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                            error.message());
        NOTREACHED();
        LOG(ERROR) << "Create: no root node.";
        return error;
      }
      csync::WriteNode::InitUniqueByCreationResult result =
          sync_node.InitUniqueByCreation(change.sync_data().GetDataType(),
                                          root_node,
                                          change.sync_data().GetTag());
      if (result != csync::WriteNode::INIT_SUCCESS) {
        std::string error_prefix = "Failed to create " + type_str + " node: ";
        switch (result) {
          case csync::WriteNode::INIT_FAILED_EMPTY_TAG: {
            csync::SyncError error;
            error.Reset(FROM_HERE, error_prefix + "empty tag", type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Create: Empty tag.";
            return error;
          }
          case csync::WriteNode::INIT_FAILED_ENTRY_ALREADY_EXISTS: {
            csync::SyncError error;
            error.Reset(FROM_HERE, error_prefix + "entry already exists", type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Create: Entry exists.";
            return error;
          }
          case csync::WriteNode::INIT_FAILED_COULD_NOT_CREATE_ENTRY: {
            csync::SyncError error;
            error.Reset(FROM_HERE, error_prefix + "failed to create entry",
                        type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Create: Could not create entry.";
            return error;
          }
          case csync::WriteNode::INIT_FAILED_SET_PREDECESSOR: {
            csync::SyncError error;
            error.Reset(FROM_HERE, error_prefix + "failed to set predecessor",
                        type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Create: Bad predecessor.";
            return error;
          }
          default: {
            csync::SyncError error;
            error.Reset(FROM_HERE, error_prefix + "unknown error", type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Create: Unknown error.";
            return error;
          }
        }
      }
      sync_node.SetTitle(UTF8ToWide(change.sync_data().GetTitle()));
      sync_node.SetEntitySpecifics(change.sync_data().GetSpecifics());
    } else if (change.change_type() == csync::SyncChange::ACTION_UPDATE) {
      // TODO(zea): consider having this logic for all possible changes?
      csync::BaseNode::InitByLookupResult result =
          sync_node.InitByClientTagLookup(change.sync_data().GetDataType(),
                                          change.sync_data().GetTag());
      if (result != csync::BaseNode::INIT_OK) {
        if (result == csync::BaseNode::INIT_FAILED_PRECONDITION) {
          csync::SyncError error;
          error.Reset(FROM_HERE,
                      "Failed to load entry w/empty tag for " + type_str + ".",
                      type);
          error_handler()->OnSingleDatatypeUnrecoverableError(
              FROM_HERE, error.message());
          LOG(ERROR) << "Update: Empty tag.";
          return error;
        } else if (result == csync::BaseNode::INIT_FAILED_ENTRY_NOT_GOOD) {
          csync::SyncError error;
          error.Reset(FROM_HERE,
                      "Failed to load bad entry for " + type_str + ".",
                      type);
          error_handler()->OnSingleDatatypeUnrecoverableError(
              FROM_HERE, error.message());
          LOG(ERROR) << "Update: bad entry.";
          return error;
        } else if (result == csync::BaseNode::INIT_FAILED_ENTRY_IS_DEL) {
          csync::SyncError error;
          error.Reset(FROM_HERE,
                      "Failed to load deleted entry for " + type_str + ".",
                      type);
          error_handler()->OnSingleDatatypeUnrecoverableError(
              FROM_HERE, error.message());
          LOG(ERROR) << "Update: deleted entry.";
          return error;
        } else {
          csync::Cryptographer* crypto = trans.GetCryptographer();
          syncable::ModelTypeSet encrypted_types(crypto->GetEncryptedTypes());
          const sync_pb::EntitySpecifics& specifics =
              sync_node.GetEntry()->Get(syncable::SPECIFICS);
          CHECK(specifics.has_encrypted());
          const bool can_decrypt = crypto->CanDecrypt(specifics.encrypted());
          const bool agreement = encrypted_types.Has(type);
          if (!agreement && !can_decrypt) {
            csync::SyncError error;
            error.Reset(FROM_HERE,
                        "Failed to load encrypted entry, missing key and "
                        "nigori mismatch for " + type_str + ".",
                        type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Update: encr case 1.";
            return error;
          } else if (agreement && can_decrypt) {
            csync::SyncError error;
            error.Reset(FROM_HERE,
                        "Failed to load encrypted entry, we have the key "
                        "and the nigori matches (?!) for " + type_str + ".",
                        type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Update: encr case 2.";
            return error;
          } else if (agreement) {
            csync::SyncError error;
            error.Reset(FROM_HERE,
                        "Failed to load encrypted entry, missing key and "
                        "the nigori matches for " + type_str + ".",
                        type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Update: encr case 3.";
            return error;
          } else {
            csync::SyncError error;
            error.Reset(FROM_HERE,
                        "Failed to load encrypted entry, we have the key"
                        "(?!) and nigori mismatch for " + type_str + ".",
                        type);
            error_handler()->OnSingleDatatypeUnrecoverableError(
                FROM_HERE, error.message());
            LOG(ERROR) << "Update: encr case 4.";
            return error;
          }
        }
      } else {
        sync_node.SetTitle(UTF8ToWide(change.sync_data().GetTitle()));
        sync_node.SetEntitySpecifics(change.sync_data().GetSpecifics());
        // TODO(sync): Support updating other parts of the sync node (title,
        // successor, parent, etc.).
      }
    } else {
      csync::SyncError error(
          FROM_HERE,
          "Received unset csync::SyncChange in the change processor.",
          type);
      error_handler()->OnSingleDatatypeUnrecoverableError(FROM_HERE,
                                                          error.message());
      NOTREACHED();
      LOG(ERROR) << "Unset sync change.";
      return error;
    }
  }
  return csync::SyncError();
}

bool GenericChangeProcessor::SyncModelHasUserCreatedNodes(
    syncable::ModelType type,
    bool* has_nodes) {
  DCHECK(CalledOnValidThread());
  DCHECK(has_nodes);
  DCHECK_NE(type, syncable::UNSPECIFIED);
  std::string type_name = syncable::ModelTypeToString(type);
  std::string err_str = "Server did not create the top-level " + type_name +
      " node. We might be running against an out-of-date server.";
  *has_nodes = false;
  csync::ReadTransaction trans(FROM_HERE, share_handle());
  csync::ReadNode type_root_node(&trans);
  if (type_root_node.InitByTagLookup(syncable::ModelTypeToRootTag(type)) !=
          csync::BaseNode::INIT_OK) {
    LOG(ERROR) << err_str;
    return false;
  }

  // The sync model has user created nodes if the type's root node has any
  // children.
  *has_nodes = type_root_node.HasChildren();
  return true;
}

bool GenericChangeProcessor::CryptoReadyIfNecessary(syncable::ModelType type) {
  DCHECK(CalledOnValidThread());
  DCHECK_NE(type, syncable::UNSPECIFIED);
  // We only access the cryptographer while holding a transaction.
  csync::ReadTransaction trans(FROM_HERE, share_handle());
  const syncable::ModelTypeSet encrypted_types = GetEncryptedTypes(&trans);
  return !encrypted_types.Has(type) ||
         trans.GetCryptographer()->is_ready();
}

void GenericChangeProcessor::StartImpl(Profile* profile) {
  DCHECK(CalledOnValidThread());
}

void GenericChangeProcessor::StopImpl() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

csync::UserShare* GenericChangeProcessor::share_handle() const {
  DCHECK(CalledOnValidThread());
  return share_handle_;
}

}  // namespace browser_sync
