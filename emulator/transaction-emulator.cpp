#include <string>
#include "transaction-emulator.h"
#include "crypto/common/refcnt.hpp"
#include "vm/vm.h"
#include "tdutils/td/utils/Time.h"

using td::Ref;
using namespace std::string_literals;

namespace emulator {
td::Result<> TransactionEmulator::prepare_emulation(block::Account& account, ton::UnixTime& utime, ton::LogicalTime& lt) {
  td::Ref<vm::Cell> old_mparams;
  td::RefInt256 masterchain_create_fee, basechain_create_fee;

  if (!utime) {
    utime = unixtime_;
  }
  if (!utime) {
    utime = (unsigned)std::time(nullptr);
  }

  auto fetch_res = block::FetchConfigParams::fetch_config_params(
      *config_, prev_blocks_info_, &old_mparams, &storage_prices_, &storage_phase_cfg_, &rand_seed_, &compute_phase_cfg_,
      &action_phase_cfg_, &serialize_config_, &masterchain_create_fee, &basechain_create_fee, account.workchain, utime);
  if (fetch_res.is_error()) {
    return fetch_res.move_as_error_prefix("cannot fetch config params ");
  }

  auto res = vm::init_vm(debug_enabled_);
  if (res.is_error()) {
    return res.move_as_error();
  }

  if (!lt) {
    lt = lt_;
  }
  if (!lt) {
    lt = (account.last_trans_lt_ / block::ConfigInfo::get_lt_align() + 1) *
         block::ConfigInfo::get_lt_align();  // next block after account_.last_trans_lt_
  }
  account.block_lt = lt - lt % block::ConfigInfo::get_lt_align();

  compute_phase_cfg_.libraries = std::make_unique<vm::Dictionary>(libraries_);
  compute_phase_cfg_.ignore_chksig = ignore_chksig_;
  compute_phase_cfg_.with_vm_log = true;
  compute_phase_cfg_.vm_log_verbosity = vm_log_verbosity_;
  return td::Unit{};
}

td::Result<std::unique_ptr<TransactionEmulator::EmulationResult>> TransactionEmulator::finish_emulation(
    block::Account&& account, block::SerializeConfig serialize_config, double elapsed) {
  if (!trans_->compute_phase->accepted && trans_->in_msg_extern) {
    auto vm_log = trans_->compute_phase->vm_log;
    auto vm_exit_code = trans_->compute_phase->exit_code;
    cleanup_shared_state();
    return std::make_unique<TransactionEmulator::EmulationExternalNotAccepted>(std::move(vm_log), vm_exit_code,
                                                                               elapsed);
  }

  if (!trans_->serialize(serialize_config)) {
    auto error = td::Status::Error(
        -669, "cannot serialize new transaction for smart contract "s + trans_->account.addr.to_hex());
    cleanup_shared_state();
    return error;
  }

  auto trans_root = trans_->commit(account);
  if (trans_root.is_null()) {
    cleanup_shared_state();
    return td::Status::Error(PSLICE() << "cannot commit new transaction for smart contract");
  }

  auto result = std::make_unique<TransactionEmulator::EmulationSuccess>(
      std::move(trans_root), std::move(account), std::move(trans_->compute_phase->vm_log),
      std::move(trans_->compute_phase->actions), elapsed);

  // Since emulator can be used many times, cleanup all shared state for clean emulation of the next transaction
  cleanup_shared_state();
  return result;
}

void TransactionEmulator::cleanup_shared_state() {
  storage_prices_ = {};
  storage_phase_cfg_ = {&storage_prices_};
  compute_phase_cfg_ = {};
  action_phase_cfg_ = {};
  trans_ = nullptr;
  account_ = {};
  external_ = false;
  serialize_config_ = {};
}

td::Result<std::unique_ptr<TransactionEmulator::EmulationResult>> TransactionEmulator::emulate_transaction(
    block::Account&& account, td::Ref<vm::Cell> msg_root, ton::UnixTime utime, ton::LogicalTime lt, int trans_type) {

    auto prepare_res = prepare_emulation(account, utime, lt);
    if (prepare_res.is_error()) {
      return prepare_res.move_as_error_prefix("cannot prepare emulation");
    }

    double start_time = td::Time::now();
    auto res = run_transaction(msg_root, &account, utime, lt, trans_type);
    double elapsed = td::Time::now() - start_time;

    if(res.is_error()) {
      return res.move_as_error_prefix("cannot run message on account ");
    }

    return finish_emulation(std::move(account), serialize_config_, elapsed);
}

td::Result<bool> TransactionEmulator::prepare_emulate_transaction_debug(
    block::Account&& account, td::Ref<vm::Cell> msg_root, ton::UnixTime utime, ton::LogicalTime lt, int trans_type) {

    account_ = std::move(account);

    auto prepare_res = prepare_emulation(account_, utime, lt);
    if (prepare_res.is_error()) {
      return prepare_res.move_as_error_prefix("cannot prepare emulation");
    }

    auto res = run_transaction_debug(msg_root, &account_, utime, lt, trans_type);
    if (res.is_error()) {
      return res.move_as_error_prefix("cannot run message on account ");
    }

    return res;
}

td::Result<TransactionEmulator::EmulationSuccess> TransactionEmulator::emulate_transaction(block::Account&& account, td::Ref<vm::Cell> original_trans) {

    block::gen::Transaction::Record record_trans;
    if (!tlb::unpack_cell(original_trans, record_trans)) {
      return td::Status::Error("Failed to unpack Transaction");
    }

    ton::LogicalTime lt = record_trans.lt;
    ton::UnixTime utime = record_trans.now;
    account.now_ = utime;
    account.block_lt = record_trans.lt - record_trans.lt % block::ConfigInfo::get_lt_align();
    td::Ref<vm::Cell> msg_root = record_trans.r1.in_msg->prefetch_ref();
    int tag = block::gen::t_TransactionDescr.get_tag(vm::load_cell_slice(record_trans.description));

    int trans_type = block::transaction::Transaction::tr_none;
    switch (tag) {
      case block::gen::TransactionDescr::trans_ord: {
        trans_type = block::transaction::Transaction::tr_ord;
        break;
      }
      case block::gen::TransactionDescr::trans_storage: {
        trans_type = block::transaction::Transaction::tr_storage;
        break;
      }
      case block::gen::TransactionDescr::trans_tick_tock: {
        block::gen::TransactionDescr::Record_trans_tick_tock tick_tock;
        if (!tlb::unpack_cell(record_trans.description, tick_tock)) {
          return td::Status::Error("Failed to unpack tick tock transaction description");
        }
        trans_type = tick_tock.is_tock ? block::transaction::Transaction::tr_tock : block::transaction::Transaction::tr_tick;
        break;
      }
      case block::gen::TransactionDescr::trans_split_prepare: {
        trans_type = block::transaction::Transaction::tr_split_prepare;
        break;
      }
      case block::gen::TransactionDescr::trans_split_install: {
        trans_type = block::transaction::Transaction::tr_split_install;
        break;
      }
      case block::gen::TransactionDescr::trans_merge_prepare: {
        trans_type = block::transaction::Transaction::tr_merge_prepare;
        break;
      }
      case block::gen::TransactionDescr::trans_merge_install: {
        trans_type = block::transaction::Transaction::tr_merge_install;
        break;
      }
    }

    TRY_RESULT(emulation, emulate_transaction(std::move(account), msg_root, utime, lt, trans_type));
    
    if (auto emulation_result_ptr = dynamic_cast<EmulationSuccess*>(emulation.get())) {
      auto& emulation_result = *emulation_result_ptr;     
    
      if (td::Bits256(emulation_result.transaction->get_hash().bits()) != td::Bits256(original_trans->get_hash().bits())) {
        return td::Status::Error("transaction hash mismatch");
      }

      if (!check_state_update(emulation_result.account, record_trans)) {
        return td::Status::Error("account hash mismatch");
      }

      return std::move(emulation_result);

    } else if (auto emulation_not_accepted_ptr = dynamic_cast<EmulationExternalNotAccepted*>(emulation.get())) {
      return td::Status::Error( PSTRING()
        << "VM Log: " << emulation_not_accepted_ptr->vm_log 
        << ", VM Exit Code: " << emulation_not_accepted_ptr->vm_exit_code 
        << ", Elapsed Time: " << emulation_not_accepted_ptr->elapsed_time);
    } else {
       return td::Status::Error("emulation failed");
    }
}

td::Result<TransactionEmulator::EmulationChain> TransactionEmulator::emulate_transactions_chain(block::Account&& account, std::vector<td::Ref<vm::Cell>>&& original_transactions) {

  std::vector<td::Ref<vm::Cell>> emulated_transactions;
  for (const auto& original_trans : original_transactions) {
    if (original_trans.is_null()) {
      continue;
    }

    TRY_RESULT(emulation_result, emulate_transaction(std::move(account), original_trans));
    emulated_transactions.push_back(std::move(emulation_result.transaction));
    account = std::move(emulation_result.account);
  }

  return TransactionEmulator::EmulationChain{ std::move(emulated_transactions), std::move(account) };
}

bool TransactionEmulator::check_state_update(const block::Account& account, const block::gen::Transaction::Record& trans) {
  block::gen::HASH_UPDATE::Record hash_update;
  return tlb::type_unpack_cell(trans.state_update, block::gen::t_HASH_UPDATE_Account, hash_update) &&
    hash_update.new_hash == account.total_state->get_hash().bits();
}

td::Result<> TransactionEmulator::prepare_transaction(
    td::Ref<vm::Cell> msg_root, block::Account* acc, ton::UnixTime utime, ton::LogicalTime lt, int trans_type) {
  external_ = false;
  bool ihr_delivered{false}, need_credit_phase{false};

  if (msg_root.not_null()) {
    auto cs = vm::load_cell_slice(msg_root);
    external_ = block::gen::t_CommonMsgInfo.get_tag(cs);
  }

  if (trans_type == block::transaction::Transaction::tr_ord) {
    need_credit_phase = !external_;
  } else if (trans_type == block::transaction::Transaction::tr_merge_install) {
    need_credit_phase = true;
  }

  trans_ = std::make_unique<block::transaction::Transaction>(*acc, trans_type, lt, utime, msg_root);

  if (msg_root.not_null() && !trans_->unpack_input_msg(ihr_delivered, &action_phase_cfg_)) {
    if (external_) {
      // inbound external message was not accepted
      return td::Status::Error(-701,"inbound external message rejected by account "s + acc->addr.to_hex() +
                                                           " before smart-contract execution");
    }
    return td::Status::Error(-669,"cannot unpack input message for a new transaction");
  }

  if (trans_->bounce_enabled) {
    if (!trans_->prepare_storage_phase(storage_phase_cfg_, true)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
    if (need_credit_phase && !trans_->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
  } else {
    if (need_credit_phase && !trans_->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
    if (!trans_->prepare_storage_phase(storage_phase_cfg_, true, need_credit_phase)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
  }
  return td::Unit{};
}

td::Result<> TransactionEmulator::run_transaction(td::Ref<vm::Cell> msg_root, block::Account* acc,
                                                     ton::UnixTime utime, ton::LogicalTime lt, int trans_type) {
  auto prepare_res = prepare_transaction(msg_root, acc, utime, lt, trans_type);
  if (prepare_res.is_error()) {
    return prepare_res.move_as_error_prefix("cannot prepare transaction");
  }

  if (!trans_->execute_compute_phase(compute_phase_cfg_)) {
    return td::Status::Error(-669,"cannot create compute phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  if (!trans_->compute_phase->accepted) {
    if (!external_ && trans_->compute_phase->skip_reason == block::ComputePhase::sk_none) {
      return td::Status::Error(-669,"new ordinary transaction for smart contract "s + acc->addr.to_hex() +
                " has not been accepted by the smart contract (?)");
    }
  }

  if (trans_->compute_phase->success && !trans_->prepare_action_phase(action_phase_cfg_)) {
    return td::Status::Error(-669,"cannot create action phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  if (trans_->bounce_enabled
  && (!trans_->compute_phase->success || trans_->action_phase->state_exceeds_limits || trans_->action_phase->bounce)
  && !trans_->prepare_bounce_phase(action_phase_cfg_)) {
    return td::Status::Error(-669,"cannot create bounce phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  return td::Unit{};
}

td::Result<bool> TransactionEmulator::run_transaction_debug(td::Ref<vm::Cell> msg_root, block::Account* acc,
                                                               ton::UnixTime utime, ton::LogicalTime lt, int trans_type) {
  auto prepare_res = prepare_transaction(msg_root, acc, utime, lt, trans_type);
  if (prepare_res.is_error()) {
    return prepare_res.move_as_error_prefix("cannot prepare transaction");
  }

  if (!trans_->prepare_debug_compute_phase(compute_phase_cfg_)) {
    return td::Status::Error(-669,"cannot create compute phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }

  return true;
}

td::Result<bool> TransactionEmulator::transaction_step_debug() const {
  if (!trans_->compute_phase_step_debug(compute_phase_cfg_)) {
    return false;
  }

  if (!trans_->compute_phase->accepted) {
    if (!external_ && trans_->compute_phase->skip_reason == block::ComputePhase::sk_none) {
      return td::Status::Error(-669,"new ordinary transaction for smart contract "s + account_.addr.to_hex() +
                " has not been accepted by the smart contract (?)");
    }
  }

  if (trans_->compute_phase->success && !trans_->prepare_action_phase(action_phase_cfg_)) {
    return td::Status::Error(-669,"cannot create action phase of a new transaction for smart contract "s + account_.addr.to_hex());
  }

  if (trans_->bounce_enabled
  && (!trans_->compute_phase->success || trans_->action_phase->state_exceeds_limits || trans_->action_phase->bounce)
  && !trans_->prepare_bounce_phase(action_phase_cfg_)) {
    return td::Status::Error(-669,"cannot create bounce phase of a new transaction for smart contract "s + account_.addr.to_hex());
  }

  return true;
}

td::Result<std::unique_ptr<TransactionEmulator::EmulationResult>> TransactionEmulator::get_emulation_result() {
  return finish_emulation(std::move(account_), serialize_config_, 0);
}

td::Result<bool> TransactionEmulator::debug_step() const {
  auto res = transaction_step_debug();
  if (res.is_error()) {
    return res.move_as_error_prefix("cannot run message on account ");
  }
  return res;
}

void TransactionEmulator::set_unixtime(ton::UnixTime unixtime) {
  unixtime_ = unixtime;
}

void TransactionEmulator::set_lt(ton::LogicalTime lt) {
  lt_ = lt;
}

void TransactionEmulator::set_rand_seed(td::BitArray<256>& rand_seed) {
  rand_seed_ = rand_seed;
}

void TransactionEmulator::set_ignore_chksig(bool ignore_chksig) {
  ignore_chksig_ = ignore_chksig;
}

void TransactionEmulator::set_config(std::shared_ptr<block::Config> config) {
  config_ = std::move(config);
}

void TransactionEmulator::set_libs(vm::Dictionary &&libs) {
  libraries_ = std::forward<vm::Dictionary>(libs);
}

void TransactionEmulator::set_debug_enabled(bool debug_enabled) {
  debug_enabled_ = debug_enabled;
}

void TransactionEmulator::set_prev_blocks_info(td::Ref<vm::Tuple> prev_blocks_info) {
  prev_blocks_info_ = std::move(prev_blocks_info);
}

} // namespace emulator
