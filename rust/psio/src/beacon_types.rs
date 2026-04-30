//! Ethereum Phase 0 BeaconState types (Rust port of
//! `libraries/psio1/cpp/benchmarks/beacon_types.hpp`).
//!
//! Wire layout matches the Ethereum consensus spec and is byte-identical
//! with the C++ port. Constants are mainnet preset. Cryptographic
//! computations (hash_tree_root, signature verification) are out of
//! scope — this is strictly for wire-layer serialization and benchmarks.
//!
//! Stack-safety note: sizeof(BeaconState) is ~2.56 MiB because randao_mixes
//! alone is 2 MiB inline. By-value construction would overflow the default
//! 8 MiB thread stack the moment a couple of these are live. Large inline
//! fixed arrays are therefore wrapped in `Box<[T; N]>`, mirroring C++'s
//! `std::make_unique<eth::BeaconState>()` pattern for the outer state.

use crate::ssz::{Bitvector, BoundedList};
use crate::{pssz_struct, pssz_struct_dwnc, ssz_struct, ssz_struct_dwnc};

// ── Constants (mainnet preset) ──────────────────────────────────────────────

pub const SLOTS_PER_HISTORICAL_ROOT:    usize = 8192;
pub const HISTORICAL_ROOTS_LIMIT:       usize = 1 << 24;  // 16 777 216
pub const SLOTS_PER_ETH1_VOTING_PERIOD: usize = 2048;
// VALIDATOR_REGISTRY_LIMIT = 2^40 — too large to name as const in Rust
// const-generics path (limited to usize range). Phase 0 genesis has only
// 21 063 validators so we use a smaller bound for the registry list that
// is still wire-compatible (BoundedList bound is decode-side only).
pub const VALIDATOR_REGISTRY_LIMIT:     usize = 1 << 30;
pub const EPOCHS_PER_HISTORICAL_VECTOR: usize = 65536;
pub const EPOCHS_PER_SLASHINGS_VECTOR:  usize = 8192;
pub const MAX_ATTESTATIONS:             usize = 128;
pub const SLOTS_PER_EPOCH:              usize = 32;
pub const MAX_VALIDATORS_PER_COMMITTEE: usize = 2048;
pub const JUSTIFICATION_BITS_LENGTH:    usize = 4;
pub const MAX_PENDING_ATTESTATIONS:     usize = SLOTS_PER_EPOCH * MAX_ATTESTATIONS;

// ── Primitive type aliases ──────────────────────────────────────────────────

pub type Slot           = u64;
pub type Epoch          = u64;
pub type CommitteeIndex = u64;
pub type ValidatorIndex = u64;
pub type Gwei           = u64;
pub type Root           = [u8; 32];
pub type Hash32         = [u8; 32];
pub type Bytes32        = [u8; 32];
pub type Version        = [u8; 4];
pub type DomainType     = [u8; 4];
pub type BLSPubkey      = [u8; 48];
pub type BLSSignature   = [u8; 96];

// ── Containers ──────────────────────────────────────────────────────────────

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fork {
    pub previous_version: Version,
    pub current_version:  Version,
    pub epoch:            Epoch,
}
ssz_struct_dwnc!(Fork {
    previous_version: Version,
    current_version:  Version,
    epoch:            Epoch,
});
pssz_struct_dwnc!(Fork {
    previous_version: Version,
    current_version:  Version,
    epoch:            Epoch,
});

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ForkData {
    pub current_version:         Version,
    pub genesis_validators_root: Root,
}
ssz_struct_dwnc!(ForkData {
    current_version:         Version,
    genesis_validators_root: Root,
});
pssz_struct_dwnc!(ForkData {
    current_version:         Version,
    genesis_validators_root: Root,
});

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Checkpoint {
    pub epoch: Epoch,
    pub root:  Root,
}
ssz_struct_dwnc!(Checkpoint { epoch: Epoch, root: Root });
pssz_struct_dwnc!(Checkpoint { epoch: Epoch, root: Root });

// `#[repr(C, packed)]` — sizeof(Validator) == 121 exactly, matching the
// SSZ wire size. Default Rust layout reorders + pads to 128 bytes, which
// defeats the single-memcpy serialization path for Vec<Validator> and
// the whole-struct memcpy in ssz_struct_dwnc!. ARM64 and x86_64 handle
// the resulting unaligned u64 accesses natively with no per-field cost.
// Counterpart of `__attribute__((packed))` on the C++ Validator.
#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Validator {
    pub pubkey:                       BLSPubkey,
    pub withdrawal_credentials:       Bytes32,
    pub effective_balance:            Gwei,
    pub slashed:                      bool,
    pub activation_eligibility_epoch: Epoch,
    pub activation_epoch:             Epoch,
    pub exit_epoch:                   Epoch,
    pub withdrawable_epoch:           Epoch,
}
ssz_struct_dwnc!(Validator {
    pubkey:                       BLSPubkey,
    withdrawal_credentials:       Bytes32,
    effective_balance:            Gwei,
    slashed:                      bool,
    activation_eligibility_epoch: Epoch,
    activation_epoch:             Epoch,
    exit_epoch:                   Epoch,
    withdrawable_epoch:           Epoch,
});
pssz_struct_dwnc!(Validator {
    pubkey:                       BLSPubkey,
    withdrawal_credentials:       Bytes32,
    effective_balance:            Gwei,
    slashed:                      bool,
    activation_eligibility_epoch: Epoch,
    activation_epoch:             Epoch,
    exit_epoch:                   Epoch,
    withdrawable_epoch:           Epoch,
});

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AttestationData {
    pub slot:              Slot,
    pub index:             CommitteeIndex,
    pub beacon_block_root: Root,
    pub source:            Checkpoint,
    pub target:            Checkpoint,
}
ssz_struct_dwnc!(AttestationData {
    slot:              Slot,
    index:             CommitteeIndex,
    beacon_block_root: Root,
    source:            Checkpoint,
    target:            Checkpoint,
});
pssz_struct_dwnc!(AttestationData {
    slot:              Slot,
    index:             CommitteeIndex,
    beacon_block_root: Root,
    source:            Checkpoint,
    target:            Checkpoint,
});

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PendingAttestation {
    pub aggregation_bits: crate::ssz::Bitlist<MAX_VALIDATORS_PER_COMMITTEE>,
    pub data:             AttestationData,
    pub inclusion_delay:  Slot,
    pub proposer_index:   ValidatorIndex,
}
ssz_struct!(PendingAttestation {
    aggregation_bits: crate::ssz::Bitlist<MAX_VALIDATORS_PER_COMMITTEE>,
    data:             AttestationData,
    inclusion_delay:  Slot,
    proposer_index:   ValidatorIndex,
});
pssz_struct!(PendingAttestation {
    aggregation_bits: crate::ssz::Bitlist<MAX_VALIDATORS_PER_COMMITTEE>,
    data:             AttestationData,
    inclusion_delay:  Slot,
    proposer_index:   ValidatorIndex,
});

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Eth1Data {
    pub deposit_root:  Root,
    pub deposit_count: u64,
    pub block_hash:    Hash32,
}
ssz_struct_dwnc!(Eth1Data {
    deposit_root:  Root,
    deposit_count: u64,
    block_hash:    Hash32,
});
pssz_struct_dwnc!(Eth1Data {
    deposit_root:  Root,
    deposit_count: u64,
    block_hash:    Hash32,
});

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BeaconBlockHeader {
    pub slot:            Slot,
    pub proposer_index:  ValidatorIndex,
    pub parent_root:     Root,
    pub state_root:      Root,
    pub body_root:       Root,
}
ssz_struct_dwnc!(BeaconBlockHeader {
    slot:           Slot,
    proposer_index: ValidatorIndex,
    parent_root:    Root,
    state_root:     Root,
    body_root:      Root,
});
pssz_struct_dwnc!(BeaconBlockHeader {
    slot:           Slot,
    proposer_index: ValidatorIndex,
    parent_root:    Root,
    state_root:     Root,
    body_root:      Root,
});

// ── BeaconState (Phase 0) ───────────────────────────────────────────────────
//
// Inline fixed-array fields with multi-MiB footprint live behind `Box`:
//   block_roots    [[u8;32]; 8192]   = 256 KiB  → Box
//   state_roots    [[u8;32]; 8192]   = 256 KiB  → Box
//   randao_mixes   [[u8;32]; 65536]  = 2 MiB    → Box  (would blow stack)
//   slashings      [u64; 8192]       = 64 KiB   → Box
//
// The outer BeaconState is also typically allocated via `Box<BeaconState>`
// by callers to avoid materializing 2.56 MiB on the stack during decode.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BeaconState {
    // Versioning
    pub genesis_time:            u64,
    pub genesis_validators_root: Root,
    pub slot:                    Slot,
    pub fork:                    Fork,

    // History
    pub latest_block_header: BeaconBlockHeader,
    pub block_roots:         Box<[Root; SLOTS_PER_HISTORICAL_ROOT]>,
    pub state_roots:         Box<[Root; SLOTS_PER_HISTORICAL_ROOT]>,
    pub historical_roots:    BoundedList<Root, HISTORICAL_ROOTS_LIMIT>,

    // Eth1
    pub eth1_data:          Eth1Data,
    pub eth1_data_votes:    BoundedList<Eth1Data, SLOTS_PER_ETH1_VOTING_PERIOD>,
    pub eth1_deposit_index: u64,

    // Registry
    pub validators: BoundedList<Validator, VALIDATOR_REGISTRY_LIMIT>,
    pub balances:   BoundedList<Gwei, VALIDATOR_REGISTRY_LIMIT>,

    // Randomness
    pub randao_mixes: Box<[Bytes32; EPOCHS_PER_HISTORICAL_VECTOR]>,

    // Slashings
    pub slashings: Box<[Gwei; EPOCHS_PER_SLASHINGS_VECTOR]>,

    // Attestations (Phase 0)
    pub previous_epoch_attestations:
        BoundedList<PendingAttestation, MAX_PENDING_ATTESTATIONS>,
    pub current_epoch_attestations:
        BoundedList<PendingAttestation, MAX_PENDING_ATTESTATIONS>,

    // Finality
    pub justification_bits:            Bitvector<JUSTIFICATION_BITS_LENGTH>,
    pub previous_justified_checkpoint: Checkpoint,
    pub current_justified_checkpoint:  Checkpoint,
    pub finalized_checkpoint:          Checkpoint,
}

ssz_struct!(BeaconState {
    genesis_time:                  u64,
    genesis_validators_root:       Root,
    slot:                          Slot,
    fork:                          Fork,
    latest_block_header:           BeaconBlockHeader,
    block_roots:                   Box<[Root; SLOTS_PER_HISTORICAL_ROOT]>,
    state_roots:                   Box<[Root; SLOTS_PER_HISTORICAL_ROOT]>,
    historical_roots:              BoundedList<Root, HISTORICAL_ROOTS_LIMIT>,
    eth1_data:                     Eth1Data,
    eth1_data_votes:               BoundedList<Eth1Data, SLOTS_PER_ETH1_VOTING_PERIOD>,
    eth1_deposit_index:            u64,
    validators:                    BoundedList<Validator, VALIDATOR_REGISTRY_LIMIT>,
    balances:                      BoundedList<Gwei, VALIDATOR_REGISTRY_LIMIT>,
    randao_mixes:                  Box<[Bytes32; EPOCHS_PER_HISTORICAL_VECTOR]>,
    slashings:                     Box<[Gwei; EPOCHS_PER_SLASHINGS_VECTOR]>,
    previous_epoch_attestations:   BoundedList<PendingAttestation, MAX_PENDING_ATTESTATIONS>,
    current_epoch_attestations:    BoundedList<PendingAttestation, MAX_PENDING_ATTESTATIONS>,
    justification_bits:            Bitvector<JUSTIFICATION_BITS_LENGTH>,
    previous_justified_checkpoint: Checkpoint,
    current_justified_checkpoint:  Checkpoint,
    finalized_checkpoint:          Checkpoint,
});
pssz_struct!(BeaconState {
    genesis_time:                  u64,
    genesis_validators_root:       Root,
    slot:                          Slot,
    fork:                          Fork,
    latest_block_header:           BeaconBlockHeader,
    block_roots:                   Box<[Root; SLOTS_PER_HISTORICAL_ROOT]>,
    state_roots:                   Box<[Root; SLOTS_PER_HISTORICAL_ROOT]>,
    historical_roots:              BoundedList<Root, HISTORICAL_ROOTS_LIMIT>,
    eth1_data:                     Eth1Data,
    eth1_data_votes:               BoundedList<Eth1Data, SLOTS_PER_ETH1_VOTING_PERIOD>,
    eth1_deposit_index:            u64,
    validators:                    BoundedList<Validator, VALIDATOR_REGISTRY_LIMIT>,
    balances:                      BoundedList<Gwei, VALIDATOR_REGISTRY_LIMIT>,
    randao_mixes:                  Box<[Bytes32; EPOCHS_PER_HISTORICAL_VECTOR]>,
    slashings:                     Box<[Gwei; EPOCHS_PER_SLASHINGS_VECTOR]>,
    previous_epoch_attestations:   BoundedList<PendingAttestation, MAX_PENDING_ATTESTATIONS>,
    current_epoch_attestations:    BoundedList<PendingAttestation, MAX_PENDING_ATTESTATIONS>,
    justification_bits:            Bitvector<JUSTIFICATION_BITS_LENGTH>,
    previous_justified_checkpoint: Checkpoint,
    current_justified_checkpoint:  Checkpoint,
    finalized_checkpoint:          Checkpoint,
});
