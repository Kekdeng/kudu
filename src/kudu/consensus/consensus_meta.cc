// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#include "kudu/consensus/consensus_meta.h"

#include <cstddef>
#include <limits>
#include <ostream>
#include <utility>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/consensus/log_util.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/opid_util.h"
#include "kudu/consensus/quorum_util.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/fault_injection.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/logging.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"

DEFINE_double(fault_crash_before_cmeta_flush, 0.0,
              "Fraction of the time when the server will crash just before flushing "
              "consensus metadata. (For testing only!)");
TAG_FLAG(fault_crash_before_cmeta_flush, unsafe);

DEFINE_bool(cmeta_force_fsync, false,
            "Whether fsync() should be called when consensus metadata files are updated");
TAG_FLAG(cmeta_force_fsync, advanced);

DECLARE_bool(cmeta_fsync_override_on_xfs);

using std::string;
using strings::Substitute;

namespace kudu {
namespace consensus {

namespace {

constexpr size_t kPackedRoleBits = 3;
constexpr size_t kPackedTermBits = 8 * sizeof(uint64_t) - kPackedRoleBits;
constexpr uint64_t kUnknownRolePacked = (1ULL << kPackedRoleBits) - 1;
constexpr uint64_t kRoleMask = kUnknownRolePacked << kPackedTermBits;
constexpr uint64_t kTermMask = ~kRoleMask;

static_assert((kRoleMask | kTermMask) == std::numeric_limits<uint64_t>::max(),
              "term and role should fit into uint64_t");
static_assert((kTermMask & kRoleMask) == 0,
              "term and role masks must not intersect");
//
// Packing role and term into uint64_t:
//
//  * * * * * ... * * * *
//  ^     ^             ^
// 63    60             0
//
// Bits 0..60 inclusive contain term.  Bits 61..63 contain role.

uint64_t PackRoleAndTerm(RaftPeerPB::Role role, int64_t term) {
  // Ensure the term is not wider than kPackedTermBits: maximum possible is
  // 2305843009213693951. Here it might be something like
  //
  //   CHECK_EQ(0, kRoleMask & term) << "term is too big: " << term;
  //
  // However, sometimes the data read from disk is corrupted, and we don't want
  // to crash just because of that. The corruption is detected and handled
  // gracefully at a higher level (e.g., the server marks corresponding replica
  // as failed). With current approach, the maximum acceptable term is
  // 2305843009213693950; 2305843009213693951 (kTermMask) is a special value.
  if (PREDICT_FALSE((term & kRoleMask) != 0)) {
    // A special value to signal that a 'non-packable' term was supplied.
    term = kTermMask;
  }

  // The allocated bit space for role is just 3 bits, but it's necessary
  // to handle the constant 999 defined in the proto file for UNKNOWN_ROLE.
  // Changing the constant behind UNKNOWN_ROLE is not an option
  // due to compatibility issues.
  uint64_t r = (role == RaftPeerPB::UNKNOWN_ROLE) ? kUnknownRolePacked
                                                  : static_cast<uint64_t>(role);
  return (r << kPackedTermBits) | term;
}

RaftPeerPB::Role UnpackRole(uint64_t role_and_term_packed) {
  const auto role = role_and_term_packed >> kPackedTermBits;
  if (PREDICT_FALSE(role == kUnknownRolePacked)) {
    return RaftPeerPB::UNKNOWN_ROLE;
  }
  return static_cast<RaftPeerPB::Role>(role);
}

int64_t UnpackTerm(uint64_t role_and_term_packed) {
  const auto t = role_and_term_packed & kTermMask;
  if (PREDICT_FALSE(t == kTermMask)) {
    LOG(FATAL) << "packed term is invalid: " << t;
  }
  return static_cast<int64_t>(t);
}

} // anonymous namespace

int64_t ConsensusMetadata::current_term() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  DCHECK(pb_.has_current_term());
  return pb_.current_term();
}

void ConsensusMetadata::set_current_term(int64_t term) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  DCHECK_GE(term, kMinimumTerm);
  pb_.set_current_term(term);
  UpdateRoleAndTermCache();
}

bool ConsensusMetadata::has_voted_for() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return pb_.has_voted_for();
}

const string& ConsensusMetadata::voted_for() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  DCHECK(pb_.has_voted_for());
  return pb_.voted_for();
}

void ConsensusMetadata::clear_voted_for() {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  pb_.clear_voted_for();
}

void ConsensusMetadata::set_voted_for(const string& uuid) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  DCHECK(!uuid.empty());
  pb_.set_voted_for(uuid);
}

bool ConsensusMetadata::IsVoterInConfig(const string& uuid,
                                        RaftConfigState type) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return IsRaftConfigVoter(uuid, GetConfig(type));
}

bool ConsensusMetadata::IsMemberInConfig(const string& uuid,
                                         RaftConfigState type) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return IsRaftConfigMember(uuid, GetConfig(type));
}

int ConsensusMetadata::CountVotersInConfig(RaftConfigState type) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return CountVoters(GetConfig(type));
}

int64_t ConsensusMetadata::GetConfigOpIdIndex(RaftConfigState type) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return GetConfig(type).opid_index();
}

const RaftConfigPB& ConsensusMetadata::CommittedConfig() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return GetConfig(COMMITTED_CONFIG);
}

const RaftConfigPB& ConsensusMetadata::GetConfig(RaftConfigState type) const {
  switch (type) {
    case ACTIVE_CONFIG:
      if (has_pending_config_) {
        return pending_config_;
      }
      DCHECK(pb_.has_committed_config());
      return pb_.committed_config();
    case COMMITTED_CONFIG:
      DCHECK(pb_.has_committed_config());
      return pb_.committed_config();
    case PENDING_CONFIG:
      CHECK(has_pending_config_) << LogPrefix() << "There is no pending config";
      return pending_config_;
    default: LOG(FATAL) << "Unknown RaftConfigState type: " << type;
  }
}

void ConsensusMetadata::set_committed_config(const RaftConfigPB& config) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  *pb_.mutable_committed_config() = config;
  if (!has_pending_config_) {
    UpdateActiveRole();
  }
}

bool ConsensusMetadata::has_pending_config() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return has_pending_config_;
}

const RaftConfigPB& ConsensusMetadata::PendingConfig() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return GetConfig(PENDING_CONFIG);
}

void ConsensusMetadata::clear_pending_config() {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  has_pending_config_ = false;
  pending_config_.Clear();
  UpdateActiveRole();
}

void ConsensusMetadata::set_pending_config(const RaftConfigPB& config) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  has_pending_config_ = true;
  pending_config_ = config;
  UpdateActiveRole();
}

const RaftConfigPB& ConsensusMetadata::ActiveConfig() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return GetConfig(ACTIVE_CONFIG);
}

const string& ConsensusMetadata::leader_uuid() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return leader_uuid_;
}

void ConsensusMetadata::set_leader_uuid(string uuid) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  leader_uuid_ = std::move(uuid);
  UpdateActiveRole();
}

RaftPeerPB::Role ConsensusMetadata::active_role() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  return active_role_;
}

ConsensusStatePB ConsensusMetadata::ToConsensusStatePB() const {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  ConsensusStatePB cstate;
  cstate.set_current_term(pb_.current_term());
  if (!leader_uuid_.empty()) {
    cstate.set_leader_uuid(leader_uuid_);
  }
  *cstate.mutable_committed_config() = CommittedConfig();
  if (has_pending_config_) {
    *cstate.mutable_pending_config() = pending_config_;
  }
  return cstate;
}

void ConsensusMetadata::MergeCommittedConsensusStatePB(const ConsensusStatePB& cstate) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  if (cstate.current_term() > current_term()) {
    set_current_term(cstate.current_term());
    clear_voted_for();
  }

  set_leader_uuid("");
  set_committed_config(cstate.committed_config());
  clear_pending_config();
}

Status ConsensusMetadata::Flush(FlushMode flush_mode) {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  MAYBE_FAULT(FLAGS_fault_crash_before_cmeta_flush);
  SCOPED_LOG_SLOW_EXECUTION_PREFIX(WARNING, 500, LogPrefix(), "flushing consensus metadata");

  flush_count_for_tests_++;
  // Sanity test to ensure we never write out a bad configuration.
  RETURN_NOT_OK_PREPEND(VerifyRaftConfig(pb_.committed_config()),
                        "Invalid config in ConsensusMetadata, cannot flush to disk");

  // Create directories if needed.
  string dir = fs_manager_->GetConsensusMetadataDir();
  bool created_dir = false;
  RETURN_NOT_OK_PREPEND(env_util::CreateDirIfMissing(
      fs_manager_->env(), dir, &created_dir),
                        "Unable to create consensus metadata root dir");
  // fsync() parent dir if we had to create the dir.
  if (PREDICT_FALSE(created_dir)) {
    string parent_dir = DirName(dir);
    RETURN_NOT_OK_PREPEND(Env::Default()->SyncDir(parent_dir),
                          "Unable to fsync consensus parent dir " + parent_dir);
  }

  const bool cmeta_force_fsync =
      FLAGS_cmeta_force_fsync || (FLAGS_cmeta_fsync_override_on_xfs && fs_manager_->meta_on_xfs());
  string meta_file_path = fs_manager_->GetConsensusMetadataPath(tablet_id_);
  RETURN_NOT_OK_PREPEND(pb_util::WritePBContainerToPath(
      fs_manager_->env(), meta_file_path, pb_,
      flush_mode == OVERWRITE ? pb_util::OVERWRITE : pb_util::NO_OVERWRITE,
      // We use FLAGS_log_force_fsync_all here because the consensus metadata is
      // essentially an extension of the primary durability mechanism of the
      // consensus subsystem: the WAL. Using the same flag ensures that the WAL
      // and the consensus metadata get the same durability guarantees.
      // We add FLAGS_cmeta_force_fsync to support an override in certain
      // cases. Some filesystems such as ext4 are more forgiving to omitting an
      // fsync() due to periodic commit with default settings, whereas other
      // filesystems such as XFS will not commit as often and need the fsync to
      // avoid significant data loss when a crash happens.
      FLAGS_log_force_fsync_all || cmeta_force_fsync ? pb_util::SYNC : pb_util::NO_SYNC),
          Substitute("Unable to write consensus meta file for tablet $0 to path $1",
                     tablet_id_, meta_file_path));
  return UpdateOnDiskSize();
}

ConsensusMetadata::RoleAndTerm ConsensusMetadata::GetRoleAndTerm() const {
  // Read the cached role and term atomically to unpack them consistently.
  const uint64_t val = role_and_term_cache_;
  return std::make_pair(UnpackRole(val), UnpackTerm(val));
}

ConsensusMetadata::ConsensusMetadata(FsManager* fs_manager,
                                     std::string tablet_id,
                                     std::string peer_uuid)
    : fs_manager_(CHECK_NOTNULL(fs_manager)),
      tablet_id_(std::move(tablet_id)),
      peer_uuid_(std::move(peer_uuid)),
      has_pending_config_(false),
      flush_count_for_tests_(0),
      active_role_(RaftPeerPB::UNKNOWN_ROLE),
      on_disk_size_(0) {
  UpdateRoleAndTermCache();
}

Status ConsensusMetadata::Create(FsManager* fs_manager,
                                 const string& tablet_id,
                                 const std::string& peer_uuid,
                                 const RaftConfigPB& config,
                                 int64_t current_term,
                                 ConsensusMetadataCreateMode create_mode,
                                 scoped_refptr<ConsensusMetadata>* cmeta_out) {

  scoped_refptr<ConsensusMetadata> cmeta(new ConsensusMetadata(fs_manager, tablet_id, peer_uuid));
  cmeta->set_committed_config(config);
  cmeta->set_current_term(current_term);

  if (create_mode == ConsensusMetadataCreateMode::FLUSH_ON_CREATE) {
    RETURN_NOT_OK(cmeta->Flush(NO_OVERWRITE)); // Create() should not clobber.
  } else {
    // Sanity check: ensure that there is no cmeta file currently on disk.
    const string& path = fs_manager->GetConsensusMetadataPath(tablet_id);
    if (fs_manager->env()->FileExists(path)) {
      return Status::AlreadyPresent(Substitute("File $0 already exists", path));
    }
  }
  if (cmeta_out) *cmeta_out = std::move(cmeta);
  return Status::OK();
}

Status ConsensusMetadata::Load(FsManager* fs_manager,
                               const std::string& tablet_id,
                               const std::string& peer_uuid,
                               scoped_refptr<ConsensusMetadata>* cmeta_out) {
  scoped_refptr<ConsensusMetadata> cmeta(new ConsensusMetadata(fs_manager, tablet_id, peer_uuid));
  RETURN_NOT_OK(pb_util::ReadPBContainerFromPath(fs_manager->env(),
                                                 fs_manager->GetConsensusMetadataPath(tablet_id),
                                                 &cmeta->pb_));
  cmeta->UpdateActiveRole(); // Needs to happen here as we sidestep the accessor APIs.

  RETURN_NOT_OK(cmeta->UpdateOnDiskSize());
  if (cmeta_out) *cmeta_out = std::move(cmeta);
  return Status::OK();
}

Status ConsensusMetadata::DeleteOnDiskData(FsManager* fs_manager, const string& tablet_id) {
  string cmeta_path = fs_manager->GetConsensusMetadataPath(tablet_id);
  RETURN_NOT_OK_PREPEND(fs_manager->env()->DeleteFile(cmeta_path),
                        Substitute("Unable to delete consensus metadata file for tablet $0",
                                   tablet_id));
  return Status::OK();
}

std::string ConsensusMetadata::LogPrefix() const {
  // No need to lock to read const members.
  return Substitute("T $0 P $1: ", tablet_id_, peer_uuid_);
}

void ConsensusMetadata::UpdateActiveRole() {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  active_role_ = GetConsensusRole(peer_uuid_, leader_uuid_, ActiveConfig());
  UpdateRoleAndTermCache();
  VLOG_WITH_PREFIX(1) << "Updating active role to " << RaftPeerPB::Role_Name(active_role_)
                      << ". Consensus state: "
                      << pb_util::SecureShortDebugString(ToConsensusStatePB());
}

void ConsensusMetadata::UpdateRoleAndTermCache() {
  DFAKE_SCOPED_RECURSIVE_LOCK(fake_lock_);
  role_and_term_cache_ = PackRoleAndTerm(
      active_role_, pb_.has_current_term() ? current_term() : -1);
}

Status ConsensusMetadata::UpdateOnDiskSize() {
  string path = fs_manager_->GetConsensusMetadataPath(tablet_id_);
  uint64_t on_disk_size;
  RETURN_NOT_OK(fs_manager_->env()->GetFileSize(path, &on_disk_size));
  on_disk_size_ = on_disk_size;
  return Status::OK();
}

} // namespace consensus
} // namespace kudu
